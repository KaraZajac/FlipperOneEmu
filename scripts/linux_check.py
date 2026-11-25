#!/usr/bin/env python3
# Copyright (c) 2026, Kara Zajac
# SPDX-License-Identifier: BSD-3-Clause
"""Headless check of the whole device: does Flipper OS run beside the MCU?

    scripts/linux_check.py [--keep] [--fresh] [--out DIR]

Boots a throwaway device with Flipper OS on its RK3576, devices/linux-PID
(removed at the end unless --keep), and checks it through its sockets:

- the RK3576 starts when the MCU firmware switches its rail on, and the
  front-panel link says so; the kernel boots on the Flipper One's device
  tree; the MCU sees its driver (CPU state 2) and then user space (3);
- Flipper OS reaches its login on the console; FlipCTL draws on the
  Flipper's screen (its frames over the display SPI, through the MCU), and
  a key pressed on the panel reaches it, through the MCU and the I2C link;
- Linux reads the main board's chips over the bus it shares with the MCU:
  the INA219 on VSYS, the five INA4230s; its root is the microSD card;
  the CPU clusters' clocks come from TF-A's SCMI server, and cpufreq has
  both clusters; the six thermal zones read the TSADC, with the trims the
  OTP holds;
- `systemctl reboot` starts the RK3576 again, and the kernel with it;
  `systemctl poweroff` switches it off (PSCI), after which the MCU cuts
  its rail; the MCU's CPU app switches it on again, and the kernel starts;
- no oops, panic or RCU stall in the kernel's log, and no crash in the
  MCU's.

A first boot takes several minutes (Flipper OS sets itself up); the first
run keeps its card as build/linux-check/sd.img, as it is after the checked
power-off, and later runs start from a copy of it (a reflink where the
filesystem has them), for the same Flipper OS build. --fresh does a first
boot anyway, and keeps that card instead. Exit status 0 = pass.
Screenshots go to --out.
"""
import argparse
import json
import os
import pathlib
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import smoke_test as st                 # noqa: E402  (its checks and links)

ROOT = st.ROOT
TEMPLATE = ROOT / "build" / "linux-check"
KMSG = re.compile(r"^\[\s*\d+\.\d+\]")
ESC = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


def text_of(path):
    raw = path.read_bytes() if path.exists() else b""
    return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "",
                  raw.decode("utf-8", "replace"))


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


def copy_card(src, dst):
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", str(src),
                    str(dst)], check=True)


def checks(check, d, out, first_boot, off):
    boot_s = 1200 if first_boot else 600
    linux, qlog, mcu = d / "linux.log", d / "qemu.log", d / "mcu.log"

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
    check("the MCU sees user space (CPU state 3)",
          st.wait_log(d, r"CPU State: 3", 300))
    check("FlipCTL draws on the Flipper's screen",
          wait_text(linux, r"first frame on the panel", 300) and
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
    check("its console answers", con.ready())
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
    freq = con.run("cat /sys/devices/system/cpu/cpufreq/policy0/"
                   "scaling_cur_freq /sys/devices/system/cpu/cpufreq/"
                   "policy4/scaling_cur_freq")
    check("the clusters' clocks over SCMI: cpufreq has both",
          bool(freq) and len(freq) == 2 and
          all(f.strip().isdigit() for f in freq), str(freq))
    temps = con.run("cat /sys/class/thermal/thermal_zone*/temp")
    check("six temperatures from the TSADC (its trims from the OTP)",
          bool(temps) and len(temps) == 6 and
          all(t.strip().isdigit() and 25000 <= int(t) < 85000
              for t in temps), str(temps))

    qmark = len(text_of(qlog))
    lmark = len(text_of(linux))
    con.send("systemctl reboot")
    check("reboot: the RK3576 starts again",
          wait_text(qlog, r"the RK3576 starts again", 180, qmark))
    check("... and the kernel with it",
          wait_text(linux, r"Booting Linux", 60, lmark))
    lmark = len(text_of(linux))
    check("... to its login again", wait_text(linux, r"login:", 600, lmark)
          and con.ready())

    qmark = len(text_of(qlog))
    mmark = len(text_of(mcu))
    con.send("systemctl poweroff")
    check("power-off: the RK3576 is off (PSCI)",
          wait_text(qlog, r"the RK3576 is off", 180, qmark))
    if check("... and the MCU cuts its rail",
             wait_text(mcu, r"cutting its power", 60, mmark) and
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
          wait_text(linux, r"Booting Linux", 60, lmark))
    return con


def log_checks(check, d):
    k = text_of(d / "linux.log")
    bad = re.findall(r"Oops|Kernel panic|BUG: |Unable to handle kernel|"
                     r"rcu: INFO", k)
    check("no oops, panic or RCU stall in the kernel's log", not bad,
          ", ".join(sorted(set(bad))))
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
    ap.add_argument("--out", help="where the screenshots go "
                    "(default: a new folder in the system's temp)")
    args = ap.parse_args()

    if os_build() is None:
        sys.exit("no Flipper OS build: run scripts/fetch_os.py")
    name = f"linux-{os.getpid()}"
    d = ROOT / "devices" / name
    out = pathlib.Path(args.out or tempfile.mkdtemp(prefix="fo-linux-"))
    out.mkdir(parents=True, exist_ok=True)
    if d.exists():
        sys.exit(f"{d} is there already")
    first_boot = args.fresh or not template_ok()
    kept = TEMPLATE / "sd.img.new"
    d.mkdir(parents=True)
    if not first_boot:
        copy_card(TEMPLATE / "sd.img", d / "sd.img")
    print(f"{'a first boot' if first_boot else 'from the kept card'}, "
          f"Flipper OS build {os_build()}", flush=True)
    check = st.Checks()
    t0 = time.time()
    run = [sys.executable, str(st.LAUNCHER), "run", "-n", name, "--detach",
           "--linux"]
    if subprocess.run(run).returncode:
        sys.exit("the device did not start")
    def off():
        if first_boot and not check.failed:
            TEMPLATE.mkdir(parents=True, exist_ok=True)
            kept.unlink(missing_ok=True)
            copy_card(d / "sd.img", kept)

    try:
        checks(check, d, out, first_boot, off)
        log_checks(check, d)
    finally:
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
