#!/usr/bin/env python3
# Copyright (c) 2026, Kara Zajac
# SPDX-License-Identifier: BSD-3-Clause
"""A flash image from .uf2 files, as the RP2350's flash would hold them.

    scripts/uf2.py OUT.bin firmware/mcu/BUILD/flipper-one-mcu-f1-recovery-*.uf2

OUT.bin is the 16 MiB flash, erased (0xff), with each block of the UF2 files
written at its address (0x10000000 is the flash's first byte). A recovery
.uf2 carries absolute addresses: the partition table at 0, the firmware in
partition A after it. A firmware .uf2's blocks are for the boot ROM to place
in a partition (family rp2350-arm-s); this tool writes only "absolute" ones
and says so for the rest. `--info` lists what a UF2 holds.
"""
import argparse
import struct
import sys

FLASH = 0x10000000
SIZE = 16 << 20
MAGIC = (0x0A324655, 0x9E5D5157, 0x0AB16F30)
FAMILIES = {
    0xe48bff57: "absolute",
    0xe48bff58: "data",
    0xe48bff59: "rp2350-arm-s",
    0xe48bff5a: "rp2350-riscv",
    0xe48bff5b: "rp2350-arm-ns",
    0xe48bff56: "rp2040",
}
F_NOT_MAIN = 0x0001
F_FAMILY = 0x2000


def blocks(path):
    data = open(path, "rb").read()
    if len(data) % 512:
        raise SystemExit(f"{path}: not whole 512-byte blocks")
    for off in range(0, len(data), 512):
        m0, m1, flags, addr, size, no, count, family = \
            struct.unpack_from("<8I", data, off)
        (end,) = struct.unpack_from("<I", data, off + 508)
        if (m0, m1, end) != MAGIC:
            raise SystemExit(f"{path}: block {off // 512} is not a UF2 block")
        yield flags, addr, data[off + 32:off + 32 + size], \
            family if flags & F_FAMILY else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("out", help="the flash image to write (or a UF2 with --info)")
    ap.add_argument("uf2", nargs="*")
    ap.add_argument("--info", action="store_true", help="list what UF2s hold")
    args = ap.parse_args()
    if args.info:
        for path in [args.out] + args.uf2:
            ranges = {}
            for flags, addr, payload, family in blocks(path):
                lo, hi = ranges.get((family, flags), (addr, addr))
                ranges[family, flags] = (min(lo, addr),
                                         max(hi, addr + len(payload)))
            print(path)
            for (family, flags), (lo, hi) in ranges.items():
                name = FAMILIES.get(family, f"{family:#x}" if family else "-")
                print(f"    {name:14} flags {flags:#06x}  {lo:#010x}..{hi:#010x}")
        return 0
    flash = bytearray(b"\xff" * SIZE)
    skipped = {}
    for path in args.uf2:
        for flags, addr, payload, family in blocks(path):
            if flags & F_NOT_MAIN:
                continue
            if family is not None and FAMILIES.get(family) != "absolute":
                skipped[FAMILIES.get(family, hex(family))] = \
                    skipped.get(FAMILIES.get(family, hex(family)), 0) + 1
                continue
            off = addr - FLASH
            if not 0 <= off <= SIZE - len(payload):
                raise SystemExit(f"{path}: a block at {addr:#x} is outside the flash")
            flash[off:off + len(payload)] = payload
    for family, n in skipped.items():
        print(f"skipped {n} blocks of family {family} (for the boot ROM to "
              "place; use a recovery .uf2)", file=sys.stderr)
    with open(args.out, "wb") as f:
        f.write(flash)
    return 0


if __name__ == "__main__":
    sys.exit(main())
