# Copyright (c) 2026, Kara Zajac
# SPDX-License-Identifier: BSD-3-Clause
"""A Flipper One, drawn: the screen in a body, with buttons to click.

FlipperOneView is the widget in the middle of the window (fo_gui.py). It shows
what the front-panel link reports (the screen, its backlight, the 17 LEDs; it
shakes while the vibration motor runs) and turns the mouse and the keyboard
into the buttons and the touchpad.

The body is fo_body's drawing, kept as a picture at the window's size and
drawn again only when a button, an LED or the zoom changes. The LCD goes on
top of it, scaled so that an LCD pixel is a whole number of the screen's.
"""
from PySide6 import QtCore, QtGui, QtWidgets
from PySide6.QtCore import QPointF, Qt
from PySide6.QtGui import QColor, QPainter

import fo_body as body
from fo_body import LCD, LCD_H, LCD_W, UNIT, VIEW, lcd_colours

KEYMAP = {
    Qt.Key_Up: "up", Qt.Key_Down: "down", Qt.Key_Left: "left",
    Qt.Key_Right: "right", Qt.Key_Return: "ok", Qt.Key_Enter: "ok",
    Qt.Key_Backspace: "back", Qt.Key_Escape: "back",
    Qt.Key_F1: "esc", Qt.Key_F2: "view", Qt.Key_F3: "power",
    Qt.Key_F4: "edit", Qt.Key_F5: "run", Qt.Key_Tab: "switch",
    Qt.Key_Space: "ptt",
}


class FlipperOneView(QtWidgets.QWidget):
    key = QtCore.Signal(str, bool)      # a button went down (True) or up
    touch = QtCore.Signal(float, float, bool)   # 0..1000 each; down/moving

    def __init__(self, name, zoom=2, parent=None):
        super().__init__(parent)
        self.name = name
        self.zoom = zoom
        self.pixels = bytearray(LCD_W * LCD_H)  # 0 black .. 255 white
        self.lcd_on = False
        self.backlight = 0
        self.hint = ()                  # lines to show on the screen
        self.down = {}                  # key -> who holds it ("mouse", "key")
        self.mouse_key = None
        self.st = body.State(name)
        self.body = None                # the body, drawn at this size
        self.cache = None               # ... with the buttons and lights
        self.colours = lcd_colours(0)
        self.shake = 0                  # the motor runs: 1..4, where it is
        self.shaker = QtCore.QTimer(self)
        self.shaker.setInterval(33)
        self.shaker.timeout.connect(self._shake)
        self.setFocusPolicy(Qt.StrongFocus)
        self.setMouseTracking(True)
        self.set_zoom(zoom)

    @property
    def leds(self):
        return self.st.leds

    @property
    def touching(self):
        return self.st.touching

    # ---- what the window tells it

    def set_zoom(self, zoom):
        self.zoom = max(1, int(zoom))
        self.k = self.zoom / UNIT
        self.setFixedSize(round(VIEW.width() * self.k),
                          round(VIEW.height() * self.k))
        self.body = None
        self._redraw()

    def set_rows(self, y, n, data):
        self.pixels[y * LCD_W:(y + n) * LCD_W] = data
        self.update(self._lcd_rect())

    def set_lcd(self, on):
        self.lcd_on = on
        self.update(self._lcd_rect())

    def set_backlight(self, level):
        self.backlight = level
        self.colours = lcd_colours(level)
        self.update(self._lcd_rect())

    def set_leds(self, colours):
        colours = list(colours)
        if colours != self.st.leds:
            self.st.leds = colours
            self._redraw()

    def set_haptic(self, on):
        """The vibration motor: the drawing shakes while it runs."""
        if on and not self.shaker.isActive():
            self.shaker.start()
        elif not on and self.shaker.isActive():
            self.shaker.stop()
            self.shake = 0
            self.update()

    def _shake(self):
        self.shake = self.shake % 4 + 1
        self.update()

    def set_hint(self, *lines):
        self.hint = tuple(lines)
        self.update(self._lcd_rect())

    def screen_image(self):
        """The screen as it is shown: 256 x 144, in the panel's colours."""
        img = QtGui.QImage(bytes(self.pixels), LCD_W, LCD_H, LCD_W,
                           QtGui.QImage.Format_Indexed8)
        img.setColorTable(self.colours if self.lcd_on
                          else [self.colours[255]] * 256)
        return img.copy()

    # ---- drawing

    def _redraw(self):
        self.cache = None
        self.update()

    def _to_view(self, p):
        return QPointF((p.x() - VIEW.x()) * self.k, (p.y() - VIEW.y()) * self.k)

    def _from_view(self, p):
        return QPointF(p.x() / self.k + VIEW.x(), p.y() / self.k + VIEW.y())

    def _lcd_rect(self):
        tl = self._to_view(LCD.topLeft())
        return QtCore.QRect(round(tl.x()), round(tl.y()),
                            LCD_W * self.zoom, LCD_H * self.zoom)

    def _picture(self, under, draw):
        """A picture at the widget's size: @under, then what @draw draws."""
        ratio = self.devicePixelRatioF()
        pix = QtGui.QPixmap(round(self.width() * ratio),
                            round(self.height() * ratio))
        pix.setDevicePixelRatio(ratio)
        pix.fill(Qt.transparent)
        p = QPainter(pix)
        if under is not None:
            p.drawPixmap(0, 0, under)
        p.setRenderHint(QPainter.Antialiasing)
        p.scale(self.k, self.k)
        p.translate(-VIEW.x(), -VIEW.y())
        draw(p)
        p.end()
        return pix

    def paintEvent(self, e):
        if self.body is None:
            self.body = self._picture(None, lambda p: body.draw_body(p, self.name))
        if self.cache is None:
            self.st.down = set(self.down)
            self.cache = self._picture(self.body,
                                       lambda p: body.draw_controls(p, self.st))
        p = QPainter(self)
        if self.shake:                  # a few of the drawing's units about
            dx, dy = ((5, 0), (0, 4), (-5, 0), (0, -4))[self.shake - 1]
            p.translate(dx * self.k, dy * self.k)
        p.drawPixmap(0, 0, self.cache)
        target = self._lcd_rect()
        p.setRenderHint(QPainter.SmoothPixmapTransform, False)
        p.drawImage(target, self.screen_image())
        if self.hint:
            f = QtGui.QFont()
            f.setFamilies(body.MONO)
            f.setPixelSize(max(10, 7 * self.zoom))
            p.setFont(f)
            p.setPen(QColor("#6a3a16"))
            p.drawText(QtCore.QRectF(target), Qt.AlignCenter, "\n".join(self.hint))

    # ---- the mouse and the keyboard

    def _hit(self, pos):
        return body.hit(self._from_view(pos))

    def _press(self, name, who):
        if name not in self.down:
            self.down[name] = who
            self.key.emit(name, True)
            self._redraw()

    def _release(self, name, who):
        if self.down.get(name) == who:
            del self.down[name]
            self.key.emit(name, False)
            self._redraw()

    def mousePressEvent(self, e):
        if e.button() != Qt.LeftButton:
            return
        name = self._hit(e.position())
        if name == "touchpad":
            self.st.touching = True
            self.touch.emit(*body.touch_at(self._from_view(e.position())), True)
            self._redraw()
        elif name:
            self.mouse_key = name
            self._press(name, "mouse")

    def mouseMoveEvent(self, e):
        if self.st.touching:
            self.touch.emit(*body.touch_at(self._from_view(e.position())), True)
            return
        hover = self._hit(e.position())
        if hover != self.st.hover:
            self.st.hover = hover
            self.setCursor(Qt.PointingHandCursor if hover else Qt.ArrowCursor)
            self._redraw()

    def leaveEvent(self, e):
        if self.st.hover:
            self.st.hover = None
            self._redraw()
        super().leaveEvent(e)

    def mouseReleaseEvent(self, e):
        if e.button() != Qt.LeftButton:
            return
        if self.st.touching:
            self.st.touching = False
            self.touch.emit(0, 0, False)
            self._redraw()
        if self.mouse_key:
            self._release(self.mouse_key, "mouse")
            self.mouse_key = None

    def keyPressEvent(self, e):
        name = KEYMAP.get(e.key())
        if name and not e.isAutoRepeat():
            self._press(name, "key")
        elif not name:
            super().keyPressEvent(e)

    def keyReleaseEvent(self, e):
        name = KEYMAP.get(e.key())
        if name and not e.isAutoRepeat():
            self._release(name, "key")
        elif not name:
            super().keyReleaseEvent(e)

    def focusNextPrevChild(self, forward):
        return False                    # Tab is App Switcher, not focus

    def release_all(self):
        for name in list(self.down):
            del self.down[name]
            self.key.emit(name, False)
        self._redraw()
