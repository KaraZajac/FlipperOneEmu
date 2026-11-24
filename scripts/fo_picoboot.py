#!/usr/bin/env python3
# Copyright (c) 2026, Kara Zajac
# SPDX-License-Identifier: BSD-3-Clause
"""PICOBOOT over USB/IP: what picotool does to an RP2350 in BOOTSEL, from here.

    ./flipperoneemu run --bootsel --usbip 3240 --detach   (and plug a computer
    in: the panel link's `usb 2`, the window's Ctrl+Shift+U)
    scripts/fo_picoboot.py [--port 3240] read ADDR SIZE [FILE]
    scripts/fo_picoboot.py [--port 3240] load FIRMWARE.uf2 [--reboot]
    scripts/fo_picoboot.py [--port 3240] reboot

The boot ROM's USB boot has a vendor interface, PICOBOOT: a 32-byte command
on its bulk OUT endpoint (magic 0x431fd10b, a token, the command, its
arguments, how many bytes follow), the data, if any, then an empty packet
the other way when the command is done. `load` writes a UF2's blocks into
the flash as picotool does: each 4 KiB sector they touch erased, then the
256-byte pages written. No root is needed: this speaks USB/IP from
userspace (fo_usbip.py), as the PC's kernel would.

Standard library only.
"""
import argparse
import pathlib
import socket
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import fo_usbip  # noqa: E402

MAGIC = 0x431FD10B
EXCLUSIVE_ACCESS, REBOOT, FLASH_ERASE, READ, WRITE, EXIT_XIP = (
    0x01, 0x02, 0x03, 0x84, 0x05, 0x06)
REBOOT2 = 0x0A
FLASH = 0x10000000
SECTOR, PAGE = 4096, 256
UF2_MAGIC = (0x0A324655, 0x9E5D5157, 0x0AB16F30)
STATUS = ("ok", "unknown command", "invalid command length",
          "invalid transfer length", "invalid address", "bad alignment",
          "interleaved write", "rebooting", "unknown error", "invalid state",
          "not permitted", "invalid argument", "buffer too small",
          "precondition not met", "modified data", "invalid data",
          "not found", "unsupported modification")
RP2350_FAMILIES = {0xE48BFF59, 0xE48BFF5A, 0xE48BFF5B, 0xE48BFF57}


class Picoboot:
    """The boot ROM's PICOBOOT interface, over a USB/IP import"""

    def __init__(self, port=3240, host="127.0.0.1"):
        self.usb = fo_usbip.UsbipClient(host=host, port=port)
        cfg = self.usb.control(0x80, 6, 0x0200, 0, 255)
        self.ep_out = self.ep_in = None
        cls = None
        self.itf = 0
        i = 0
        while i + 2 <= len(cfg) and cfg[i] >= 2:
            d = cfg[i:i + cfg[i]]
            if d[1] == 4:                               # INTERFACE
                cls = d[5]
                if cls == 0xFF:
                    self.itf = d[2]
            elif d[1] == 5 and cls == 0xFF and d[3] & 3 == 2:
                if d[2] & 0x80:
                    self.ep_in = d[2] & 0x0F
                else:
                    self.ep_out = d[2] & 0x0F
            i += cfg[i]
        if self.ep_in is None or self.ep_out is None:
            raise IOError("no PICOBOOT interface: is it the boot ROM's USB "
                          "boot (BOOTSEL)?")
        self.usb.control(0x00, 9, cfg[5], 0)           # SET_CONFIGURATION
        self.token = 1

    def close(self):
        self.usb.close()

    def status(self):
        """GET_COMMAND_STATUS: the last command's, as the boot ROM says"""
        s = self.usb.control(0xC1, 0x42, 0, self.itf, 16)
        token, code, cmd, busy = struct.unpack_from("<IIBB", s)
        return STATUS[code] if code < len(STATUS) else f"status {code}"

    def _failed(self, what, status):
        try:
            why = self.status()
        except OSError:
            why = "?"
        raise IOError(f"PICOBOOT: {what} failed ({status}): {why}")

    def _out(self, data):
        seq = self.usb.submit(self.ep_out, False, data=data)
        status = self._reply(seq)[2]
        if status:
            self._failed("OUT", status)

    def _in(self, length):
        seq = self.usb.submit_in(self.ep_in, length)
        _, _, status, data, _ = self._reply(seq)
        if status:
            self._failed("IN", status)
        return data

    def _reply(self, seq):
        """the reply to submission @seq (anything else is passed over)"""
        while True:
            r = self.usb.receive()
            if r[0] == fo_usbip.RET_SUBMIT and r[1] == seq:
                return r

    def command(self, cmd, args=b"", data=b"", read=0):
        """one command: its data out (or @read bytes in), then its ack"""
        try:
            return self._command(cmd, args, data, read)
        except socket.timeout:
            raise IOError(f"PICOBOOT: command {cmd:#04x} "
                          f"({args[:8].hex()}): no answer") from None

    def _command(self, cmd, args, data, read):
        length = read if cmd & 0x80 else len(data)
        self._out(struct.pack("<IIBBHI16s", MAGIC, self.token, cmd, len(args),
                              0, length, args))
        self.token += 1
        got = b""
        if cmd & 0x80:
            while len(got) < read:
                chunk = self._in(min(read - len(got), 4096))
                if not chunk:
                    break
                got += chunk
            self._out(b"")                              # the ack: empty OUT
        else:
            for i in range(0, len(data), 4096):
                self._out(data[i:i + 4096])
            self._in(0)                                 # the ack: empty IN
        return got

    def exclusive(self, on=True):
        self.command(EXCLUSIVE_ACCESS, bytes([1 if on else 0]))

    def exit_xip(self):
        self.command(EXIT_XIP)

    def read(self, addr, size):
        return self.command(READ, struct.pack("<II", addr, size), read=size)

    def erase(self, addr, size):
        self.command(FLASH_ERASE, struct.pack("<II", addr, size))

    def write(self, addr, data):
        self.command(WRITE, struct.pack("<II", addr, len(data)), data=data)

    def reboot(self, delay_ms=100):
        """REBOOT2: a normal boot, from the flash"""
        self.command(REBOOT2, struct.pack("<IIII", 0, delay_ms, 0, 0))


def uf2_blocks(path):
    """(address, 256 bytes) of each block for the RP2350's flash"""
    data = pathlib.Path(path).read_bytes()
    for off in range(0, len(data) - 511, 512):
        m0, m1, flags, addr, size, _, _, family = struct.unpack_from(
            "<IIIIIIII", data, off)
        if (m0, m1) != UF2_MAGIC[:2] or \
                struct.unpack_from("<I", data, off + 508)[0] != UF2_MAGIC[2]:
            raise ValueError(f"{path}: not a UF2 at {off:#x}")
        if flags & 1:                                   # not for the flash
            continue
        if flags & 0x2000 and family not in RP2350_FAMILIES:
            continue                                    # another chip's
        if size != PAGE or not FLASH <= addr < FLASH + 0x1000000:
            continue
        yield addr, data[off + 32:off + 32 + PAGE]


def load(pb, path, progress=print):
    blocks = sorted(uf2_blocks(path))
    sectors = sorted({a & ~(SECTOR - 1) for a, _ in blocks})
    pb.exclusive()
    pb.exit_xip()
    for n, s in enumerate(sectors):
        pb.erase(s, SECTOR)
        if progress and n % 64 == 0:
            progress(f"erasing: {n}/{len(sectors)} sectors")
    for n, (a, page) in enumerate(blocks):
        pb.write(a, page)
        if progress and n % 512 == 0:
            progress(f"writing: {n}/{len(blocks)} pages")
    return len(blocks)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=3240)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("read")
    r.add_argument("addr", type=lambda s: int(s, 0))
    r.add_argument("size", type=lambda s: int(s, 0))
    r.add_argument("file", nargs="?")
    lo = sub.add_parser("load")
    lo.add_argument("uf2")
    lo.add_argument("--reboot", action="store_true")
    sub.add_parser("reboot")
    args = ap.parse_args()
    try:
        pb = Picoboot(args.port)
    except (OSError, socket.error) as e:
        sys.exit(f"fo_picoboot: {e}")
    try:
        if args.cmd == "read":
            data = pb.read(args.addr, args.size)
            if args.file:
                pathlib.Path(args.file).write_bytes(data)
            else:
                for i in range(0, len(data), 16):
                    print(f"{args.addr + i:08x}  {data[i:i + 16].hex(' ')}")
        elif args.cmd == "load":
            n = load(pb, args.uf2)
            print(f"{n} pages written")
            if args.reboot:
                pb.reboot()
        elif args.cmd == "reboot":
            pb.reboot()
    finally:
        pb.close()


if __name__ == "__main__":
    main()
