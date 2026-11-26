#!/usr/bin/env python3
# Copyright (c) 2026, Kara Zajac
# SPDX-License-Identifier: BSD-3-Clause
"""Rockchip's maskrom and Rockusb protocols over USB/IP: what rockusb (or
rkdeveloptool) does to an RK3576 in its MaskROM, or to a Rockusb device in
its Loader mode, from here.

    ./flipperoneemu run --usbip 3240 --detach   (and plug a computer in: the
    panel link's `usb 2`, the window's Ctrl+Shift+U); then the MCU's Maskrom
    (its CPU app's, or `desktop start_app cpu_app_maskrom` on its CLI) puts
    the RK3576 in its MaskROM, its USB0 on the cable
    scripts/fo_rockusb.py [--port 3240] list
    scripts/fo_rockusb.py [--port 3240] download-boot LOADER.bin [--no-crc]
    scripts/fo_rockusb.py [--port 3240] download-sram | download-ddr FILE
    scripts/fo_rockusb.py [--port 3240] chip-info | flash-id | flash-info
    scripts/fo_rockusb.py [--port 3240] capability | storage
    scripts/fo_rockusb.py [--port 3240] reset [maskrom | power-off ...]
    scripts/fo_rockusb.py [--port 3240] switch-storage ufs | sd | emmc ...
    scripts/fo_rockusb.py [--port 3240] read START COUNT FILE | write START FILE

The boot ROM takes code in vendor requests on the control endpoint (0x40,
0x0c, index 0x471 or 0x472, up to 4096 bytes each). A shorter request ends
the payload; its last two bytes are a CRC-16 (CCITT, from 0xffff) of what
came before them, or two zeros, which the RK3576 takes unchecked
(--no-crc). download-boot sends a Rockchip loader's 0x471 entries (the DDR
blob), each followed by its delay, then its 0x472 ones (SPL, or a usbplug),
as rockusb does: for the Flipper OS installer, its installer-falcon-loader.bin
(download-sram and download-ddr send a file as 0x471 or 0x472 code). No root
is needed: this speaks USB/IP from userspace (fo_usbip.py), as the PC's
kernel would. With the real rockusb, `usbip attach` the device instead.

The Loader mode is what a usbplug runs (0x472 code with no FIT: the
emulator plays one, the UFS behind it), or U-Boot's `rockusb 0 mmc 0` (its
shell: `s` on the console at power-on): bulk-only transport, a 31-byte
command block (USBC: Rockchip's command, a sector, a count) out on the bulk
endpoint, the data, a 13-byte status (USBS) in. chip-info, flash-id,
flash-info, capability, storage, switch-storage, read and write (512-byte
sectors of the storage) and reset are rockusb's commands of the same names.
(U-Boot's rockusb takes no flash-info or storage, and its chip-info is zeros
unless the SoC's code gives one.)

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
SECTOR = 512
# Rockusb's storages: their indexes (rockusb's, rkdeveloptool's)
STORAGES = {"nand": 0, "emmc": 1, "sd": 2, "sd0": 2, "sd1": 3, "spinor": 4,
            "spinand": 5, "ram": 6, "sata": 10, "pcie": 11, "ufs": 12}
# Rockusb's reset: its subcodes (rockusb's names; rkdeveloptool's rd N)
RESETS = {"reset": 0, "msc": 1, "power-off": 2, "maskrom": 3, "disconnect": 4}
CAPABILITIES = ((0, 0x01, "direct LBA"), (0, 0x02, "vendor storage"),
                (0, 0x04, "first 4M access"), (0, 0x08, "read LBA"),
                (0, 0x20, "read com log"), (0, 0x40, "read IDB config"),
                (0, 0x80, "read secure mode"), (1, 0x01, "new IDB"),
                (1, 0x02, "switch storage"))


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


class Loader:
    """A device in Rockusb's Loader mode, over USB/IP"""

    def __init__(self, c):
        self.usb = c
        cfg = c.control(0x80, 6, 0x0200, 0, 255)
        self.ep_in = self.ep_out = None
        cls, i = None, 0
        while i + 2 <= len(cfg) and cfg[i] >= 2:
            d = cfg[i:i + cfg[i]]
            if d[1] == 4:                               # INTERFACE
                cls = d[5]
            elif d[1] == 5 and cls == 0xFF and d[3] & 3 == 2:
                if d[2] & 0x80:
                    self.ep_in = d[2] & 0x0F
                else:
                    self.ep_out = d[2] & 0x0F
            i += cfg[i]
        if self.ep_in is None or self.ep_out is None:
            raise IOError("no Rockusb interface with bulk endpoints")
        c.control(0x00, 9, cfg[5], 0)                   # SET_CONFIGURATION
        self.tag = 0

    def _reply(self, seq):
        while True:
            r = self.usb.receive()
            if r[0] == fo_usbip.RET_SUBMIT and r[1] == seq:
                return r

    def _out(self, data):
        status = self._reply(self.usb.submit(self.ep_out, False, data=data))[2]
        if status:
            raise IOError(f"Rockusb: bulk OUT failed ({status})")

    def _in(self, length):
        _, _, status, data, _ = self._reply(self.usb.submit_in(self.ep_in, length))
        if status == -75 and length < 13:
            # more than was asked for: a status (13 bytes) where the data
            # should have been, a command the device does not take (U-Boot's
            # rockusb answers flash-info so)
            raise IOError("Rockusb: the device answered with a status at once: "
                          "it does not take this command")
        if status:
            raise IOError(f"Rockusb: bulk IN failed ({status})")
        return data

    def command(self, code, length=0, read=True, address=0, count=0, cdb=6,
                opcode=0, data=b""):
        """a command block, its data (in, or @data out), its status"""
        self.tag += 1
        # (the tag, the sector and the count big-endian, the length little)
        cbw = (b"USBC" + struct.pack(">I", self.tag) + struct.pack("<I", length) +
               bytes((0x80 if read else 0, 0, cdb, code, opcode)) +
               struct.pack(">IxH", address, count))
        self._out(cbw.ljust(31, b"\0"))
        got, csw = b"", None
        if read:
            while len(got) < length:
                chunk = self._in(min(length - len(got), 65536))
                if len(chunk) == 13 and chunk[:4] == b"USBS" and \
                        struct.unpack(">I", chunk[4:8])[0] == self.tag:
                    csw = chunk         # (its status at once: no data)
                    break
                if not chunk:
                    break
                got += chunk
        else:
            for i in range(0, len(data), 65536):
                self._out(data[i:i + 65536])
        csw = csw or self._in(13)
        if len(csw) < 13 or csw[:4] != b"USBS" or \
                struct.unpack(">I", csw[4:8])[0] != self.tag:
            raise IOError(f"Rockusb: command {code:#04x}: no status for it")
        if csw[12]:
            raise IOError(f"Rockusb: command {code:#04x} failed (status {csw[12]})")
        return got

    def read_lba(self, start, count):
        out = b""
        while count:
            n = min(count, 128)
            out += self.command(0x14, n * SECTOR, True, start, n, cdb=0x0A)
            start, count = start + n, count - n
        return out

    def write_lba(self, start, data):
        data += bytes(-len(data) % SECTOR)
        for i in range(0, len(data), 128 * SECTOR):
            chunk = data[i:i + 128 * SECTOR]
            self.command(0x15, len(chunk), False, start + i // SECTOR,
                         len(chunk) // SECTOR, cdb=0x0A, data=chunk)


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
    for name, code in (("download-sram", 0x471), ("download-ddr", 0x472)):
        dl = sub.add_parser(name, help=f"a file as {code:#x} code")
        dl.add_argument("file", type=pathlib.Path)
        dl.add_argument("--no-crc", action="store_true",
                        help="two zero bytes where the CRC goes")
    sub.add_parser("chip-info", help="(Loader mode) the chip's 16 bytes")
    sub.add_parser("flash-id", help="(Loader mode) the storage's 5 bytes")
    sub.add_parser("flash-info", help="(Loader mode) the storage's size")
    sub.add_parser("capability", help="(Loader mode) what it takes")
    sub.add_parser("storage", help="(Loader mode) the storage it is on")
    ss = sub.add_parser("switch-storage", help="(Loader mode) another storage")
    ss.add_argument("storage", choices=sorted(STORAGES))
    rd = sub.add_parser("read", help="(Loader mode) sectors into a file")
    rd.add_argument("start", type=int)
    rd.add_argument("count", type=int)
    rd.add_argument("file", type=pathlib.Path)
    wr = sub.add_parser("write", help="(Loader mode) a file into sectors")
    wr.add_argument("start", type=int)
    wr.add_argument("file", type=pathlib.Path)
    rs = sub.add_parser("reset", help="(Loader mode) the device restarts "
                        "(maskrom: into its MaskROM; power-off: off)")
    rs.add_argument("subcode", nargs="?", default="reset", choices=RESETS)
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

    if a.cmd in ("download-sram", "download-ddr"):
        c = connect(a.host, a.port, a.busid)
        try:
            data = a.file.read_bytes()
            code = 0x471 if a.cmd == "download-sram" else 0x472
            write_area(c, code, data, crc=not a.no_crc)
            print(f"{code:#x}: {len(data)} bytes")
        finally:
            c.close()
        return 0

    if a.cmd != "download-boot":
        c = connect(a.host, a.port, a.busid)
        try:
            ld = Loader(c)
            if a.cmd == "chip-info":
                info = ld.command(0x1B, 16)
                print(info.hex(" "), repr(info[:4][::-1].decode("latin-1")))
            elif a.cmd == "flash-id":
                info = ld.command(0x01, 5)
                print(info.hex(" "), repr(info.decode("latin-1")))
            elif a.cmd == "capability":
                info = ld.command(0xAA, 8)
                print(info.hex(" ") + ": " + (", ".join(
                    name for byte, bit, name in CAPABILITIES
                    if len(info) > byte and info[byte] & bit) or "nothing"))
            elif a.cmd == "storage":
                bits = int.from_bytes(ld.command(0x2B, 4).ljust(4, b"\0"), "little")
                index = (bits & -bits).bit_length() - 1
                names = [n for n, i in STORAGES.items() if i == index]
                print(f"{index} ({names[0] if names else 'unknown'})"
                      if bits else "none")
            elif a.cmd == "switch-storage":
                ld.command(0x2A, 0, False, opcode=STORAGES[a.storage])
                print(f"on {a.storage} ({STORAGES[a.storage]})")
            elif a.cmd == "flash-info":
                info = ld.command(0x1A, 11)
                sectors = struct.unpack_from("<I", info)[0]
                print(f"{sectors} sectors of {SECTOR} bytes "
                      f"({sectors * SECTOR / 1e9:.2f} GB); {info.hex(' ')}")
            elif a.cmd == "read":
                t0 = time.monotonic()
                a.file.write_bytes(ld.read_lba(a.start, a.count))
                print(f"{a.count} sectors from {a.start} in "
                      f"{time.monotonic() - t0:.1f} s")
            elif a.cmd == "write":
                t0 = time.monotonic()
                data = a.file.read_bytes()
                ld.write_lba(a.start, data)
                print(f"{-(-len(data) // SECTOR)} sectors at {a.start} in "
                      f"{time.monotonic() - t0:.1f} s")
            else:
                ld.command(0xFF, 0, False, opcode=RESETS[a.subcode])
                print("reset" + (f" ({a.subcode})" if a.subcode != "reset"
                                 else ""))
        finally:
            c.close()
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
