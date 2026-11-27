#!/usr/bin/env python3
# Copyright (c) 2026, Kara Zajac
# SPDX-License-Identifier: BSD-3-Clause
"""Headless check of the whole device: does Flipper OS run beside the MCU?

    scripts/linux_check.py [--keep] [--fresh] [--uboot | --mcu-update] [--out DIR]
    scripts/linux_check.py --maskrom [--loader LOADER.bin] [--keep] [--out DIR]

Boots a throwaway device with Flipper OS on its RK3576, devices/linux-PID
(removed at the end unless --keep), and checks it through its sockets:

- the RK3576 starts when the MCU firmware switches its rail on, and the
  front-panel link says so; the kernel boots on the Flipper One's device
  tree; the MCU sees its driver (CPU state 2) and then user space (3);
- Flipper OS reaches its login on the console; FlipCTL draws on the
  Flipper's screen (its frames over the display SPI, through the MCU), and
  a key pressed on the panel reaches it, through the MCU and the I2C link;
- Linux reads the main board's chips over the bus it shares with the MCU:
  the INA219 on VSYS, the five INA4230s; its root is the microSD card, and
  its UFS is provisioned as the Flipper OS installer's scheme has it (its
  LUs, boot LUs A and B, bBootLunEn); an NVMe in the M.2 slot (PCIe0, `run --nvme`)
  is a 256 GB one, and 4 MiB written to it read back the same; a monitor
  in the HDMI port (`run --hdmi`) lights at 1280x720 with the console on
  it, the Flipper's screen still fb0, its picture not black, a 10-bit
  framebuffer (AR30, as a compositor's) shown as drawn, and Linux sees it
  pulled out and plugged in again; USB0 is in
  its device role with its Type-C port, USB1's root hubs are there, and the
  battery reads (its chain waits on USB0's role switch); the MT7921's
  Bluetooth is on USB1 with its firmware loaded, behind the board's hub (its
  USB 2.0 and SuperSpeed hubs, the device tree's nodes) at SuperSpeed, hci0,
  BlueZ's controller
  powered, a scan finds the LE beacon and sensor near it, nothing else, and
  BlueZ connects to the sensor (its GATT services, its battery); its Wi-Fi is
  mt7921u's with its
  firmware running, wlx000ce7464f02, NetworkManager's, and a scan completes
  and finds nothing, or with passt (`run --wifi`) the access point near it,
  whose open network NetworkManager joins (an address by DHCP, a gateway that
  answers, its data at 802.11n's MCS 15), then its WPA2 one (the 4-way
  handshake);
  with its rfkill blocked the chip leaves USB, unblocked it comes back cold;
  each Ethernet port
  has its RTL8211F, and with passt installed port 1 is on this PC's network
  (`run --net`): a link, an address by DHCP, a gateway that answers; port 2
  has a cable to a LAN of its own (`run --lan`), where scripts/lan_host.py
  is the DHCP server and gateway: a link, an address from it, its ping
  answered, and the Flipper's to it;
  the CPU clusters' clocks come from TF-A's SCMI server, and cpufreq has
  both clusters; the cores idle with WFI (the machine drops the tree's
  power-down idle state); the six thermal zones read the TSADC, with the trims the
  OTP holds; the SARADC reads the boot strap's divider, the MCU's BOOT1
  (and follows it as the MCU's `saradc set_id` changes it) and the board's
  revision;
- a computer plugged into the USB-C port (the panel's `usb 2`) reaches
  USB0, and enumerates Flipper OS's gadget (NCM, MTP, a serial port), which
  Linux sees configured at high speed; a line goes each way over its
  serial port (ttyGS0, the host's chardev at the other end); unplugged,
  Linux sees the computer go;
- Flipper OS's sound card (the NAU8822 codec on SAI2) plays a tone of 2 s
  at 1 kHz, and the codec gives it out: the device's audio.wav, from QEMU's
  wav backend (`run --audio`), has 2 s of 1 kHz; its internal microphone
  hears the device's mic.wav (`run --mic`), 440 Hz, and arecord gets that
  at its level; so does a headset's, plugged in on the panel link (and
  pulled out: silence);
- `systemctl reboot` starts the RK3576 again, and the kernel with it;
  `systemctl poweroff` switches it off (PSCI), after which the MCU cuts
  its rail; the MCU's CPU app switches it on again, and the kernel starts;
- no oops, panic or RCU stall in the kernel's log, and no crash in the
  MCU's.

The getty on the USB gadget's serial port is masked on the kernel's command
line (systemd.mask=), for Flipper OS's OS-02 (docs/FIRMWARE_BUGS.md): with
no computer on the gadget it can keep the console's getty from starting
(and the check uses that serial port itself).

With --uboot it checks the boot through U-Boot instead, as on the device
(`run --uboot`), from the kept card. First U-Boot stops at its shell (`s`
at power-on), and its rockusb makes USB0 a Rockusb device, which the
computer enumerates; in Rockusb's Loader mode over USB/IP (fo_rockusb.py)
sectors read are the card's, and 64 KiB written read back the same and are
in the card's image (then put back); `boot` goes on. Then: the machine loads
U-Boot from the card's u-boot.itb as SPL would, and U-Boot sees the board's
revision and the 4 GiB of DRAM the DDR blob left word of; it finds the
card's boot entries and shows its boot menu on the Flipper's screen; the
Flipper's keys pick the Minimal profile of the build's kernel, and U-Boot
boots it with the entry's own options, a seed for KASLR (from the TRNG) and
all of the DRAM past TF-A's 2 MiB; Flipper OS reaches its login; a reboot
comes back to U-Boot's menu, and from there to the kernel and the login
again; and a reboot with --reboot-argument=maskrom (the tree's reboot mode)
leaves the RK3576 in its MaskROM, which the computer enumerates. A computer
is on the USB-C port from the start: the boot entry's options mask no getty
(OS-02).

With --mcu-update it checks the MCU's firmware updated from Flipper OS, as
the device's own update does it (needs passt): the MCU's updater answers over
I2C (its version, slot A, a normal boot); Flipper OS fetches the MCU's own
image over port 1's network (from this PC's loopback) and writes it to the
firmware class, which sends it to the MCU over I2C to be staged in slot B; at
a reboot the MCU boots it (the boot ROM's flash-update boot), buys it (its
try-before-you-buy), and powers Flipper OS again, which reads slot B and a
flash-update boot.

With --maskrom it checks the RK3576's boot ROM in its USB download instead,
on a blank device (`run --uboot`, `--usbip`): the boot ROM finds nothing to
boot on the UFS's boot LU or the card, and waits in its MaskROM; a computer
plugged in enumerates it (2207:350e) and fo_rockusb.py lists it, a Rockusb
device; over USB/IP, 0x471 code with a wrong CRC is dropped, and with its CRC
runs (the machine's DDR blob); with --loader, the loader's download-boot (the
Flipper OS installer's installer-falcon-loader.bin: SPL in Falcon mode, the
machine's, starts the installer's kernel, and its interface comes to the
Flipper's screen), else 0x472 code with no FIT in it, a usbplug, which the
machine plays: the device comes back in Rockusb's Loader mode (bcdUSB 2.01);
its chip-info, flash-info, capability and storage (the UFS's LU 0); 64 KiB
written far out on the UFS read back the same and are in its image, then
erased; the same on the card, after a switch of storage; a read past the
end fails; fo_rockusb.py's write-bmap writes an image's mapped ranges onto
the card, and leaves its other blocks. With a kept card
(build/linux-check/sd.img), its first 12 MiB (Flipper OS's U-Boot) written
to the blank card through it: a reset into the MaskROM (rkdeveloptool's rd
3) stays there, the card notwithstanding; the usbplug again, and its reset
boots U-Boot from the card. (Without one, its reset finds nothing to boot,
and the boot ROM waits in its MaskROM.) Then Stop, and the MCU's Maskrom
(MASKROM_EN) puts it in its USB download again.

A first boot takes several minutes (Flipper OS sets itself up); the first
run keeps its card as build/linux-check/sd.img, as it is after the checked
power-off, and later runs start from a copy of it (a reflink where the
filesystem has them), for the same Flipper OS build. --fresh does a first
boot anyway, and keeps that card instead. On a slow PC (busy, or its clock
down) the waits after the first login stretch as much as that boot did
over 200 s (400 s for a first boot). Exit status 0 = pass. Screenshots go
to --out.
"""
import argparse
import base64
import hashlib
import json
import math
import os
import pathlib
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import smoke_test as st                 # noqa: E402  (its checks and links)

ROOT = st.ROOT
TEMPLATE = ROOT / "build" / "linux-check"
MIC_HZ, MIC_PEAK = 440, 1638            # the microphone's file: -26 dBFS
KMSG = re.compile(r"^\[\s*\d+\.\d+\]")
# terminal escapes: CSI (with intermediates: agetty's \e[!p), OSC (\e]104\a),
# and the cursor's save and restore (\e7, \e8: U-Boot's query of the
# terminal's size wraps its other output, in the middle of a line now and
# then)
ESC = re.compile(r"\x1b(?:\[[0-?]*[ -/]*[@-~]"            # CSI
                 r"|\][^\x07\x1b]*(?:\x07|\x1b\\)"        # OSC
                 r"|[78])")                                # \e7, \e8


def text_of(path):
    """the file's text, without terminal escapes (ESC)"""
    raw = path.read_bytes() if path.exists() else b""
    return ESC.sub("", raw.decode("utf-8", "replace"))


def wait_text(path, pattern, timeout, after=0):
    """@pattern in the file (from byte @after on), within @timeout s"""
    end = time.time() + timeout
    while time.time() < end:
        if re.search(pattern, text_of(path)[after:]):
            return True
        time.sleep(0.5)
    return False


class Console:
    """Linux's console (UART0): a command, and what it printed"""

    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(str(path))
        self.lock = threading.Lock()
        self.buf = ""
        self.n = 0
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        while True:
            try:
                chunk = self.s.recv(1 << 16)
            except OSError:
                return
            if not chunk:
                return
            with self.lock:                 # (an escape may be split:
                self.buf += chunk.decode("utf-8", "replace")  # read whole)

    def run(self, command, timeout=30):
        """@command's output lines (not the kernel's), or None"""
        self.n += 1
        begin, end = f"@@B{self.n}", f"@@E{self.n}"
        with self.lock:
            start = len(self.buf)
        # the markers come out whole, but are not whole in the echo
        self.s.sendall(f'echo "@@"B{self.n}; {command}; '
                       f'echo "@@"E{self.n}\r'.encode())
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                text = ESC.sub("", self.buf[start:]).replace("\r", "")
            lines = text.split("\n")
            if any(line.strip() == end for line in lines):
                out, inside = [], False
                for line in lines:
                    if line.strip() == begin:
                        inside = True
                    elif line.strip() == end:
                        break
                    elif inside and not KMSG.match(line):
                        out.append(line)
                return out
            time.sleep(0.2)
        return None

    def send(self, command):
        self.s.sendall(command.encode() + b"\r")

    def ready(self, timeout=60):
        """a shell that answers: what is typed before getty and the shell
        are up is thrown away"""
        end = time.time() + timeout
        while time.time() < end:
            if self.run("true", 3) is not None:
                return True
        return False


def os_build():
    builds = sorted((p for p in (ROOT / "firmware" / "os").glob("*/os.json")),
                    key=lambda p: int(p.parent.name)
                    if p.parent.name.isdigit() else -1)
    return json.loads(builds[-1].read_text())["build"] if builds else None


def template_ok():
    try:
        return (TEMPLATE / "sd.img").exists() and \
            json.loads((TEMPLATE / "card.json").read_text())["build"] == \
            os_build()
    except (OSError, ValueError, KeyError):
        return False


def gadget_serial(con, path):
    """a line from Linux to the computer's end of the gadget's serial port,
    and one back (the computer's end: the host's chardev, @path): (whether
    both came, what did)"""
    s = socket.socket(socket.AF_UNIX)
    s.connect(str(path))                    # (the computer raises DTR)
    s.settimeout(0.5)
    try:
        # one shell holds the tty open, so that its raw mode stays
        con.run("rm -f /tmp/from-pc; (sudo sh -c 'exec 3<>/dev/ttyGS0; "
                "stty raw -echo <&3; echo to-the-pc >&3; "
                "timeout 30 head -c 11 <&3 > /tmp/from-pc' "
                ">/dev/null 2>&1 &)")
        got, end = b"", time.time() + 20
        while b"to-the-pc" not in got and time.time() < end:
            try:
                got += s.recv(4096)
            except socket.timeout:
                pass
        s.sendall(b"from-the-pc")
        back = None
        for _ in range(20):
            back = con.run("cat /tmp/from-pc 2>/dev/null; echo")
            if back and back[0] == "from-the-pc":
                break
            time.sleep(1)
        return (b"to-the-pc" in got and bool(back) and
                back[0] == "from-the-pc", f"{got[-40:]!r} / {back}")
    finally:
        s.close()


def wav_tone(path):
    """the sound in a WAV file from QEMU's wav backend (S16 stereo, its data
    after a 44-byte header that is finished only at the end): (its seconds,
    from the first sample above silence to the last; its frequency, from the
    left channel's rising zero crossings), or None"""
    try:
        data = path.read_bytes()
    except OSError:
        return None
    n = (len(data) - 44) // 4
    if n <= 0:
        return None
    left = struct.unpack(f"<{2 * n}h", data[44:44 + 4 * n])[::2]
    loud = [i for i, v in enumerate(left) if abs(v) > 300]
    if not loud:
        return None
    a, b = loud[0], loud[-1]
    ups = sum(1 for i in range(a + 1, b) if left[i - 1] < 0 <= left[i])
    secs = (b - a) / 48000
    return round(secs, 3), round(ups / secs, 1) if secs else 0


def mic_wav(path):
    """what the internal microphone hears: 3 s of MIC_HZ, mono, 48 kHz"""
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(48000)
        w.writeframes(b"".join(
            struct.pack("<h", round(MIC_PEAK * math.sin(
                2 * math.pi * MIC_HZ * i / 48000))) for i in range(3 * 48000)))


def png_lit(path):
    """a PNG's (width, height), as QEMU's screendump writes one, and the
    share of its rows' bytes (decompressed, filtered) that are not 0: none
    in a black picture"""
    try:
        data = path.read_bytes()
    except OSError:
        return None, 0.0
    p, idat, size = 8, b"", None
    while data[:8] == b"\x89PNG\r\n\x1a\n" and p + 8 <= len(data):
        n, kind = struct.unpack(">I4s", data[p:p + 8])
        if kind == b"IHDR":
            size = struct.unpack(">II", data[p + 8:p + 16])
        elif kind == b"IDAT":
            idat += data[p + 8:p + 8 + n]
        p += 12 + n
    raw = zlib.decompress(idat) if idat else b""
    return size, 1 - raw.count(0) / len(raw) if raw else 0.0


def png_pixels(path, points):
    """the RGB at each (x, y) of an 8-bit RGB or RGBA PNG (QEMU's
    screendump), its rows' filters undone; None if it cannot be read"""
    try:
        data = path.read_bytes()
    except OSError:
        return None
    p, idat, w, h, bpp = 8, b"", 0, 0, 3
    while data[:8] == b"\x89PNG\r\n\x1a\n" and p + 8 <= len(data):
        n, kind = struct.unpack(">I4s", data[p:p + 8])
        if kind == b"IHDR":
            w, h, _, ctype = struct.unpack(">IIBB", data[p + 8:p + 18])
            bpp = 4 if ctype == 6 else 3
        elif kind == b"IDAT":
            idat += data[p + 8:p + 8 + n]
        p += 12 + n
    if not idat:
        return None
    raw, stride, rows = zlib.decompress(idat), w * bpp, []
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            up, c = prev[x], prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + up) & 255
            elif f == 3:
                line[x] = (line[x] + (a + up) // 2) & 255
            elif f == 4:
                q = a + up - c
                pa, pb, pc = abs(q - a), abs(q - up), abs(q - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else
                                      up if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return [tuple(rows[y][x * bpp:x * bpp + 3]) for x, y in points]


def copy_card(src, dst):
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", str(src),
                    str(dst)], check=True)


def checks(check, d, out, first_boot, off, net):
    boot_s = 1200 if first_boot else 600
    linux, qlog, mcu = d / "linux.log", d / "qemu.log", d / "mcu.log"
    t0 = time.time()

    panel = st.Panel(d / "panel.sock")
    check("the RK3576 starts when the MCU switches its rail on",
          wait_text(qlog, r"the RK3576 starts", 20) and
          st.until(lambda: True in panel.rk3576, 5),
          text_of(qlog)[-200:])
    check("the kernel boots on the Flipper One's device tree",
          wait_text(linux, r"Machine model: Flipper One", 60))
    check("the MCU sees the kernel's driver (CPU state 2)",
          st.wait_log(d, r"CPU State: 2", 120))
    before = panel.screen()
    if not check("Flipper OS reaches its login",
                 wait_text(linux, r"login:", boot_s)):
        return None                     # (nothing after it can pass)
    # a slow PC (busy, or its clock down) stretches the waits from here on
    # as much as it stretched this boot (200 s at most, from the kept card,
    # on an idle one): the check is of what works, not of how fast
    took = time.time() - t0
    slow = max(1.0, took / (400 if first_boot else 200))
    if slow > 1:
        print(f"      (a slow PC: the boot took {took:.0f} s, and the waits "
              f"from here on are {slow:.1f} times as long)", flush=True)
    check("the MCU sees user space (CPU state 3)",
          st.wait_log(d, r"CPU State: 3", 300 * slow))
    check("FlipCTL draws on the Flipper's screen",
          wait_text(linux, r"first frame on the panel", 300 * slow) and
          st.until(lambda: panel.screen() != before, 30))
    time.sleep(5)
    panel.save(out / "flipctl.png")
    shown = panel.screen()
    panel.tap("view")
    check("a key reaches FlipCTL (View: its menu)",
          st.until(lambda: panel.screen() != shown, 30))
    time.sleep(2)
    panel.save(out / "flipctl-menu.png")
    panel.tap("view")

    con = Console(d / "linux.sock")
    check("its console answers", con.ready(60 * slow))
    vsys = con.run("cat /sys/bus/i2c/devices/2-0045/hwmon/hwmon*/in1_input")
    check("Linux reads VSYS on the main bus (the INA219)",
          bool(vsys) and vsys[-1].strip().isdigit() and
          7800 < int(vsys[-1]) < 8200, str(vsys))
    n = con.run("grep -l ina4230 /sys/class/hwmon/hwmon*/name | wc -l")
    check("... and its five INA4230s", n is not None and n[-1:] == ["5"],
          str(n))
    root = con.run("findmnt -no SOURCE /")
    check("its root is the microSD card",
          bool(root) and "mmcblk0" in root[-1], str(root))
    # its UFS, provisioned as the Flipper OS installer's scheme has it: by
    # LUN (the disks' names follow the SCSI scan), its 512-byte sectors and
    # its unit descriptor's boot LU; and the device's bBootLunEn (A)
    ufs = con.run("for d in /sys/block/sd*; do echo $(basename "
                  "$(readlink -f $d/device) | cut -d: -f4) $(cat $d/size) "
                  "$(cat $d/device/unit_descriptor/boot_lun_id); done | sort; "
                  "cat /sys/bus/platform/devices/2a2d0000.ufshc/attributes/"
                  "boot_lun_enabled")
    want = ["0 124665856 0x00", "1 32768 0x01", "2 32768 0x02",
            "3 262144 0x00", "0x00000001"]
    check("its UFS provisioned as the installer's scheme: LU 0 (63.8 GB), "
          "boot LUs A and B (16 MiB), recovery (128 MiB); boot LU A enabled",
          [u.strip() for u in ufs or []] == want, str(ufs))
    # PCIe0: the NVMe in the M.2 slot (`run --nvme`), behind the root port,
    # a 256 GB disk; 4 MiB through it and back (its DMA, its MSI-X)
    dev = "/sys/bus/pci/devices/0000:01:00.0"
    nvme = con.run(f"cat {dev}/vendor {dev}/device /sys/block/nvme0n1/size")
    check("PCIe0: an NVMe in the M.2 slot (1b36:0010), a 256 GB disk",
          nvme == ["0x1b36", "0x0010", "500000000"], str(nvme))
    rw = con.run("sudo dd if=/dev/urandom of=/tmp/r bs=1M count=4 "
                 "2>/dev/null && sudo dd if=/tmp/r of=/dev/nvme0n1 bs=1M "
                 "oflag=direct 2>/dev/null && sudo dd if=/dev/nvme0n1 "
                 "of=/tmp/r2 bs=1M count=4 iflag=direct 2>/dev/null && "
                 "cmp /tmp/r /tmp/r2 && echo same", 120)
    check("... and 4 MiB written to it read back the same", rw == ["same"],
          str(rw))
    # the HDMI port, a monitor in it from the start (`run --hdmi`): Linux
    # reads its EDID and lights it at 1280x720, the console on it (fbcon's
    # map:1, fb1) and the Flipper's own screen still fb0; its picture,
    # QEMU's console, is not black; pulled out and plugged in again
    hdmi = "/sys/class/drm/card1-HDMI-A-1"
    seen = con.run(f"cat {hdmi}/status; head -1 {hdmi}/modes; cat "
                   "/sys/class/graphics/fb0/name /sys/class/graphics/fb1/name")
    check("HDMI: a monitor, at 1280x720 with the console on it (fb1), the "
          "Flipper's screen still fb0", seen == ["connected", "1280x720",
          "flipper_one_dis", "rockchipdrmfb"], str(seen))
    subprocess.run([sys.executable, str(st.LAUNCHER), "shot", "-n", d.name,
                    "--hdmi", str(out / "hdmi.png")], stdout=subprocess.DEVNULL)
    size, lit = png_lit(out / "hdmi.png")
    check("... and its picture is the console's (1280x720, not black)",
          size == (1280, 720) and lit > 0.001, f"{size} {lit:.4f}")
    # a compositor's 10-bit framebuffer (AR30, KWin's): four bands from a
    # DRM client (scripts/drm_bands.py, sent in over the console), as drawn
    blob = base64.b64encode((ROOT / "scripts" / "drm_bands.py")
                            .read_bytes()).decode()
    con.run("rm -f /tmp/drm.b64", 30)
    for k in range(0, len(blob), 2000):
        con.run(f"echo {blob[k:k + 2000]} >> /tmp/drm.b64", 30)
    con.send("base64 -d /tmp/drm.b64 > /tmp/drm_bands.py && sudo python3 "
             f"/tmp/drm_bands.py AR30 {round(15 * slow)} > /tmp/drm.out 2>&1 &")
    shown = st.until(lambda: (con.run("cat /tmp/drm.out", 30) or [""])[0]
                     .startswith("shown: AR30"), 30 * slow, 1)
    time.sleep(2)
    subprocess.run([sys.executable, str(st.LAUNCHER), "shot", "-n", d.name,
                    "--hdmi", str(out / "hdmi-ar30.png")],
                   stdout=subprocess.DEVNULL)
    bands = png_pixels(out / "hdmi-ar30.png", [(160, 360), (480, 360),
                                                (800, 360), (1120, 360)])
    check("... and a compositor's 10-bit framebuffer (AR30): its red, green, "
          "blue and grey bands, as drawn",
          shown and bands == [(255, 0, 0), (0, 255, 0), (0, 0, 255),
                              (128, 128, 128)],
          f"{bands} {con.run('cat /tmp/drm.out', 30)}")
    st.until(lambda: not con.run("pgrep -f drm_bands.py", 30), 30 * slow, 1)
    panel.s.sendall(b"hdmi 0\n")
    gone = st.until(lambda: con.run(f"cat {hdmi}/status") ==
                    ["disconnected"], 60 * slow, 2)
    panel.s.sendall(b"hdmi 1\n")
    back = st.until(lambda: con.run(f"cat {hdmi}/status") == ["connected"],
                    60 * slow, 2)
    check("... pulled out, and plugged in again: Linux sees both",
          gone and back, f"{gone} {back}")
    usb = con.run("cat /sys/class/usb_role/23000000.usb-role-switch/role; "
                  "ls /sys/class/udc /sys/class/typec | paste -sd' '; "
                  "lsusb | grep -c 'root hub'")
    check("USB: USB0 (the USB-C port's) in its device role, with its UDC and "
          "Type-C port; USB1's root hubs",
          bool(usb) and len(usb) == 3 and usb[0].strip() == "device" and
          "23000000.usb" in usb[1] and "port0" in usb[1] and
          usb[2].strip() == "2", str(usb))
    bat = con.run("cat /sys/class/power_supply/bq28z620-0/capacity "
                  "/sys/class/power_supply/bq28z620-0/status")
    check("... so the charger and battery come up: Linux reads the gauge",
          bool(bat) and len(bat) == 2 and bat[0].strip().isdigit() and
          70 <= int(bat[0]) <= 90, str(bat))

    # the MT7921's Bluetooth: on USB1 while its rfkill is unblocked (Flipper
    # OS unblocks it at boot), its patch loaded through MediaTek's WMT
    bt = con.run("lsusb -d 0e8d:7961 | wc -l; ls /sys/class/bluetooth; "
                 "sudo dmesg -t | grep -c 'Loading BT firmware: "
                 "mediatek/BT_RAM_CODE_MT7961_1_2_hdr.bin'", 60)
    loads = int(bt[2]) if bt and len(bt) == 3 and bt[2].isdigit() else 0
    check("Bluetooth: the MT7921 on USB1 (0e8d:7961), its firmware loaded "
          "(BT_RAM_CODE_MT7961_1_2), hci0",
          bt is not None and bt[:2] == ["1", "hci0"] and loads >= 1, str(bt))
    # the board's USB hub on USB1 (a CoreChips SL6341): its USB 2.0 hub on
    # the USB 2 root port, its SuperSpeed hub on the USB 3 one, each the
    # device tree's node (hub@1, hub@2), the onboard hub driver's; the
    # MT7921 on the SuperSpeed hub's port 1, at SuperSpeed, its node device@1
    hubs = con.run("for d in /sys/bus/usb/devices/*-*; do "
                   "[ -f $d/idVendor ] || continue; "
                   "n=$(readlink -f $d/of_node); "
                   "v=$(readlink -f $d/driver); "
                   "echo ${d##*/} $(cat $d/idVendor):$(cat $d/idProduct) "
                   "$(cat $d/speed) ${n##*/} ${v##*/}; done | "
                   "grep -E ' (3431|0e8d):'", 60) or []
    rows = [line.split() for line in hubs]
    got = sorted(" ".join(r[1:]) for r in rows)
    at = {r[1]: r[0] for r in rows if len(r) > 1}
    check("USB1's hub: its USB 2.0 hub (3431:6241) and its SuperSpeed hub "
          "(3431:6341), the device tree's, the onboard hub driver's; the "
          "MT7921 behind the SuperSpeed one at 5 Gb/s",
          got == ["0e8d:7961 5000 device@1 usb",
                  "3431:6241 480 hub@1 onboard-usb-dev",
                  "3431:6341 5000 hub@2 onboard-usb-dev"] and
          at.get("0e8d:7961") == at.get("3431:6341", "?") + ".1",
          " / ".join(hubs))
    show = con.run("timeout 20 bluetoothctl show", 60) or []
    check("... BlueZ's controller, powered (MediaTek's, Bluetooth 5.2)",
          any("Controller 00:0C:E7:46:4F:01" in x for x in show) and
          any("Powered: yes" in x for x in show) and
          any("Manufacturer: 0x0046" in x for x in show),
          " / ".join(show[:4]))
    scan = con.run("timeout 30 bluetoothctl --timeout 10 scan on", 60) or []
    found = sorted(x[x.index("Device"):] for x in scan if "[NEW] Device" in x)
    check("... a scan (inquiry and LE) finds the beacon and the sensor near "
          "it (LE), nothing else",
          any("Discovery started" in x for x in scan) and found == [
              "Device C0:FE:46:4F:42:01 FlipperOneEmu Beacon",
              "Device C0:FE:46:4F:42:02 FlipperOneEmu Sensor"],
          " / ".join(found or scan[-3:]))
    sensor = con.run("A=C0:FE:46:4F:42:02; timeout 40 bluetoothctl connect $A "
                     ">/dev/null 2>&1; for i in $(seq 40); do bluetoothctl info "
                     "$A | grep -q 'Device Information' && break; sleep 0.5; "
                     "done; bluetoothctl info $A | grep -c UUID; bluetoothctl "
                     "info $A | grep -o 'Battery Percentage: 0x57'; timeout 20 "
                     "bluetoothctl disconnect $A >/dev/null 2>&1; echo $?",
                     150) or []
    check("... it connects to the sensor: its GATT services found (five), "
          "its battery read (87 %), and disconnects",
          sensor == ["5", "Battery Percentage: 0x57", "0"], str(sensor))
    # its Wi-Fi: mt7921u on interface 3 of the same chip, its firmware
    # running; NetworkManager's; a scan that finds nothing (no radio)
    wifi = con.run("W=$(ls /sys/class/ieee80211/phy0/device/net/ | head -1); "
                   "echo $W; sudo dmesg -t | grep -c 'WM Firmware Version'; "
                   "nmcli -t -f DEVICE,TYPE,STATE dev | grep \"^$W:\"; "
                   "for i in 1 2 3 4 5; do out=$(sudo timeout 60 iw dev $W "
                   "scan) && break; sleep 3; done; "   # (NetworkManager's own)
                   "echo \"scan $? $(echo -n \"$out\" | grep -c '^BSS')\"",
                   150) or []
    check("Wi-Fi: the MT7921's (mt7921u), its firmware running, its interface "
          "named by its MAC (wlx000ce7464f02)",
          wifi[:1] == ["wlx000ce7464f02"] and len(wifi) == 4 and
          wifi[1].isdigit() and int(wifi[1]) >= 1, str(wifi))
    check("... NetworkManager's, disconnected",
          len(wifi) == 4 and wifi[2] == "wlx000ce7464f02:wifi:disconnected",
          str(wifi))
    if not net:
        check("... a scan completes, and finds nothing (no radio)",
              wifi[-1:] == ["scan 0 0"], str(wifi))
    else:
        # --wifi: an access point near it (an open network), its uplink
        # through passt
        check("... a scan finds the access point near it, its two networks "
              "(run --wifi)", wifi[-1:] == ["scan 0 2"], str(wifi))
        led = panel.leds[1:2]                   # (the Wi-Fi LED, off)
        join = con.run("W=$(ls /sys/class/ieee80211/phy0/device/net/ | head "
                       "-1); sudo timeout 90 nmcli dev wifi connect "
                       "FlipperOneEmu ifname $W >/dev/null 2>&1; echo $?; "
                       "sleep 3; ip -4 -br addr show $W | awk '{print $3}'; "
                       "G=$(ip route | awk '/^default/ && /wl/ {print $3; "
                       "exit}'); ping -c 2 -W 2 -I $W $G >/dev/null 2>&1; "
                       "echo ping $?; sudo iw dev $W link | grep -E "
                       "'SSID|signal|rx bitrate' | xargs", 150) or []
        check("... NetworkManager joins it (FlipperOneEmu, -42 dBm, 802.11n: "
              "its data at MCS 15): an address by DHCP, and the gateway "
              "answers (passt)",
              len(join) == 4 and join[0] == "0" and "/" in join[1] and
              join[2] == "ping 0" and "SSID: FlipperOneEmu" in join[3] and
              "signal: -42 dBm" in join[3] and
              "rx bitrate: 144.4 MBit/s MCS 15 short GI" in join[3],
              str(join))
        check("... and Flipper OS lights the Wi-Fi LED (the MCU's chain)",
              led == ["000000"] and st.until(
                  lambda: panel.leds[1:2] not in ([], ["000000"]), 15),
              f"{led} -> {panel.leds[1:2]}")
        wpa = con.run("W=$(ls /sys/class/ieee80211/phy0/device/net/ | head "
                      "-1); sudo timeout 90 nmcli dev wifi connect "
                      "FlipperOneEmu-WPA2 password flipperone ifname $W "
                      ">/dev/null 2>&1; echo $?; sleep 3; G=$(ip route | awk "
                      "'/^default/ && /wl/ {print $3; exit}'); ping -c 2 -W 2 "
                      "-I $W $G >/dev/null 2>&1; echo ping $?; sudo iw dev $W "
                      "link | grep SSID | xargs", 150) or []
        check("... and its WPA2 one (FlipperOneEmu-WPA2: the 4-way "
              "handshake), the gateway answering through it",
              wpa == ["0", "ping 0", "SSID: FlipperOneEmu-WPA2"], str(wpa))
    cycle = con.run("sudo rfkill block wlan; sleep 3; "
                    "lsusb -d 0e8d:7961 | wc -l; ls /sys/class/bluetooth | "
                    "wc -l; sudo rfkill unblock wlan; "
                    "for i in $(seq 40); do timeout 10 bluetoothctl show "
                    "2>/dev/null | grep -q 'Powered: yes' && break; sleep 1; "
                    "done; timeout 20 bluetoothctl show | grep -c "
                    "'Powered: yes'; sudo dmesg -t | grep -c "
                    "'Loading BT firmware'", 120)
    check("... rfkill blocked: the chip leaves USB; unblocked: back, its "
          "firmware loaded again, powered",
          cycle is not None and cycle[:3] == ["0", "0", "1"] and
          len(cycle) == 4 and cycle[3].isdigit() and int(cycle[3]) > loads,
          str(cycle))
    eth = con.run("for p in end0 end1; do echo $p "
                  "$(cat /sys/class/net/$p/phydev/phy_id) "
                  "$(cat /sys/class/net/$p/carrier 2>/dev/null || echo -); "
                  "done")
    want = [f"end0 0x001cc916 {1 if net else 0}", "end1 0x001cc916 1"]
    check("the Ethernet ports: an RTL8211F on each, "
          + ("port 1 with a link" if net else "port 1 without (no network)")
          + ", port 2 with one (the LAN host's cable)",
          eth is not None and [e.strip() for e in eth] == want, str(eth))
    if net:
        ping = con.run("ip -4 -br addr show end0; ping -c 2 -W 3 "
                       "$(ip route | awk '/default/{print $3; exit}') "
                       ">/dev/null && echo pinged", 60)
        check("... an address by DHCP, and the gateway answers (passt)",
              bool(ping) and len(ping[0].split()) >= 3 and
              ping[-1].strip() == "pinged", str(ping))
    leased = st.until(lambda: "ping answered by 192.168.77.10" in
                      text_of(d / "lan.log"), 120 * slow, 1)
    lan = con.run("ip -4 -br addr show end1; ping -c 2 -W 3 -I end1 "
                  "192.168.77.1 >/dev/null && echo pinged", 60)
    check("... port 2: an address by DHCP from the LAN's host, which it "
          "answers, and pings",
          leased and bool(lan) and "192.168.77.10/24" in lan[0] and
          lan[-1].strip() == "pinged" and
          "ping from 192.168.77.10" in text_of(d / "lan.log"),
          f"{lan} {text_of(d / 'lan.log')[-200:]!r}")
    freq = con.run("cat /sys/devices/system/cpu/cpufreq/policy0/"
                   "scaling_cur_freq /sys/devices/system/cpu/cpufreq/"
                   "policy4/scaling_cur_freq")
    check("the clusters' clocks over SCMI: cpufreq has both",
          bool(freq) and len(freq) == 2 and
          all(f.strip().isdigit() for f in freq), str(freq))
    idle = con.run("cat /sys/devices/system/cpu/cpuidle/current_driver; "
                   "ls /sys/devices/system/cpu/cpu0/cpuidle 2>/dev/null | "
                   "wc -l", 60) or []
    check("its cores idle with WFI: the tree's power-down idle state "
          "(cpu-sleep), which QEMU's PSCI runs as a WFI, dropped",
          idle == ["none", "0"], str(idle))
    temps = con.run("for z in /sys/class/thermal/thermal_zone*; do "
                    "case $(cat $z/type) in *-thermal) cat $z/temp;; esac; "
                    "done")                     # (not the battery's zone)
    check("six temperatures from the TSADC (its trims from the OTP)",
          bool(temps) and len(temps) == 6 and
          all(t.strip().isdigit() and 25000 <= int(t) < 85000
              for t in temps), str(temps))
    # the SARADC: the boot strap's divider (UFS, the microSD card, USB:
    # Rockchip's config 8), BOOT1 (the MCU's PWM, its default ID: 0 V) and
    # the board's revision (f0b0c1: 0); then BOOT1 as the MCU sets it
    adc = ("d=$(dirname $(grep -l 2ae00000.adc /sys/bus/iio/devices/*/name))"
           "; for c in 0 1 2; do cat $d/in_voltage${c}_raw; done")
    codes = [c.strip() for c in con.run(adc) or []]
    check("the SARADC: the boot strap (2862: UFS, SD, USB), BOOT1 (0) and "
          "the board's revision (0: f0b0c1)", codes == ["2862", "0", "0"],
          str(codes))
    st.cli(d, "saradc set_id 6", r"SARADC ID set to", 10)
    codes = [c.strip() for c in con.run(adc) or []]
    check("... BOOT1 as the MCU sets it (saradc set_id 6: 2048)",
          len(codes) == 3 and codes[1].isdigit() and
          abs(int(codes[1]) - 2048) <= 4, str(codes))
    st.cli(d, "saradc set_id 1", r"SARADC ID set to", 10)

    # a computer on the USB-C port: with the Linux side on, the USB mux has
    # its USB 2 on the RK3576's USB0, where Flipper OS's gadget is
    qout = d / "qemu.out"
    qmark = len(text_of(qout))
    panel.send("usb 2")
    check("a computer plugged in enumerates Flipper OS's USB gadget "
          "(37C1:F12F: NCM, MTP, a serial port)",
          wait_text(qout, r"USB: 37c1:f12f .*configured; its CDC ACM", 120,
                    qmark), text_of(qout)[qmark:][-300:])
    udc = "/sys/class/udc/23000000.usb"
    state = con.run(f"cat {udc}/state {udc}/current_speed")
    check("... configured, at high speed, as Linux sees it",
          state == ["configured", "high-speed"], str(state))
    check("... and its serial port (ttyGS0) carries a line each way",
          *gadget_serial(con, d / "usb.sock"))
    panel.send("usb 0")
    check("unplugged: Linux sees the computer go",
          st.until(lambda: con.run(f"cat {udc}/state") ==
                   ["not attached"], 30))

    # the audio codec on SAI2: Flipper OS's sound card, and a tone through
    # it, as QEMU's wav backend takes what the codec plays
    card = con.run("cat /proc/asound/NAU8822/id")
    tone = con.run("sox -n -r 48000 -c 2 -b 16 /tmp/tone.wav synth 2 sine "
                   "1000 vol 0.5 && aplay -q -D hw:NAU8822 /tmp/tone.wav && "
                   "echo played", 90)
    check("audio: Flipper OS's sound card (the NAU8822 on SAI2) plays a "
          "tone", card == ["NAU8822"] and bool(tone) and tone[-1] == "played",
          f"{card} {tone}")
    time.sleep(2)                       # (the last of it, to the file)
    got = wav_tone(d / "audio.wav")
    check("... and the codec gives it out: 2 s of 1 kHz",
          got is not None and 1.9 <= got[0] <= 2.1 and
          990 <= got[1] <= 1010, str(got))
    # its internal microphone hears mic.wav (MIC_HZ at MIC_PEAK), which
    # comes out of the ADC as it is at the codec's default gains; the
    # headset's microphone hears nothing
    rec = con.run("arecord -q -D hw:NAU8822 -f S16_LE -r 48000 -c 2 -d 2 "
                  "/tmp/rec.wav && sox /tmp/rec.wav -n remix 1 stat 2>&1 && "
                  "sox /tmp/rec.wav -n remix 2 stat 2>&1", 90) or []
    stat = [float(m.group(1)) for line in rec for m in
            [re.search(r"(?:Rough\s+frequency|Maximum amplitude):\s+"
                       r"([\d.]+)", line)] if m]
    check(f"... and its internal microphone records ({MIC_HZ} Hz from "
          "--mic's file, at its level)",
          len(stat) == 4 and abs(stat[1] - MIC_HZ) <= 5 and
          abs(stat[0] - MIC_PEAK / 32768) < 0.005 and stat[2] < 0.001,
          str(rec[-12:]))
    # a headset in the jack (the panel link): its microphone, on the right
    # channel, hears the file too; pulled out, nothing again
    def right_peak():
        out = con.run("arecord -q -D hw:NAU8822 -f S16_LE -r 48000 -c 2 -d 1 "
                      "/tmp/rec.wav && sox /tmp/rec.wav -n remix 2 stat 2>&1",
                      90) or []
        m = [re.search(r"Maximum amplitude:\s+([\d.]+)", line) for line in out]
        return next((float(x.group(1)) for x in m if x), None)
    panel.s.sendall(b"headset 1\n")
    time.sleep(1)
    with_headset = right_peak()
    panel.s.sendall(b"headset 0\n")
    time.sleep(1)
    without = right_peak()
    check("... and a headset's microphone, plugged in, and not once pulled out",
          with_headset is not None and without is not None and
          abs(with_headset - MIC_PEAK / 32768) < 0.005 and without < 0.001,
          f"{with_headset} {without}")

    qmark = len(text_of(qlog))
    lmark = len(text_of(linux))
    con.send("systemctl reboot")
    check("reboot: the RK3576 starts again",
          wait_text(qlog, r"the RK3576 starts again", 180 * slow, qmark))
    check("... and the kernel with it",
          wait_text(linux, r"Booting Linux", 60 * slow, lmark))
    lmark = len(text_of(linux))
    check("... to its login again",
          wait_text(linux, r"login:", 600 * slow, lmark) and
          con.ready(60 * slow))

    qmark = len(text_of(qlog))
    mmark = len(text_of(mcu))
    con.send("systemctl poweroff")
    check("power-off: the RK3576 is off (PSCI)",
          wait_text(qlog, r"the RK3576 is off", 180 * slow, qmark))
    if check("... and the MCU cuts its rail",
             wait_text(mcu, r"cutting its power", 60 * slow, mmark) and
             st.until(lambda: panel.rk3576[-1:] == [False], 10)):
        off()                           # (its card as Linux left it)

    qmark = len(text_of(qlog))
    lmark = len(text_of(linux))
    started = False
    for _ in range(6):                  # (the old CPU app may still be on
        st.cli(d, "desktop start_app cpu_app_start", None, 2)  # its way out)
        if wait_text(qlog, r"the RK3576 starts", 10, qmark):
            started = True
            break
    check("the MCU's CPU app switches it on again",
          started and st.until(lambda: panel.rk3576[-1:] == [True], 10))
    check("... and the kernel starts",
          wait_text(linux, r"Booting Linux", 60 * slow, lmark))
    return con


# U-Boot's boot menu on the Flipper's screen: its first entry's rows from row
# 18, 8 rows each, the entry selected drawn light on dark
MENU_ROW0, MENU_ROWS = 18, 8


def uboot_selected(panel):
    """the entry U-Boot's boot menu shows selected (1 ...), or None"""
    px = panel.screen()
    for k in range(1, 14):
        y = MENU_ROW0 + MENU_ROWS * (k - 1)
        if y + MENU_ROWS > st.H - 18:
            break
        if all(sum(px[r * st.LCD_W + x] < 128 for x in range(120, 250)) > 78
               for r in (y + 1, y + MENU_ROWS - 2)):
            return k
    return None


def uboot_pick(panel, linux, want, mark, shot=None):
    """U-Boot's boot menu, after @mark in the console's log: the entry whose
    file is @want's picked with the pad, and OK. The selection is read off the
    Flipper's screen after each key: U-Boot polls the keys between redraws of
    the screen, which are slow here, so it can miss a short press or repeat a
    long one. Its number and the screen as the menu showed it (saved to
    @shot), or None"""
    if not wait_text(linux, r"bootflows, \d+ valid", 600, mark):
        return None
    # (U-Boot's word that its screen's redraw ran past its 50 ms can come in
    # the middle of a line, an entry's among them: out of the way first)
    text = re.sub(r"cyclic function \S+ took too long: \d+us vs \d+us max\n",
                  "", text_of(linux)[mark:].replace("\r", ""))
    listed = re.findall(r"^\s*(\d+)\s+bls\s+ready\s.*/loader/entries/(\S+)$",
                        text, re.M)
    seq = next((int(n) for n, f in listed if want in f), None)
    if seq is None:
        return None
    end, stable = time.time() + 120, 0
    while True:
        cur = uboot_selected(panel)
        if cur == seq:                  # (and still, 1.5 s on: no repeat of
            stable += 1                 # the last key on its way)
            if stable == 4:
                break
            time.sleep(0.5)
            continue
        stable = 0
        if time.time() > end:
            return None
        if cur is not None:
            panel.tap("down" if cur < seq else "up", 0.15)
        moved = time.time() + 6
        while time.time() < moved and uboot_selected(panel) == cur:
            time.sleep(0.2)
    time.sleep(0.5)
    screen = panel.screen()
    if shot:
        panel.save(shot)
    for _ in range(3):                  # (OK, until U-Boot has it)
        panel.tap("ok", 0.25)
        if wait_text(linux, r"Selected: ", 15, mark):
            break
    return seq, screen


def uboot_rockusb_checks(check, d, port):
    """U-Boot stopped at its shell (`s` at power-on): its rockusb on USB0, in
    Rockusb's Loader mode over USB/IP; then `boot`, on to its menu"""
    sys.path.insert(0, str(ROOT / "scripts"))
    import fo_rockusb
    qout = d / "qemu.out"
    con = Console(d / "linux.sock")
    end, seen = time.time() + 120, ""
    while time.time() < end:
        with con.lock:
            seen = ESC.sub("", con.buf)
        if "=>" in seen:
            break
        con.s.sendall(b"s")
        time.sleep(0.3)
    if not check("U-Boot stops at its shell (`s` at power-on)", "=>" in seen):
        con.s.close()
        return
    con.s.sendall(b"\x03")              # (the line: clear)
    time.sleep(1)
    before = text_of(qout).count("flipper-one: USB:")
    con.send("rockusb 0 mmc 0")
    check("its rockusb on USB0: the computer enumerates a Rockusb device "
          "(2207:350e, through U-Boot's own DWC3 driver)",
          st.until(lambda: text_of(qout).count("flipper-one: USB:") > before,
                   60))
    card = d / "sd.img"
    far = card.stat().st_size // 512 - 256      # (clear of the backup GPT)
    try:
        c = fo_rockusb.connect(port=port)
        try:
            ld = fo_rockusb.Loader(c)
            with open(card, "rb") as f:
                first = f.read(8 * 512)
            check("... Rockusb's Loader mode over USB/IP (fo_rockusb.py): "
                  "sectors read are the card's", ld.read_lba(0, 8) == first)
            was = ld.read_lba(far, 128)
            pattern = bytes((i * 7 + 3) & 0xFF for i in range(4096)) * 16
            ld.write_lba(far, pattern)
            back = ld.read_lba(far, 128)
            with open(card, "rb") as f:
                f.seek(far * 512)
                there = f.read(len(pattern))
            check("... and 64 KiB written read back the same, and are in "
                  "the card's image", back == pattern and there == pattern)
            ld.write_lba(far, was)              # (as they were)
        finally:
            c.close()
    except (OSError, ValueError) as e:
        check("Rockusb's Loader mode over USB/IP", False, str(e))
    con.s.sendall(b"\x03")              # (out of rockusb)
    time.sleep(1)
    con.send("boot")
    time.sleep(1)
    con.s.close()                       # (the console takes one at a time)


def uboot_checks(check, d, out, port):
    linux, qlog = d / "linux.log", d / "qemu.log"
    info = json.loads((ROOT / "firmware" / "os" / str(os_build()) /
                       "os.json").read_text())
    # (its entry's file, with or without its boot counter: once Flipper OS
    # has booted it well, its boot counting takes the "+3-0" off the name,
    # and the reboot's menu lists it so)
    want = f"flipperos-{info['profile']}-{info['version']}"
    panel = st.Panel(d / "panel.sock")
    panel.send("usb 2")                 # (a computer: OS-02)
    uboot_rockusb_checks(check, d, port)
    check("SPL's part: U-Boot and its device tree from the card's u-boot.itb",
          wait_text(qlog, r"SPL: U-Boot at 0x40800000", 20) and
          wait_text(qlog, r"the RK3576 starts", 20), text_of(qlog)[-300:])
    check("U-Boot starts on the board's revision, with the DDR blob's word "
          "on the DRAM (4 GiB)",
          wait_text(linux, r"Model: Flipper One rev\. F0B0C1", 120) and
          wait_text(linux, r"DRAM:\s+4 GiB", 30))
    before = panel.screen()
    picked = uboot_pick(panel, linux, want, 0, out / "uboot-menu.png")
    check("its boot menu on the Flipper's screen, with the card's boot "
          f"entries; the keys pick {info['profile']} {info['version']}",
          picked is not None and picked[1] != before,
          picked and f"entry {picked[0]}")
    check("... and U-Boot boots it: its kernel, initrd and device tree from "
          "the card's btrfs root",
          wait_text(linux, re.escape(f"/@{info['profile']}/usr/lib/modules/"
                                     f"{info['version']}/vmlinuz"), 120) and
          wait_text(linux, r"Starting kernel", 300))
    check("... the kernel with a seed for KASLR (U-Boot's, from the TRNG)",
          wait_text(linux, r"KASLR enabled", 60))
    check("... and all of the DRAM past TF-A's 2 MiB",
          wait_text(linux, r"node\s+0: \[mem 0x0000000040200000-"
                           r"0x000000013fffffff\]", 30))
    if not check("Flipper OS reaches its login",
                 wait_text(linux, r"login:", 1200)):
        return None
    con = Console(d / "linux.sock")
    check("its console answers", con.ready(300))
    cmdline = con.run("cat /proc/cmdline", 60) or [""]
    check("its command line: the boot entry's options, as U-Boot gave them",
          cmdline[0].strip() == info["options"], cmdline[0][:120])
    qmark, lmark = len(text_of(qlog)), len(text_of(linux))
    con.send("systemctl reboot")
    check("reboot: the RK3576 starts again, at U-Boot",
          wait_text(qlog, r"the RK3576 starts again", 300, qmark) and
          wait_text(linux, r"U-Boot 20\d\d", 120, lmark))
    check("... its menu again, and from there the kernel",
          uboot_pick(panel, linux, want, lmark) is not None and
          wait_text(linux, r"Booting Linux", 300, lmark))
    lmark = len(text_of(linux))
    if not check("... to its login again",
                 wait_text(linux, r"login:", 1200, lmark) and con.ready(300)):
        return con
    # the device tree's reboot mode: BOOT_BROM_DOWNLOAD in PMU0_GRF, which
    # the boot ROM takes at the reset
    qmark = len(text_of(qlog))
    seen = text_of(d / "qemu.out").count("flipper-one: USB: 2207:350e")
    con.send("sudo systemctl reboot --reboot-argument=maskrom")
    check("a reboot into the MaskROM (systemctl's --reboot-argument=maskrom): "
          "the RK3576 starts again there, from the boot mode Linux wrote",
          wait_text(qlog, r"waits in its MaskROM", 300, qmark))
    check("... which the computer enumerates (2207:350e)",
          st.until(lambda: text_of(d / "qemu.out").count(
              "flipper-one: USB: 2207:350e") > seen, 30))
    return con


def mcu_update_checks(check, d, out, port, fw):
    linux, mcu = d / "linux.log", d / "mcu.log"
    sysfs = "/sys/bus/platform/drivers/flipper-one-fw-update/*"
    if not check("Flipper OS reaches its login",
                 wait_text(linux, r"login:", 1200)):
        return None
    con = Console(d / "linux.sock")
    check("its console answers", con.ready(300))

    def state():
        got = con.run("for f in firmware_version staged_version boot_slot "
                      f"boot_type; do echo $(cat {sysfs}/$f); done", 60) or []
        return [line.strip() for line in got]
    now = state()
    check("the MCU's updater, over I2C: its firmware's version, slot A, a "
          "normal boot",
          len(now) == 4 and now[0] != "0.0.0" and now[2:] == ["A", "normal"],
          str(now))
    size = fw.stat().st_size
    got = con.run("G=$(ip route | awk '/default/{print $3; exit}'); "
                  f"curl -s -o /tmp/fw.uf2 http://$G:{port}/{fw.name} && "
                  "stat -c %s /tmp/fw.uf2", 300)
    t0 = time.time()
    sent = bool(got) and got[-1].strip() == str(size) and con.run(
        "D=/sys/class/firmware/flipper-one-mcu; "
        "echo 1 | sudo tee $D/loading >/dev/null; "
        "sudo dd if=/tmp/fw.uf2 of=$D/data bs=64k status=none; "
        "echo 0 | sudo tee $D/loading >/dev/null; echo sent", 300) == ["sent"]
    idle = False
    while sent and time.time() - t0 < 1800:
        status = con.run("cat /sys/class/firmware/flipper-one-mcu/status", 60)
        if status and status[0].strip() == "idle":
            idle = True
            break
        time.sleep(10)
    now = state()
    check(f"its own image ({size} bytes) over port 1's network, to the MCU "
          f"through the firmware class, staged in slot B "
          f"({time.time() - t0:.0f} s over I2C)",
          idle and len(now) == 4 and now[1] != "0.0.0" and
          wait_text(mcu, r"\[Updater\] Image staged at", 10), f"{got} {now}")
    mmark, lmark = len(text_of(mcu)), len(text_of(linux))
    con.send("sudo systemctl reboot")
    check("reboot: the MCU boots the staged image (the boot ROM's flash-update "
          "boot) and buys it",
          wait_text(mcu, r"Booting the staged image", 600, mmark) and
          wait_text(mcu, r"Flash-update boot: committing new image", 120,
                    mmark) and
          wait_text(mcu, r"Current FW slot: B", 60, mmark))
    back = wait_text(linux, r"login:", 1500, lmark) and con.ready(300)
    now = state() if back else []
    check("... and powers Flipper OS again, which reads slot B and a "
          "flash-update boot", back and now[2:] == ["B", "flash-update"],
          str(now))
    return con


def maskrom_checks(check, d, out, port, loader):
    sys.path.insert(0, str(ROOT / "scripts"))
    import fo_rockusb
    qlog, qout = d / "qemu.log", d / "qemu.out"
    rock = [sys.executable, str(ROOT / "scripts" / "fo_rockusb.py"),
            "--port", str(port)]

    def enumerated():
        return text_of(qout).count("flipper-one: USB: 2207:350e")

    panel = st.Panel(d / "panel.sock")
    check("a blank device: its boot ROM finds nothing to boot on the UFS's "
          "boot LU or the card, and waits in its MaskROM",
          wait_text(qlog, r"finds nothing to boot", 60) and
          wait_text(qlog, r"waits in its MaskROM", 10))
    panel.send("usb 2")
    check("a computer enumerates its USB download (2207:350e)",
          st.until(lambda: enumerated() > 0, 15))
    listed = subprocess.run(rock + ["list"], capture_output=True, text=True,
                            timeout=30).stdout
    check("fo_rockusb.py list: a Rockusb device",
          "2207:350e" in listed and "(Rockusb)" in listed, listed.strip())
    c, mark = None, len(text_of(qlog))
    try:
        c = fo_rockusb.connect(port=port)
        c.control(0x40, 0x0C, 0, 0x471, data=bytes(100) + b"\x12\x34")
        check("0x471 code with a wrong CRC: dropped",
              wait_text(qlog, r"failed its CRC", 5, mark))
        fo_rockusb.write_area(c, 0x471, bytes(range(256)) * 20)
        check("0x471 code with its CRC, in two requests: it runs (the "
              "machine's DDR blob)", wait_text(qlog, r"0x471 code ran", 5,
                                               mark))
        if not loader:
            seen = enumerated()
            (d / "ddr.bin").write_bytes(bytes(range(256)) * 20)
            (d / "usbplug.bin").write_bytes(b"usbplug" + bytes(1000))
            fo_rockusb.write_area(c, 0x472, b"usbplug" + bytes(1000))
            check("0x472 code with no FIT in it, a usbplug: the device "
                  "leaves the bus and comes back (the machine's Loader mode)",
                  wait_text(qlog, r"a usbplug, which the machine plays", 5,
                            mark) and st.until(lambda: enumerated() > seen, 15))
    except (OSError, ValueError) as e:
        check("the maskrom protocol over USB/IP", False, str(e))
    finally:
        if c:
            c.close()
    if not loader:
        loader_checks(check, d, port, rock, enumerated)
    if loader:
        before = panel.screen()
        t = time.time()
        r = subprocess.run(rock + ["download-boot", str(loader)],
                           capture_output=True, text=True, timeout=600)
        check(f"download-boot {loader.name} ({time.time() - t:.0f} s): SPL in "
              "Falcon mode starts its kernel",
              r.returncode == 0 and
              wait_text(qlog, r"SPL \(Falcon\): the kernel at", 30, mark) and
              wait_text(qlog, r"starts what the boot ROM's USB download", 10,
                        mark), (r.stdout + r.stderr).strip()[-300:])
        check("... and the installer's interface comes up (its console's "
              "Selections), on the Flipper's screen too",
              wait_text(d / "linux.log", r"Selections", 180) and
              st.until(lambda: panel.screen() != before, 30))
        time.sleep(5)
        panel.save(out / "installer.png")
    # the MCU's Maskrom: MASKROM_EN when it switches the rail on (its CPU
    # app's Power menu, for Stop, takes keys once the app has settled)
    st.wait_log(d, r"CPU app started", 30)
    time.sleep(5)
    mark, seen = len(text_of(qlog)), enumerated()
    apps = st.log_text(d).count("Application stopped")
    panel.tap("power")
    time.sleep(1.5)
    panel.tap("ok")
    check("Stop: the RK3576 off, the MCU's desktop",
          wait_text(qlog, r"the RK3576 is off", 30, mark) and
          st.until(lambda: st.log_text(d).count("Application stopped") >
                   apps, 15))
    st.cli(d, "desktop start_app cpu_app_maskrom", None, 1)
    check("the MCU's Maskrom (MASKROM_EN): the RK3576 in its USB download "
          "again", wait_text(qlog, r"waits in its MaskROM", 15, mark))
    check("... which the computer enumerates again",
          st.until(lambda: enumerated() > seen, 15))


def loader_checks(check, d, port, rock, enumerated):
    """Rockusb's Loader mode, as the machine plays a usbplug: its commands,
    the UFS's LU 0 and the card behind it (their images read where it
    wrote), its reset"""
    import fo_rockusb
    qlog = d / "qemu.log"

    def image_at(name, lba, n):         # (the image's bytes there alone)
        with open(d / name, "rb") as f:
            f.seek(lba * fo_rockusb.SECTOR)
            return f.read(n)

    def run(*args):
        r = subprocess.run(rock + list(args), capture_output=True, text=True,
                           timeout=60)
        return (r.stdout + r.stderr).strip()

    c = None
    try:
        c = fo_rockusb.connect(port=port)
        desc = c.control(0x80, 6, 0x0100, 0, 18)
        check("its device descriptor says Loader (bcdUSB 2.01, as rockusb "
              "and rkdeveloptool tell it)", desc[2:4] == b"\x01\x02",
              desc[:4].hex(" "))
        ld = fo_rockusb.Loader(c)
        chip = ld.command(0x1B, 16)
        check("chip-info: the RK3576's (6753)", chip[:4] == b"6753",
              chip.hex(" "))
        # (LU 0 as the launcher gives it: the 64 GB less 160 MiB for the
        # other LUs, in whole 4 MiB allocation units)
        lu0 = (64 * 10**9 - (160 << 20)) // (4 << 20) * (4 << 20)
        for name, image, index, size in (
                ("the UFS's LU 0", "ufs.img", 12, lu0),
                ("the card", "sd.img", 2, (d / "sd.img").stat().st_size)):
            if index != 12:
                ld.command(0x2A, 0, False, opcode=index)
            bits = int.from_bytes(ld.command(0x2B, 4), "little")
            sectors = size // fo_rockusb.SECTOR
            info = ld.command(0x1A, 11)
            check(f"{name}: storage {index}, flash-info its {sectors} "
                  "sectors", bits == 1 << index and
                  int.from_bytes(info[:4], "little") == sectors,
                  f"storage bits {bits:#x}, {info.hex(' ')}")
            lba, data = sectors - 1000, os.urandom(64 << 10)
            ld.write_lba(lba, data)
            back = ld.read_lba(lba, len(data) // fo_rockusb.SECTOR)
            check(f"... 64 KiB written at sector {lba} read back the same, "
                  f"and are in {image}", back == data and
                  image_at(image, lba, len(data)) == data)
            ld.command(0x25, 0, False, lba, len(data) // fo_rockusb.SECTOR,
                       cdb=0x0A)
            zero = bytes(len(data))
            check("... erased: zeros, there and in the image",
                  ld.read_lba(lba, len(data) // fo_rockusb.SECTOR) == zero
                  and image_at(image, lba, len(data)) == zero)
            try:
                ld.read_lba(sectors, 1)
                past = "read"
            except IOError as e:
                past = str(e)
            check("... a read past its end fails", "failed (status 1)" in past,
                  past)
        ld.command(0x2A, 0, False, opcode=12)
        lba, data = lu0 // fo_rockusb.SECTOR - 10000, os.urandom(4 << 20)
        t0 = time.monotonic()
        ld.write_lba(lba, data)
        t1 = time.monotonic()
        back = ld.read_lba(lba, len(data) // fo_rockusb.SECTOR)
        t2 = time.monotonic()
        ld.command(0x25, 0, False, lba, len(data) // fo_rockusb.SECTOR,
                   cdb=0x0A)
        print(f"      (Rockusb: 4 MiB written in {t1 - t0:.1f} s, read back "
              f"in {t2 - t1:.1f} s{'' if back == data else ', NOT the same'})")
    except (OSError, ValueError) as e:
        check("Rockusb's Loader mode over USB/IP", False, str(e))
    finally:
        if c:
            c.close()
    out = "\n".join(run(c) for c in ("capability", "storage", "flash-id"))
    check("fo_rockusb.py capability, storage and flash-id: direct LBA, a "
          "switch of storage; the UFS; SATA (a SCSI disk to the usbplug)",
          "switch storage" in out and "12 (ufs)" in out and "'SATA '" in out,
          out)

    # write-bmap: an image's mapped ranges alone (a 4 MiB one with two),
    # onto the card filled with a marker first: the rest keeps the marker
    block, ranges = 4096, ((0, 1), (512, 519))
    img = bytearray(4 << 20)
    for first, last in ranges:
        img[first * block:(last + 1) * block] = os.urandom(
            (last - first + 1) * block)
    (d / "bmap.img").write_bytes(img)
    sums = [hashlib.sha256(img[f * block:(l + 1) * block]).hexdigest()
            for f, l in ranges]
    (d / "bmap.img.bmap").write_text(
        "<?xml version=\"1.0\" ?>\n<bmap version=\"2.0\">\n"
        f"<ImageSize> {len(img)} </ImageSize>\n"
        f"<BlockSize> {block} </BlockSize>\n"
        f"<BlocksCount> {len(img) // block} </BlocksCount>\n"
        "<ChecksumType> sha256 </ChecksumType>\n<BlockMap>\n" +
        "".join(f'<Range chksum="{s}"> {f}-{l} </Range>\n'
                for (f, l), s in zip(ranges, sums)) +
        "</BlockMap>\n</bmap>\n")
    c = None
    try:
        c = fo_rockusb.connect(port=port)
        ld = fo_rockusb.Loader(c)
        ld.command(0x2A, 0, False, opcode=2)
        ld.write_lba(0, b"\xa5" * len(img))
    except (OSError, ValueError) as e:
        check("the card filled with a marker", False, str(e))
    finally:
        if c:
            c.close()
    out = run("write-bmap", str(d / "bmap.img"))
    on_card = image_at("sd.img", 0, len(img))
    mapped = {b for f, l in ranges for b in range(f, l + 1)}
    check("fo_rockusb.py write-bmap: the image's two mapped ranges onto the "
          "card (their SHA-256 checked), its other blocks left as they were",
          "in 2 ranges" in out and all(
              on_card[b * block:(b + 1) * block] ==
              (img[b * block:(b + 1) * block] if b in mapped
               else b"\xa5" * block) for b in range(len(img) // block)),
          out[-120:])

    def maskrom_again(what, seen):
        c = None
        try:
            check(f"{what}: which the computer enumerates again",
                  st.until(lambda: enumerated() > seen, 15))
            c = fo_rockusb.connect(port=port)
            desc = c.control(0x80, 6, 0x0100, 0, 18)
            check("... a MaskROM again (bcdUSB 2.00)",
                  desc[2:4] == b"\x00\x02", desc[:4].hex(" "))
        except (OSError, ValueError) as e:
            check(f"{what}: the MaskROM", False, str(e))
        finally:
            if c:
                c.close()

    kept = TEMPLATE / "sd.img"
    if not kept.exists():
        mark, seen = len(text_of(qlog)), enumerated()
        out = run("reset")
        check("reset: the RK3576 starts again, finds nothing to boot, and "
              "waits in its MaskROM", out == "reset" and
              wait_text(qlog, r"finds nothing to boot", 15, mark) and
              wait_text(qlog, r"waits in its MaskROM", 10, mark), out)
        maskrom_again("the reset", seen)
        return
    with open(kept, "rb") as f:         # (U-Boot's part of it alone)
        boot = f.read(12 << 20)
    c = None
    try:
        c = fo_rockusb.connect(port=port)
        ld = fo_rockusb.Loader(c)
        ld.command(0x2A, 0, False, opcode=2)
        t0 = time.monotonic()
        ld.write_lba(0, boot)
        took = time.monotonic() - t0
        check(f"Flipper OS's U-Boot (the kept card's first 12 MiB) written to "
              f"the card ({took:.1f} s), as rkdeveloptool's wl would",
              image_at("sd.img", 0, len(boot)) == boot)
    except (OSError, ValueError) as e:
        check("U-Boot written to the card", False, str(e))
    finally:
        if c:
            c.close()
    mark, seen = len(text_of(qlog)), enumerated()
    out = run("reset", "maskrom")
    check("reset maskrom (rkdeveloptool's rd 3): the RK3576 starts again in "
          "its MaskROM, the card's U-Boot notwithstanding",
          out == "reset (maskrom)" and
          wait_text(qlog, r"waits in its MaskROM", 15, mark) and
          "SPL" not in text_of(qlog)[mark:], out)
    maskrom_again("reset maskrom", seen)
    seen = enumerated()
    out = run("download-sram", str(d / "ddr.bin")) + "\n" + \
        run("download-ddr", str(d / "usbplug.bin"))
    check("the usbplug again (download-sram, download-ddr): the Loader mode",
          wait_text(qlog, r"a usbplug, which the machine plays", 15, mark) and
          st.until(lambda: enumerated() > seen, 15), out)
    mark = len(text_of(qlog))
    out = run("reset")
    check("reset: the boot ROM boots the card, U-Boot as the PC wrote it",
          out == "reset" and
          wait_text(qlog, r"SPL: U-Boot at 0x40800000", 15, mark) and
          wait_text(qlog, r"the RK3576 starts", 10, mark), out)


def log_checks(check, d):
    k = text_of(d / "linux.log")
    bad = re.findall(r"Oops|Kernel panic|BUG: |Unable to handle kernel|"
                     r"rcu: INFO", k)
    check("no oops, panic or RCU stall in the kernel's log", not bad,
          ", ".join(sorted(set(bad))))
    # the warnings that were the emulator's own (log 96): the DDR blob's
    # word (the DRAM's kind), the UFS's clock gating wait, the GMACs' CSR
    # clock
    mine = re.findall(r"unsupported memory type|Undefined ref clk gating|"
                      r"clk_csr value out of range", k)
    check("none of the warnings that were the emulator's (the DRAM's kind, "
          "the UFS's clock gating wait, the GMACs' CSR clock)", not mine,
          ", ".join(sorted(set(mine))))
    check("no crash in the MCU's log", not st.CRASH.search(st.log_text(d)))
    late = k.count("Interrupt latency")
    print(f"      (the SD driver's interrupt-latency warnings: {late})")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--keep", action="store_true",
                    help="keep the device (devices/linux-PID) afterwards")
    ap.add_argument("--fresh", action="store_true",
                    help="a first boot, from the build's image, whatever "
                    "card is kept; keep that one instead")
    ap.add_argument("--uboot", action="store_true",
                    help="the boot through U-Boot instead (run --uboot), "
                    "from the kept card")
    ap.add_argument("--mcu-update", action="store_true",
                    help="the MCU's firmware updated from Flipper OS instead, "
                    "from the kept card (needs passt)")
    ap.add_argument("--maskrom", action="store_true",
                    help="a blank device's boot ROM in its USB download "
                    "instead (run --uboot, the maskrom protocol over USB/IP)")
    ap.add_argument("--loader", type=pathlib.Path,
                    help="with --maskrom: a Rockchip loader to download-boot "
                    "(the Flipper OS installer's installer-falcon-loader.bin)")
    ap.add_argument("--out", help="where the screenshots go "
                    "(default: a new folder in the system's temp)")
    args = ap.parse_args()

    if args.loader and not (args.maskrom and args.loader.is_file()):
        sys.exit("--loader: a file, with --maskrom")
    if os_build() is None and not args.maskrom:
        sys.exit("no Flipper OS build: run scripts/fetch_os.py")
    name = f"linux-{os.getpid()}"
    d = ROOT / "devices" / name
    out = pathlib.Path(args.out or tempfile.mkdtemp(prefix="fo-linux-"))
    out.mkdir(parents=True, exist_ok=True)
    if d.exists():
        sys.exit(f"{d} is there already")
    first_boot = args.fresh or not template_ok()
    if (args.uboot or args.mcu_update) and first_boot:
        sys.exit("--uboot, --mcu-update: from a kept card (run the check "
                 "without them first)")
    kept = TEMPLATE / "sd.img.new"
    d.mkdir(parents=True)
    if not first_boot and not args.maskrom:
        copy_card(TEMPLATE / "sd.img", d / "sd.img")
    net = shutil.which("passt") is not None     # (QEMU's network backend)
    print("a blank device, its boot ROM's USB download" if args.maskrom else
          f"{'a first boot' if first_boot else 'from the kept card'}, "
          f"Flipper OS build {os_build()}, "
          f"{'a network on Ethernet port 1' if net else 'no passt: no network'}",
          flush=True)
    check = st.Checks()
    t0 = time.time()
    # the getty on the USB gadget's serial port is masked: with no computer
    # on the gadget, its setup blocks while it holds the console's lock,
    # and the console's getty may then never start (Flipper OS's OS-02,
    # docs/FIRMWARE_BUGS.md; 2 boots of 19 here). The audio codec's sound
    # goes to the device's audio.wav.
    mic_wav(d / "mic.wav")
    mask = ["--kernel-args", "systemd.mask=serial-getty@flipserial0.service"]
    if args.mcu_update and not net:
        sys.exit("--mcu-update: Flipper OS fetches the image over the "
                 "network: passt is needed")
    port = st.free_port()
    run = [sys.executable, str(st.LAUNCHER), "run", "-n", name, "--detach",
           "--linux"] + (["--uboot", "--usbip", str(port)] if args.uboot else
                         mask + ["--net"] if args.mcu_update else mask + [
           "--audio", str(d / "audio.wav"), "--mic", str(d / "mic.wav"),
           "--nvme", "--hdmi", "--lan"] +
           (["--net", "--wifi"] if net else []))
    if args.maskrom:                    # (no card: the launcher's blank one)
        port = st.free_port()
        run = [sys.executable, str(st.LAUNCHER), "run", "-n", name,
               "--detach", "--uboot", "--usbip", str(port)]
    if subprocess.run(run).returncode:
        sys.exit("the device did not start")
    lan_host = web = None
    if args.mcu_update:                 # (the MCU's own image, to be fetched)
        fw = pathlib.Path(json.loads((d / "firmware.json").read_text())[
            "uf2"].replace("-recovery-", "-firmware-"))
        port = 18000 + os.getpid() % 1000
        web = subprocess.Popen(
            [sys.executable, "-m", "http.server", "--bind", "127.0.0.1",
             str(port), "--directory", str(fw.parent)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    elif not args.uboot and not args.maskrom:   # (port 2's other end)
        lan_host = subprocess.Popen(
            [sys.executable, str(ROOT / "scripts" / "lan_host.py"), "-n", name],
            stdout=open(d / "lan.log", "w"), stderr=subprocess.STDOUT)
    def off():
        if first_boot and not check.failed:
            TEMPLATE.mkdir(parents=True, exist_ok=True)
            kept.unlink(missing_ok=True)
            copy_card(d / "sd.img", kept)

    try:
        if args.maskrom:
            maskrom_checks(check, d, out, port, args.loader)
        elif args.uboot:
            uboot_checks(check, d, out, port)
        elif args.mcu_update:
            mcu_update_checks(check, d, out, port, fw)
        else:
            checks(check, d, out, first_boot, off, net)
        log_checks(check, d)
    finally:
        for p in (lan_host, web):
            if p:
                p.terminate()
        subprocess.run([sys.executable, str(st.LAUNCHER), "stop", "-n", name],
                       stdout=subprocess.DEVNULL)
    if kept.exists():
        if check.failed:
            kept.unlink()
        else:
            kept.replace(TEMPLATE / "sd.img")
            (TEMPLATE / "card.json").write_text(json.dumps(
                {"build": os_build(),
                 "made": time.strftime("%Y-%m-%d %H:%M:%S")},
                indent=1) + "\n")
            print(f"kept its card: {TEMPLATE.relative_to(ROOT)}/sd.img")
    if not args.keep and d.is_dir() and d.name.startswith("linux-"):
        shutil.rmtree(d)
    print(f"{'PASS' if not check.failed else 'FAIL'}: "
          f"{check.failed} failed, {time.time() - t0:.0f} s; "
          f"screenshots in {out}")
    return 1 if check.failed else 0


if __name__ == "__main__":
    sys.exit(main())
