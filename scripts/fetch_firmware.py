#!/usr/bin/env python3
# Copyright (c) 2026, Kara Zajac
# SPDX-License-Identifier: BSD-3-Clause
"""Fetch what the emulator runs: the MCU firmware's builds and the RP2350's boot ROM.

    scripts/fetch_firmware.py             # the newest dev build, and boot ROM A4
    scripts/fetch_firmware.py --list      # the builds on Flipper's server

Into firmware/, which is not tracked:
  firmware/mcu/<build>/   the MCU firmware as Flipper's CI builds it on every push to
                          `dev`: .elf (with symbols), .bin and .uf2 of the targets f1
                          (rev F0B0C1) and f2 (rev 2.F0B1C2), the recovery .uf2s
                          (partition table and firmware) and the partition table's
  firmware/bootrom/A4/    Raspberry Pi's build of the RP2350 boot ROM, release A4:
                          bootrom-combined.bin (the 32 KiB mask ROM), arm-bootrom.elf
                          and nsboot.elf (the same with symbols)
Each folder gets a SHA256SUMS. A file already there is not fetched again.
"""
import argparse
import hashlib
import pathlib
import re
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
MCU = "https://update.flipperzero.one/builds/flipper-one-mcu/dev/"
BOOTROM = "https://github.com/raspberrypi/pico-bootrom-rp2350/releases/download/"
BOOTROM_RELEASE = "A4"
BOOTROM_FILES = ("bootrom-combined.bin", "arm-bootrom.elf", "nsboot.elf")
AGENT = {"User-Agent": "FlipperOneEmu-fetch/1"}


def get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=AGENT),
                                timeout=120) as r:
        return r.read()


def builds():
    """{build: [file names]} on the server, build = "dev-DDMMYYYY-commit"."""
    page = get(MCU).decode("utf-8", "replace")
    found = {}
    for name in sorted(set(re.findall(r'href="(flipper-one-mcu-[^"/]+)"', page))):
        m = re.search(r"-(dev-\d{8}-[0-9a-f]+)\.(uf2|bin|elf)$", name)
        if m:
            found.setdefault(m.group(1), []).append(name)
    return found


def fetch(url, out):
    if out.exists():
        return False
    data = get(url)
    tmp = out.with_suffix(out.suffix + ".part")
    tmp.write_bytes(data)
    tmp.rename(out)
    return True


def sums(folder):
    lines = [f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}"
             for p in sorted(folder.iterdir())
             if p.is_file() and p.name != "SHA256SUMS"]
    (folder / "SHA256SUMS").write_text("\n".join(lines) + "\n")
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--list", action="store_true", help="list the builds and stop")
    args = ap.parse_args()
    found = builds()
    if args.list:
        for build, names in sorted(found.items()):
            print(build)
            for name in names:
                print("   ", name)
        return 0
    if not found:
        print(f"no builds found at {MCU}", file=sys.stderr)
        return 1
    # the newest by its date (DDMMYYYY), then its name
    build = max(found, key=lambda b: (b[10:14] + b[8:10] + b[6:8], b))
    folder = ROOT / "firmware" / "mcu" / build
    folder.mkdir(parents=True, exist_ok=True)
    for name in found[build]:
        if fetch(MCU + name, folder / name):
            print(f">> {folder.relative_to(ROOT)}/{name}")
    for line in sums(folder):
        print("   ", line)
    folder = ROOT / "firmware" / "bootrom" / BOOTROM_RELEASE
    folder.mkdir(parents=True, exist_ok=True)
    for name in BOOTROM_FILES:
        if fetch(f"{BOOTROM}{BOOTROM_RELEASE}/{name}", folder / name):
            print(f">> {folder.relative_to(ROOT)}/{name}")
    for line in sums(folder):
        print("   ", line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
