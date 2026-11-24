#!/usr/bin/env python3
# Copyright (c) 2026, Kara Zajac
# SPDX-License-Identifier: BSD-3-Clause
"""The Flipper One emulator's window: the device, drawn, live.

    ./flipperoneemu gui [-n NAME]          (or scripts/fo_gui.py)

It shows a Flipper One of this project's own drawing around the emulated
screen, and takes its buttons and touchpad from the mouse and the keyboard:
arrows and Enter the pad, Backspace or Esc Back, F1-F5 the soft keys (Esc,
View, Power, Edit, Run), Tab App Switcher, Space PTT; drag on the touchpad;
Ctrl+U plugs a USB charger in, or pulls it out. It talks to the device over
its front-panel link (docs/PANEL_LINK.md), and starts the device with the
launcher when it is not running.

FlipperOneEmu is not made by Flipper Devices, and this drawing is not theirs.
"""
import argparse
import os
import pathlib
import subprocess
import sys

from PySide6 import QtCore, QtGui, QtWidgets
from PySide6.QtCore import Qt

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from fo_link import PanelLink                   # noqa: E402
from fo_view import FlipperOneView              # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
LAUNCHER = ROOT / "flipperoneemu"


class MainWindow(QtWidgets.QMainWindow):
    def __init__(self, name, zoom=2, start=True):
        super().__init__()
        self.name = name
        self.dir = ROOT / "devices" / name
        self.state = "connecting"
        self.setWindowTitle(f"{name}: Flipper One (FlipperOneEmu)")
        self.view = FlipperOneView(name, zoom)
        holder = QtWidgets.QWidget()
        lay = QtWidgets.QHBoxLayout(holder)
        lay.setContentsMargins(12, 12, 12, 12)
        lay.addWidget(self.view, 0, Qt.AlignCenter)
        self.setCentralWidget(holder)
        self.status = QtWidgets.QLabel()
        self.statusBar().addWidget(self.status, 1)

        self.link = PanelLink(self.dir / "panel.sock", parent=self)
        self.link.hello.connect(self._hello)
        self.link.lost.connect(self._lost)
        self.link.busy.connect(lambda: self._say("busy: another program has "
                                                 "the panel"))
        self.link.rows.connect(self.view.set_rows)
        self.link.lcd.connect(self._lcd)
        self.link.backlight.connect(self.view.set_backlight)
        self.link.leds.connect(self.view.set_leds)
        self.link.haptic.connect(self.view.set_haptic)
        self.link.usb.connect(self._usb)
        self.link.stopped.connect(self._stopped)
        self.view.key.connect(self._key)
        self.view.touch.connect(self._touch)
        self.lcd_on = False
        self.usb = False

        self._menus()
        self._lost()
        self.link.start()
        if start and not (self.dir / "panel.sock").exists():
            self.start()

    def _menus(self):
        device = self.menuBar().addMenu("&Device")
        self._action(device, "&Start", self.start)
        self._action(device, "S&top", self.stop)
        self._action(device, "&Restart", self.restart)
        device.addSeparator()
        self.usb_action = self._action(device, "&USB charger plugged in",
                                       self._plug, "Ctrl+U")
        self.usb_action.setCheckable(True)
        device.addSeparator()
        self._action(device, "Save the &screen as a picture…", self.save_screen,
                     "Ctrl+S")
        device.addSeparator()
        self._action(device, "&Quit", self.close, "Ctrl+Q")
        view = self.menuBar().addMenu("&View")
        group = QtGui.QActionGroup(self)
        for z in (1, 2, 3):
            a = self._action(view, f"Zoom {z}×", lambda z=z: self.view.set_zoom(z),
                             f"Ctrl+{z}")
            a.setCheckable(True)
            a.setChecked(z == self.view.zoom)
            group.addAction(a)
        help_ = self.menuBar().addMenu("&Help")
        self._action(help_, "Keys and buttons", self._keys_help)
        self._action(help_, "About FlipperOneEmu", self._about)

    def _action(self, menu, text, slot, shortcut=None):
        a = menu.addAction(text)
        a.triggered.connect(slot)
        if shortcut:
            a.setShortcut(shortcut)
        return a

    # ---- the device, through the launcher

    def _launch(self, *args):
        return subprocess.run([sys.executable, str(LAUNCHER), *args, "-n",
                               self.name], capture_output=True, text=True,
                              stdin=subprocess.DEVNULL)

    def start(self):
        r = self._launch("run", "--detach")
        if r.returncode:
            self._say((r.stderr or r.stdout).strip().splitlines()[-1]
                      if (r.stderr or r.stdout).strip() else "could not start")
        else:
            self._say("starting")

    def stop(self):
        self._launch("stop")

    def restart(self):
        self.stop()
        QtCore.QTimer.singleShot(600, self.start)

    # ---- what the link says

    def _say(self, text):
        self.status.setText(text)

    def _hello(self):
        self.state = "running"
        self._say(self._running())
        self._hint()

    def _running(self):
        return "running" + (" · a USB charger plugged in" if self.usb else "")

    def _plug(self, plugged):
        self.link.set_usb(plugged)
        self.usb_action.setChecked(self.usb)    # until the device says so

    def _usb(self, plugged):
        self.usb = plugged
        self.usb_action.setChecked(plugged)
        if self.state == "running":
            self._say(self._running())

    def _lost(self):
        self.state = "off"
        self.view.release_all()
        self.view.set_lcd(False)
        self.view.set_haptic(False)
        self._say("not running: Device > Start")
        self._hint()

    def _lcd(self, on):
        self.lcd_on = on
        self.view.set_lcd(on)
        self._hint()

    def _stopped(self, why):
        if why:
            self._say(f"stands still ({why})")
        elif self.state == "running":
            self._say(self._running())

    def _hint(self):
        if self.state != "running":
            self.view.set_hint("not running")
        elif not self.lcd_on:
            self.view.set_hint("the screen is off", "(the firmware has not",
                               "switched it on)")
        else:
            self.view.set_hint()

    def _key(self, name, down):
        (self.link.press if down else self.link.release)(name)

    def _touch(self, x, y, down):
        if down:
            self.link.touch(x, y)
        else:
            self.link.touch_end()

    # ---- the rest

    def save_screen(self):
        path, _ = QtWidgets.QFileDialog.getSaveFileName(
            self, "Save the screen", str(pathlib.Path.home() / "screen.png"),
            "PNG pictures (*.png)")
        if path:
            self.view.screen_image().scaled(
                256 * 4, 144 * 4, Qt.IgnoreAspectRatio,
                Qt.FastTransformation).save(path)

    def _keys_help(self):
        QtWidgets.QMessageBox.information(self, "Keys and buttons", (
            "Arrows: the pad · Enter: OK · Backspace or Esc: Back\n"
            "F1 Esc · F2 View · F3 Power · F4 Edit · F5 Run (the soft keys)\n"
            "Tab: App Switcher · Space: PTT\n"
            "Click the buttons; drag on the touchpad.\n"
            "Ctrl+U: a USB charger plugged in, or pulled out."))

    def _about(self):
        QtWidgets.QMessageBox.about(self, "About FlipperOneEmu", (
            "<p><b>FlipperOneEmu</b>: the Flipper One in QEMU, its MCU first."
            "</p><p>This window draws a Flipper One of its own around the "
            "emulator's screen; it is not Flipper Devices' artwork, and "
            "FlipperOneEmu is not made by Flipper Devices.</p>"
            "<p>BSD-3-Clause. github.com/KaraZajac/FlipperOneEmu</p>"))

    def closeEvent(self, e):
        self.view.release_all()
        self.link.close()
        super().closeEvent(e)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-n", "--name", default="one", help="the device's name")
    ap.add_argument("--zoom", type=int, default=2)
    ap.add_argument("--no-start", action="store_true",
                    help="do not start the device; wait for it")
    ap.add_argument("--shot", metavar="FILE.png",
                    help="save the window as a picture after --after seconds, "
                         "and quit (for tests; runs off-screen too)")
    ap.add_argument("--after", type=float, default=3.0)
    args = ap.parse_args()
    app = QtWidgets.QApplication(sys.argv[:1])
    app.setApplicationName("FlipperOneEmu")
    w = MainWindow(args.name, args.zoom, start=not args.no_start)
    w.show()
    if args.shot:
        def shoot():
            w.grab().save(args.shot)
            app.quit()
        QtCore.QTimer.singleShot(int(args.after * 1000), shoot)
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
