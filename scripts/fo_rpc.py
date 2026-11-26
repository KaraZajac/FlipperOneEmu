#!/usr/bin/env python3
# Copyright (c) 2026, Kara Zajac
# SPDX-License-Identifier: BSD-3-Clause
"""The MCU firmware's RPC, as a computer reaches it: its screen streamed, its
buttons pressed.

    scripts/fo_rpc.py [-n NAME] [--usb] [--frames N] [--out DIR]
                      [--press BUTTON ...]

Over the firmware's CLI (UART1, devices/NAME/cli.sock; with --usb its USB
serial port, devices/NAME/usb.sock, with a computer plugged in) it types
`rpc` and then talks the firmware's protocol (flipperdevices/
flipperone-mcu-protobuf): each message after its length, a varint. It asks
for the screen (StartVirtualDisplayRequest) and saves the frames it gets as
PNGs (256x144, 8-bit grey), presses buttons (a ButtonEvent each of PRESS,
SHORT and RELEASE: the firmware derives nothing from an injected event), and
closes the session (RpcSessionCloseRequest). BUTTON: ok, back, key1, key2,
power, key4, key5, sw, down, right, left, up, ptt. Over the UART (115200
baud) a frame takes about 3.5 s, and keys sent while frames go can be late
to arrive; --usb is the computer's own way. A frame drawn while one is on
its way is lost (the firmware's FW-02, docs/FIRMWARE_BUGS.md): --frames
waits for the screen's next change. Standard library only.
"""
import argparse
import pathlib
import socket
import struct
import sys
import time
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUTTONS = ["ok", "back", "key1", "key2", "power", "key4", "key5", "sw", "down",
           "right", "left", "up", "ptt"]
PRESS, RELEASE, SHORT = 0, 1, 2
READY = 0xFD                            # the firmware's "RPC mode" marker


def varint(n):
    out = b""
    while True:
        b = n & 0x7f
        n >>= 7
        if n:
            out += bytes([b | 0x80])
        else:
            return out + bytes([b])


def field(num, kind, payload):
    """a field: varint (kind 0, payload an int) or bytes (kind 2)"""
    if kind == 0:
        return varint(num << 3) + varint(payload)
    return varint(num << 3 | 2) + varint(len(payload)) + payload


def fields(data):
    """{number: value} of a message: ints for varints, bytes for the rest"""
    out, i = {}, 0
    while i < len(data):
        key, i = read_varint(data, i)
        num, kind = key >> 3, key & 7
        if kind == 0:
            out[num], i = read_varint(data, i)
        elif kind == 2:
            n, i = read_varint(data, i)
            out[num], i = data[i:i + n], i + n
        elif kind == 5:
            out[num], i = data[i:i + 4], i + 4
        elif kind == 1:
            out[num], i = data[i:i + 8], i + 8
        else:
            raise ValueError(f"wire type {kind}")
    return out


def read_varint(data, i):
    n = shift = 0
    while True:
        b = data[i]
        i += 1
        n |= (b & 0x7f) << shift
        shift += 7
        if not b & 0x80:
            return n, i


def png(path, width, height, grey):
    raw = b"".join(b"\0" + grey[y * width:(y + 1) * width]
                   for y in range(height))

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data)))
    pathlib.Path(path).write_bytes(
        b"\x89PNG\r\n\x1a\n" +
        chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)) +
        chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


class Rpc:
    """an RPC session over the firmware's CLI"""

    def __init__(self, path, timeout=10):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(str(path))
        self.buf = b""
        self.s.sendall(b"\r")
        time.sleep(0.5)
        self._drain()
        self.s.sendall(b"rpc\r")
        end = time.time() + timeout
        while READY not in self.buf:
            if time.time() > end:
                raise TimeoutError("no RPC session: " +
                                   self.buf[-200:].decode(errors="replace"))
            self._recv(end - time.time())
        self.buf = self.buf[self.buf.index(READY) + 1:]

    def _drain(self):
        self.s.settimeout(0.2)
        try:
            while self.s.recv(65536):
                pass
        except socket.timeout:
            pass

    def _recv(self, timeout):
        self.s.settimeout(max(timeout, 0.01))
        try:
            chunk = self.s.recv(65536)
        except socket.timeout:
            return
        if not chunk:
            raise EOFError("the emulator has gone")
        self.buf += chunk

    def send(self, num, payload=b""):
        msg = field(num, 2, payload)
        self.s.sendall(varint(len(msg)) + msg)

    def message(self, timeout):
        """the next message's fields, or None"""
        end = time.time() + timeout
        while True:
            try:
                n, i = read_varint(self.buf, 0)
                if len(self.buf) >= i + n:
                    msg, self.buf = self.buf[i:i + n], self.buf[i + n:]
                    return fields(msg)
            except IndexError:
                pass
            if time.time() > end:
                return None
            self._recv(end - time.time())

    def frame(self, timeout=10):
        """the next frame: (width, height, its grey pixels), or None"""
        end = time.time() + timeout
        while time.time() < end:
            msg = self.message(end - time.time())
            if msg is None:
                return None
            if 1 in msg:
                f = fields(msg[1])
                w, h, enc = f.get(1, 0), f.get(2, 0), f.get(3, 0)
                if enc != 0:
                    raise ValueError(f"encoding {enc}: only PLAIN is read")
                return w, h, f.get(5, b"")
        return None

    def start_screen(self):
        self.send(4)                    # StartVirtualDisplayRequest

    def stop_screen(self):
        self.send(5)

    def press(self, button, hold=0.15):
        """PRESS, then after @hold SHORT and RELEASE, as a key pressed"""
        b = BUTTONS.index(button)
        self.send(2, field(1, 0, b) + field(2, 0, PRESS))
        time.sleep(hold)
        for action in (SHORT, RELEASE):
            self.send(2, field(1, 0, b) + field(2, 0, action))

    def close(self):
        self.send(6)                    # RpcSessionCloseRequest
        time.sleep(0.3)
        self.s.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("-n", "--name", default="one", help="the device's name")
    ap.add_argument("--usb", action="store_true",
                    help="over its USB serial port (a computer plugged in)")
    ap.add_argument("--frames", type=int, default=1,
                    help="frames to save (default 1)")
    ap.add_argument("--out", default=".", help="where the PNGs go")
    ap.add_argument("--press", nargs="*", default=[], choices=BUTTONS,
                    metavar="BUTTON", help="buttons to press, after the "
                    "first frame")
    args = ap.parse_args()
    d = ROOT / "devices" / args.name
    rpc = Rpc(d / ("usb.sock" if args.usb else "cli.sock"))
    rpc.start_screen()
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    saved = 0
    try:
        while saved < args.frames:
            got = rpc.frame(10)
            if got is None:
                print("no frame in 10 s", file=sys.stderr)
                return 1
            w, h, pixels = got
            path = out / f"rpc-{saved:03d}.png"
            png(path, w, h, pixels)
            print(f"{path}: {w}x{h}", flush=True)
            saved += 1
            if saved == 1:
                for b in args.press:
                    rpc.press(b)
                    time.sleep(0.5)
    finally:
        rpc.stop_screen()
        rpc.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
