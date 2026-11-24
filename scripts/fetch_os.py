#!/usr/bin/env python3
# Copyright (c) 2026, Kara Zajac
# SPDX-License-Identifier: BSD-3-Clause
"""Fetch a Flipper OS build for the RK3576: a profile's kernel, its initrd and
the device trees, for `./flipperoneemu run --linux`.

    scripts/fetch_os.py [--build N] [--profile Minimal]

Downloads the Flipper One image of a build (the newest unless told) from
Flipper Devices' builds, checks it against the build's manifest, and takes out
of its btrfs root what a direct boot needs: the boot entry (BLS) of a profile,
the kernel and initrd it names, and the device trees beside the kernel. That
needs no mount and no root: `btrfs restore` reads the filesystem from a file.
Everything goes into firmware/os/BUILD/, with os.json saying what is where;
the compressed image stays there too (the storage to come will use it).
Needs zstd and btrfs-progs. Standard library otherwise.
"""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
OS_DIR = ROOT / "firmware" / "os"
BUILDS = "https://dl-linux-images.flipp.dev/full-img/"
BOARD = "flipper-one"
AGENT = {"User-Agent": "fetch/1.0"}     # (Python's own is turned away)


def die(msg):
    sys.exit(f"fetch_os: {msg}")


def get_json(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=AGENT),
                                timeout=60) as r:
        return json.load(r)


def download(url, path, size, sha256):
    """@url into @path, unless it is there already with that sha256"""
    if path.exists() and path.stat().st_size == size:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 22), b""):
                h.update(chunk)
        if h.hexdigest() == sha256:
            print(f"have {path.name}")
            return
    print(f"downloading {path.name} ({size / 2**30:.2f} GiB)")
    h, got = hashlib.sha256(), 0
    part = path.with_suffix(path.suffix + ".part")
    with urllib.request.urlopen(urllib.request.Request(url, headers=AGENT),
                                timeout=60) as r, open(part, "wb") as f:
        while chunk := r.read(1 << 22):
            f.write(chunk)
            h.update(chunk)
            got += len(chunk)
            print(f"\r  {got * 100 // size:3d} %", end="", flush=True)
    print()
    if got != size or h.hexdigest() != sha256:
        part.unlink()
        die(f"{path.name}: not what the manifest says (size or sha256)")
    part.rename(path)


def gpt_partition(img, name):
    """(first byte, length) of the GPT partition called @name"""
    with open(img, "rb") as f:
        f.seek(512)
        hdr = f.read(92)
        if hdr[:8] != b"EFI PART":
            die(f"{img.name}: no GPT")
        start, count, size = struct.unpack_from("<QII", hdr, 72)
        f.seek(start * 512)
        for _ in range(count):
            e = f.read(size)
            first, last = struct.unpack_from("<QQ", e, 32)
            if e[56:128].decode("utf-16-le").rstrip("\0") == name:
                return first * 512, (last - first + 1) * 512
    die(f"{img.name}: no partition '{name}'")


def subvolumes(root_img):
    """{name: id} of the btrfs filesystem's subvolumes"""
    out = subprocess.run(["btrfs", "inspect-internal", "dump-tree", "-t",
                          "root", str(root_img)], capture_output=True,
                         text=True, check=True).stdout
    found = {}
    for m in re.finditer(r"key \(FS_TREE ROOT_REF (\d+)\)[^\n]*\n[^\n]*"
                         r"name (\S+)", out):
        found[m.group(2)] = int(m.group(1))
    return found


def restore(root_img, subvol, regex, into):
    into.mkdir(parents=True, exist_ok=True)
    subprocess.run(["btrfs", "restore", "-r", str(subvol), "--path-regex",
                    regex, str(root_img), str(into)], check=True,
                   stdout=subprocess.DEVNULL)


def regex_for(path):
    """btrfs restore's --path-regex for one path and its parents"""
    parts = path.strip("/").split("/")
    rx = ""
    for p in reversed(parts):
        p = "".join("\\" + c if c in ".[]()*+?{}|^$\\" else c for c in p)
        rx = f"/{p}" + (f"(|{rx})" if rx else "(|/.*)")
    return f"^/(|{rx[1:]})$"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--build", type=int, help="the build (default: newest)")
    ap.add_argument("--profile", default="Minimal",
                    help="Minimal, Desktop, Router, TV-Media-Box, No-Graphics")
    args = ap.parse_args()
    for tool in ("zstd", "btrfs"):
        if not shutil.which(tool):
            die(f"needs {tool} (zstd, btrfs-progs)")

    build = args.build or max(int(d["name"].rstrip("/"))
                              for d in get_json(BUILDS + "manifest.json")
                              ["directories"]
                              if d["name"].rstrip("/").isdigit())
    manifest = get_json(f"{BUILDS}{build}/manifest.json")
    name = f"debian-512-{BOARD}-{build}.img.zst"
    entry = next((f for f in manifest["files"] if f["path"] == name), None)
    if not entry:
        die(f"build {build} has no {name}")
    d = OS_DIR / str(build)
    d.mkdir(parents=True, exist_ok=True)
    (d / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
    zst = d / name
    download(f"{BUILDS}{build}/{name}", zst, entry["size"], entry["sha256"])

    work = d / "work"
    work.mkdir(exist_ok=True)
    img, root_img = work / "image.img", work / "root.img"
    try:
        print("decompressing")
        subprocess.run(["zstd", "-q", "-d", "-f", "--sparse", "-o", str(img),
                        str(zst)], check=True)
        first, length = gpt_partition(img, "root")
        print(f"the root partition: {length / 2**30:.2f} GiB at {first:#x}")
        subprocess.run(["dd", f"if={img}", f"of={root_img}", "bs=1M",
                        f"skip={first // 2**20}", f"count={length // 2**20}",
                        "conv=sparse", "status=none"], check=True)
        img.unlink()
        vols = subvolumes(root_img)
        profile = "@" + args.profile
        if "boot" not in vols or profile not in vols:
            die(f"subvolumes: {', '.join(sorted(vols))}: no boot or {profile}")
        shutil.rmtree(d / "boot", ignore_errors=True)
        restore(root_img, vols["boot"], "^/(|loader(|/entries(|/.*)))$",
                d / "boot")
        # the profile's newest kernel, as its boot entry says
        bls = []
        for f in (d / "boot" / "loader" / "entries").glob(
                f"*-flipperos-{args.profile}-*.conf"):
            e = dict(line.split(None, 1) for line in f.read_text().splitlines()
                     if line and not line.startswith("#") and " " in line)
            bls.append(e)
        if not bls:
            die(f"no boot entry for {args.profile}")
        e = max(bls, key=lambda e: [int(x) for x in
                                    re.findall(r"\d+", e["version"])[:3]])
        inside = lambda p: p.strip().split(profile, 1)[1]   # noqa: E731
        kernel, initrd = inside(e["linux"]), inside(e["initrd"])
        dtbs = inside(e["devicetreedir"])
        out = d / args.profile
        shutil.rmtree(out, ignore_errors=True)
        for p in (kernel, initrd, dtbs):
            restore(root_img, vols[profile], regex_for(p), out)
    finally:
        img.unlink(missing_ok=True)
        root_img.unlink(missing_ok=True)
    info = {
        "build": build, "profile": args.profile, "version": e["version"].strip(),
        "kernel": str((out / kernel.lstrip("/")).relative_to(ROOT)),
        "initrd": str((out / initrd.lstrip("/")).relative_to(ROOT)),
        "dtbs": str((out / dtbs.lstrip("/")).relative_to(ROOT)),
        "options": e["options"].strip(),
    }
    for k in ("kernel", "initrd", "dtbs"):
        if not (ROOT / info[k]).exists():
            die(f"{info[k]}: not restored")
    (d / "os.json").write_text(json.dumps(info, indent=1) + "\n")
    work.rmdir()
    print(f"build {build}, {args.profile}: {info['version']} -> {d}")


if __name__ == "__main__":
    main()
