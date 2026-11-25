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
  the INA219 on VSYS, the five INA4230s; its root is the microSD card, and
  its UFS is a 64 GB disk; USB0 is in its device role with its Type-C
  port, USB1's root hubs are there, and the battery reads (its chain waits
  on USB0's role switch); each Ethernet port has its RTL8211F, and with
  passt installed port 1 is on this PC's network (`run --net`): a link,
  an address by DHCP, a gateway that answers;
  the CPU clusters' clocks come from TF-A's SCMI server, and cpufreq has
  both clusters; the six thermal zones read the TSADC, with the trims the
  OTP holds;
- a computer plugged into the USB-C port (the panel's `usb 2`) reaches
  USB0, and enumerates Flipper OS's gadget (NCM, MTP, a serial port), which
  Linux sees configured at high speed; a line goes each way over its
  serial port (ttyGS0, the host's chardev at the other end); unplugged,
  Linux sees the computer go;
- Flipper OS's sound card (the NAU8822 codec on SAI2) plays a tone of 2 s
  at 1 kHz, and the codec gives it out: the device's audio.wav, from QEMU's
  wav backend (`run --audio`), has 2 s of 1 kHz;
- `systemctl reboot` starts the RK3576 again, and the kernel with it;
  `systemctl poweroff` switches it off (PSCI), after which the MCU cuts
  its rail; the MCU's CPU app switches it on again, and the kernel starts;
- no oops, panic or RCU stall in the kernel's log, and no crash in the
  MCU's.

The getty on the USB gadget's serial port is masked on the kernel's command
line (systemd.mask=), for Flipper OS's OS-02 (docs/FIRMWARE_BUGS.md): with
no computer on the gadget it can keep the console's getty from starting
(and the check uses that serial port itself).

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
import struct
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


def copy_card(src, dst):
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", str(src),
                    str(dst)], check=True)


def checks(check, d, out, first_boot, off, net):
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
    ufs = con.run("lsblk -bdno SIZE /dev/sda; "
                  "cat /sys/class/scsi_host/host*/proc_name")
    check("its UFS: a 64 GB disk (sda) on the UFS host",
          bool(ufs) and ufs[0].strip() == "64000000000" and
          any(u.strip() == "ufshcd" for u in ufs[1:]), str(ufs))
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
    eth = con.run("for p in end0 end1; do echo $p "
                  "$(cat /sys/class/net/$p/phydev/phy_id) "
                  "$(cat /sys/class/net/$p/carrier 2>/dev/null || echo -); "
                  "done")
    want = [f"end0 0x001cc916 {1 if net else 0}", "end1 0x001cc916 0"]
    check("the Ethernet ports: an RTL8211F on each, "
          + ("port 1 with a link" if net else "no link (no network given)"),
          eth is not None and [e.strip() for e in eth] == want, str(eth))
    if net:
        ping = con.run("ip -4 -br addr show end0; ping -c 2 -W 3 "
                       "$(ip route | awk '/default/{print $3; exit}') "
                       ">/dev/null && echo pinged", 60)
        check("... an address by DHCP, and the gateway answers (passt)",
              bool(ping) and len(ping[0].split()) >= 3 and
              ping[-1].strip() == "pinged", str(ping))
    freq = con.run("cat /sys/devices/system/cpu/cpufreq/policy0/"
                   "scaling_cur_freq /sys/devices/system/cpu/cpufreq/"
                   "policy4/scaling_cur_freq")
    check("the clusters' clocks over SCMI: cpufreq has both",
          bool(freq) and len(freq) == 2 and
          all(f.strip().isdigit() for f in freq), str(freq))
    temps = con.run("for z in /sys/class/thermal/thermal_zone*; do "
                    "case $(cat $z/type) in *-thermal) cat $z/temp;; esac; "
                    "done")                     # (not the battery's zone)
    check("six temperatures from the TSADC (its trims from the OTP)",
          bool(temps) and len(temps) == 6 and
          all(t.strip().isdigit() and 25000 <= int(t) < 85000
              for t in temps), str(temps))

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
    net = shutil.which("passt") is not None     # (QEMU's network backend)
    print(f"{'a first boot' if first_boot else 'from the kept card'}, "
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
    run = [sys.executable, str(st.LAUNCHER), "run", "-n", name, "--detach",
           "--linux", "--kernel-args",
           "systemd.mask=serial-getty@flipserial0.service",
           "--audio", str(d / "audio.wav")] + \
        (["--net"] if net else [])
    if subprocess.run(run).returncode:
        sys.exit("the device did not start")
    def off():
        if first_boot and not check.failed:
            TEMPLATE.mkdir(parents=True, exist_ok=True)
            kept.unlink(missing_ok=True)
            copy_card(d / "sd.img", kept)

    try:
        checks(check, d, out, first_boot, off, net)
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
