# Copyright (c) 2026, Kara Zajac
# SPDX-License-Identifier: BSD-3-Clause
"""The window's end of the front-panel link (docs/PANEL_LINK.md).

The emulator serves devices/NAME/panel.sock; PanelLink connects to it (again
and again, as the emulator comes and goes), turns the lines it sends into Qt
signals, and sends the buttons and the touchpad back. FlipperZeroEmu's
fe_gui_link.py, for the Flipper One's messages.
"""
import base64
import os

from PySide6 import QtCore, QtNetwork

KEYS = ("esc", "view", "power", "edit", "run", "up", "down", "left", "right",
        "ok", "back", "switch", "ptt")
LCD_W, LCD_H, LEDS = 256, 144, 17


class PanelLink(QtCore.QObject):
    hello = QtCore.Signal()             # the emulator is there, and talks to us
    lost = QtCore.Signal()              # ... and now it is gone
    busy = QtCore.Signal()              # connected, but another window has it
    rows = QtCore.Signal(int, int, bytes)   # first row, rows, 256 bytes each
    lcd = QtCore.Signal(bool)           # the controller's display is on
    backlight = QtCore.Signal(int)      # 0..255
    leds = QtCore.Signal(list)          # 17 x 0xRRGGBB
    haptic = QtCore.Signal(bool)        # the vibration motor runs
    usb = QtCore.Signal(int)            # in the USB-C port: 0 nothing,
                                        # 1 a charger, 2 a computer
    headset = QtCore.Signal(str)        # in the jack: "0", "1", or the
                                        # button held ("play", "volup", ...)
    ambient = QtCore.Signal(float)      # around the device, in °C
    power = QtCore.Signal(bool)         # the board has its supply (the
                                        # charger's ship FET closed)
    stopped = QtCore.Signal(str)        # time stands still ("paused", "debug"
                                        # ...: why), or goes on ("")

    RETRY_MS = 250
    PATIENCE_MS = 2000                  # no hello for this long: someone else's

    def __init__(self, path, who="FlipperOneEmu window", parent=None):
        super().__init__(parent)
        self.path = os.path.abspath(path)
        self.who = who
        self.up = False                 # the emulator said hello
        self.buf = bytearray()
        self.sock = QtNetwork.QLocalSocket(self)
        self.sock.connected.connect(self._connected)
        self.sock.disconnected.connect(self._disconnected)
        self.sock.readyRead.connect(self._read)
        self.retry = QtCore.QTimer(self)
        self.retry.setInterval(self.RETRY_MS)
        self.retry.timeout.connect(self._try)
        self.patience = QtCore.QTimer(self)
        self.patience.setSingleShot(True)
        self.patience.setInterval(self.PATIENCE_MS)
        self.patience.timeout.connect(self.busy)

    # ---- the connection

    def start(self):
        self.retry.start()
        self._try()

    def close(self):
        self.retry.stop()
        self.patience.stop()
        self.sock.abort()

    def connected(self):
        return self.sock.state() == QtNetwork.QLocalSocket.ConnectedState

    def _try(self):
        if (self.sock.state() == QtNetwork.QLocalSocket.UnconnectedState
                and os.path.exists(self.path)):
            self.sock.connectToServer(self.path)

    def _connected(self):
        self.send("hello " + self.who)
        self.patience.start()

    def _disconnected(self):
        self._read()
        self.buf.clear()
        self.patience.stop()
        self.up = False
        self.lost.emit()

    def send(self, line):
        if self.connected():
            self.sock.write(line.encode() + b"\n")

    # ---- to the emulator

    def press(self, key):
        if self.up and key in KEYS:
            self.send("press " + key)

    def release(self, key):
        if self.up and key in KEYS:
            self.send("release " + key)

    def touch(self, x, y, pressure=128):
        """x, y: 0..1000 across the touchpad"""
        if self.up:
            self.send(f"touch {int(x)} {int(y)} {int(pressure)}")

    def touch_end(self):
        if self.up:
            self.send("touch end")

    def refresh(self):
        if self.up:
            self.send("refresh")

    def set_headset(self, what):
        """the jack: 0 empty, 1 a headset, or its button held: play, volup,
        voldown or assist"""
        if self.up:
            self.send(f"headset {what}")

    def set_usb(self, what):
        """into the USB-C port: 0 nothing, 1 a charger, 2 a computer"""
        if self.up:
            self.send(f"usb {int(what)}")

    def set_ambient(self, celsius):
        """the temperature around the device: -40 to 125 °C"""
        if self.up:
            self.send(f"ambient {celsius:g}")

    # ---- from the emulator

    def _read(self):
        self.buf += self.sock.readAll().data()
        while True:
            end = self.buf.find(b"\n")
            if end < 0:
                break
            line = bytes(self.buf[:end])
            del self.buf[:end + 1]
            try:
                self._line(line.decode("ascii", "replace"))
            except (ValueError, IndexError):
                pass                    # not a line of ours: unknown ones pass

    def _line(self, line):
        word, _, rest = line.partition(" ")
        if word == "rows":
            y, n, data = rest.split(" ", 2)
            pixels = base64.b64decode(data)
            if len(pixels) == int(n) * LCD_W:
                self.rows.emit(int(y), int(n), pixels)
        elif word == "lcd":
            self.lcd.emit(rest.strip() == "on")
        elif word == "backlight":
            self.backlight.emit(int(rest))
        elif word == "leds":
            colours = [int(x, 16) for x in rest.split()]
            if len(colours) == LEDS:
                self.leds.emit(colours)
        elif word == "haptic":
            self.haptic.emit(int(rest) != 0)
        elif word == "usb":
            self.usb.emit(int(rest))
        elif word == "headset":
            self.headset.emit(rest.strip())
        elif word == "ambient":
            self.ambient.emit(float(rest))
        elif word == "power":
            self.power.emit(rest.strip() == "on")
        elif word == "running":
            on, _, why = rest.partition(" ")
            self.stopped.emit("" if int(on) else why.strip() or "paused")
        elif word == "hello" and rest.startswith("flipperoneemu panel"):
            self.patience.stop()
            self.up = True
            self.hello.emit()
