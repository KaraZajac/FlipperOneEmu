#!/usr/bin/env python3
# Copyright (c) 2026, Kara Zajac
# SPDX-License-Identifier: BSD-3-Clause
"""The MCU firmware's log from its SEGGER RTT buffer, through QEMU's monitor.

    scripts/rtt.py MONITOR.sock firmware/mcu/BUILD/flipper-one-mcu-f1-firmware-*.elf
    scripts/rtt.py MONITOR.sock 0x20005350

The firmware writes its log to RTT (a ring in its RAM, which a debugger
reads) as well as to its log UART. This finds `_SEGGER_RTT` in the ELF (or
takes its address), reads up-buffer 0 with the monitor's `xp` (start QEMU with
`-monitor unix:MONITOR.sock,server,nowait`), and prints what is in it.
"""
import argparse
import re
import socket
import struct
import sys


def elf_symbol(path, name):
    """A symbol's address in a 32-bit little-endian ELF."""
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 1:
        raise SystemExit(f"{path}: not a 32-bit ELF")
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x2e)
    sections = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize)
                for i in range(shnum)]
    for sh in sections:
        if sh[1] != 2:                              # SHT_SYMTAB
            continue
        strtab = sections[sh[6]]
        for off in range(sh[4], sh[4] + sh[5], 16):
            st_name, st_value = struct.unpack_from("<II", data, off)
            end = data.index(b"\0", strtab[4] + st_name)
            if data[strtab[4] + st_name:end].decode() == name:
                return st_value
    raise SystemExit(f"{path}: no symbol {name}")


class Monitor:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.connect(path)
        self.sock.settimeout(5)
        self._until_prompt()

    def _until_prompt(self):
        out = b""
        while not out.rstrip().endswith(b"(qemu)"):
            chunk = self.sock.recv(65536)
            if not chunk:
                break
            out += chunk
        return out.decode("latin1")

    def cmd(self, line):
        self.sock.sendall(line.encode() + b"\n")
        return self._until_prompt()

    def words(self, addr, count):
        text = self.cmd(f"xp /{count}wx {addr:#x}")
        found = []
        for row in re.findall(r"^[0-9a-f]+:((?:\s+0x[0-9a-f]+)+)", text, re.M):
            found += [int(x, 16) for x in row.split()]
        return found[:count]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("monitor", help="the monitor's Unix socket")
    ap.add_argument("where", help="the firmware's ELF, or _SEGGER_RTT's address")
    args = ap.parse_args()
    if re.fullmatch(r"(0x)?[0-9a-fA-F]+", args.where):
        cb = int(args.where, 16)
    else:
        cb = elf_symbol(args.where, "_SEGGER_RTT")
    m = Monitor(args.monitor)
    head = m.words(cb, 14)
    ident = b"".join(w.to_bytes(4, "little") for w in head[:4]).split(b"\0")[0]
    if ident != b"SEGGER RTT":
        raise SystemExit(f"no RTT control block at {cb:#x} ({ident!r})")
    _name, buf, size, wr, _rd, _flags = head[6:12]
    data = bytearray()
    for addr in range(buf, buf + size, 256):
        for w in m.words(addr, min(64, (buf + size - addr) // 4)):
            data += w.to_bytes(4, "little")
    # a ring: once wrapped, the oldest text starts after the write offset
    text = data[wr:] + data[:wr] if data[wr:].strip(b"\0") else data[:wr]
    sys.stdout.write(text.decode("utf-8", "replace").replace("\0", ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
