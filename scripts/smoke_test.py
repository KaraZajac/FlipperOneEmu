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
  gauge, the charger and the current meter as the battery model has them,
  and the battery's temperature as the panel link's `ambient` sets it;
- the buttons and the touchpad: `input dump` sees a key and a touch sent on
  the front-panel link; a headset in the jack, its mic, a button on it;
- Power's Flipper OS menu over the frame, and Stop there: the MCU's desktop;
  the RPC (the CLI's `rpc`, scripts/fo_rpc.py): the screen streamed, as it
  shows, and keys pressed through it;
- a charger plugged in, then pulled out: the charger's interrupts through
  the main board's expander, `power show` charging;
- a computer plugged in: it enumerates the MCU's USB device, and the CLI
  answers on its USB serial port, and over USB/IP (fo_usbip.py playing the
  PC's kernel);
- an update, as picotool makes one: `power boot` (the firmware reboots into
  the boot ROM's USB boot, through the watchdog), PICOBOOT over USB/IP
  (fo_picoboot.py): the flash read, a spare sector erased, written and read
  back, the device's own firmware loaded, a reboot: the firmware starts
  again;
- the charger's power switch: `power reboot` (its system power reset: the
  board off a moment, and the firmware starting again), `power ship` (off
  until a charger is plugged in);
- the same device again with BOOTSEL held: the RP2350's boot ROM's own USB
  boot, which the computer enumerates.

Exit status 0 = pass. Screenshots go to --out.
"""
import argparse
import base64
import json
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

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import fo_rpc                           # noqa: E402  (the RPC's client)

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
        self.power = []                 # the board's supply, as told
        self.rk3576 = []                # ... and the RK3576's run
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
                    elif word == "power":
                        self.power.append(rest.strip() == "on")
                    elif word == "rk3576":
                        self.rk3576.append(rest.strip() == "on")

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

    def send(self, line):
        self.s.sendall(line.encode() + b"\n")

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
        #                                 (a byte before it: FW-04)
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


def shows_gui(frame, screen):
    """the firmware's own canvas (258 columns, as the RPC streams it) on the
    screen: its columns 1-256 are the ones that show"""
    return len(frame) == FRAME_W * H and all(
        screen[y * LCD_W:(y + 1) * LCD_W] ==
        frame[y * FRAME_W + 1:y * FRAME_W + 1 + LCD_W] for y in range(H))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--keep", action="store_true",
                    help="keep the device (devices/smoke-PID) afterwards")
    ap.add_argument("--out", help="where the screenshots go "
                    "(default: a new folder in the system's temp)")
    ap.add_argument("--uf2", help="the MCU firmware (default: the newest "
                    "recovery .uf2 fetched)")
    ap.add_argument("--gdb", type=int, metavar="PORT",
                    help="the device's gdb stub, for `./flipperoneemu gdb "
                    "-n smoke-PID` (the first start only)")
    args = ap.parse_args()

    name = f"smoke-{os.getpid()}"
    d = ROOT / "devices" / name
    out = pathlib.Path(args.out or tempfile.mkdtemp(prefix="fo-smoke-"))
    out.mkdir(parents=True, exist_ok=True)
    if d.exists():
        sys.exit(f"{d} is there already")
    check = Checks()
    port = free_port()
    run = [sys.executable, str(LAUNCHER), "run", "-n", name, "--detach",
           "--usbip", str(port)]
    if args.uf2:
        run += ["--uf2", args.uf2]
    if args.gdb:
        run += ["--gdb", str(args.gdb)]
    t0 = time.time()
    if subprocess.run(run).returncode:
        sys.exit("the device did not start")
    try:
        panel = body(check, name, d, out, port)
        update(check, d, port, panel)
        power(check, d, panel)
        log_checks(check, d)
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


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def usbip_cli(port, command):
    """the CLI over USB/IP, as the PC's kernel would drive it: configured,
    DTR up, the session's banner read, @command, its answer, DTR down"""
    sys.path.insert(0, str(ROOT / "scripts"))
    import fo_usbip
    c = fo_usbip.UsbipClient(port=port)
    try:
        c.control(0x00, 9, 1, 0)                    # SET_CONFIGURATION
        c.control(0x21, 0x22, 3, 0)                 # DTR, RTS
        c.sock.settimeout(1.5)

        def read_in(rounds):
            out = b""
            for _ in range(rounds):
                c.submit_in(2, 64)
                try:
                    out += c.receive()[3]
                except socket.timeout:
                    break
            return out
        read_in(80)                                 # the banner
        c.sock.settimeout(5)
        c.submit(2, False, data=command.encode() + b"\r")
        while c.receive()[3] != b"":                # the OUT's reply
            pass
        c.sock.settimeout(1.5)
        answer = read_in(10)
        c.sock.settimeout(5)
        c.control(0x21, 0x22, 0, 0, pending=[])     # DTR down
        return answer.decode("utf-8", "replace")
    finally:
        c.close()


def body(check, name, d, out, port):
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
    panel.send("ambient 40")            # the temperature around the device
    text = cli(d, "power show", None, 2.5, ctrl_c=True)
    temp, die = value("BatTemp"), value("ChgTemp")
    check("the panel's ambient: the battery at 40 °C, the charger's die 45",
          temp is not None and abs(temp - 40) < 0.5 and die is not None and
          abs(die - 45) < 0.6, f"{temp} {die}")
    panel.send("ambient 25")
    # the five INA4230s: with no Linux, only the control board's 3.3 V is up
    text = cli(d, "power consumption", None, 3, ctrl_c=True)

    def rail(name):
        m = re.findall(rf"{name}\s*\):\s*Bus Voltage: ([\d.]+) V,\s*Shunt "
                       rf"Voltage: [-\d.]+ mV,\s*Current: ([-\d.]+) A", text)
        return (float(m[-1][0]), float(m[-1][1])) if m else None
    control, big = rail("VCC3V3_CONTROL"), rail("VDD0V75_CPU_BIG_S0")
    check("power consumption: five INA4230s answer",
          len(set(re.findall(r"INA4230 (\d)", text))) == 5 and
          "Failed to initialize" not in text, text[-300:])
    check("... the control board's 3.3 V at 60 mA",
          control is not None and abs(control[0] - 3.3) < 0.01 and
          abs(control[1] - 0.06) < 0.002, str(control))
    check("... the RK3576's rails off without it", big == (0.0, 0.0),
          str(big))

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

    # ------------------------------------------ a headset in the jack
    panel.s.sendall(b"headset 1\n")
    check("a headset in the jack: the firmware sees it, and its mic",
          wait_log(d, r"Headphones status changed: 00000011", 5))
    time.sleep(0.3)
    panel.s.sendall(b"headset volup\n")
    check("... its volume up button (B)",
          wait_log(d, r"Headphones status changed: 00001011", 3))
    panel.s.sendall(b"headset 1\n")
    time.sleep(0.5)
    panel.s.sendall(b"headset 0\n")
    check("... and its going", wait_log(d, r"(?s)00001011.*"
                                         r"Headphones status changed: 00000000",
                                         3))

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

    # ------------------------------------------- the RPC: the screen, keys
    # (the CLI's `rpc`, as a computer's app speaks it; over the UART here,
    # a frame each 3.5 s or so at its 115200 baud)
    try:
        rpc = fo_rpc.Rpc(d / "cli.sock")
        rpc.start_screen()
        first = rpc.frame(15)
        check("the RPC (the CLI's `rpc`): the screen streamed, as it shows",
              first is not None and first[:2] == (FRAME_W, H) and
              shows_gui(first[2], panel.screen()), first and first[:2])
        # keys, judged on the screen itself (the stream's frames of them come
        # seconds later here), in the desktop's Power tab: its menu holds
        # still, where the desktop's sleeping face moves. Down, Up and Back
        # through the RPC; each one should change the screen at once
        body = lambda: panel.screen()[16 * LCD_W:]     # (not the status bar)

        def settled(timeout=6, still=1.0):
            """the screen once it has held still for @still seconds"""
            end = time.time() + timeout
            a, since = body(), time.time()
            while time.time() < end:
                time.sleep(0.1)
                if body() != a:
                    a, since = body(), time.time()
                elif time.time() - since >= still:
                    return a
            return None

        def key(name):
            before = settled()
            rpc.press(name)
            return before is not None and until(lambda: body() != before, 2)
        desk = body()
        panel.tap("power")
        until(lambda: body() != desk, 3)
        keys = {k: key(k) for k in ("down", "up", "back")}
        check("... and keys through it: Down, Up and Back in the Power tab",
              all(keys.values()), str(keys))
        rpc.stop_screen()
        rpc.close()
        # (a frame on its way still ends before the CLI's prompt is back)
        cli(d, "", r"control>:\s*$", 15)
    except (OSError, TimeoutError, ValueError) as e:
        check("the RPC (the CLI's `rpc`)", False, str(e))

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
    # (to the prompt after it: a port closed with output on its way leaves
    # the firmware's USB CLI mute until the MCU restarts, FW-05)
    text = cli(d, "help", r"Available commands:(.|\n)*gpio(.|\n)*control>:",
               10, usb=True)
    check("the CLI answers on its USB serial port",
          "Available commands" in text, text[-200:])
    try:
        text = usbip_cli(port, "uptime")
    except (OSError, ValueError) as e:
        text = f"({e})"
    check("over USB/IP, the PC's side: the CLI answers", "Uptime:" in text,
          text[-200:])
    panel.s.sendall(b"usb 0\n")
    return panel


SPARE = 0x10700000      # a sector of FW A's partition far past the image


def update(check, d, port, panel):
    """the firmware's update path: `power boot` into the boot ROM's USB boot,
    PICOBOOT over USB/IP as picotool drives it, a reboot into the firmware"""
    sys.path.insert(0, str(ROOT / "scripts"))
    import fo_picoboot

    def boot_roms():
        return (d / "qemu.out").read_text(errors="replace").count(
            '"RP2350 Boot"')
    seen, starts = boot_roms(), log_text(d).count("Startup complete")
    cli(d, "power boot", None, 0.5)
    panel.s.sendall(b"usb 2\n")
    if not check("power boot: a computer enumerates the boot ROM's USB boot "
                 "(2E8A:000F)", until(lambda: boot_roms() > seen, 15)):
        return
    uf2 = json.loads((d / "firmware.json").read_text())["uf2"]
    try:
        pb = fo_picoboot.Picoboot(port)
    except OSError as e:
        check("PICOBOOT: its interface, over USB/IP", False, str(e))
        return
    try:
        pb.exclusive()
        pb.exit_xip()
        flash = (d / "flash.bin").read_bytes()
        check("PICOBOOT: a read of the flash is what it holds",
              pb.read(0x10000000, 4096) == flash[:4096])
        pb.erase(SPARE, 4096)
        check("... a sector erased reads 0xFF",
              pb.read(SPARE, 4096) == b"\xff" * 4096)
        pattern = bytes((i * 7 + 3) & 0xff for i in range(256))
        pb.write(SPARE + 256, pattern)
        check("... a page written reads back", pb.read(SPARE, 768) ==
              b"\xff" * 256 + pattern + b"\xff" * 256)
        t = time.time()
        n = fo_picoboot.load(pb, uf2, progress=None)
        blocks = sorted(fo_picoboot.uf2_blocks(uf2))
        back = all(pb.read(a, len(page)) == page
                   for a, page in blocks[::len(blocks) // 7])
        check(f"the device's firmware loaded through it ({n} pages, "
              f"{time.time() - t:.1f} s), and reads back", back)
        pb.reboot()
    except OSError as e:
        check("PICOBOOT", False, str(e))
        return
    finally:
        pb.close()
    check("a reboot into it: the firmware starts again", until(
        lambda: log_text(d).count("Startup complete") > starts, 20, 0.2))
    panel.s.sendall(b"usb 0\n")


def power(check, d, panel):
    """the charger's ship FET, as the firmware's CLI works it"""
    def starts():
        return log_text(d).count("Startup complete")

    def went_off_and_on(n):
        return False in panel.power[n:] and panel.power[-1]

    n, told = starts(), len(panel.power)
    cli(d, "power reboot", None, 0.5)
    check("power reboot: the board's supply goes and comes (the charger's "
          "system power reset)", until(lambda: went_off_and_on(told), 5))
    check("... and the firmware starts again",
          until(lambda: starts() > n, 15, 0.2))

    n, told = starts(), len(panel.power)
    cli(d, "power ship", None, 0.5)
    check("power ship: the board is off (ship mode)", until(
        lambda: panel.power[told:] and not panel.power[-1], 5))
    time.sleep(1)
    check("... and stays off", starts() == n and not panel.power[-1])
    panel.s.sendall(b"usb 1\n")
    check("a charger plugged in wakes it: the firmware starts again",
          until(lambda: starts() > n, 15, 0.2))
    panel.s.sendall(b"usb 0\n")


def log_checks(check, d):
    """the log, at the end: no crash, no error but the known ones"""
    text = log_text(d)
    crash = CRASH.search(text)
    check("no crash in the log", not crash, crash and crash.group(0))
    lines = text.splitlines()
    if lines and not text.endswith(("\n", "\r")):
        lines.pop()             # still being written: the firmware has just
                                # started again, and its CPU app asks for its
                                # slot then ("... but sel")
    errors = [i for i, line in enumerate(lines)
              if "[E]" in line and not KNOWN_ERRORS.search(line)]
    check("no error in the log", not errors,
          "; ".join(lines[i] for i in errors[:3]))
    if errors:                  # what came before the first: its story
        print("\n".join("        " + re.sub(r"\x1b\[[0-9;]*m", "", x)
                        for x in lines[max(0, errors[0] - 12):errors[0] + 1]))


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
