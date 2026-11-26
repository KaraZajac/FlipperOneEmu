#!/usr/bin/env python3
# Copyright (c) 2026, Kara Zajac
# SPDX-License-Identifier: BSD-3-Clause
"""Rockchip's maskrom protocol over USB/IP: what rockusb (or rkdeveloptool)
does to an RK3576 in its MaskROM, from here.

    ./flipperoneemu run --usbip 3240 --detach   (and plug a computer in: the
    panel link's `usb 2`, the window's Ctrl+Shift+U); then the MCU's Maskrom
    (its CPU app's, or `desktop start_app cpu_app_maskrom` on its CLI) puts
    the RK3576 in its MaskROM, its USB0 on the cable
    scripts/fo_rockusb.py [--port 3240] list
    scripts/fo_rockusb.py [--port 3240] download-boot LOADER.bin [--no-crc]

The boot ROM takes code in vendor requests on the control endpoint (0x40,
0x0c, index 0x471 or 0x472, up to 4096 bytes each). A shorter request ends
the payload; its last two bytes are a CRC-16 (CCITT, from 0xffff) of what
came before them, or two zeros, which the RK3576 takes unchecked
(--no-crc). download-boot sends a Rockchip loader's 0x471 entries (the DDR
blob), each followed by its delay, then its 0x472 ones (SPL), as rockusb
does: for the Flipper OS installer, its installer-falcon-loader.bin. No root
is needed: this speaks USB/IP from userspace (fo_usbip.py), as the PC's
kernel would. With the real rockusb, `usbip attach` the device instead.

Standard library only.
"""
import argparse
import binascii
import pathlib
import struct
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import fo_usbip  # noqa: E402

ROCKCHIP = 0x2207
CHUNK = 4096


def loader_entries(blob):
    """A Rockchip loader's (rkdeveloptool's "BOOT"/"LDR " file) 0x471 and
    0x472 entries: (code, name, data, delay in ms), in the order they go."""
    if len(blob) < 102 or blob[:4] not in (b"BOOT", b"LDR "):
        raise ValueError("not a Rockchip loader (no BOOT or LDR tag)")
    c471, o471, s471, c472, o472, s472 = struct.unpack_from("<BIBBIB", blob, 25)
    entries = []
    for code, count, off, size in ((0x471, c471, o471, s471),
                                   (0x472, c472, o472, s472)):
        for i in range(count):
            e = off + i * size
            name = blob[e + 5:e + 45].decode("utf-16-le", "replace").split("\0")[0]
            doff, dsize, delay = struct.unpack_from("<III", blob, e + 45)
            if doff + dsize > len(blob):
                raise ValueError(f"its {code:#x} entry {name!r} runs past the file")
            entries.append((code, name, blob[doff:doff + dsize], delay))
    return entries


def connect(host="127.0.0.1", port=3240, busid="1-1", wait=5.0):
    """The device, imported over USB/IP. A moment's wait if the import is
    refused: after another client lets go of it, the computer takes it back
    with a reset and enumerates it again first."""
    end = time.monotonic() + wait
    while True:
        try:
            return fo_usbip.UsbipClient(host, port, busid)
        except ConnectionError:
            if time.monotonic() > end:
                raise
            time.sleep(0.2)


def write_area(c, code, data, crc=True):
    """A payload, as rockusb and rkdeveloptool send one: 4096-byte requests,
    the last one shorter, with the CRC; one of a single byte after it if the
    whole came out at a multiple of 4096 (else nothing would be shorter)."""
    if len(data) % CHUNK == CHUNK - 1:
        data += b"\0"                   # (the CRC whole in one request)
    v = binascii.crc_hqx(data, 0xFFFF) if crc else 0
    body = data + bytes((v >> 8, v & 0xFF))
    for at in range(0, len(body), CHUNK):
        c.control(0x40, 0x0C, 0, code, data=body[at:at + CHUNK])
    if len(body) % CHUNK == 0:
        c.control(0x40, 0x0C, 0, code, data=b"\0")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=3240)
    ap.add_argument("--busid", default="1-1")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list", help="the Rockchip devices on the USB/IP server")
    db = sub.add_parser("download-boot", help="a loader's 0x471 and 0x472 code")
    db.add_argument("loader", type=pathlib.Path)
    db.add_argument("--no-crc", action="store_true",
                    help="two zero bytes where the CRC goes")
    a = ap.parse_args()

    if a.cmd == "list":
        devs = [d for d in fo_usbip.devlist(a.host, a.port) if d["vid"] == ROCKCHIP]
        if not devs:
            print("no Rockchip device (the RK3576 in its MaskROM, a computer "
                  "plugged in?)")
            return 1
        for d in devs:
            itf = " ".join("%02x/%02x/%02x" % i for i in d["interfaces"])
            print(f"{d['busid']}: ID {d['vid']:04x}:{d['pid']:04x}, interface {itf}"
                  + (" (Rockusb)" if (0xFF, 0x06, 0x05) in d["interfaces"] else ""))
        return 0

    entries = loader_entries(a.loader.read_bytes())
    c = connect(a.host, a.port, a.busid)
    try:
        if c.dev["vid"] != ROCKCHIP:
            print(f"{a.busid} is {c.dev['vid']:04x}:{c.dev['pid']:04x}, not a "
                  "Rockchip device")
            return 1
        for code, name, data, delay in entries:
            t0 = time.monotonic()
            write_area(c, code, data, crc=not a.no_crc)
            print(f"{code:#x} {name}: {len(data)} bytes in "
                  f"{time.monotonic() - t0:.1f} s"
                  + (f", then {delay} ms" if delay else ""), flush=True)
            time.sleep(delay / 1000)
    finally:
        c.close()
    print("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
