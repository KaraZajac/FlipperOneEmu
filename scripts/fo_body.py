# Copyright (c) 2026, Kara Zajac
# SPDX-License-Identifier: BSD-3-Clause
"""The Flipper One's front, drawn: every part but the LCD's picture.

This project's own vector drawing of the device, part by part where the
device has it: measured from a 2000 x 1125 render of its front, whose units
it is in (an LCD pixel is 677/256 of them). draw(p, state) paints it, the
LEDs and the buttons as @state has them; fo_view.py scales it and puts the
LCD on top. The geometry here is also what the mouse hits.

The parts, back to front: the loop for a lanyard; the right wing with the
pad, App Switcher, the MIC and the black bumpers (Back on the lower one, the
power meter in the upper one's slot); the left wing with its shoulders, the
nose, the network lights and the touchpad; PTT on the top edge; the speaker;
the screen's module in the middle, its cap, the glass, the five soft keys
and the chin with the device's name where the maker's would be.
"""
import math
import random

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import (QBrush, QColor, QFont, QImage, QLinearGradient,
                           QPainter, QPainterPath, QPen, QRadialGradient,
                           QTransform)

LCD_W, LCD_H = 256, 144
UNIT = 677.0 / LCD_W                    # drawing units in an LCD pixel
VIEW = QRectF(0, 145, 1966, 835)        # the part of the drawing shown

# ---------------------------------------------------------------- geometry
# Points are (x, y) or (x, y, the corner's radius).

LCD = QRectF(658, 324, 677, 677 * LCD_H / LCD_W)
GLASS = QRectF(645, 306, 703, 412)              # black, around the LCD
RIM = QRectF(636, 301, 721, 425)

MODULE = ((571, 166, 12), (1428, 166, 12), (1428, 234, 12), (1384, 289, 14),
          (1384, 753, 14), (1430, 810, 12), (1430, 958, 14), (565, 958, 14),
          (565, 810, 12), (614, 753, 14), (614, 289, 14), (571, 234, 12))
CAP_Y, CHIN_Y = 236, 840                # the cap's lower edge, the chin's top

LEFT_UPPER = ((640, 185), (312, 185, 30), (168, 385), (110, 466),
              (110, 640), (640, 640))
LEFT_LOWER = ((640, 600), (110, 600), (110, 770), (183, 845),
              (248, 937, 36), (640, 937))
NOSE = ((144, 388, 8), (170, 390, 10), (300, 469, 4), (300, 739, 4),
        (175, 839, 10), (146, 841, 8), (-8, 604, 64))
NOSE_RIDGE = 587                        # its upper half lit, the lower not
# the upper shoulder's flat top, above the crease from its corner; the lower
# shoulder's band in shade, from the recess's corner down to the bottom
TOP_FACE = ((336, 185), (640, 185), (640, 240), (478, 240), (462, 228))
CREASE = ((357, 808), (403, 808), (300, 937), (232, 937))
RECESS = ((640, 240), (474, 240, 18), (336, 466, 6), (336, 744, 6),
          (372, 806, 16), (640, 808))
STATUS_PANEL = ((468, 285, 10), (590, 285, 10), (590, 441, 10),
                (360, 441, 10))
PLATE = QRectF(294, 470, 294, 267)
TOUCH = QRectF(246, 494, 313, 221)              # the ring's outside
CROSS = QPointF(403, 600)
PTT = ((404, 171, 6), (537, 171, 6), (546, 187, 2), (396, 187, 2))
GRILLE = ((387, 861, 4), (527, 861, 4), (497, 901, 4), (358, 901, 4))
STATUS = (("ETH 0", 465, 309, 3), ("ETH 1", 442, 342, 2),
          ("WI-FI", 418, 378, 1))               # label, the slit's x, y, LED
LINK_BOX, LINK_LED, LINK_INDEX = QRectF(386, 395, 101, 31), QPointF(415, 410.5), 0

RIGHT_WING = ((1370, 185), (1640, 185), (1640, 300), (1951, 560),
              (1951, 937), (1370, 937))
BUMPER_TOP = ((1600, 165, 22), (1722, 165, 28), (1951, 486, 42),
              (1951, 560, 8), (1700, 560), (1600, 420))
BUMPER_LOW = ((1571, 729, 12), (1951, 729, 12), (1951, 957, 46),
              (1555, 957, 6), (1555, 934, 4), (1571, 926, 4))
LOOP = ((1738, 163, 40), (1950, 163, 40), (1950, 453, 28), (1738, 453, 0))
LOOP_IN = QRectF(1768, 208, 129, 238)           # its opening
WING_TOP = ((1384, 185), (1640, 185), (1640, 236), (1384, 236))
# the bumper's light: a soft ridge inside its chamfer (the render's
# highlight is there, not on the edge), brightening down to its corner,
# whose face is lit most
BUMPER_CORNER = ((1858, 446), (1990, 446), (1990, 600), (1910, 600),
                 (1910, 545, 24), (1894, 500, 24))
SIDE = QRectF(1879, 566, 72, 158)               # the recess down the side
RAIL = ((1883, 571, 3), (1917, 571, 13), (1917, 721, 13), (1883, 721, 3))
SHELF = (805, 823)                              # the right wing's step

PAD, PAD_R, OK_R, SOCKET_R = QPointF(1585, 514), 151.5, 49.5, 177.0
SOCKET = QPointF(1585, 518)                     # the pad's seat: a bit lower
SWITCH_CUT = 192.5                              # App Switcher's arc, round it
BACK, BACK_R = QPointF(1753, 729), 52.0
SWITCH = QRectF(1415, 657, 136, 91)
MIC = QRectF(1446, 262, 152, 50)

SOFT_KEYS = ("esc", "view", "power", "edit", "run")
SOFT_X = (724.5, 860.5, 996.5, 1132.5, 1268.5)
SOFT_TOP, SOFT_W, SOFT_H = 752.0, 120.0, 59.0
POWER_LED = 4

# the power meter, down its panel's middle (measured across and along the
# render's panel): from where, at what angle, then how far and what
METER_FROM = QPointF(1669.2, 268.9)
METER_ANGLE = 54.86
METER = ((-6, 10), (23.5, 9), (53, 8), (82.5, 7), (125, "battery"),
         (174.5, 12), (204, 13), (232.5, 14), (260.5, 15), (304, 11))
BATTERY_OUTLINE, BATTERY_CENTRE = (5, 6), 16
METER_MARK = "#5b5a5f"                  # a meter mark whose light is off
METER_PANEL = ((1635, 244, 9), (1680, 246, 6), (1880, 530, 6), (1880, 545, 4),
               (1834, 545, 6), (1636, 265, 6))

# ----------------------------------------------------------------- colours

ORANGE, ORANGE_LIGHT, ORANGE_DARK = "#fb8a3c", "#ffad70", "#c95c1c"
INK = ("#1a0d04", "#521f02")            # an LCD pixel's dark, unlit and lit
PAPER = ("#3b2818", "#f7994c")          # ... and its light
LABEL = "#d2d0d5"
MONO = ["Share Tech Mono", "JetBrains Mono", "DejaVu Sans Mono", "monospace"]
SANS = ["DejaVu Sans", "Liberation Sans", "Arial", "sans-serif"]


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


def lcd_colours(backlight):
    """The LCD's 256 levels (0 dark ... 255 light) as RGB, at a backlight."""
    t = lightness(backlight)
    ink, paper = mix(INK[0], INK[1], t), mix(PAPER[0], PAPER[1], t)
    return [mix(ink, paper, v / 255.0).rgb() for v in range(256)]


# ----------------------------------------------------------------- helpers

def poly(points, r=0.0):
    """A closed outline through @points, each corner rounded."""
    pts = [(QPointF(pt[0], pt[1]), pt[2] if len(pt) > 2 else r)
           for pt in points]
    path = QPainterPath()
    n = len(pts)
    for i in range(n):
        cur, rad = pts[i]
        prev, nxt = pts[i - 1][0], pts[(i + 1) % n][0]
        a, b = prev - cur, nxt - cur
        la, lb = math.hypot(a.x(), a.y()), math.hypot(b.x(), b.y())
        ra, rb = min(rad, la / 2), min(rad, lb / 2)
        p1, p2 = cur + a * (ra / la), cur + b * (rb / lb)
        if i == 0:
            path.moveTo(p1)
        else:
            path.lineTo(p1)
        if ra > 0:
            path.cubicTo(p1 + (cur - p1) * 0.55, p2 + (cur - p2) * 0.55, p2)
    path.closeSubpath()
    return path


def rrect(r, radius):
    path = QPainterPath()
    path.addRoundedRect(r, radius, radius)
    return path


def circle(c, radius):
    path = QPainterPath()
    path.addEllipse(c, radius, radius)
    return path


def rgba(colour, alpha):
    c = QColor(colour)
    c.setAlpha(alpha)
    return c


def vgrad(top, bottom, *stops):
    """A vertical gradient from y @top to y @bottom: (at, colour)..."""
    g = QLinearGradient(0, top, 0, bottom)
    for at, colour in stops:
        g.setColorAt(at, QColor(colour))
    return QBrush(g)


def lgrad(p0, p1, *stops):
    g = QLinearGradient(QPointF(*p0), QPointF(*p1))
    for at, colour in stops:
        g.setColorAt(at, QColor(colour))
    return QBrush(g)


def fill(p, path, brush, pen=None):
    p.setPen(pen if pen is not None else Qt.NoPen)
    p.setBrush(brush)
    p.drawPath(path)


def edges(p, path, width, top, bottom, diagonal=False, soft=False):
    """Shade @path's edges on the inside: @top where they face up (or up and
    left), @bottom where they face down, as light and shadow on a bevel; a
    @soft bevel fades in from its inner side."""
    r = path.boundingRect()
    top, bottom = QColor(top), QColor(bottom)
    p.save()
    p.setClipPath(path, Qt.IntersectClip)
    p.setBrush(Qt.NoBrush)
    steps = (1.0, 0.8, 0.6, 0.4, 0.2) if soft else (1.0,)
    for k in steps:
        if diagonal:
            g = QLinearGradient(r.topLeft(), r.bottomRight())
        else:
            g = QLinearGradient(0, r.top(), 0, r.bottom())
        share = 1 / len(steps)
        g.setColorAt(0, rgba(top, round(top.alpha() * share)))
        g.setColorAt(0.5, rgba(top, 0))
        g.setColorAt(0.5001, rgba(bottom, 0))
        g.setColorAt(1, rgba(bottom, round(bottom.alpha() * share)))
        p.setPen(QPen(QBrush(g), width * 2 * k, Qt.SolidLine, Qt.RoundCap,
                      Qt.RoundJoin))
        p.drawPath(path)
    p.restore()


_noise = None


def grain():
    """A brush of fine light and dark specks: the textured plastic."""
    global _noise
    if _noise is None:
        rnd = random.Random(1)
        size = 160
        img = QImage(size, size, QImage.Format_ARGB32_Premultiplied)
        img.fill(0)
        for y in range(size):
            for x in range(size):
                v = rnd.random()
                if v < 0.24:
                    a = rnd.randint(10, 30)
                    img.setPixel(x, y, (a << 24) | (a << 16) | (a << 8) | a)
                elif v < 0.50:
                    a = rnd.randint(14, 46)
                    img.setPixel(x, y, a << 24)
        _noise = QBrush(img)
        _noise.setTransform(QTransform.fromScale(1.7, 1.7))
    return _noise


def textured(p, path, brush):
    fill(p, path, brush)
    fill(p, path, grain())


def font(families, px, bold=False, spacing=0.0):
    f = QFont()
    f.setFamilies(families)
    f.setPixelSize(round(px))
    f.setBold(bold)
    if spacing:
        f.setLetterSpacing(QFont.AbsoluteSpacing, spacing)
    f.setHintingPreference(QFont.PreferNoHinting)
    return f


def label(p, x, y, text, px, colour=LABEL, align=Qt.AlignLeft, spacing=1.0,
          families=MONO, bold=False):
    """@text with its middle at @y; @x is its left (or middle, or right)."""
    p.setFont(font(families, px, bold, spacing))
    p.setPen(QColor(colour))
    w = 2000
    left = {Qt.AlignLeft: x, Qt.AlignHCenter: x - w / 2,
            Qt.AlignRight: x - w}[align]
    p.drawText(QRectF(left, y - px, w, 2 * px), align | Qt.AlignVCenter, text)


def glow(p, c, colour, radius, alpha=150):
    g = QRadialGradient(c, radius)
    g.setColorAt(0, rgba(colour, alpha))
    g.setColorAt(0.45, rgba(colour, alpha // 3))
    g.setColorAt(1, rgba(colour, 0))
    p.setPen(Qt.NoPen)
    p.setBrush(QBrush(g))
    p.drawEllipse(c, radius, radius)


def rgb(value):
    return QColor((value >> 16) & 255, (value >> 8) & 255, value & 255)


def lit(colour):
    return colour.red() + colour.green() + colour.blue() > 24


def bright(colour):
    """An LED's colour as its die looks: toward white, its hue kept."""
    m = max(colour.red(), colour.green(), colour.blue(), 1)
    scaled = QColor(*(min(255, round(v * 255 / m)) for v in
                      (colour.red(), colour.green(), colour.blue())))
    return mix(scaled, "#ffffff", 0.35)


class State:
    """What the drawing shows besides its parts."""

    def __init__(self, name="one"):
        self.name = name
        self.down = set()               # the buttons held
        self.hover = None               # the button under the mouse
        self.leds = [0] * 17            # 0xRRGGBB each
        self.touching = False


# ----------------------------------------------------------------- drawing

def draw(p, st):
    """All of it: the body, then the buttons and the lights on it."""
    draw_body(p, st.name)
    draw_controls(p, st)


def draw_body(p, name):
    """What does not change while the device runs: drawn once a zoom."""
    p.setRenderHint(QPainter.Antialiasing)
    _shadow(p)
    _loop(p)
    _right_wing(p)
    _left_wing(p)
    _bumpers(p)
    _socket(p)
    _mic(p)
    _network(p)
    _plate(p)
    _nose(p)
    _speaker(p)
    _module(p)
    _glass(p)
    _tray(p)
    _name(p, name)


def draw_controls(p, st):
    """The buttons and the lights as @st has them, over the body: nothing
    of the body is drawn over any of them."""
    p.setRenderHint(QPainter.Antialiasing)
    _meter(p, st)
    _network_lights(p, st)
    _pad(p, st)
    _switch_and_back(p, st)
    _touchpad(p, st)
    _ptt(p, st)
    _soft_keys(p, st)


_outline = None


def outline():
    """The device's silhouette."""
    global _outline
    if _outline is None:
        path = poly(MODULE)
        for part in (poly(LEFT_UPPER), poly(LEFT_LOWER), poly(NOSE),
                     poly(RIGHT_WING), poly(BUMPER_TOP), poly(BUMPER_LOW)):
            path = path.united(part)
        _outline = path.simplified()
    return _outline


def _shadow(p):
    shape = outline().translated(0, 10)
    p.setBrush(Qt.NoBrush)
    for width, alpha in ((36, 3), (24, 4), (12, 6)):
        p.setPen(QPen(QColor(0, 0, 0, alpha), width, Qt.SolidLine,
                      Qt.RoundCap, Qt.RoundJoin))
        p.drawPath(shape)
    fill(p, shape, QColor(0, 0, 0, 30))


def _loop(p):
    """The loop for a lanyard: a metal bar seen a little from above, its top
    face lit with a bright line along it, its underside black; the right
    leg's inner face in shade behind a thin lit edge, a streak and a
    highlight down its face. Both legs go into the bumper."""
    outer, inner = poly(LOOP), rrect(LOOP_IN, 20)
    body = outer.subtracted(inner)
    textured(p, body, QColor("#4d4c50"))
    p.save()
    p.setClipPath(body)
    fill(p, outer.subtracted(outer.translated(0, 10)), vgrad(
        163, 174, (0, "#8c8a8f"), (1, "#7e7c81")))           # the top face
    fill(p, outer.subtracted(outer.translated(-12, 0)), lgrad(
        (1938, 0), (1952, 0), (0, "#6e6c71"), (0.4, "#4e4d51"),
        (1, "#7f7d82")))                                      # the outer side
    fill(p, inner.translated(0, -13).subtracted(inner),
         QColor("#0f0f10"))                                   # the underside
    fill(p, inner.translated(16, 0).subtracted(inner),
         lgrad((1897, 0), (1913, 0), (0, "#5c5b5f"), (0.2, "#2a2a2c"),
               (1, "#1d1d1f")))                               # the leg's inside
    p.setPen(QPen(QColor("#a3a1a6"), 1.6))
    p.drawLine(QPointF(1748, 173.5), QPointF(1922, 173.5))
    p.setPen(QPen(QColor("#616065"), 1.6))
    p.drawLine(QPointF(1768, 191), QPointF(1897, 191))
    p.setPen(QPen(QColor("#5f5e62"), 3.5))
    p.drawLine(QPointF(1922.5, 206), QPointF(1922.5, 450))
    p.setPen(QPen(QColor("#76747a"), 3))
    p.drawLine(QPointF(1940.5, 196), QPointF(1940.5, 450))
    p.restore()
    p.setPen(QPen(QColor(0, 0, 0, 120), 1.5))
    p.setBrush(Qt.NoBrush)
    p.drawPath(body)


def _right_wing(p):
    wing = poly(RIGHT_WING, 10)
    textured(p, wing, vgrad(185, 937, (0, "#424144"), (0.5, "#3a393c"),
                            (1, "#38373a")))
    # the step under App Switcher, and the face below it
    lower = QPainterPath()
    lower.addRect(QRectF(1370, SHELF[1], 220, 937 - SHELF[1]))
    lower = lower.intersected(wing)
    textured(p, lower, vgrad(SHELF[1], 937, (0, "#5a585c"), (1, "#4c4a4e")))
    p.fillRect(QRectF(1380, SHELF[0], 200, SHELF[1] - SHELF[0]),
               vgrad(SHELF[0], SHELF[1], (0, "#1c1c1e"), (1, "#2a292c")))
    p.save()
    p.setClipPath(wing)
    textured(p, poly(WING_TOP), vgrad(185, 236, (0, "#5f5d62"),
                                      (0.85, "#58565b"), (1, "#6a686d")))
    p.fillRect(QRectF(1384, 236, 260, 6), vgrad(236, 242, (0, rgba("#000000", 70)),
                                                (1, rgba("#000000", 0))))
    p.restore()
    edges(p, wing, 4, rgba("#8a888d", 160), rgba("#18171a", 200))
    # the recess down the right side, a rail in it
    p.fillRect(SIDE, QColor("#2b2a2d"))
    p.fillRect(QRectF(1931, SIDE.top(), 14, SIDE.height()),
               QColor("#0f0f10"))                  # a deep slot by the edge
    p.fillRect(QRectF(1945, SIDE.top(), 6, SIDE.height()), QColor("#38373a"))
    rail = poly(RAIL)
    fill(p, rail, lgrad((1883, 0), (1917, 0), (0, "#4a494d"),
                        (0.3, "#3e3d41"), (1, "#3a393c")))
    p.setPen(QPen(QColor("#8e8c92"), 2.2))
    p.drawLine(QPointF(1891, 576), QPointF(1891, 716))
    edges(p, rail, 2, rgba("#6a686d", 160), rgba("#0a0a0b", 200))


def _left_wing(p):
    """Its faces, flat, as the light finds them: the upper chamfer lit, the
    flat top above its crease less, the lower shoulder between, a band in
    shade down it, its bottom edge catching the light."""
    upper = poly(LEFT_UPPER)
    textured(p, upper, lgrad((200, 380), (420, 230), (0, "#8a888b"),
                             (1, "#8e8c8f")))
    top = poly(TOP_FACE)
    p.save()
    p.setClipPath(upper)
    textured(p, top, lgrad((350, 0), (640, 0), (0, "#5d5b5e"),
                           (1, "#57555a")))
    p.setPen(QPen(QColor(255, 255, 255, 40), 2))
    p.drawLine(QPointF(336, 186), QPointF(462, 229))
    p.restore()
    lower = poly(LEFT_LOWER)
    textured(p, lower, vgrad(640, 937, (0, "#605e63"), (0.85, "#646267"),
                             (1, "#6c6a6f")))
    p.save()
    p.setClipPath(lower)
    textured(p, poly(CREASE), QColor("#4d4b4e"))
    p.restore()
    wing = upper.united(lower)
    edges(p, wing, 4, rgba("#aeacb1", 130), rgba("#7a787d", 180))
    edges(p, wing, 1.5, rgba("#d0ced3", 90), rgba("#1a1a1c", 200))
    # the recess the network lights and the touchpad sit in
    recess = poly(RECESS)
    textured(p, recess, vgrad(240, 808, (0, "#3d3d40"), (1, "#353437")))
    edges(p, recess, 7, rgba("#0c0c0d", 230), rgba("#6a686d", 150))
    p.setPen(QPen(QColor("#121213"), 3))
    p.setBrush(Qt.NoBrush)
    p.drawPath(recess)


def _nose(p):
    nose = poly(NOSE)
    ridge = (NOSE_RIDGE - 386) / (843 - 386)
    fill(p, nose, vgrad(386, 843, (0, "#504e51"), (ridge, "#4d4b4e"),
                        (ridge + 0.0001, "#38383a"), (1, "#343436")))
    # the ridge through its tip: a fine light line on the upper face's edge
    p.save()
    p.setClipPath(nose)
    p.setPen(QPen(QColor(255, 255, 255, 34), 1.6))
    p.drawLine(QPointF(0, NOSE_RIDGE - 1), QPointF(300, NOSE_RIDGE - 1))
    p.restore()
    edges(p, nose, 16, rgba("#7a787e", 200), rgba("#141416", 220), True, True)
    edges(p, nose, 2.5, rgba("#a3a1a7", 200), rgba("#0c0c0d", 220), True)
    p.setPen(QPen(QColor(0, 0, 0, 120), 1.5))
    p.setBrush(Qt.NoBrush)
    p.drawPath(nose)


def _network(p):
    panel = poly(STATUS_PANEL)
    fill(p, panel, vgrad(285, 441, (0, "#323234"), (1, "#3a3a3c")))
    edges(p, panel, 5, rgba("#0a0a0b", 220), rgba("#6c6a70", 170))
    for text, x, y, index in STATUS:
        label(p, x + 49, y, text, 22.5, spacing=0.6)
    # LINK: a light label with a round light in it
    box = rrect(LINK_BOX, 6)
    fill(p, box, vgrad(LINK_BOX.top(), LINK_BOX.bottom(), (0, "#d6d4d9"),
                       (1, "#b3b1b6")))
    edges(p, box, 2, rgba("#ffffff", 160), rgba("#6e6c71", 200))
    label(p, 431, 410.5, "LINK", 22.5, "#202022", spacing=0.6)


def _network_lights(p, st):
    for text, x, y, index in STATUS:
        _slit(p, QRectF(x, y - 6.5, 37, 13), _led(st, index))
    led = _led(st, LINK_INDEX)
    if lit(led):
        glow(p, LINK_LED, led, 26)
        fill(p, circle(LINK_LED, 9), bright(led), QPen(mix(led, "#000000", 0.4), 1.5))
    else:
        fill(p, circle(LINK_LED, 9), vgrad(401, 420, (0, "#3a3436"),
                                            (1, "#5a5254")),
             QPen(QColor("#2a2627"), 1.5))


def _slit(p, r, led):
    path = rrect(r, 4.5)
    if lit(led):
        glow(p, r.center(), led, 34)
        fill(p, path, bright(led), QPen(mix(led, "#000000", 0.3), 1.2))
    else:
        fill(p, path, vgrad(r.top(), r.bottom(), (0, "#0c0c0d"),
                            (1, "#1c1c1e")))
        edges(p, path, 1.5, rgba("#000000", 255), rgba("#5c5a60", 200))


def _plate(p):
    """The touchpad's plate: the nose hides its left end."""
    plate = rrect(PLATE, 18)
    fill(p, plate.translated(0, 4), QColor(0, 0, 0, 70))
    fill(p, plate, vgrad(PLATE.top(), PLATE.bottom(), (0, "#2b2b2c"),
                         (1, "#232324")))
    edges(p, plate, 3, rgba("#5e5c61", 200), rgba("#0e0e0f", 220))


def _touchpad(p, st):
    ring = rrect(TOUCH, TOUCH.height() / 2)
    fill(p, ring.translated(0, 3), QColor(0, 0, 0, 90))
    inner_r = TOUCH.adjusted(10.5, 10.5, -10.5, -10.5)
    inner = rrect(inner_r, inner_r.height() / 2)
    hot = st.touching or st.hover == "touchpad"
    fill(p, ring, vgrad(TOUCH.top(), TOUCH.bottom(),
                        (0, "#ffa766" if hot else "#fb9447"),
                        (1, "#f58a40" if hot else "#e9792f")),
         QPen(QColor("#141414"), 2))
    edges(p, ring, 3, rgba("#ffd2a8", 200), rgba("#9a4a14", 200))
    fill(p, inner, vgrad(inner_r.top(), inner_r.bottom(), (0, "#262626"),
                         (1, "#2e2e2f")))
    # the contour lines, pressed into its surface
    p.setBrush(Qt.NoBrush)
    for i in range(1, 10):
        r = inner_r.adjusted(i * 9.0, i * 8.0, -i * 9.0, -i * 8.0)
        if r.height() <= 8:
            break
        p.setPen(QPen(QColor(0, 0, 0, 110), 1.6))
        p.drawRoundedRect(r, r.height() / 2, r.height() / 2)
        p.setPen(QPen(QColor(255, 255, 255, 16), 1.2))
        p.drawRoundedRect(r.translated(0, 1.4), r.height() / 2, r.height() / 2)
    edges(p, inner, 6, rgba("#000000", 200), rgba("#4a4a4c", 120))
    p.setPen(QPen(QColor("#a7a5ab"), 2.6, Qt.SolidLine, Qt.FlatCap))
    c = CROSS
    for a, b in (((-117, 0), (-67, 0)), ((67, 0), (116, 0)),
                 ((0, -82), (0, -33)), ((0, 33), (0, 82))):
        p.drawLine(c + QPointF(*a), c + QPointF(*b))


def _ptt(p, st):
    path = poly(PTT)
    down = "ptt" in st.down
    fill(p, path, vgrad(171, 187, (0, "#e0804c" if down else "#f6b08f"),
                        (0.5, "#e07a42" if down else "#ec9163"),
                        (1, "#c05a1e" if down else "#dc7232")),
         QPen(QColor("#7c3a12"), 1))
    if st.hover == "ptt":
        _hover(p, path)


def _speaker(p):
    plate = poly(GRILLE)
    fill(p, plate, vgrad(861, 901, (0, "#3a393d"), (1, "#47464b")),
         QPen(QColor("#151517"), 2))
    edges(p, plate, 3, rgba("#000000", 160), rgba("#8a888e", 170))
    pen = QPen(QColor("#bebcc2"), 2.4, Qt.SolidLine, Qt.RoundCap,
               Qt.RoundJoin)
    p.setPen(pen)
    p.setBrush(Qt.NoBrush)
    body = QPainterPath(QPointF(392, 876.5))
    for pt in ((398, 876.5), (406, 870), (406, 891), (398, 884.5),
               (392, 884.5)):
        body.lineTo(QPointF(*pt))
    body.closeSubpath()
    p.drawPath(body)
    for r in (6.0, 11.5):
        p.drawArc(QRectF(410 - r, 880.5 - r, 2 * r, 2 * r), -50 * 16, 100 * 16)
    p.setPen(QPen(QColor("#0d0d0e"), 7.5, Qt.SolidLine, Qt.FlatCap))
    for x in (433, 462, 491):
        p.drawLine(QPointF(x, 897), QPointF(x + 24, 865))


def _module(p):
    shape = poly(MODULE)
    p.save()
    p.setClipPath(shape)
    p.fillRect(QRectF(560, 160, 880, CAP_Y - 160),
               vgrad(166, CAP_Y, (0, "#626065"), (0.68, "#57555a"),
                     (0.7, "#47454a"), (0.74, "#5a585d"), (0.88, "#747277"),
                     (1, "#545257")))
    p.fillRect(QRectF(560, CAP_Y, 880, CHIN_Y - CAP_Y),
               vgrad(CAP_Y, CHIN_Y, (0, "#4b4a4e"), (0.5, "#3f3e42"),
                     (1, "#323134")))
    p.fillRect(QRectF(560, CHIN_Y, 880, 960 - CHIN_Y),
               lgrad((565, 0), (1430, 0), (0, "#59575b"), (0.5, "#4b4a4e"),
                     (1, "#454447")))
    # the cap's shadow on the face, the chin's edge
    p.fillRect(QRectF(560, CAP_Y, 880, 14),
               vgrad(CAP_Y, CAP_Y + 14, (0, rgba("#000000", 90)),
                     (1, rgba("#000000", 0))))
    p.fillRect(QRectF(560, CHIN_Y - 2, 880, 4),
               vgrad(CHIN_Y - 2, CHIN_Y + 2, (0, rgba("#000000", 60)),
                     (1, rgba("#9a989d", 70))))
    p.restore()
    edges(p, shape, 5, rgba("#a9a7ad", 210), rgba("#141416", 220))
    p.setPen(QPen(QColor(0, 0, 0, 150), 1.5))
    p.setBrush(Qt.NoBrush)
    p.drawPath(shape)


def _glass(p):
    rim = rrect(RIM, 15)
    fill(p, rim, vgrad(RIM.top(), RIM.bottom(), (0, "#5a5456"),
                       (1, "#3c3638")))
    edges(p, rim, 3, rgba("#a8a2a4", 220), rgba("#1a1617", 220))
    glass = rrect(GLASS, 11)
    fill(p, glass, vgrad(GLASS.top(), GLASS.bottom(), (0, "#22201f"),
                         (1, "#161514")))
    label(p, 988.5, 271.5, "PORTABLE LINUX COMPUTER", 21, "#c9c7cc",
          Qt.AlignHCenter, spacing=0.4)


def _tray(p):
    tray = rrect(QRectF(SOFT_X[0] - 70, SOFT_TOP - 6, SOFT_X[-1] - SOFT_X[0]
                        + 140, SOFT_H + 12), 13)
    fill(p, tray, QColor("#18181a"))
    edges(p, tray, 3, rgba("#000000", 200), rgba("#5a585d", 160))
    for x in SOFT_X:
        p.fillRect(QRectF(x - 50.5, 736.5, 101, 3), QColor("#f08a48"))


def _soft_keys(p, st):
    for name, x in zip(SOFT_KEYS, SOFT_X):
        r = QRectF(x - SOFT_W / 2, SOFT_TOP, SOFT_W, SOFT_H)
        down = name in st.down
        key = rrect(r.adjusted(2, 2, -2, -2), 9)
        fill(p, key, vgrad(r.top(), r.bottom(),
                           (0, "#222224" if down else "#38383a"),
                           (1, "#1e1e20" if down else "#29292b")),
             QPen(QColor("#0b0b0c"), 1.6))
        edges(p, key, 2.5, rgba("#000000", 200) if down else
              rgba("#7e7c82", 200), rgba("#000000", 160))
        if st.hover == name and not down:
            _hover(p, key)
        colour = QColor("#dcdadf")
        if name == "power":
            led = _led(st, POWER_LED)
            if lit(led):
                glow(p, r.center(), led, 34, 90)
                colour = bright(led)
        _icon(p, name, r.center() + QPointF(0, 1.5 if down else 0), colour)


def _icon(p, name, c, colour):
    p.setPen(QPen(colour, 3.4, Qt.SolidLine, Qt.FlatCap, Qt.MiterJoin))
    p.setBrush(Qt.NoBrush)
    if name == "esc":
        s = 15
        p.drawLine(c + QPointF(-s, -s), c + QPointF(s, s))
        p.drawLine(c + QPointF(-s, s), c + QPointF(s, -s))
    elif name == "view":
        for dy in (-9.5, 0, 9.5):
            p.drawLine(c + QPointF(-15, dy), c + QPointF(15, dy))
    elif name == "power":
        p.setPen(QPen(colour, 3.6, Qt.SolidLine, Qt.FlatCap))
        p.drawArc(QRectF(c.x() - 14, c.y() - 12.5, 28, 28), 118 * 16, 304 * 16)
        p.drawLine(c + QPointF(0, -17), c + QPointF(0, -1))
    elif name == "edit":
        sx, sy, k = 15, 13, 8
        for dx, dy in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            corner = c + QPointF(dx * sx, dy * sy)
            path = QPainterPath(corner + QPointF(-dx * k, 0))
            path.lineTo(corner)
            path.lineTo(corner + QPointF(0, -dy * k))
            p.drawPath(path)
    elif name == "run":
        path = QPainterPath(c + QPointF(-15, 1))
        path.lineTo(c + QPointF(-5, 11))
        path.lineTo(c + QPointF(16, -12))
        p.drawPath(path)


def _name(p, name):
    text = (name or "").upper()
    if not text:
        return
    f = font(SANS, 100, bold=True)
    f.setWeight(QFont.Black)
    path = QPainterPath()
    path.addText(0, 0, f, text)
    path = QTransform(1.45, 0, -0.42, 1, 0, 0).map(path)    # wide, leaning
    r = path.boundingRect()
    box = QRectF(735, 876, 525, 42)
    k = min(box.height() / r.height(), box.width() / r.width())
    t = QTransform()
    t.translate(box.center().x() - r.center().x() * k,
                box.center().y() - r.center().y() * k)
    t.scale(k, k)
    path = t.map(path)
    fill(p, path.translated(1.2, 2.2), QColor("#78767b"))
    fill(p, path.translated(-0.6, -1.0), QColor("#2a292c"))
    fill(p, path, QColor("#1c1b1e"))


def _bumpers(p):
    top = poly(BUMPER_TOP)
    fill(p, top, vgrad(165, 560, (0, "#38373a"), (0.3, "#333235"),
                       (1, "#2e2d30")))
    edges(p, top, 18, rgba("#5a585d", 230), rgba("#121213", 200), True, True)
    p.setPen(QPen(QColor(0, 0, 0, 160), 1.5))
    p.setBrush(Qt.NoBrush)
    p.drawPath(top)
    # its faces: the top lit, a band round the chamfer and the corner lit
    # most where it turns down the side
    p.save()
    p.setClipPath(top)
    p.fillRect(QRectF(1590, 160, 200, 34), vgrad(165, 194, (0, "#504e51"),
                                                 (0.7, "#4a484c"),
                                                 (1, rgba("#4a484c", 0))))
    # light on its face beside the meter's panel, fading toward the edge:
    # across the panel's direction, from its upper edge (1680, 246)
    across = QLinearGradient(QPointF(1680, 246), QPointF(1753.6, 194.2))
    across.setColorAt(0, QColor(255, 255, 255, 0))
    across.setColorAt(0.3, QColor(255, 255, 255, 30))
    across.setColorAt(1, QColor(255, 255, 255, 0))
    fill(p, poly(((1600, 170), (1770, 170), (1990, 480), (1990, 600),
                  (1600, 600))), QBrush(across))
    g = QRadialGradient(QPointF(1936, 512), 110)
    g.setColorAt(0, QColor("#a8a6ab"))
    g.setColorAt(0.5, QColor("#8c8a8f"))
    g.setColorAt(1, QColor("#69676c"))
    corner = [pt[:2] for pt in BUMPER_CORNER]
    for inset, alpha in ((0, 0.5), (7, 1.0)):       # its edge soft
        p.setOpacity(alpha)
        fill(p, poly([(x + (inset if i >= 4 or i == 0 else 0), y)
                      for i, (x, y) in enumerate(corner)], 20), QBrush(g))
    p.setOpacity(1)
    p.restore()
    edges(p, top, 3, rgba("#9a989e", 220), rgba("#0a0a0b", 220), True)
    _meter_slot(p)
    low = poly(BUMPER_LOW)
    fill(p, low, vgrad(729, 957, (0, "#2c2c2c"), (0.4, "#2b2b2b"),
                       (0.42, "#1f1f20"), (0.46, "#262626"), (1, "#262627")))
    p.save()
    p.setClipPath(low)
    p.fillRect(QRectF(1925, 729, 30, 230), lgrad((1925, 0), (1951, 0),
                                                 (0, rgba("#7a787d", 0)),
                                                 (1, rgba("#7a787d", 150))))
    p.fillRect(QRectF(1550, 944, 410, 14), vgrad(944, 957,
                                                (0, rgba("#5a585d", 0)),
                                                (1, rgba("#5a585d", 170))))
    p.restore()
    edges(p, low, 10, rgba("#5a585d", 220), rgba("#0e0e0f", 160), soft=True)
    p.setPen(QPen(QColor(0, 0, 0, 160), 1.5))
    p.setBrush(Qt.NoBrush)
    p.drawPath(low)
    # Back's seat: a notch in the bumper's top, dark below the button
    p.save()
    p.setClipPath(low)
    fill(p, circle(BACK, BACK_R + 9), vgrad(BACK.y(), BACK.y() + 61,
                                            (0, "#0a0a0b"), (1, "#151516")))
    p.restore()


def _meter_slot(p):
    """The power meter's panel: a shallow inset along the chamfer, its ends
    cut square to the bumper's top and side."""
    panel = poly(METER_PANEL)
    p.setPen(QPen(QColor(0, 0, 0, 70), 7))
    p.setBrush(Qt.NoBrush)
    p.drawPath(panel.translated(1.2, -1.0))         # its rim's shadow
    fill(p, panel, QColor("#2c2c2e"), QPen(QColor("#0f0f10"), 3.4))
    edges(p, panel, 4, rgba("#000000", 120), rgba("#5a585e", 110), True)


def _meter(p, st):
    """The meter's lights, in the panel's frame: along it from the top, the
    battery's four watt bars, the battery, the USB's four chevrons (pointing
    to the battery: charging), the plug. A light that is off is a dark slit
    (a bar) or a mark printed in grey (the rest)."""
    p.save()
    p.translate(METER_FROM)
    p.rotate(METER_ANGLE)
    for at, what in METER:
        c = QPointF(at, 0)
        if what == "battery":
            _battery(p, c, st)
            continue
        led = _led(st, what)
        on = lit(led)
        if on:
            glow(p, c, led, 26)
        colour = bright(led) if on else QColor(METER_MARK)
        if what == 11:                  # the plug, its prongs to the battery
            p.setPen(Qt.NoPen)
            p.setBrush(colour)
            plug = rrect(QRectF(c.x() - 11, -11, 21, 22), 6).united(
                poly(((c.x() + 8, -7.5), (c.x() + 15, -2.8),
                      (c.x() + 15, 2.8), (c.x() + 8, 7.5))))
            p.drawPath(plug)
            p.setPen(QPen(colour, 3.4, Qt.SolidLine, Qt.FlatCap))
            for dy in (-5, 5):
                p.drawLine(c + QPointF(-10, dy), c + QPointF(-21, dy))
            p.setPen(QPen(colour, 5, Qt.SolidLine, Qt.FlatCap))
            p.drawLine(c + QPointF(14, 0), c + QPointF(22, 0))
        elif what in (12, 13, 14, 15):  # chevrons, opening 105 degrees
            p.setPen(QPen(colour, 8, Qt.SolidLine, Qt.FlatCap, Qt.MiterJoin))
            p.setBrush(Qt.NoBrush)
            path = QPainterPath(c + QPointF(5.7, -16.7))
            path.lineTo(c + QPointF(-7, 0))
            path.lineTo(c + QPointF(5.7, 16.7))
            p.drawPath(path)
        else:                                           # watts: a bar each
            bar = rrect(QRectF(c.x() - 4, -11.5, 8, 23), 2.5)
            if on:
                fill(p, bar, colour, QPen(mix(led, "#000000", 0.25), 1))
            else:
                fill(p, bar.translated(1.3, -0.25), rgba("#ffffff", 26))
                fill(p, bar, QColor("#19191a"))
    p.restore()


def _battery(p, c, st):
    outline = [_led(st, i) for i in BATTERY_OUTLINE]
    centre = _led(st, BATTERY_CENTRE)
    edge = next((led for led in outline if lit(led)), None)
    if edge is not None:
        glow(p, c, edge, 36, 110)
    colour = bright(edge) if edge is not None else QColor(METER_MARK)
    p.setPen(QPen(colour, 3.6))
    p.setBrush(Qt.NoBrush)
    p.drawRoundedRect(QRectF(c.x() - 16, -12.5, 36, 25), 4.5, 4.5)
    p.setPen(Qt.NoPen)
    p.setBrush(colour)
    p.drawRoundedRect(QRectF(c.x() - 22, -6, 6, 12), 1.5, 1.5)
    bolt = QPainterPath(c + QPointF(-8, 1.8))
    for pt in ((4, -8), (2, -1.2), (10, -1.8), (-2, 8), (0, 1.2)):
        bolt.lineTo(c + QPointF(*pt))
    bolt.closeSubpath()
    if lit(centre):
        glow(p, c + QPointF(1, 0), centre, 20, 120)
    p.setBrush(bright(centre) if lit(centre) else QColor(METER_MARK))
    p.drawPath(bolt)


def _socket(p):
    socket = circle(SOCKET, SOCKET_R)
    fill(p, socket, vgrad(SOCKET.y() - SOCKET_R, SOCKET.y() + SOCKET_R,
                          (0, "#2e2e30"), (1, "#2a2a2c")),
         QPen(QColor("#121213"), 3))
    edges(p, socket, 6, rgba("#3a393c", 200), rgba("#0e0e0f", 200), True, True)


def _pad(p, st):
    disc = circle(PAD, PAD_R)
    fill(p, disc.translated(0, 5), QColor(0, 0, 0, 110))
    # a shallow dish: its far, lower right side faces the light
    g = QLinearGradient(PAD + QPointF(-110, -110), PAD + QPointF(110, 110))
    g.setColorAt(0, QColor("#fd8a38"))
    g.setColorAt(0.5, QColor("#fd9246"))
    g.setColorAt(1, QColor("#fba468"))
    fill(p, disc, QBrush(g), QPen(QColor("#a5481a"), 2))
    edges(p, disc, 5, rgba("#ffc89e", 170), rgba("#c45a1c", 150), True)
    for name, (dx, dy) in (("up", (0, -1)), ("down", (0, 1)),
                           ("left", (-1, 0)), ("right", (1, 0))):
        at = PAD + QPointF(dx, dy) * 100
        if name in st.down or st.hover == name:
            shade = QRadialGradient(at, 66)
            alpha = 70 if name in st.down else 26
            shade.setColorAt(0, QColor(0, 0, 0, alpha) if name in st.down
                             else QColor(255, 255, 255, alpha))
            shade.setColorAt(1, QColor(0, 0, 0, 0))
            p.save()
            p.setClipPath(disc)
            fill(p, circle(at, 66), QBrush(shade))
            p.restore()
        p.setPen(QPen(QColor("#c9581a"), 4.2, Qt.SolidLine, Qt.RoundCap))
        p.drawLine(PAD + QPointF(dx, dy) * 71, PAD + QPointF(dx, dy) * 130)
        p.setPen(QPen(QColor(255, 220, 190, 90), 1.4, Qt.SolidLine,
                      Qt.RoundCap))
        p.drawLine(PAD + QPointF(dx, dy) * 71 + QPointF(1.6, 1.6),
                   PAD + QPointF(dx, dy) * 130 + QPointF(1.6, 1.6))
    # OK: a cap with a dark edge, in a dip
    fill(p, circle(PAD, OK_R + 9), QBrush(_halo(PAD, OK_R + 9)))
    fill(p, circle(PAD, OK_R + 3.5), QColor("#2e1608"))
    down = "ok" in st.down
    g = QRadialGradient(PAD + QPointF(-18, -22), OK_R * 1.6)
    g.setColorAt(0, QColor("#f48a42" if down else "#ffb578"))
    g.setColorAt(1, QColor("#e07630" if down else "#fb9850"))
    fill(p, circle(PAD, OK_R), QBrush(g))
    edges(p, circle(PAD, OK_R), 3, rgba("#ffe0c4", 160), rgba("#a84c14", 160),
          True)
    if st.hover == "ok" and not down:
        _hover(p, circle(PAD, OK_R))


def _halo(c, radius):
    g = QRadialGradient(c, radius)
    g.setColorAt(0.85, QColor(150, 60, 10, 110))
    g.setColorAt(1, QColor(150, 60, 10, 0))
    return g


def _switch_and_back(p, st):
    key = rrect(SWITCH, 13).subtracted(circle(SOCKET, SWITCH_CUT))
    down = "switch" in st.down
    fill(p, key.translated(0, 3), QColor(0, 0, 0, 90))
    fill(p, key, vgrad(SWITCH.top(), SWITCH.bottom(),
                       (0, "#232325" if down else "#343436"),
                       (1, "#1f1f21" if down else "#29292b")),
         QPen(QColor("#0d0d0e"), 1.6))
    edges(p, key, 3, rgba("#000000", 180) if down else rgba("#78767c", 200),
          rgba("#000000", 170))
    p.save()                        # the bevel along its arc, in shade
    p.setClipPath(key)
    p.setPen(QPen(QColor(0, 0, 0, 90), 16))
    p.setBrush(Qt.NoBrush)
    p.drawEllipse(SOCKET, SWITCH_CUT, SWITCH_CUT)
    p.restore()
    if st.hover == "switch" and not down:
        _hover(p, key)
    o = QPointF(0, 1.5) if down else QPointF(0, 0)
    pen = QPen(QColor("#ee7d36"), 3, Qt.SolidLine, Qt.RoundCap, Qt.RoundJoin)
    back = QRectF(1446, 700, 33, 22).translated(o)
    front = QRectF(1456.5, 709.5, 34.5, 23.5).translated(o)
    p.setPen(pen)
    p.setBrush(Qt.NoBrush)
    p.drawRoundedRect(back, 4, 4)
    p.setBrush(QColor("#2d2d2f" if not down else "#212123"))
    p.drawRoundedRect(front, 4, 4)
    # Back
    down = "back" in st.down
    fill(p, circle(BACK, BACK_R).translated(0, 4), QColor(0, 0, 0, 120))
    g = QRadialGradient(BACK + QPointF(-16, -20), BACK_R * 1.7)
    g.setColorAt(0, QColor("#f08a44" if down else "#ffa462"))
    g.setColorAt(1, QColor("#d86e2a" if down else "#f5843a"))
    fill(p, circle(BACK, BACK_R), QBrush(g), QPen(QColor("#9a4418"), 1.5))
    edges(p, circle(BACK, BACK_R), 3.5, rgba("#ffd0aa", 190),
          rgba("#a04814", 190), True)
    if st.hover == "back" and not down:
        _hover(p, circle(BACK, BACK_R))
    c = BACK + (QPointF(0, 1.5) if down else QPointF(0, 0))
    p.setPen(QPen(QColor("#dc743a"), 4.2, Qt.SolidLine, Qt.RoundCap,
                  Qt.RoundJoin))
    p.setBrush(Qt.NoBrush)
    arrow = QPainterPath(c + QPointF(-14, -26))
    arrow.lineTo(c + QPointF(-23, -17))
    arrow.lineTo(c + QPointF(-14, -8))
    p.drawPath(arrow)
    turn = QPainterPath(c + QPointF(-22, -17))
    turn.lineTo(c + QPointF(4, -17))
    turn.arcTo(QRectF(c.x() - 13, c.y() - 17, 34, 34), 90, -180)
    turn.lineTo(c + QPointF(-16, 17))
    p.drawPath(turn)


def _mic(p):
    box = rrect(MIC, 11)
    fill(p, box, vgrad(MIC.top(), MIC.bottom(), (0, "#353437"),
                       (1, "#3c3b3e")), QPen(QColor("#121213"), 1.6))
    edges(p, box, 4.5, rgba("#0c0c0d", 200), rgba("#9a989e", 230))
    for y in (274, 294):
        hole = rrect(QRectF(1458, y, 38, 9.5), 3)
        fill(p, hole, QColor("#030303"))
        edges(p, hole, 1.2, rgba("#000000", 255), rgba("#3a3a3c", 220))
    pen = QPen(QColor("#d0ced3"), 2.3, Qt.SolidLine, Qt.RoundCap)
    p.setPen(pen)
    p.setBrush(Qt.NoBrush)
    p.drawRoundedRect(QRectF(1515, 270, 11.5, 22), 5.7, 5.7)
    p.drawArc(QRectF(1509.5, 278, 22.5, 20), 180 * 16, 180 * 16)
    p.drawLine(QPointF(1520.7, 298), QPointF(1520.7, 306))
    label(p, 1539, 290, "MIC", 20, "#d0ced3", spacing=0.6)


def _hover(p, path):
    p.setPen(QPen(QColor(255, 255, 255, 70), 2.2))
    p.setBrush(QColor(255, 255, 255, 14))
    p.drawPath(path)


def _led(st, index):
    leds = st.leds
    return rgb(leds[index] if 0 <= index < len(leds) else 0)


# ----------------------------------------------------------- the buttons

def hit(q):
    """The button (or "touchpad") at a point of the drawing, or None."""
    for name, x in zip(SOFT_KEYS, SOFT_X):
        if QRectF(x - SOFT_W / 2, SOFT_TOP, SOFT_W, SOFT_H).contains(q):
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
    d = q - SOCKET
    if SWITCH.contains(q) and math.hypot(d.x(), d.y()) > SWITCH_CUT:
        return "switch"
    if poly(PTT).boundingRect().adjusted(-6, -10, 6, 8).contains(q):
        return "ptt"
    if rrect(TOUCH, TOUCH.height() / 2).contains(q):
        return "touchpad"
    return None


def touch_at(q):
    """Where a point of the drawing is on the touchpad: 0 ... 1000 each."""
    x = (q.x() - TOUCH.left()) / TOUCH.width() * 1000
    y = (q.y() - TOUCH.top()) / TOUCH.height() * 1000
    return min(max(x, 0), 1000), min(max(y, 0), 1000)
