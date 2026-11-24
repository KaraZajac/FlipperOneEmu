#!/usr/bin/env python3
# Copyright (c) 2026, Kara Zajac
# SPDX-License-Identifier: BSD-3-Clause
"""Headless smoke test: does the MCU firmware boot and work in FlipperOneEmu?

    scripts/smoke_test.py [--keep] [--out DIR] [--uf2 FILE]

Boots a throwaway device, devices/smoke-PID (removed at the end unless
--keep), and checks it through its sockets, as a program would:

- its log: each chip on the control and main buses found, "Startup
  complete", the intercom's slave mode, the CPU app; no crash, and no error
  but those the firmware logs on any board (a blank OTP);
- the host link: the intercom's version; the system interrupt the MCU
  raises at its start, behind CPU_INT, which reading it lets go; the CPU
  state; a frame over the display SPI, on the panel pixel for pixel; an LED
  from its colour register; the haptic register running the motor;
- its CLI: the firmware's own unit tests pass, and `power show` reads the
  gauge, the charger and the current meter as the battery model has them;
- the buttons and the touchpad: `input dump` sees a key and a touch sent on
  the front-panel link;
- Power's Flipper OS menu over the frame, and Stop there: the MCU's desktop;
- a charger plugged in, then pulled out: the charger's interrupts through
  the main board's expander, `power show` charging;
- a computer plugged in: it enumerates the MCU's USB device, and the CLI
  answers on its USB serial port;
- the same device again with BOOTSEL held: the RP2350's boot ROM's own USB
  boot, which the computer enumerates.

Exit status 0 = pass. Screenshots go to --out.
"""
import argparse
import base64
import os
import pathlib
import queue
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
LAUNCHER = ROOT / "flipperoneemu"
FRAME_W, H = 258, 144           # a frame from the host
LCD_W = 256                     # ... and the columns of it that show

CRASH = re.compile(r"furi_check failed|\[CRASH\]|HardFault|Kernel is Dead")
# errors the firmware logs on any board: a blank OTP; settings not written
# yet, on a new device; the self-check holding the desktop's slot when the
# CPU app first asks for it
KNOWN_ERRORS = re.compile(r"USB white label is not valid|"
                          r"BOOTSEL LED is not valid|"
                          r"\[FuriHalNvm\] Key = \"\w+\", item not found|"
                          r"Failed to read \w+ from NVM, defaulting|"
                          r"but self_check is already running")
CHIPS = {
    "charger (BQ25792)": "BQ2579X device ready at address 0x6B",
    "USB-C PD (FUSB302)": "FUSB302 device ready at address 0x22",
    "touchpad (IQS7211E)": "IQS7211E device ready at address 0x56",
    "current meter (INA219)": "INA219 device ready at address 0x45",
    "gauge (BQ28Z620)": "BQ28Z620 device ready at address 0x55",
    "USB mux (HD3SS3220)": "HD3SS3220 device ready at address 0x47",
}


class Checks:
    def __init__(self):
        self.failed = 0
        self.t0 = time.time()

    def __call__(self, what, ok, detail=""):
        print(f"{time.time() - self.t0:5.1f}  {'ok  ' if ok else 'FAIL'}  "
              f"{what}" + (f": {detail}" if detail and not ok else ""),
              flush=True)
        if not ok:
            self.failed += 1
        return ok


def log_text(d):
    raw = (d / "mcu.log").read_bytes() if (d / "mcu.log").exists() else b""
    return re.sub(r"\x1b\[[0-9;]*m", "", raw.decode("utf-8", "replace"))


def wait_log(d, pattern, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if re.search(pattern, log_text(d)):
            return True
        time.sleep(0.1)
    return False


def until(test, timeout, step=0.05):
    end = time.time() + timeout
    while time.time() < end:
        if test():
            return True
        time.sleep(step)
    return test()


class Panel:
    """The front-panel link: what the screen shows, the LEDs, the motor;
    the buttons and the touchpad"""

    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(str(path))
        self.s.sendall(b"hello flipperoneemu smoke_test\n")
        self.lock = threading.Lock()
        self.pixels = bytearray(LCD_W * H)
        self.leds = []
        self.buzzed = False
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        buf = b""
        while True:
            try:
                chunk = self.s.recv(1 << 16)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                word, _, rest = line.decode("ascii", "replace").partition(" ")
                with self.lock:
                    if word == "rows":
                        y, n, data = rest.split(" ", 2)
                        y, n = int(y), int(n)
                        self.pixels[y * LCD_W:(y + n) * LCD_W] = \
                            base64.b64decode(data)
                    elif word == "leds":
                        self.leds = rest.split()
                    elif word == "haptic" and rest.strip() == "1":
                        self.buzzed = True

    def screen(self):
        with self.lock:
            return bytes(self.pixels)

    def save(self, path):
        """the screen as a grey PNG"""
        rows = self.screen()
        raw = b"".join(b"\0" + rows[y * LCD_W:(y + 1) * LCD_W]
                       for y in range(H))

        def chunk(kind, data):
            return (struct.pack(">I", len(data)) + kind + data +
                    struct.pack(">I", zlib.crc32(kind + data)))
        pathlib.Path(path).write_bytes(
            b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", LCD_W, H, 8, 0, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))

    def tap(self, key, hold=0.15):
        self.s.sendall(f"press {key}\n".encode())
        time.sleep(hold)
        self.s.sendall(f"release {key}\n".encode())

    def swipe(self, x0, x1, y, steps=5):
        for i in range(steps):
            x = x0 + (x1 - x0) * i // (steps - 1)
            self.s.sendall(f"touch {x} {y} 128\n".encode())
            time.sleep(0.05)
        self.s.sendall(b"touch end\n")


class Host:
    """The host link: the RK3576's side of I2C1, CPU_INT and the display"""

    EVENTS = ("int", "sent", "hello")

    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(str(path))
        self.replies = queue.Queue()
        self.ints = []                  # CPU_INT's levels, as told
        self.sent = 0                   # frames that went out
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        buf = b""
        while True:
            try:
                chunk = self.s.recv(1 << 16)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.decode("ascii", "replace").strip()
                word = line.split(" ", 1)[0]
                if word == "int":
                    self.ints.append(int(line.split()[1]))
                elif word == "sent":
                    self.sent += 1
                elif word != "hello":
                    self.replies.put(line)

    def do(self, command, timeout=5):
        self.s.sendall(command.encode() + b"\n")
        try:
            return self.replies.get(timeout=timeout)
        except queue.Empty:
            return "(no reply)"

    def frame(self, data, timeout=10):
        before = self.sent
        self.s.sendall(b"frame " + data.hex().encode() + b"\n")
        return until(lambda: self.sent > before, timeout)


def cli(d, command, done, timeout, ctrl_c=False, usb=False):
    """a CLI command's output, up to @done (a regex) or @timeout; a command
    that goes on until stopped (`power show`) is stopped with Ctrl-C; @usb:
    on the MCU's USB serial port, not its UART"""
    def plain(raw):
        return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "",
                      raw.decode("utf-8", "replace"))

    s = socket.socket(socket.AF_UNIX)
    s.connect(str(d / ("usb.sock" if usb else "cli.sock")))
    s.settimeout(0.2)
    if usb:
        time.sleep(0.5)                 # DTR up: the session's banner first
    s.sendall(command.encode() + b"\r")
    out, end = b"", time.time() + timeout
    while time.time() < end:
        try:
            out += s.recv(1 << 16)
        except socket.timeout:
            pass
        if done and re.search(done, plain(out)):
            break
    if ctrl_c:
        s.sendall(b"\x03")
        time.sleep(0.3)
    s.close()
    return plain(out)


def last_value(text, label):
    """the number after "LABEL:" the last time it was printed"""
    m = re.findall(rf"{label}:\s+(-?[\d.]+)", text)
    return float(m[-1]) if m else None


def test_frame():
    """a frame to know again: a ramp, a chequered band, a border round
    what shows, a black block"""
    f = bytearray(FRAME_W * H)
    for y in range(H):
        for x in range(FRAME_W):
            v = min(x, 255)
            if 64 <= y < 128 and (x // 16 + y // 16) % 2 == 0:
                v = 255 - v
            if x in (0, LCD_W - 1) or y in (0, H - 1):
                v = 255
            if 100 <= x < 156 and 20 <= y < 50:
                v = 0
            f[y * FRAME_W + x] = v
    return bytes(f)


def shows(frame, screen):
    return all(screen[y * LCD_W:(y + 1) * LCD_W] ==
               frame[y * FRAME_W:y * FRAME_W + LCD_W] for y in range(H))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--keep", action="store_true",
                    help="keep the device (devices/smoke-PID) afterwards")
    ap.add_argument("--out", help="where the screenshots go "
                    "(default: a new folder in the system's temp)")
    ap.add_argument("--uf2", help="the MCU firmware (default: the newest "
                    "recovery .uf2 fetched)")
    args = ap.parse_args()

    name = f"smoke-{os.getpid()}"
    d = ROOT / "devices" / name
    out = pathlib.Path(args.out or tempfile.mkdtemp(prefix="fo-smoke-"))
    out.mkdir(parents=True, exist_ok=True)
    if d.exists():
        sys.exit(f"{d} is there already")
    check = Checks()
    run = [sys.executable, str(LAUNCHER), "run", "-n", name, "--detach"]
    if args.uf2:
        run += ["--uf2", args.uf2]
    t0 = time.time()
    if subprocess.run(run).returncode:
        sys.exit("the device did not start")
    try:
        body(check, name, d, out)
        bootsel(check, name, d)
    finally:
        subprocess.run([sys.executable, str(LAUNCHER), "stop", "-n", name],
                       stdout=subprocess.DEVNULL)
        if not args.keep and d.is_dir() and d.name.startswith("smoke-"):
            shutil.rmtree(d)
    print(f"{'PASS' if not check.failed else 'FAIL'}: "
          f"{check.failed} failed, {time.time() - t0:.0f} s; "
          f"screenshots in {out}")
    return 1 if check.failed else 0


def body(check, name, d, out):
    # -------------------------------------------------------- the log
    check("the firmware starts", wait_log(d, r"Startup complete", 30))
    text = log_text(d)
    for chip, line in CHIPS.items():
        check(f"the {chip} is found", line in text)
    check("the intercom's slave mode is set up",
          wait_log(d, r"enabling I2C slave mode", 10))
    check("the CPU app starts (the Linux side is powered)",
          wait_log(d, r"CPU app started", 20))

    # ---------------------------------------------------- the host link
    panel = Panel(d / "panel.sock")
    host = Host(d / "host.sock")
    reply = ""
    for _ in range(50):
        reply = host.do("read 0080 1")
        if reply.startswith("regs"):
            break
        time.sleep(0.2)
    check("the host link reads the intercom's version", reply ==
          "regs 0080 0001", reply)
    check("CPU_INT is asserted after the MCU's start",
          until(lambda: host.ints and host.ints[-1] == 0, 2), str(host.ints))
    status = host.do("read 0000 1")
    check("its status says: a system interrupt", status.startswith("regs") and
          int(status.split()[2], 16) & 4, status)
    sysint = host.do("read 0104 1")
    check("the system interrupt: the MCU restarted", sysint.startswith("regs")
          and int(sysint.split()[2], 16) & 1, sysint)
    check("reading it lets CPU_INT go",
          until(lambda: host.ints and host.ints[-1] == 1, 2), str(host.ints))
    check("CPU state 3 (online) is written", host.do("write 0040 0003") ==
          "ok")
    check("... and the MCU takes it", wait_log(d, r"CPU State: 3", 3))

    frame = test_frame()
    t = time.time()
    check("a frame goes over the display SPI", host.frame(frame))
    took = time.time() - t
    ok = until(lambda: shows(frame, panel.screen()), 5)
    check(f"the frame shows pixel for pixel ({took:.2f} s)", ok)
    panel.save(out / "frame.png")

    check("an LED's colour register is written",
          host.do("write 0310 f800") == "ok")
    check("... and the LED lights red", until(
        lambda: panel.leds and int(panel.leds[0], 16) >> 16 > 0 and
        int(panel.leds[0], 16) & 0xffff == 0, 3), str(panel.leds[:1]))
    check("the haptic register is written", host.do("write 0400 8f32") ==
          "ok")
    check("... and the motor runs", until(lambda: panel.buzzed, 3))

    # ------------------------------------------------------------ the CLI
    text = cli(d, "unit_tests", r"Status: (PASSED|FAILED)", 60)
    check("the firmware's unit tests pass", "Status: PASSED" in text,
          text[-300:])
    # it draws its page again and again: a few times over, then Ctrl-C
    text = cli(d, "power show", None, 2.5, ctrl_c=True)

    def value(label):
        return last_value(text, label)
    check("power show: the gauge's charge is 80 %", value("SoC") == 80,
          str(value("SoC")))
    vsys = value("VSYS")    # the battery's 8.02 V less the load's drop
    check("power show: VSYS is the battery's", vsys is not None and
          7.8 < vsys < 8.2, str(vsys))
    temp = value("BatTemp")
    check("power show: the battery is at 25 °C", temp is not None and
          abs(temp - 25) < 0.5, str(temp))

    s = socket.socket(socket.AF_UNIX)
    s.connect(str(d / "cli.sock"))
    s.settimeout(0.2)
    s.sendall(b"input dump\r")
    dump, end = b"", time.time() + 5
    while b"Press CTRL+C to stop" not in dump and time.time() < end:
        try:
            dump += s.recv(1 << 16)
        except socket.timeout:
            pass
    time.sleep(0.2)
    panel.tap("up")
    time.sleep(0.3)
    panel.swipe(300, 500, 400)
    end = time.time() + 2
    while time.time() < end:
        try:
            dump += s.recv(1 << 16)
        except socket.timeout:
            pass
    s.sendall(b"\x03")
    time.sleep(0.3)
    s.close()
    dump = dump.decode("utf-8", "replace")
    check("input dump: a button (Up)", "key: Up type: Press" in dump,
          dump[-200:])
    check("input dump: a touch", "touch: Start" in dump and
          "touch: Move" in dump, dump[-200:])

    # ----------------------------------------- the Flipper OS menu, Stop
    before = panel.screen()
    panel.tap("power")
    check("Power opens the Flipper OS menu over the frame",
          until(lambda: panel.screen() != before, 3))
    time.sleep(0.5)
    panel.save(out / "menu.png")
    menu = panel.screen()
    panel.tap("ok")
    check("Stop turns the Linux side off",
          wait_log(d, r"CPU app shutdown requested", 3))
    check("... and the MCU's desktop shows",
          until(lambda: panel.screen() not in (menu, before), 3))
    time.sleep(1.0)
    panel.save(out / "desktop.png")

    # ---------------------------------- a charger, with the MCU in charge
    # (with the Linux side off, the main board's expander is the MCU's)
    panel.s.sendall(b"usb 1\n")
    check("a charger plugged in: its interrupts reach the firmware",
          wait_log(d, r"IRQ1:.*BC1\.2_DONE VBUS CHG", 5))
    text = cli(d, "power show", None, 2.5, ctrl_c=True)
    check("power show: VBUS is 5 V", last_value(text, "VBUS") == 5.0,
          str(last_value(text, "VBUS")))
    check("power show: fast charge (CC)", "Fast charge (CC mode)" in text)
    ibat = last_value(text, "IBAT")
    check("power show: the battery charges", ibat is not None and ibat > 1000,
          str(ibat))
    panel.s.sendall(b"usb 0\n")
    check("pulled out: the firmware hears that too",
          wait_log(d, r"IRQ0:\s+0x0B\s+VBUS_PRESENT", 3))

    # ------------------- a computer: the MCU's USB (the Linux side is off)
    panel.s.sendall(b"usb 2\n")
    found = until(lambda: "flipper-one: USB: 37c1:f101" in
                  (d / "qemu.out").read_text(errors="replace"), 10)
    check("a computer plugged in enumerates the MCU's USB (37C1:F101)", found)
    text = cli(d, "help", r"Available commands:(.|\n)*gpio", 10, usb=True)
    check("the CLI answers on its USB serial port",
          "Available commands" in text, text[-200:])
    panel.s.sendall(b"usb 0\n")

    # ---------------------------------------------- the log, at the end
    text = log_text(d)
    crash = CRASH.search(text)
    check("no crash in the log", not crash, crash and crash.group(0))
    errors = [line for line in text.splitlines()
              if "[E]" in line and not KNOWN_ERRORS.search(line)]
    check("no error in the log", not errors, "; ".join(errors[:3]))


def bootsel(check, name, d):
    """the same device again, BOOTSEL held: the boot ROM's own USB boot"""
    subprocess.run([sys.executable, str(LAUNCHER), "stop", "-n", name],
                   stdout=subprocess.DEVNULL)
    if subprocess.run([sys.executable, str(LAUNCHER), "run", "-n", name,
                       "--detach", "--bootsel"],
                      stdout=subprocess.DEVNULL).returncode:
        check("the device starts again with BOOTSEL held", False)
        return
    panel = None
    for _ in range(50):
        try:
            panel = Panel(d / "panel.sock")
            break
        except OSError:
            time.sleep(0.1)
    if not check("BOOTSEL: the panel link is there", panel is not None):
        return
    panel.s.sendall(b"usb 2\n")
    found = until(lambda: "flipper-one: USB: 2e8a:000f" in
                  (d / "qemu.out").read_text(errors="replace"), 15)
    check("BOOTSEL: a computer enumerates the boot ROM's USB boot (2E8A:000F)",
          found)


if __name__ == "__main__":
    sys.exit(main())
