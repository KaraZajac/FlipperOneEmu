# Copyright (c) 2026, Kara Zajac
# SPDX-License-Identifier: BSD-3-Clause
"""A Flipper One, drawn: the screen in a body, with buttons to click.

FlipperOneView is the widget in the middle of the window (fo_gui.py). It shows
what the front-panel link reports (the screen, its backlight, the 17 LEDs) and
turns the mouse and the keyboard into the buttons and the touchpad.

The drawing is this project's own, laid out as the device is: a wedge at the
left with the touchpad and the network lights, the screen in the middle with
five soft keys under it, the round pad, App Switcher and Back at the right, a
power meter down the right edge, a loop for a lanyard. It is in the units of a
2000 x 1125 picture of the device (one LCD pixel is 685/256 of them) and is
scaled so that an LCD pixel is a whole number of the screen's.
"""
import math

from PySide6 import QtCore, QtGui, QtWidgets
from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QPainter, QPainterPath, QPen

LCD_W, LCD_H = 256, 144
UNIT = 685.0 / LCD_W                    # drawing units in an LCD pixel
VIEW = QRectF(0, 140, 1980, 840)        # the part of the drawing shown

# ---- where everything is, in drawing units

LCD = QRectF(655, 318, 685, 685 * LCD_H / LCD_W)
BEZEL = LCD.adjusted(-27, -26, 27, 26)
CENTER = QRectF(565, 165, 865, 793)
TOPBAR = QRectF(565, 165, 865, 88)
LEFT_WING = ((160, 388), (332, 388), (332, 840), (160, 840), (16, 614))
WING_FACE = ((186, 420), (332, 420), (332, 808), (186, 808), (64, 614))
LEFT_BLOCK = ((300, 182), (612, 182), (612, 942), (300, 942), (168, 842),
              (168, 386))
RIGHT_BLOCK = ((1392, 168), (1636, 168), (1958, 472), (1958, 956),
               (1392, 956))
LOOP = QRectF(1736, 158, 214, 290)
BACK_BLOCK = QRectF(1572, 724, 386, 232)
METER_FROM, METER_TO = QPointF(1664, 252), QPointF(1852, 532)

SOFT_KEYS = ("esc", "view", "power", "edit", "run")
SOFT_X = (724, 860, 996, 1132, 1268)
SOFT_Y, SOFT_W, SOFT_H = 752, 122, 58
PAD, PAD_R, OK_R = QPointF(1586, 518), 152.0, 50.0
BACK, BACK_R = QPointF(1756, 732), 50.0
SWITCH = QRectF(1418, 662, 134, 90)
PTT = QRectF(398, 168, 148, 20)
TOUCH = QRectF(248, 490, 312, 225)
STATUS = (("ETH 0", 309, 3), ("ETH 1", 343, 2), ("WI-FI", 377, 1),
          ("LINK", 411, 0))             # label, y, LED (Eth1, Eth2, WiFi, Net)
POWER_LED = 4
# down the meter: battery watts 4..1, the battery (outline 5, 6; centre 16),
# USB watts 1..4, the USB plug (charging)
METER = (10, 9, 8, 7, "battery", 12, 13, 14, 15, 11)

ORANGE, ORANGE_LIGHT, ORANGE_DARK = "#f47b27", "#ff9a4f", "#b8561a"
BODY, BODY_LIGHT, BODY_DARK = "#36373a", "#46474b", "#242527"
INK, PAPER_OFF, PAPER_ON = "#1c0e05", "#3b2818", "#ff9d52"
LABEL = "#c8c8c4"

KEYMAP = {
    Qt.Key_Up: "up", Qt.Key_Down: "down", Qt.Key_Left: "left",
    Qt.Key_Right: "right", Qt.Key_Return: "ok", Qt.Key_Enter: "ok",
    Qt.Key_Backspace: "back", Qt.Key_Escape: "back",
    Qt.Key_F1: "esc", Qt.Key_F2: "view", Qt.Key_F3: "power",
    Qt.Key_F4: "edit", Qt.Key_F5: "run", Qt.Key_Tab: "switch",
    Qt.Key_Space: "ptt",
}


def rounded(points, r):
    """A closed outline through @points, each corner rounded by @r."""
    path = QPainterPath()
    pts = [QPointF(*p) for p in points]
    n = len(pts)
    for i in range(n):
        prev, cur, nxt = pts[i - 1], pts[i], pts[(i + 1) % n]
        a, b = prev - cur, nxt - cur
        la, lb = math.hypot(a.x(), a.y()), math.hypot(b.x(), b.y())
        ra, rb = min(r, la / 2), min(r, lb / 2)
        p1 = cur + a * (ra / la)
        p2 = cur + b * (rb / lb)
        if i == 0:
            path.moveTo(p1)
        else:
            path.lineTo(p1)
        path.quadTo(cur, p2)
    path.closeSubpath()
    return path


def lightness(level):
    """How lit the backlight looks, 0 to 1, at a PWM level of 0 to 255.

    The LEDs' light is the duty, but the eye sees lightness (CIE L*), not
    light: the firmware's default of 10 % looks about 38 % as bright as full.
    """
    y = max(0, min(255, level)) / 255.0
    return (116 * y ** (1 / 3) - 16) / 100 if y > 0.008856 else 9.033 * y


def mix(c1, c2, t):
    c1, c2 = QColor(c1), QColor(c2)
    return QColor(round(c1.red() + (c2.red() - c1.red()) * t),
                  round(c1.green() + (c2.green() - c1.green()) * t),
                  round(c1.blue() + (c2.blue() - c1.blue()) * t))


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
        self.leds = [0] * 17
        self.hint = ()                  # lines to show on the screen
        self.down = {}                  # key -> who holds it ("mouse", "key")
        self.mouse_key = None
        self.touching = False
        self.hover = None
        self._palette()
        self.setFocusPolicy(Qt.StrongFocus)
        self.setMouseTracking(True)
        self.set_zoom(zoom)

    # ---- what the window tells it

    def set_zoom(self, zoom):
        self.zoom = max(1, int(zoom))
        self.k = self.zoom / UNIT
        self.setFixedSize(round(VIEW.width() * self.k),
                          round(VIEW.height() * self.k))
        self.update()

    def set_rows(self, y, n, data):
        self.pixels[y * LCD_W:(y + n) * LCD_W] = data
        self.update()

    def set_lcd(self, on):
        self.lcd_on = on
        self.update()

    def set_backlight(self, level):
        self.backlight = level
        self._palette()
        self.update()

    def set_leds(self, colours):
        self.leds = list(colours)
        self.update()

    def set_hint(self, *lines):
        self.hint = tuple(lines)
        self.update()

    def _palette(self):
        paper = mix(PAPER_OFF, PAPER_ON, lightness(self.backlight))
        self.colours = [mix(INK, paper, v / 255.0).rgb() for v in range(256)]

    def screen_image(self):
        """The screen as it is shown: 256 x 144, in the panel's colours."""
        img = QtGui.QImage(bytes(self.pixels), LCD_W, LCD_H, LCD_W,
                           QtGui.QImage.Format_Indexed8)
        img.setColorTable(self.colours if self.lcd_on
                          else [self.colours[255]] * 256)
        return img.copy()

    # ---- drawing

    def _to_view(self, p):
        return QPointF((p.x() - VIEW.x()) * self.k, (p.y() - VIEW.y()) * self.k)

    def _from_view(self, p):
        return QPointF(p.x() / self.k + VIEW.x(), p.y() / self.k + VIEW.y())

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        p.fillRect(self.rect(), self.palette().window())
        p.save()
        p.scale(self.k, self.k)
        p.translate(-VIEW.x(), -VIEW.y())
        self._body(p)
        self._left(p)
        self._center(p)
        self._right(p)
        p.restore()
        self._screen(p)

    def _fill(self, p, path, top, bottom, edge=None):
        r = path.boundingRect()
        g = QtGui.QLinearGradient(r.topLeft(), r.bottomLeft())
        g.setColorAt(0, QColor(top))
        g.setColorAt(1, QColor(bottom))
        p.setPen(QPen(QColor(edge), 3) if edge else Qt.NoPen)
        p.setBrush(g)
        p.drawPath(path)

    def _body(self, p):
        # the loop for a lanyard, behind
        p.setPen(QPen(QColor("#505154"), 26))
        p.setBrush(Qt.NoBrush)
        p.drawRoundedRect(LOOP.adjusted(13, 13, -13, -13), 34, 34)
        self._fill(p, rounded(RIGHT_BLOCK, 44), BODY_LIGHT, BODY_DARK, "#1b1c1e")
        self._fill(p, rounded(LEFT_BLOCK, 40), BODY_LIGHT, BODY_DARK, "#1b1c1e")
        self._fill(p, rounded(LEFT_WING, 22), "#5a5b5f", "#3c3d40", "#202123")
        self._fill(p, rounded(WING_FACE, 16), "#4b4c50", "#36373a")
        path = QPainterPath()
        path.addRoundedRect(CENTER, 30, 30)
        self._fill(p, path, "#3f4043", "#2c2d30", "#1b1c1e")
        path = QPainterPath()
        path.addRoundedRect(TOPBAR, 24, 24)
        self._fill(p, path, "#55565a", "#3e3f42")

    def _label(self, p, rect, text, size=24, colour=LABEL, align=Qt.AlignCenter,
               bold=False):
        f = QtGui.QFont("DejaVu Sans Mono")
        f.setPixelSize(size)
        f.setBold(bold)
        f.setLetterSpacing(QtGui.QFont.AbsoluteSpacing, size * 0.08)
        p.setFont(f)
        p.setPen(QColor(colour))
        p.drawText(rect, align, text)

    def _led(self, index):
        c = self.leds[index] if 0 <= index < len(self.leds) else 0
        return QColor((c >> 16) & 255, (c >> 8) & 255, c & 255)

    def _lit(self, colour):
        return colour.red() + colour.green() + colour.blue() > 24

    def _left(self, p):
        # PTT, on the top edge
        p.setPen(Qt.NoPen)
        p.setBrush(QColor(ORANGE_DARK if "ptt" in self.down else ORANGE))
        p.drawRoundedRect(PTT, 8, 8)
        # the network lights, with their names
        for text, y, index in STATUS:
            led = self._led(index)
            on = self._lit(led)
            box = QRectF(456 - (y - 309) * 0.6, y - 7, 40, 14)
            p.setBrush(led if on else QColor("#151516"))
            p.setPen(QPen(QColor("#0e0e0f"), 2))
            p.drawRoundedRect(box, 6, 6)
            if on:
                glow = QtGui.QRadialGradient(box.center(), 40)
                glow.setColorAt(0, QColor(led.red(), led.green(), led.blue(), 120))
                glow.setColorAt(1, QColor(0, 0, 0, 0))
                p.setPen(Qt.NoPen)
                p.setBrush(glow)
                p.drawEllipse(box.center(), 40, 40)
            self._label(p, QRectF(box.right() + 12, y - 16, 100, 32), text, 24,
                        align=Qt.AlignLeft | Qt.AlignVCenter)
        # the touchpad: an orange rim, rings, a cross
        pad = QPainterPath()
        pad.addRoundedRect(TOUCH, TOUCH.height() / 2, TOUCH.height() / 2)
        p.setPen(QPen(QColor(ORANGE_LIGHT if self.touching else ORANGE), 12))
        p.setBrush(QColor("#1d1e20"))
        p.drawPath(pad)
        p.setPen(QPen(QColor("#2b2c2f"), 3))
        p.setBrush(Qt.NoBrush)
        for inset in range(18, 100, 14):
            r = TOUCH.adjusted(inset, inset * 0.72, -inset, -inset * 0.72)
            p.drawRoundedRect(r, r.height() / 2, r.height() / 2)
        p.setPen(QPen(QColor("#77787c"), 3))
        c = TOUCH.center()
        for a, b in (((-112, 0), (-36, 0)), ((36, 0), (112, 0)),
                     ((0, -84), (0, -26)), ((0, 26), (0, 84))):
            p.drawLine(c + QPointF(*a), c + QPointF(*b))
        # the speaker's slots
        p.setPen(QPen(QColor("#1a1b1d"), 9, Qt.SolidLine, Qt.RoundCap))
        for i in range(3):
            x = 462 + i * 24
            p.drawLine(QPointF(x, 900), QPointF(x + 16, 866))
        self._label(p, QRectF(380, 864, 60, 36), "◁))", 22, "#8d8e92")

    def _center(self, p):
        self._label(p, QRectF(CENTER.x(), 254, CENTER.width(), 36),
                    "PORTABLE LINUX COMPUTER", 24)
        p.setPen(Qt.NoPen)
        p.setBrush(QColor("#0c0c0d"))
        p.drawRoundedRect(BEZEL, 22, 22)
        # the soft keys, a line over each
        for name, x in zip(SOFT_KEYS, SOFT_X):
            r = QRectF(x - SOFT_W / 2, SOFT_Y, SOFT_W, SOFT_H)
            pressed = name in self.down
            p.setPen(Qt.NoPen)
            p.setBrush(QColor(ORANGE))
            p.drawRoundedRect(QRectF(x - 52, SOFT_Y - 20, 104, 5), 2, 2)
            p.setBrush(QColor("#1c1d1f" if pressed else "#292a2d"))
            p.setPen(QPen(QColor("#141516"), 3))
            p.drawRoundedRect(r, 12, 12)
            if self.hover == name and not pressed:
                p.setPen(QPen(QColor("#5c5d61"), 2))
                p.setBrush(Qt.NoBrush)
                p.drawRoundedRect(r.adjusted(3, 3, -3, -3), 10, 10)
            colour = QColor("#dcdcdc")
            if name == "power":
                led = self._led(POWER_LED)
                colour = led if self._lit(led) else QColor("#6fc35a")
            self._icon(p, name, r.center(), colour)
        # the device's name, where the maker's would be
        f = QtGui.QFont("DejaVu Sans")
        f.setPixelSize(70)
        f.setBold(True)
        f.setItalic(True)
        f.setLetterSpacing(QtGui.QFont.AbsoluteSpacing, 10)
        p.setFont(f)
        p.setPen(QColor("#1d1e20"))
        p.drawText(QRectF(CENTER.x(), 846, CENTER.width(), 96), Qt.AlignCenter,
                   self.name.upper())

    def _icon(self, p, name, c, colour):
        p.setPen(QPen(colour, 5, Qt.SolidLine, Qt.RoundCap, Qt.RoundJoin))
        p.setBrush(Qt.NoBrush)
        s = 14
        if name == "esc":
            p.drawLine(c + QPointF(-s, -s), c + QPointF(s, s))
            p.drawLine(c + QPointF(-s, s), c + QPointF(s, -s))
        elif name == "view":
            for dy in (-10, 0, 10):
                p.drawLine(c + QPointF(-s, dy), c + QPointF(s, dy))
        elif name == "power":
            p.drawArc(QRectF(c.x() - 15, c.y() - 13, 30, 30), 120 * 16, 300 * 16)
            p.drawLine(c + QPointF(0, -18), c + QPointF(0, 0))
        elif name == "edit":
            for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                corner = c + QPointF(sx * s, sy * 11)
                p.drawLine(corner, corner + QPointF(-sx * 8, 0))
                p.drawLine(corner, corner + QPointF(0, -sy * 7))
        elif name == "run":
            path = QPainterPath(c + QPointF(-s, 0))
            path.lineTo(c + QPointF(-4, 11))
            path.lineTo(c + QPointF(s + 2, -12))
            p.drawPath(path)

    def _right(self, p):
        # the power meter, down the right edge
        d = METER_TO - METER_FROM
        angle = math.degrees(math.atan2(d.y(), d.x()))
        mid = (METER_FROM + METER_TO) / 2
        length = math.hypot(d.x(), d.y())
        p.save()
        p.translate(mid)
        p.rotate(angle)
        p.setPen(Qt.NoPen)
        p.setBrush(QColor("#1f2022"))
        p.drawRoundedRect(QRectF(-length / 2 - 30, -32, length + 60, 64), 28, 28)
        step = length / (len(METER) - 1)
        for i, what in enumerate(METER):
            x = -length / 2 + i * step
            if what == "battery":
                outline = self._led(5)
                centre = self._led(16)
                p.setPen(QPen(outline if self._lit(outline) else QColor("#3a3b3e"), 4))
                p.setBrush(Qt.NoBrush)
                p.drawRoundedRect(QRectF(x - 13, -18, 26, 36), 5, 5)
                p.setPen(Qt.NoPen)
                p.setBrush(centre if self._lit(centre) else QColor("#2b2c2f"))
                p.drawRect(QRectF(x - 6, -10, 12, 20))
                continue
            led = self._led(what)
            p.setBrush(led if self._lit(led) else QColor("#2e2f32"))
            if what == 11:                          # the plug
                p.drawRoundedRect(QRectF(x - 9, -12, 18, 24), 4, 4)
            elif what in (12, 13, 14, 15):          # chevrons
                path = QPainterPath(QPointF(x - 6, -14))
                path.lineTo(QPointF(x + 6, 0))
                path.lineTo(QPointF(x - 6, 14))
                p.setPen(QPen(p.brush().color(), 5, Qt.SolidLine, Qt.RoundCap))
                p.setBrush(Qt.NoBrush)
                p.drawPath(path)
                p.setPen(Qt.NoPen)
            else:                                   # battery watts: bars
                p.drawRoundedRect(QRectF(x - 5, -14, 10, 28), 4, 4)
        p.restore()
        # MIC
        p.setPen(QPen(QColor("#151617"), 6, Qt.SolidLine, Qt.RoundCap))
        for dy in (-8, 4):
            p.drawLine(QPointF(1462, 286 + dy), QPointF(1494, 286 + dy))
        self._label(p, QRectF(1504, 268, 90, 40), "🎙MIC", 22)
        # the pad
        g = QtGui.QRadialGradient(PAD - QPointF(30, 40), PAD_R * 1.3)
        g.setColorAt(0, QColor(ORANGE_LIGHT))
        g.setColorAt(1, QColor(ORANGE))
        p.setPen(QPen(QColor(ORANGE_DARK), 3))
        p.setBrush(g)
        p.drawEllipse(PAD, PAD_R, PAD_R)
        p.setPen(QPen(QColor(ORANGE_DARK), 4, Qt.SolidLine, Qt.RoundCap))
        for name, (dx, dy) in (("up", (0, -1)), ("down", (0, 1)),
                               ("left", (-1, 0)), ("right", (1, 0))):
            if name in self.down:
                shade = QtGui.QRadialGradient(PAD + QPointF(dx, dy) * 100, 70)
                shade.setColorAt(0, QColor(0, 0, 0, 70))
                shade.setColorAt(1, QColor(0, 0, 0, 0))
                p.save()
                p.setPen(Qt.NoPen)
                p.setBrush(shade)
                p.drawEllipse(PAD + QPointF(dx, dy) * 100, 70, 70)
                p.restore()
            p.drawLine(PAD + QPointF(dx, dy) * 92, PAD + QPointF(dx, dy) * 130)
            p.drawLine(PAD + QPointF(dx, dy) * 66 + QPointF(dy, dx) * 0,
                       PAD + QPointF(dx, dy) * 66)
        p.setPen(QPen(QColor(ORANGE_DARK), 3))
        p.setBrush(QColor(ORANGE_DARK if "ok" in self.down else ORANGE_LIGHT))
        p.drawEllipse(PAD, OK_R, OK_R)
        # App Switcher, and the dark part that Back is in
        p.setPen(Qt.NoPen)
        p.setBrush(QColor("#202123"))
        p.drawRoundedRect(BACK_BLOCK, 34, 34)
        p.setBrush(QColor("#1a1b1d" if "switch" in self.down else "#26272a"))
        p.setPen(QPen(QColor("#151617"), 3))
        p.drawRoundedRect(SWITCH, 18, 18)
        p.setPen(QPen(QColor(ORANGE), 5))
        p.setBrush(Qt.NoBrush)
        c = SWITCH.center()
        p.drawRoundedRect(QRectF(c.x() - 30, c.y() - 20, 40, 28), 5, 5)
        p.drawRoundedRect(QRectF(c.x() - 18, c.y() - 8, 40, 28), 5, 5)
        # Back
        p.setPen(QPen(QColor(ORANGE_DARK), 3))
        p.setBrush(QColor(ORANGE_DARK if "back" in self.down else ORANGE))
        p.drawEllipse(BACK, BACK_R, BACK_R)
        p.setPen(QPen(QColor("#7a3610"), 6, Qt.SolidLine, Qt.RoundCap,
                      Qt.RoundJoin))
        p.setBrush(Qt.NoBrush)
        p.drawArc(QRectF(BACK.x() - 22, BACK.y() - 18, 44, 40), -90 * 16, 180 * 16)
        p.drawLine(BACK + QPointF(0, 22), BACK + QPointF(-14, 22))
        p.drawLine(BACK + QPointF(0, -18), BACK + QPointF(-14, -18))
        p.drawLine(BACK + QPointF(-14, -18), BACK + QPointF(-6, -26))
        p.drawLine(BACK + QPointF(-14, -18), BACK + QPointF(-6, -10))

    def _screen(self, p):
        """The LCD, a whole number of the screen's pixels to each of its own."""
        tl = self._to_view(LCD.topLeft())
        target = QtCore.QRect(round(tl.x()), round(tl.y()),
                              LCD_W * self.zoom, LCD_H * self.zoom)
        p.setRenderHint(QPainter.SmoothPixmapTransform, False)
        p.drawImage(target, self.screen_image())
        if self.hint:
            f = QtGui.QFont("DejaVu Sans Mono")
            f.setPixelSize(max(10, 7 * self.zoom))
            p.setFont(f)
            p.setPen(QColor("#6a3a16"))
            p.drawText(QtCore.QRectF(target), Qt.AlignCenter, "\n".join(self.hint))

    # ---- the mouse and the keyboard

    def _hit(self, pos):
        q = self._from_view(pos)
        for name, x in zip(SOFT_KEYS, SOFT_X):
            if QRectF(x - SOFT_W / 2, SOFT_Y, SOFT_W, SOFT_H).contains(q):
                return name
        d = q - PAD
        r = math.hypot(d.x(), d.y())
        if r <= OK_R:
            return "ok"
        if r <= PAD_R:
            if abs(d.x()) > abs(d.y()):
                return "right" if d.x() > 0 else "left"
            return "down" if d.y() > 0 else "up"
        d = q - BACK
        if math.hypot(d.x(), d.y()) <= BACK_R:
            return "back"
        if SWITCH.contains(q):
            return "switch"
        if PTT.adjusted(-6, -10, 6, 14).contains(q):
            return "ptt"
        if TOUCH.contains(q):
            return "touchpad"
        return None

    def _touch_at(self, pos):
        q = self._from_view(pos)
        x = (q.x() - TOUCH.left()) / TOUCH.width() * 1000
        y = (q.y() - TOUCH.top()) / TOUCH.height() * 1000
        return min(max(x, 0), 1000), min(max(y, 0), 1000)

    def _press(self, name, who):
        if name not in self.down:
            self.down[name] = who
            self.key.emit(name, True)
            self.update()

    def _release(self, name, who):
        if self.down.get(name) == who:
            del self.down[name]
            self.key.emit(name, False)
            self.update()

    def mousePressEvent(self, e):
        if e.button() != Qt.LeftButton:
            return
        name = self._hit(e.position())
        if name == "touchpad":
            self.touching = True
            self.touch.emit(*self._touch_at(e.position()), True)
            self.update()
        elif name:
            self.mouse_key = name
            self._press(name, "mouse")

    def mouseMoveEvent(self, e):
        if self.touching:
            self.touch.emit(*self._touch_at(e.position()), True)
            return
        hover = self._hit(e.position())
        if hover != self.hover:
            self.hover = hover
            self.setCursor(Qt.PointingHandCursor if hover else Qt.ArrowCursor)
            self.update()

    def mouseReleaseEvent(self, e):
        if e.button() != Qt.LeftButton:
            return
        if self.touching:
            self.touching = False
            self.touch.emit(0, 0, False)
            self.update()
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
        self.update()
