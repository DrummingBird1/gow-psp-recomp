"""The launcher window in PySide6 (AGENTS.md, "Launcher design"): a live background (the game's own main menu,
rendered headless on the player's PC after the first build, darkened and blurred, with a slow Ken Burns pan and
mouse parallax; a generated stone texture before that), drifting embers, a flickering fire glow behind Play, a
shimmering gold meander, game cards that lift on hover, animated page transitions, a slide-in options panel.
The build/play logic stays in launcher.pyw.

GOW_NO_DISC_ART=1 keeps every piece of the game out of the window (card art and menu background), for
screenshots that may leave the PC; GOW_NO_MENU_BG=1 only turns off the menu background."""
import math
import os
import random
import re
import subprocess
import sys
import threading
import time
import webbrowser
from pathlib import Path

from PySide6.QtCore import (QEasingCurve, QObject, QParallelAnimationGroup, QPoint, QPointF, QPropertyAnimation, QRect, QRectF, QSize, Qt,
                            QTimer, QUrl, Signal, Property)
from PySide6.QtGui import (QIcon, QColor, QCursor, QDesktopServices, QFont, QFontDatabase, QImage, QKeySequence, QLinearGradient,
                           QPainter, QPainterPath, QPen, QPixmap, QRadialGradient, QRegion, QShortcut)
from PySide6.QtWidgets import (QApplication, QCheckBox, QComboBox, QDialog, QFileDialog, QFrame, QGraphicsDropShadowEffect,
                               QGraphicsOpacityEffect, QGridLayout, QHBoxLayout, QLabel, QLineEdit, QMessageBox,
                               QPlainTextEdit, QPushButton, QScrollArea, QSizePolicy, QSlider, QVBoxLayout, QWidget)

import launcher_art as art

BG, PANEL, PANEL2, TEXT, TEXT2 = "#121212", "#1c1c1c", "#262626", "#f0efec", "#a8a69e"
RED, RED_HI, GOLD = "#b3261e", "#c8372e", "#c9a227"
REPO_URL = "https://github.com/DrummingBird1/gow-psp-recomp"

PRESETS = {
    "Original": {"res_scale": 1, "msaa": 1, "fxaa": False, "aniso": 1},
    "Balanced": {"res_scale": 4, "msaa": 1, "fxaa": False, "aniso": 8},
    "High": {"res_scale": 6, "msaa": 4, "fxaa": False, "aniso": 16},
    "Ultra": {"res_scale": 8, "msaa": 4, "fxaa": False, "aniso": 16},
}
RES = [("x1 480x272 (PSP)", 1), ("x2 960x544", 2), ("x3 1440x816", 3), ("x4 1920x1088 (1080p)", 4), ("x6 2880x1632", 6),
       ("x8 3840x2176 (4K)", 8)]
AA = [("Off", (1, False)), ("MSAA 2x", (2, False)), ("MSAA 4x", (4, False)), ("MSAA 8x", (8, False)), ("FXAA", (1, True))]
FPS = [("60 (original)", 60), ("120", 120), ("144", 144), ("165", 165), ("240", 240), ("Unlimited (VRR)", 1000)]
ASPECT = [("Original", "native"), ("21:9", "21:9"), ("32:9", "32:9")]
HDR = [("Auto", True), ("Off", False)]
WINDOW = [("x1 480x272", 1), ("x2 960x544", 2), ("x3 1440x816", 3), ("x4 1920x1088", 4)]
RENDERER = [("Hardware", "gpu"), ("Software (accurate, slow)", "soft")]
FX = [("Match resolution", 0), ("Original glow", 1)]
ANISO = [("Off", 1), ("2x", 2), ("4x", 4), ("8x", 8), ("16x", 16)]
GPU_API = [("Auto (Direct3D 12)", ""), ("Direct3D 12", "direct3d12"), ("Vulkan", "vulkan")]


def label_of(table, value):
    return next((lab for lab, v in table if v == value), table[0][0])


def value_of(table, label):
    return next((v for lab, v in table if lab == label), table[0][1])


def env_on(name):
    return os.environ.get(name, "") not in ("", "0")


def assets_dir():
    return Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent)) / "launcher_assets"


def detect_pc():
    """(discrete GPU?, refresh rate in Hz) for the starting preset."""
    discrete, hz = False, 60
    if os.name == "nt":
        try:
            r = subprocess.run(["powershell", "-NoProfile", "-Command", "(Get-CimInstance Win32_VideoController).Name -join ';'"],
                               capture_output=True, text=True, timeout=20, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            discrete = bool(re.search(r"NVIDIA|GeForce|Quadro|Radeon (RX|Pro)|Arc", r.stdout, re.I))
        except (OSError, subprocess.TimeoutExpired):
            pass
    screen = QApplication.primaryScreen()
    if screen:
        hz = int(round(screen.refreshRate())) or 60
    return discrete, hz


def pil_to_qimage(img):
    img = img.convert("RGBA")
    data = img.tobytes("raw", "RGBA")
    q = QImage(data, img.width, img.height, img.width * 4, QImage.Format.Format_RGBA8888)
    return q.copy()


STYLE = f"""
QWidget {{ color: {TEXT}; font-family: 'Segoe UI'; font-size: 10pt; }}
QLabel#dim {{ color: {TEXT2}; }}
QLabel#head {{ color: {GOLD}; font-family: 'Cinzel'; font-size: 12pt; font-weight: 700; }}
QLabel#bigq {{ font-family: 'Cinzel'; font-size: 18pt; font-weight: 700; }}
QPushButton {{ background: {PANEL2}; border: 1px solid #3a3a3a; border-radius: 6px; padding: 7px 16px;
  font-family: 'Cinzel'; font-weight: 700; }}
QPushButton:hover {{ background: #323232; border-color: #6b5a24; }}
QPushButton:pressed {{ background: #1a1a1a; }}
QPushButton:focus {{ border-color: {GOLD}; }}
QPushButton:disabled {{ color: #6a6a6a; background: {PANEL}; border-color: #2a2a2a; }}
QPushButton#play {{ background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 {RED_HI}, stop:1 #8f1e18); border: 1px solid #e0574d;
  border-radius: 10px; font-size: 20pt; padding: 10px 58px; letter-spacing: 3px; }}
QPushButton#play:hover {{ background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #dd4a40, stop:1 {RED}); }}
QPushButton#play:focus {{ border: 2px solid {GOLD}; }}
QPushButton#play:disabled {{ background: #3a2422; color: #8a7f7c; border-color: #4a2e2b; }}
QPushButton#big {{ background: {RED}; border-color: #e0574d; font-size: 15pt; padding: 10px 34px; }}
QPushButton#big:hover {{ background: {RED_HI}; }}
QPushButton#preset {{ padding: 6px 12px; }}
QPushButton#preset[on="true"] {{ background: {RED}; border-color: {GOLD}; }}
QPushButton#link {{ background: transparent; border: none; color: {TEXT2}; text-decoration: underline; padding: 2px 4px;
  font-family: 'Segoe UI'; font-weight: 400; }}
QPushButton#link:hover, QPushButton#link:focus {{ color: {GOLD}; }}
QPushButton#gear {{ background: rgba(18,18,18,150); border: 1px solid #4a3d18; border-radius: 19px; color: {GOLD};
  font-size: 16pt; padding: 0; min-width: 38px; min-height: 38px; font-family: 'Segoe UI Symbol'; }}
QPushButton#gear:hover, QPushButton#gear:focus {{ border-color: {GOLD}; }}
QLineEdit {{ background: rgba(38,38,38,220); border: 1px solid #3a3a3a; border-radius: 5px; padding: 5px 8px; }}
QComboBox {{ background: {PANEL2}; border: 1px solid #3a3a3a; border-radius: 5px; padding: 5px 8px; min-width: 170px; }}
QComboBox:focus, QComboBox:hover {{ border-color: {GOLD}; }}
QComboBox::drop-down {{ border: none; width: 22px; }}
QComboBox QAbstractItemView {{ background: {PANEL2}; selection-background-color: {RED}; border: 1px solid #3a3a3a; }}
QCheckBox::indicator {{ width: 16px; height: 16px; border: 1px solid #555; border-radius: 3px; background: {PANEL2}; }}
QCheckBox::indicator:checked {{ background: {RED}; border-color: {GOLD}; }}
QCheckBox:focus {{ color: {GOLD}; }}
QSlider::groove:horizontal {{ height: 6px; border-radius: 3px; background: {PANEL2}; }}
QSlider::sub-page:horizontal {{ border-radius: 3px; background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #7a1a14, stop:1 {RED_HI}); }}
QSlider::handle:horizontal {{ width: 16px; height: 16px; margin: -6px 0; border-radius: 8px; background: {GOLD};
  border: 2px solid #f3dd8a; }}
QSlider::handle:horizontal:focus {{ background: #f3dd8a; }}
QScrollArea, QScrollArea > QWidget > QWidget {{ background: transparent; border: none; }}
QScrollBar:vertical {{ background: transparent; width: 8px; }}
QScrollBar::handle:vertical {{ background: #4a3d18; border-radius: 4px; min-height: 30px; }}
QScrollBar::add-line, QScrollBar::sub-line {{ height: 0; }}
QToolTip {{ background: {PANEL}; color: {TEXT}; border: 1px solid {GOLD}; padding: 4px; }}
QPlainTextEdit {{ background: {PANEL}; border: none; font-family: Consolas; font-size: 9pt; }}
"""


FRAME_MS = 33   # ~30 fps animation tick
BG_SCALE = 0.5   # background cache resolution relative to the screen
STEP = FRAME_MS / 16.0   # motion constants were tuned per 16 ms


class Bridge(QObject):
    call = Signal(object)

    def __init__(self):
        super().__init__()
        self.call.connect(lambda fn: fn())


class Card(QWidget):
    """A game card: the disc's own art (or an original placeholder), title, status; lifts and glows on hover."""
    clicked = Signal(str)
    activated = Signal(str)

    def __init__(self, app, game):
        super().__init__()
        self.app, self.game = app, game
        self._lift = 0.0
        self.selected = False
        self.pix = None
        self.icon = None
        self.status = ""
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setMinimumSize(300, 160)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        self.anim = QPropertyAnimation(self, b"lift", self)
        self.anim.setDuration(220)
        self.anim.setEasingCurve(QEasingCurve.Type.OutCubic)
        self.placeholder = None

    def get_lift(self):
        return self._lift

    def set_lift(self, v):
        self._lift = v
        self.update()

    lift = Property(float, get_lift, set_lift)

    def hover_to(self, v):
        if self.app.reduce_motion:
            self.set_lift(v)
            return
        self.anim.stop()
        self.anim.setStartValue(self._lift)
        self.anim.setEndValue(v)
        self.anim.start()

    def enterEvent(self, e):
        self.hover_to(1.0)

    def leaveEvent(self, e):
        if not self.hasFocus():
            self.hover_to(0.0)

    def focusInEvent(self, e):
        self.hover_to(1.0)
        self.update()

    def focusOutEvent(self, e):
        if not self.underMouse():
            self.hover_to(0.0)
        self.update()

    def mousePressEvent(self, e):
        self.clicked.emit(self.game)

    def mouseDoubleClickEvent(self, e):
        self.activated.emit(self.game)

    def keyPressEvent(self, e):
        k = e.key()
        if k in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
            self.activated.emit(self.game)
        elif k == Qt.Key.Key_Space:
            self.clicked.emit(self.game)
        elif k in (Qt.Key.Key_Left, Qt.Key.Key_Right):
            self.app.cycle_game(-1 if k == Qt.Key.Key_Left else 1, focus=True)
        else:
            super().keyPressEvent(e)

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        w, h = self.width(), self.height()
        v = self._lift
        s = 1.0 + 0.025 * v
        p.translate(w / 2, h / 2 - 6 * v)
        p.scale(s, s)
        p.translate(-w / 2, -h / 2)
        r = QRectF(16, 16, w - 32, h - 32)
        # soft glow/shadow around the card, painted (a QGraphicsEffect would re-blur on every repaint)
        glow = QColor(GOLD) if self.selected else QColor(201, 120, 40)
        k = 0.25 + 0.75 * v if self.selected else 0.6 * v
        p.setPen(Qt.PenStyle.NoPen)
        for i in range(8, 0, -1):
            a = int((1 - i / 9) ** 2 * 60 * k) if k > 0 else 0
            col = QColor(glow.red(), glow.green(), glow.blue(), a) if a else QColor(0, 0, 0, int((1 - i / 9) ** 2 * 50))
            p.setBrush(col)
            p.drawRoundedRect(r.adjusted(-i * 1.6, -i * 1.6 + 4, i * 1.6, i * 1.6 + 4), 14 + i * 1.6, 14 + i * 1.6)
        path = QPainterPath()
        path.addRoundedRect(r, 14, 14)
        p.setClipPath(path)
        src = self.pix
        if src is None:
            if self.placeholder is None or self.placeholder.size() != r.size().toSize():
                img = art.stone(int(r.width()), int(r.height()), seed=hash(self.game) & 0xFFFF).point(lambda x: min(255, int(x * 1.5)))
                self.placeholder = QPixmap.fromImage(pil_to_qimage(img))
            p.drawPixmap(r.topLeft(), self.placeholder)
        else:
            sc = max(r.width() / src.width(), r.height() / src.height())
            tw, th = src.width() * sc, src.height() * sc
            p.drawPixmap(QRectF(r.center().x() - tw / 2, r.center().y() - th / 2, tw, th), src, QRectF(src.rect()))
        g = QLinearGradient(0, r.top(), 0, r.bottom())
        g.setColorAt(0, QColor(0, 0, 0, 30))
        g.setColorAt(0.45, QColor(0, 0, 0, 60))
        g.setColorAt(1, QColor(0, 0, 0, 225 - int(35 * v)))
        p.fillRect(r, g)
        icon = self.icon if self.icon is not None else self.app.app_icon  # the disc's ICON0.PNG, else ours
        if icon is not None and not icon.isNull():
            ih = 52
            iw = icon.width() * ih / icon.height()
            p.drawPixmap(QRectF(r.left() + 16, r.top() + 16, iw, ih), icon, QRectF(icon.rect()))
        p.setClipping(False)
        title = self.app.L.GAMES[self.game]["title"].replace("God of War: ", "")
        f = QFont("Cinzel", 17)
        f.setWeight(QFont.Weight.Bold)
        p.setFont(f)
        p.setPen(QColor(TEXT))
        p.drawText(QRectF(r.left() + 18, r.bottom() - 72, r.width() - 36, 36), Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignBottom, title)
        f2 = QFont("Segoe UI", 10)
        p.setFont(f2)
        col = {"Ready to play": QColor("#9fd18b"), "Needs building": QColor(GOLD)}.get(self.status, QColor(TEXT2))
        p.setPen(col)
        p.drawText(QRectF(r.left() + 18, r.bottom() - 34, r.width() - 36, 22), Qt.AlignmentFlag.AlignLeft, "\u25cf  " + self.status)
        if self.selected:
            p.setPen(QPen(QColor(GOLD), 2))
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 14, 14)
            band = self.app.meander_pix(int(r.width() - 28))
            p.setOpacity(0.9)
            p.drawPixmap(QPointF(r.left() + 14, r.top() + 8), band)
            self.app.draw_shimmer(p, QRectF(r.left() + 14, r.top() + 8, band.width() / band.devicePixelRatio(),
                                            band.height() / band.devicePixelRatio()), band)
            p.setOpacity(1)
        elif v > 0.01:
            p.setPen(QPen(QColor(150, 120, 45, int(200 * v)), 1.5))
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 14, 14)
        if self.hasFocus():
            p.setPen(QPen(QColor(240, 239, 236, 160), 1, Qt.PenStyle.DashLine))
            p.drawRoundedRect(r.adjusted(5, 5, -5, -5), 11, 11)


class Progress(QWidget):
    """A progress bar with a smooth value and a moving sheen."""

    def __init__(self, app):
        super().__init__()
        self.app = app
        self.value = 0.0
        self.target = 0.0
        self.text = ""
        self.setMinimumHeight(26)
        self.setMinimumWidth(320)

    def set(self, v, text):
        self.target = v
        self.text = text
        self.update()

    def tick(self, t):
        if self.app.reduce_motion:
            self.value = self.target
        else:
            self.value += (self.target - self.value) * 0.12
        self.update()

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        r = QRectF(0, 4, self.width(), self.height() - 8)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor(38, 38, 38, 230))
        p.drawRoundedRect(r, r.height() / 2, r.height() / 2)
        fill = QRectF(r.left(), r.top(), max(r.height(), r.width() * self.value), r.height())
        g = QLinearGradient(fill.topLeft(), fill.topRight())
        g.setColorAt(0, QColor("#7a1a14"))
        g.setColorAt(1, QColor(RED_HI))
        p.setBrush(g)
        p.drawRoundedRect(fill, r.height() / 2, r.height() / 2)
        if not self.app.reduce_motion and self.value < 0.999:
            x = fill.left() + (time.monotonic() * 160) % max(1, fill.width() + 80) - 40
            sg = QLinearGradient(x - 40, 0, x + 40, 0)
            sg.setColorAt(0, QColor(255, 255, 255, 0))
            sg.setColorAt(0.5, QColor(255, 220, 160, 90))
            sg.setColorAt(1, QColor(255, 255, 255, 0))
            path = QPainterPath()
            path.addRoundedRect(fill, r.height() / 2, r.height() / 2)
            p.setClipPath(path)
            p.fillRect(fill, sg)
            p.setClipping(False)
        p.setPen(QColor(TEXT))
        p.setFont(QFont("Segoe UI", 9))
        p.drawText(r, Qt.AlignmentFlag.AlignCenter, self.text)


# Qt key -> SDL key name (what --controls reads); keys not listed can't be bound
def sdl_key_name(e):
    k = e.key()
    if Qt.Key.Key_A <= k <= Qt.Key.Key_Z or Qt.Key.Key_0 <= k <= Qt.Key.Key_9:
        return chr(k)
    if Qt.Key.Key_F1 <= k <= Qt.Key.Key_F10:
        return f"F{k - Qt.Key.Key_F1 + 1}"
    nat = e.nativeVirtualKey()
    if k == Qt.Key.Key_Shift:
        return "Right Shift" if nat == 0xA1 or e.nativeScanCode() == 0x36 else "Left Shift"
    if k == Qt.Key.Key_Control:
        return "Right Ctrl" if e.nativeScanCode() & 0x100 else "Left Ctrl"
    if k == Qt.Key.Key_Alt:
        return "Left Alt"
    names = {Qt.Key.Key_Up: "Up", Qt.Key.Key_Down: "Down", Qt.Key.Key_Left: "Left", Qt.Key.Key_Right: "Right",
             Qt.Key.Key_Space: "Space", Qt.Key.Key_Return: "Return", Qt.Key.Key_Enter: "Keypad Enter",
             Qt.Key.Key_Tab: "Tab", Qt.Key.Key_Backspace: "Backspace", Qt.Key.Key_Comma: ",", Qt.Key.Key_Period: ".",
             Qt.Key.Key_Slash: "/", Qt.Key.Key_Semicolon: ";", Qt.Key.Key_Apostrophe: "'", Qt.Key.Key_BracketLeft: "[",
             Qt.Key.Key_BracketRight: "]", Qt.Key.Key_Minus: "-", Qt.Key.Key_Equal: "=", Qt.Key.Key_Escape: "Escape",
             Qt.Key.Key_F11: "F11", Qt.Key.Key_F12: "F12", Qt.Key.Key_Insert: "Insert", Qt.Key.Key_Delete: "Delete",
             Qt.Key.Key_Home: "Home", Qt.Key.Key_End: "End", Qt.Key.Key_PageUp: "PageUp", Qt.Key.Key_PageDown: "PageDown"}
    return names.get(k)


PSP_LABELS = [("up", "D-pad up"), ("down", "D-pad down"), ("left", "D-pad left"), ("right", "D-pad right"),
              ("cross", "Cross \u2715"), ("circle", "Circle \u25cb"), ("square", "Square \u25a1"), ("triangle", "Triangle \u25b3"),
              ("l", "L"), ("r", "R"), ("start", "Start"), ("select", "Select"), ("stick_up", "Stick up (keyboard)"),
              ("stick_down", "Stick down (keyboard)"), ("stick_left", "Stick left (keyboard)"), ("stick_right", "Stick right (keyboard)")]
PAD_LABELS = [("(none)", ""), ("A / Cross", "a"), ("B / Circle", "b"), ("X / Square", "x"), ("Y / Triangle", "y"),
              ("LB / L1", "leftshoulder"), ("RB / R1", "rightshoulder"), ("LT / L2", "lefttrigger"), ("RT / R2", "righttrigger"),
              ("Back / Share", "back"), ("Start / Options", "start"), ("D-pad up", "dpup"), ("D-pad down", "dpdown"),
              ("D-pad left", "dpleft"), ("D-pad right", "dpright"), ("Left stick click", "leftstick"), ("Right stick click", "rightstick")]


class KeyButton(QPushButton):
    """Shows a key; click it, then press the new key (Backspace alone clears it, Esc cancels)."""
    changed = Signal()

    def __init__(self, key):
        super().__init__(key or "\u2014")
        self.key = key
        self.waiting = False
        self.setMinimumWidth(110)
        self.clicked.connect(self.listen)

    def listen(self):
        self.waiting = True
        self.setText("press a key\u2026")
        self.grabKeyboard()

    def keyPressEvent(self, e):
        if not self.waiting:
            return super().keyPressEvent(e)
        self.waiting = False
        self.releaseKeyboard()
        name = sdl_key_name(e)
        if e.key() == Qt.Key.Key_Escape:
            pass
        elif e.key() == Qt.Key.Key_Backspace:
            self.key = ""
        elif name and name not in ("Escape", "F11", "F12"):
            self.key = name
        self.setText(self.key or "\u2014")
        self.changed.emit()

    def focusOutEvent(self, e):
        if self.waiting:
            self.waiting = False
            self.releaseKeyboard()
            self.setText(self.key or "\u2014")
        super().focusOutEvent(e)


class ControlsDialog(QDialog):
    """Keyboard and gamepad bindings per PSP button: two keys and two gamepad buttons each. Saved in the launcher
    settings and passed to the game as --controls."""

    def __init__(self, win):
        super().__init__(win)
        self.win, self.L = win, win.L
        self.setWindowTitle("Controls")
        self.setStyleSheet(STYLE + f"QDialog {{ background: {BG}; }}")
        v = QVBoxLayout(self)
        l = QLabel("Click a key box, then press the key (Backspace clears it). Esc, F11, F12 and Alt+F4 stay fixed; "
                   "Enter/Space/click/Start still skip movies. The gamepad's left stick is always the PSP stick.")
        l.setObjectName("dim")
        l.setWordWrap(True)
        v.addWidget(l)
        grid = QGridLayout()
        for c, t in enumerate(("PSP button", "Key", "Key 2", "Gamepad", "Gamepad 2")):
            h = QLabel(t)
            h.setObjectName("head")
            grid.addWidget(h, 0, c)
        self.rows = {}
        cur = self.L.controls_of(win.cfg)
        for r, (b, label) in enumerate(PSP_LABELS, 1):
            keys, pads = cur[b]
            grid.addWidget(QLabel(label), r, 0)
            kb = [KeyButton(keys[i] if i < len(keys) else "") for i in range(2)]
            for i, w in enumerate(kb):
                w.changed.connect(self.check)
                grid.addWidget(w, r, 1 + i)
            pc = []
            for i in range(2):
                cb = QComboBox()
                cb.setMinimumWidth(150)
                cb.addItems([t for t, _ in PAD_LABELS])
                cb.setCurrentText(label_of(PAD_LABELS, pads[i] if i < len(pads) else ""))
                cb.setEnabled(not b.startswith("stick"))
                cb.currentTextChanged.connect(self.check)
                grid.addWidget(cb, r, 3 + i)
                pc.append(cb)
            self.rows[b] = (kb, pc)
        v.addLayout(grid)
        self.warn = QLabel("")
        self.warn.setWordWrap(True)
        self.warn.setStyleSheet(f"color: {GOLD};")
        v.addWidget(self.warn)
        h = QHBoxLayout()
        b = QPushButton("Defaults")
        b.clicked.connect(self.defaults)
        h.addWidget(b)
        h.addStretch()
        self.ok = QPushButton("Save")
        self.ok.clicked.connect(self.save)
        h.addWidget(self.ok)
        b = QPushButton("Cancel")
        b.clicked.connect(self.reject)
        h.addWidget(b)
        v.addLayout(h)
        self.check()

    def current(self):
        out = {}
        for b, (kb, pc) in self.rows.items():
            keys = [w.key for w in kb if w.key]
            pads = [value_of(PAD_LABELS, cb.currentText()) for cb in pc]
            out[b] = (keys, [p for p in pads if p])
        return out

    def check(self):
        probs = self.L.controls_problems(self.current())
        self.warn.setText("\n".join(probs[:6]))
        self.ok.setEnabled(not any("reserved" in p for p in probs))

    def defaults(self):
        for b, (kb, pc) in self.rows.items():
            keys, pads = self.L.CONTROLS[b]
            for i, w in enumerate(kb):
                w.key = keys[i] if i < len(keys) else ""
                w.setText(w.key or "\u2014")
            for i, cb in enumerate(pc):
                cb.setCurrentText(label_of(PAD_LABELS, pads[i] if i < len(pads) else ""))
        self.check()

    def save(self):
        cur = self.current()
        dflt = {b: (list(k), list(p)) for b, (k, p) in self.L.CONTROLS.items()}
        # only what differs from the defaults is stored (the 3rd+ default entries, e.g. RT, count as changed
        # when a row is edited: the dialog shows two per column)
        self.win.cfg["controls"] = {b: {"keys": k, "pad": p} for b, (k, p) in cur.items()
                                    if (k, p) != tuple(dflt[b]) and (k, p) != (dflt[b][0][:2], dflt[b][1][:2])}
        self.win.collect()
        self.accept()


class Window(QWidget):
    def __init__(self, L):
        super().__init__()
        self.L = L
        self.cfg = L.load_config()
        self.no_art = env_on("GOW_NO_DISC_ART")
        self.app_icon = QPixmap(str(assets_dir() / "icon.png"))
        self.no_menu = self.no_art or env_on("GOW_NO_MENU_BG")
        self.reduce_motion = bool(self.cfg.get("reduce_motion", False))
        self.bridge = Bridge()
        self.log_lines, self.log_win, self.busy = [], None, False
        self.preset = self.cfg.get("preset") or "Custom"
        self.applying = False
        self.view = None
        self.t0 = time.monotonic()
        self.particles = []
        self.bg = {}       # game -> list of QPixmap (menu loop), or None
        self.bg_cur, self.bg_prev, self.fade = None, None, 1.0
        self._meander = {}
        self.parallax = QPointF(0, 0)
        self.cache, self.cache_dirty, self.cache_t = None, True, 0.0
        self.setWindowTitle("God of War PSP: launcher")
        self.setMinimumSize(1000, 680)
        self.restore_geometry()
        self.setStyleSheet(STYLE)
        self.make_stone()
        self.build_ui()
        if not self.cfg.get("preset"):
            self.auto_preset()
        self.load_found_discs()
        for fix in self.cfg.get("_fixed", []):
            self.log(f"settings: invalid value reset: {fix}")
        self.timer = QTimer(self)
        self.timer.setTimerType(Qt.TimerType.PreciseTimer)
        self.timer.timeout.connect(self.tick)
        self.timer.start(FRAME_MS)
        QShortcut(QKeySequence("F1"), self, activated=self.toggle_options)
        QShortcut(QKeySequence("Escape"), self, activated=lambda: self.options_open and self.toggle_options())
        self.show_view("main" if self.cfg.get("game_folder") else "welcome", animate=False)
        self.refresh()
        QTimer.singleShot(1500, self.maybe_render_menus)
        if self.cfg.get("check_updates", True) and self.L.FROZEN:
            QTimer.singleShot(2500, self.check_update)

    # --- geometry, background ----------------------------------------------------------------------------
    def restore_geometry(self):
        m = re.match(r"(\d+)x(\d+)\+(-?\d+)\+(-?\d+)", self.cfg.get("geometry") or "")
        if m:
            w, h, x, y = map(int, m.groups())
            self.resize(max(w, 1000), max(h, 680))
            self.move(x, y)
        else:
            self.resize(1180, 760)

    def make_stone(self):
        img = art.stone(1600, 1000)
        self.stone = QPixmap.fromImage(pil_to_qimage(img))
        sprite = QImage(32, 32, QImage.Format.Format_ARGB32_Premultiplied)
        sprite.fill(Qt.GlobalColor.transparent)
        sp = QPainter(sprite)
        g = QRadialGradient(16, 16, 16)
        g.setColorAt(0, QColor(255, 220, 150, 255))
        g.setColorAt(0.25, QColor(255, 140, 50, 200))
        g.setColorAt(1, QColor(200, 40, 10, 0))
        sp.fillRect(sprite.rect(), g)
        sp.end()
        self.ember = QPixmap.fromImage(sprite)

    def meander_pix(self, w):
        w = max(16, w)
        dpr = self.devicePixelRatioF()
        key = (w, dpr)
        if key not in self._meander:
            img = art.meander(int(w * dpr), cell=max(2, int(2 * dpr)))
            pm = QPixmap.fromImage(pil_to_qimage(img))
            pm.setDevicePixelRatio(dpr)
            self._meander[key] = pm
        return self._meander[key]

    def draw_shimmer(self, p, rect, band):
        if self.reduce_motion:
            return
        t = time.monotonic() - self.t0
        x = rect.left() + ((t * 0.18) % 1.6 - 0.3) * rect.width()
        tmp = QPixmap(band.size())
        tmp.setDevicePixelRatio(band.devicePixelRatio())
        tmp.fill(Qt.GlobalColor.transparent)
        q = QPainter(tmp)
        q.drawPixmap(0, 0, band)
        q.setCompositionMode(QPainter.CompositionMode.CompositionMode_SourceIn)
        g = QLinearGradient(x - rect.left() - 90, 0, x - rect.left() + 90, 0)
        g.setColorAt(0, QColor(255, 250, 220, 0))
        g.setColorAt(0.5, QColor(255, 248, 210, 230))
        g.setColorAt(1, QColor(255, 250, 220, 0))
        q.fillRect(tmp.rect(), g)
        q.end()
        p.drawPixmap(rect.topLeft(), tmp)

    def load_menu_bg(self, g):
        if self.no_menu or g in self.bg:
            return
        frames = self.L.menu_frames(g)
        if not frames:
            return
        from PIL import Image, ImageEnhance, ImageFilter
        pix = []
        for f in frames:
            im = Image.open(f).convert("RGB")
            im = im.resize((960, int(960 * im.height / im.width)), Image.LANCZOS).filter(ImageFilter.GaussianBlur(3))
            im = ImageEnhance.Brightness(im).enhance(0.42)
            pix.append(QPixmap.fromImage(pil_to_qimage(im)))
        self.bg[g] = pix

    def set_bg_game(self, g):
        target = g if g in self.bg else None
        if target == self.bg_cur:
            return
        self.bg_prev, self.bg_cur, self.fade = self.bg_cur, target, 0.0 if not self.reduce_motion else 1.0
        self.cache_dirty = True
        self.update()

    def maybe_render_menus(self):
        if self.no_menu:
            return
        todo = [g for g in self.L.GAMES if self.L.exe_path(g).exists() and self.game_state(g) == "Ready to play"
                and not self.L.menu_frames(g)]
        for g in self.L.GAMES:
            self.load_menu_bg(g)
        self.set_bg_game(self.cfg.get("game", "coo"))
        if todo and not self.busy:
            def work():
                for g in todo:
                    self.L.render_menu(self.cfg, g, self.log)
                self.bridge.call.emit(lambda: (self.bg.clear(), [self.load_menu_bg(x) for x in self.L.GAMES],
                                               self.set_bg_game(self.cfg.get("game", "coo"))))
            threading.Thread(target=work, daemon=True).start()

    # --- animation ---------------------------------------------------------------------------------------
    def tick(self):
        """30 Hz: embers, glow, meander and progress repaint only their own areas; the slow background (Ken Burns,
        menu loop) is recomposed into a cached pixmap about 6 times a second, which repaints the whole window."""
        if self.isMinimized() or not self.isVisible():
            return
        now = time.monotonic()
        t = now - self.t0
        region = QRegion()
        if not self.reduce_motion:
            c = self.mapFromGlobal(QCursor.pos())
            tx = max(-0.6, min(0.6, c.x() / max(1, self.width()) - 0.5))
            ty = max(-0.6, min(0.6, c.y() / max(1, self.height()) - 0.5))
            self.parallax += (QPointF(tx, ty) - self.parallax) * 0.05
            if len(self.particles) < 60 and random.random() < 0.4:
                self.particles.append(self.new_particle())
            for q in self.particles:
                region += QRect(int(q[0] - q[7]), int(q[1] - q[7]), int(2 * q[7]) + 2, int(2 * q[7]) + 2)
                q[0] += (q[2] + math.sin(t * q[5] + q[6]) * 0.25) * STEP
                q[1] += q[3] * STEP
                q[4] -= 0.0035 * STEP
                region += QRect(int(q[0] - q[7]), int(q[1] - q[7]), int(2 * q[7]) + 2, int(2 * q[7]) + 2)
            self.particles = [q for q in self.particles if q[4] > 0 and q[1] > -20]
            bh = 40
            if int(t * 10) != getattr(self, 'shim_t', -1):
                self.shim_t = int(t * 10)
                region += QRect(0, 0, self.width(), bh)
                region += QRect(0, self.height() - bh, self.width(), bh)
            if self.view == "main" and self.play.isVisible():
                cpt = self.play.mapTo(self, self.play.rect().center())
                rad = int(self.play.width() * 1.25)
                region += QRect(cpt.x() - rad, cpt.y() - rad // 2, 2 * rad, rad + 12)
            if now - self.cache_t >= 1 / 6 or self.fade < 1:
                self.cache_dirty = True
        if self.fade < 1:
            self.fade = min(1.0, self.fade + 0.025 * STEP)
        self.prog.tick(t)
        self.prep_prog.tick(t)
        if self.cache_dirty:
            self.update()
        elif not region.isEmpty():
            self.update(region)
        self.card_tick = (getattr(self, "card_tick", 0) + 1) % 3
        if self.card_tick == 0 and not self.reduce_motion:
            for c in self.cards.values():
                if c.selected and c.isVisible():
                    c.update(0, 0, c.width(), 40)

    def new_particle(self):
        w, h = self.width(), self.height()
        return [random.uniform(0, w), h + random.uniform(0, 40), random.uniform(-0.25, 0.25), -random.uniform(0.35, 1.1),
                random.uniform(0.6, 1.0), random.uniform(0.6, 1.6), random.uniform(0, 6.28), random.uniform(3, 9)]

    def changeEvent(self, e):
        super().changeEvent(e)
        if e.type() == e.Type.WindowStateChange:
            if self.isMinimized():
                self.timer.stop()
            elif not self.timer.isActive() and not self.game_running():
                self.timer.start(FRAME_MS)

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        W, H = self.width(), self.height()
        t = time.monotonic() - self.t0
        if self.reduce_motion:
            t = 0.0
        if self.cache_dirty or self.cache is None or self.cache.size() != self.cache_size():
            self.compose_cache(t)
        # always stretched over the whole window: no reliance on fractional pixmap pixel ratios (125/150% scaling)
        p.drawPixmap(QRectF(0, 0, W, H), self.cache, QRectF(self.cache.rect()))
        self.paint_overlay(p, t)

    def cache_size(self):
        """Pixel size of the background cache: half the window's device pixels (blurred, so the lower resolution is
        invisible and saves CPU), the same rounding everywhere so it is not recomposed on every paint."""
        dpr = self.devicePixelRatioF() * BG_SCALE
        return QSize(max(1, round(self.width() * dpr)), max(1, round(self.height() * dpr)))

    def compose_cache(self, t):
        W, H = max(1, self.width()), max(1, self.height())
        size = self.cache_size()
        self.cache = QPixmap(size)   # pixel ratio 1; the painter scales window coordinates onto it
        self.cache_dirty, self.cache_t = False, time.monotonic()
        p = QPainter(self.cache)
        p.scale(size.width() / W, size.height() / H)
        p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        p.fillRect(QRect(0, 0, W, H), QColor(BG))

        def draw_bg(src_list, opacity):
            if src_list is None:
                src, loop = self.stone, False
            else:
                n = len(src_list)
                i = int(t * 15) % (2 * n - 2) if n > 1 else 0
                src, loop = src_list[i if i < n else 2 * n - 2 - i], True
            zoom = 1.12 + 0.06 * math.sin(t * 0.05)
            sc = max(W / src.width(), H / src.height()) * zoom
            tw, th = src.width() * sc, src.height() * sc
            ox = (W - tw) / 2 + math.sin(t * 0.031) * 0.03 * W - self.parallax.x() * 30
            oy = (H - th) / 2 + math.cos(t * 0.027) * 0.03 * H - self.parallax.y() * 20
            p.setOpacity(opacity)
            p.drawPixmap(QRectF(ox, oy, tw, th), src, QRectF(src.rect()))
            p.setOpacity(1)

        prev = self.bg.get(self.bg_prev) if self.bg_prev else None
        cur = self.bg.get(self.bg_cur) if self.bg_cur else None
        if self.fade < 1:
            draw_bg(prev, 1.0)
        draw_bg(cur, self.fade if self.fade < 1 else 1.0)
        vg = QRadialGradient(W / 2, H * 0.45, max(W, H) * 0.75)
        vg.setColorAt(0, QColor(0, 0, 0, 0))
        vg.setColorAt(0.6, QColor(0, 0, 0, 70))
        vg.setColorAt(1, QColor(0, 0, 0, 220))
        p.fillRect(QRect(0, 0, W, H), vg)
        p.end()

    def paint_overlay(self, p, t):
        W, H = self.width(), self.height()
        # fire glow behind Play
        if self.view == "main" and self.play.isVisible():
            c = self.play.mapTo(self, self.play.rect().center())
            fl = 0.78 + 0.12 * math.sin(t * 7.1) + 0.06 * math.sin(t * 13.7 + 1.3) + 0.04 * math.sin(t * 2.3)
            if not self.play.isEnabled():
                fl *= 0.35
            rad = self.play.width() * (1.15 + 0.06 * math.sin(t * 3.3))
            g = QRadialGradient(QPointF(0, 0), rad)
            g.setColorAt(0, QColor(230, 70, 30, int(150 * fl)))
            g.setColorAt(0.35, QColor(180, 38, 20, int(90 * fl)))
            g.setColorAt(0.7, QColor(140, 25, 12, int(30 * fl)))
            g.setColorAt(1, QColor(120, 20, 10, 0))
            p.save()
            p.translate(c.x(), c.y() + 6)
            p.scale(1.0, 0.55)
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(g)
            p.drawEllipse(QPointF(0, 0), rad, rad)
            p.restore()
        # embers
        for q in self.particles:
            p.setOpacity(max(0.0, min(1.0, q[4])) * 0.85)
            s = q[7] * (0.6 + 0.4 * q[4])
            p.drawPixmap(QRectF(q[0] - s / 2, q[1] - s / 2, s, s), self.ember, QRectF(self.ember.rect()))
        p.setOpacity(1)
        # meander borders, shimmering
        band = self.meander_pix(W)
        bh = band.height() / band.devicePixelRatio()
        for y in (6, H - 6 - bh):
            p.drawPixmap(QPointF(0, y), band)
            self.draw_shimmer(p, QRectF(0, y, W, bh), band)
        # title
        f = QFont("Cinzel", 24)
        f.setWeight(QFont.Weight.Bold)
        f.setLetterSpacing(QFont.SpacingType.PercentageSpacing, 108)
        p.setFont(f)
        p.setPen(QColor(0, 0, 0, 160))
        p.drawText(QRectF(2, 38, W, 50), Qt.AlignmentFlag.AlignHCenter, "GOD OF WAR PSP")
        tg = QLinearGradient(0, 40, 0, 84)
        tg.setColorAt(0, QColor("#f3dd8a"))
        tg.setColorAt(0.55, QColor(GOLD))
        tg.setColorAt(1, QColor("#8a6a12"))
        p.setPen(QPen(tg, 1))
        p.drawText(QRectF(0, 36, W, 50), Qt.AlignmentFlag.AlignHCenter, "GOD OF WAR PSP")
        f2 = QFont("Cinzel", 11)
        p.setFont(f2)
        p.setPen(QColor(TEXT2))
        p.drawText(QRectF(0, 86, W, 24), Qt.AlignmentFlag.AlignHCenter, "native PC ports (unofficial)")

    # --- UI ----------------------------------------------------------------------------------------------
    def build_ui(self):
        root = QVBoxLayout(self)
        root.setContentsMargins(44, 120, 44, 34)
        root.setSpacing(0)
        self.pages = QWidget()
        self.pages.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        root.addWidget(self.pages, 1)
        # welcome
        self.p_welcome = QWidget(self.pages)
        v = QVBoxLayout(self.p_welcome)
        v.addStretch(2)
        for text, name in (("Step 1 of 3", "dim"), ("Choose the folder with your games", "bigq")):
            l = QLabel(text)
            l.setObjectName(name)
            l.setAlignment(Qt.AlignmentFlag.AlignCenter)
            v.addWidget(l)
        l = QLabel("Your own disc images (.chd or .iso) of Chains of Olympus and/or Ghost of Sparta.\n"
                   "The launcher finds each game by its disc ID, not by the file name.")
        l.setObjectName("dim")
        l.setAlignment(Qt.AlignmentFlag.AlignCenter)
        v.addSpacing(8)
        v.addWidget(l)
        v.addSpacing(22)
        b = QPushButton("Choose game folder\u2026")
        b.setObjectName("big")
        b.clicked.connect(self.choose_folder)
        self.b_choose = b
        h = QHBoxLayout()
        h.addStretch()
        h.addWidget(b)
        h.addStretch()
        v.addLayout(h)
        v.addStretch(3)
        # preparing
        self.p_prep = QWidget(self.pages)
        v = QVBoxLayout(self.p_prep)
        v.addStretch(2)
        l = QLabel("Step 2 of 3")
        l.setObjectName("dim")
        l.setAlignment(Qt.AlignmentFlag.AlignCenter)
        v.addWidget(l)
        self.prep_title = QLabel("")
        self.prep_title.setObjectName("bigq")
        self.prep_title.setAlignment(Qt.AlignmentFlag.AlignCenter)
        v.addWidget(self.prep_title)
        v.addSpacing(20)
        self.prep_prog = Progress(self)
        self.prep_prog.setMaximumWidth(620)
        h = QHBoxLayout()
        h.addStretch()
        h.addWidget(self.prep_prog, 3)
        h.addStretch()
        v.addLayout(h)
        lb = QPushButton("Show log")
        lb.setObjectName("link")
        lb.clicked.connect(self.show_log)
        h = QHBoxLayout()
        h.addStretch()
        h.addWidget(lb)
        h.addStretch()
        v.addLayout(h)
        v.addStretch(3)
        # main
        self.p_main = QWidget(self.pages)
        v = QVBoxLayout(self.p_main)
        v.setContentsMargins(0, 0, 0, 0)
        cards = QHBoxLayout()
        cards.setSpacing(18)
        self.cards = {}
        for g in self.L.GAMES:
            c = Card(self, g)
            c.clicked.connect(self.pick)
            c.activated.connect(lambda g: (self.pick(g), self.do_play()))
            cards.addWidget(c)
            self.cards[g] = c
        v.addLayout(cards, 5)
        v.addSpacing(12)
        fr = QHBoxLayout()
        l = QLabel("Game folder")
        l.setObjectName("head")
        fr.addWidget(l)
        self.folder = QLineEdit(self.cfg.get("game_folder", ""))
        self.folder.setReadOnly(True)
        self.folder.setFocusPolicy(Qt.FocusPolicy.ClickFocus)
        fr.addWidget(self.folder, 1)
        b = QPushButton("Change\u2026")
        b.clicked.connect(self.choose_folder)
        fr.addWidget(b)
        v.addLayout(fr)
        v.addStretch(1)
        self.play = QPushButton("PLAY")
        self.play.setObjectName("play")
        self.play.clicked.connect(self.do_play)
        h = QHBoxLayout()
        h.addStretch()
        h.addWidget(self.play)
        h.addStretch()
        v.addLayout(h)
        h = QHBoxLayout()
        h.addStretch()
        self.b_steam = QPushButton("Add to Steam")
        self.b_steam.setObjectName("link")
        self.b_steam.setToolTip("Adds the selected game to Steam as a non-Steam game that starts it directly. Close Steam first.")
        self.b_steam.clicked.connect(self.do_add_steam)
        h.addWidget(self.b_steam)
        self.b_lnk = QPushButton("Create desktop shortcut")
        self.b_lnk.setObjectName("link")
        self.b_lnk.setToolTip("A desktop shortcut that starts the selected game directly, with the launcher's settings.")
        self.b_lnk.clicked.connect(self.do_shortcut)
        h.addWidget(self.b_lnk)
        h.addStretch()
        v.addLayout(h)
        v.addSpacing(4)
        h = QHBoxLayout()
        h.addStretch()
        self.b_build = QPushButton("Build")
        self.b_build.clicked.connect(lambda: self.do_build())
        h.addWidget(self.b_build)
        self.prog = Progress(self)
        self.prog.hide()
        h.addWidget(self.prog)
        lb = QPushButton("Show log")
        lb.setObjectName("link")
        lb.clicked.connect(self.show_log)
        h.addWidget(lb)
        h.addStretch()
        v.addLayout(h)
        v.addSpacing(8)
        # footer
        foot = QHBoxLayout()
        foot.setContentsMargins(0, 6, 0, 0)
        k = QLabel(self.L.KEYS)
        k.setObjectName("dim")
        k.setWordWrap(True)
        k.setStyleSheet("font-size: 8pt;")
        foot.addWidget(k, 1)
        for text, fn in (("Check for updates", lambda: self.check_update(manual=True)),
                         ("Report a bug", self.report_bug), ("Open logs folder", self.open_logs),
                         ("GitHub", lambda: webbrowser.open(REPO_URL)), ("License (GPL-3.0)", self.open_license),
                         ("Third-party notices", self.open_notices)):
            b = QPushButton(text)
            b.setObjectName("link")
            b.clicked.connect(fn)
            foot.addWidget(b)
        root.addLayout(foot)
        # gear + options panel
        self.gear = QPushButton("\u2699", self)
        self.gear.setObjectName("gear")
        self.gear.setToolTip("Options (F1)")
        self.gear.setFixedSize(42, 42)
        self.gear.clicked.connect(self.toggle_options)
        self.build_options()
        self.options_open = False
        self.page_anim = None
        for pg in (self.p_welcome, self.p_prep, self.p_main):
            pg.hide()

    def build_options(self):
        panel = self.panel = QFrame(self)
        panel.setObjectName("panel")
        panel.setStyleSheet(f"QFrame#panel {{ background: rgba(24,24,24,238); border-left: 1px solid #4a3d18; }}")
        panel.hide()
        outer = QVBoxLayout(panel)
        outer.setContentsMargins(0, 0, 0, 0)
        head = QHBoxLayout()
        head.setContentsMargins(20, 16, 12, 6)
        l = QLabel("Options")
        l.setObjectName("bigq")
        head.addWidget(l)
        head.addStretch()
        head.addSpacing(56)  # the gear (which closes the panel, as Esc does) sits here
        outer.addLayout(head)
        sc = QScrollArea()
        sc.setWidgetResizable(True)
        sc.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        body = QWidget()
        sc.setWidget(body)
        outer.addWidget(sc, 1)
        v = QVBoxLayout(body)
        v.setContentsMargins(20, 4, 20, 20)
        l = QLabel("Preset")
        l.setObjectName("head")
        v.addWidget(l)
        row = QGridLayout()
        self.preset_btn = {}
        for i, name in enumerate(PRESETS):
            b = QPushButton(name)
            b.setObjectName("preset")
            b.clicked.connect(lambda _=False, n=name: self.apply_preset(n))
            row.addWidget(b, 0, i)
            self.preset_btn[name] = b
        v.addLayout(row)
        self.custom_lbl = QLabel("Custom")
        v.addWidget(self.custom_lbl)
        v.addSpacing(8)
        l = QLabel("Display")
        l.setObjectName("head")
        v.addWidget(l)
        grid = QGridLayout()
        grid.setColumnStretch(1, 1)
        self.combo = {}
        c = self.cfg
        rows = [("Resolution", "res", RES, int(c["res_scale"])), ("Anti-aliasing", "aa", AA, (1, True) if c["fxaa"] else (int(c["msaa"]), False)),
                ("Frame rate", "fps", FPS, int(c["fps"])), ("Aspect ratio", "aspect", ASPECT, c["aspect"]),
                ("HDR", "hdr", HDR, bool(c["hdr"])), ("Window size", "window", WINDOW, int(c["window_scale"]))]
        for i, (lab, key, table, val) in enumerate(rows):
            grid.addWidget(QLabel(lab), i, 0)
            cb = QComboBox()
            cb.addItems([t[0] for t in table])
            cb.setCurrentText(label_of(table, val))
            cb.currentTextChanged.connect(self.on_option)
            grid.addWidget(cb, i, 1)
            self.combo[key] = (cb, table)
        v.addLayout(grid)
        self.combo["fps"][0].setToolTip("Above 60 the game runs smoother if your PC holds that rate every frame. If it can't, "
                                        "the game skips ahead to keep time and the sound gets choppy: go back to 60.")
        self.chk = {}
        for key, text in (("fullscreen", "Fullscreen"),):
            ch = QCheckBox(text)
            ch.setChecked(bool(c[key]))
            ch.toggled.connect(self.on_option)
            v.addWidget(ch)
            self.chk[key] = ch
        v.addSpacing(8)
        self.adv_btn = QPushButton("Advanced  \u25b8")
        self.adv_btn.setCheckable(True)
        self.adv_btn.toggled.connect(self.toggle_adv)
        v.addWidget(self.adv_btn)
        self.adv = QWidget()
        a = QGridLayout(self.adv)
        a.setContentsMargins(0, 6, 0, 0)
        a.setColumnStretch(1, 1)
        arows = [("Renderer", "renderer", RENDERER, c["renderer"]), ("Effects resolution", "fx", FX, 1 if int(c["fx_scale"]) == 1 else 0),
                 ("Anisotropic filtering", "aniso", ANISO, int(c["aniso"])), ("Graphics API", "api", GPU_API, c.get("gpu_api", ""))]
        for i, (lab, key, table, val) in enumerate(arows):
            a.addWidget(QLabel(lab), i, 0)
            cb = QComboBox()
            cb.addItems([t[0] for t in table])
            cb.setCurrentText(label_of(table, val))
            cb.currentTextChanged.connect(self.on_option)
            a.addWidget(cb, i, 1)
            self.combo[key] = (cb, table)
        self.combo["api"][0].setToolTip("Which graphics API the Hardware renderer uses. If the game crashes or draws wrongly "
                                        "on your graphics card, try the other one.")
        a.addWidget(QLabel("Sharpening"), 4, 0)
        sh = QHBoxLayout()
        self.sharpen = QSlider(Qt.Orientation.Horizontal)
        self.sharpen.setRange(0, 100)
        self.sharpen.setValue(int(round(float(c["sharpen"]) * 100)))
        self.sharpen_lbl = QLabel(f"{self.sharpen.value() / 100:.2f}")
        self.sharpen_lbl.setMinimumWidth(34)
        self.sharpen.valueChanged.connect(lambda x: (self.sharpen_lbl.setText(f"{x / 100:.2f}"), self.on_option()))
        sh.addWidget(self.sharpen, 1)
        sh.addWidget(self.sharpen_lbl)
        a.addLayout(sh, 4, 1)
        r = 5
        for key, text in (("vsync", "V-Sync"), ("mute", "Mute")):
            ch = QCheckBox(text)
            ch.setChecked(bool(c[key]))
            ch.toggled.connect(self.on_option)
            a.addWidget(ch, r, 0, 1, 2)
            self.chk[key] = ch
            r += 1
        a.addWidget(QLabel("Texture pack folder"), r, 0, 1, 2)
        tp = QHBoxLayout()
        self.tpack = QLineEdit(c["texture_pack"])
        self.tpack.editingFinished.connect(self.on_option)
        tp.addWidget(self.tpack, 1)
        b = QPushButton("\u2026")
        b.clicked.connect(lambda: (self.tpack.setText(QFileDialog.getExistingDirectory(self, "Texture pack folder") or self.tpack.text()),
                                   self.on_option()))
        tp.addWidget(b)
        a.addLayout(tp, r + 1, 0, 1, 2)
        self.tpack_lbl = QLabel("")
        self.tpack_lbl.setObjectName("dim")
        self.tpack_lbl.setWordWrap(True)
        self.tpack_lbl.setStyleSheet("font-size: 8pt;")
        a.addWidget(self.tpack_lbl, r + 2, 0, 1, 2)
        ch = QCheckBox("Dump textures (to make a pack)")
        ch.setToolTip("Writes each new texture as a .png into build/texture_dump/<game>. Edit or upscale them, put them in a "
                      "folder and choose it as the texture pack folder. PPSSPP texture packs don't work here.")
        ch.setChecked(bool(c.get("texture_dump")))
        ch.toggled.connect(self.on_option)
        a.addWidget(ch, r + 3, 0, 1, 2)
        self.chk["texture_dump"] = ch
        cb = QPushButton("Controls\u2026")
        cb.setToolTip("Keyboard and gamepad bindings")
        cb.clicked.connect(lambda: ControlsDialog(self).exec())
        a.addWidget(cb, r + 5, 0, 1, 2)
        self.upd = QCheckBox("Check for launcher updates at start")
        self.upd.setChecked(bool(c.get("check_updates", True)))
        self.upd.toggled.connect(lambda on: (self.cfg.__setitem__("check_updates", on), self.collect()))
        a.addWidget(self.upd, r + 6, 0, 1, 2)
        self.rm = QCheckBox("Reduce motion (launcher animations)")
        self.rm.setChecked(self.reduce_motion)
        self.rm.toggled.connect(self.set_reduce_motion)
        a.addWidget(self.rm, r + 4, 0, 1, 2)
        self.update_tpack()
        self.adv.hide()
        v.addWidget(self.adv)
        v.addStretch()
        self.update_presets()

    def update_tpack(self):
        ok, text = self.L.texture_pack_status(self.tpack.text().strip())
        self.tpack_lbl.setText(text)
        self.tpack_lbl.setStyleSheet(f"font-size: 8pt; color: {'#9fd18b' if ok else GOLD};")

    def set_reduce_motion(self, on):
        self.reduce_motion = on
        self.cfg["reduce_motion"] = on
        self.particles = []
        self.L.save_config(self.cfg)

    def toggle_adv(self, on):
        self.adv.setVisible(on)
        self.adv_btn.setText("Advanced  \u25be" if on else "Advanced  \u25b8")

    def panel_rect(self, open_):
        w = min(440, int(self.width() * 0.42))
        return (self.width() - (w if open_ else 0), 0, w, self.height())

    def toggle_options(self):
        self.options_open = not self.options_open
        x1, y, w, h = self.panel_rect(self.options_open)
        x0 = self.panel_rect(not self.options_open)[0]
        self.panel.setGeometry(x0, y, w, h)
        self.panel.show()
        self.panel.raise_()
        self.gear.raise_()
        if self.reduce_motion:
            self.panel.move(x1, y)
            self.panel.setVisible(self.options_open)
            return
        an = QPropertyAnimation(self.panel, b"pos", self)
        an.setDuration(320)
        an.setEasingCurve(QEasingCurve.Type.OutCubic)
        an.setStartValue(QPoint(x0, y))
        an.setEndValue(QPoint(x1, y))
        if not self.options_open:
            an.finished.connect(self.panel.hide)
        an.start()
        self.panel_anim = an
        if self.options_open:
            self.preset_btn[next(iter(PRESETS))].setFocus()

    def resizeEvent(self, e):
        super().resizeEvent(e)
        self.cache_dirty = True
        self.gear.move(self.width() - 70, 22)
        x, y, w, h = self.panel_rect(self.options_open)
        self.panel.setGeometry(x, y, w, h)
        for pg in (self.p_welcome, self.p_prep, self.p_main):
            pg.setGeometry(0, 0, self.pages.width(), self.pages.height())

    def show_view(self, name, animate=True):
        pages = {"welcome": self.p_welcome, "prep": self.p_prep, "main": self.p_main}
        new = pages[name]
        old = pages.get(self.view)
        self.view = name
        self.gear.setVisible(name == "main")
        new.setGeometry(0, 0, self.pages.width(), self.pages.height())
        if old is new:
            return
        if not animate or self.reduce_motion or old is None:
            if old:
                old.hide()
            new.show()
            new.setGraphicsEffect(None)
            return
        eff_in = QGraphicsOpacityEffect(new)
        new.setGraphicsEffect(eff_in)
        new.show()
        new.raise_()
        grp = QParallelAnimationGroup(self)
        a1 = QPropertyAnimation(eff_in, b"opacity")
        a1.setDuration(420)
        a1.setStartValue(0.0)
        a1.setEndValue(1.0)
        a2 = QPropertyAnimation(new, b"pos")
        a2.setDuration(420)
        a2.setEasingCurve(QEasingCurve.Type.OutCubic)
        a2.setStartValue(QPoint(60, 0))
        a2.setEndValue(QPoint(0, 0))
        grp.addAnimation(a1)
        grp.addAnimation(a2)
        eff_out = QGraphicsOpacityEffect(old)
        old.setGraphicsEffect(eff_out)
        a3 = QPropertyAnimation(eff_out, b"opacity")
        a3.setDuration(300)
        a3.setStartValue(1.0)
        a3.setEndValue(0.0)
        grp.addAnimation(a3)

        def done():
            old.hide()
            old.setGraphicsEffect(None)
            new.setGraphicsEffect(None)
        grp.finished.connect(done)
        grp.start()
        self.page_anim = grp

    # --- options state -----------------------------------------------------------------------------------
    def collect(self):
        c = self.cfg
        get = lambda k: value_of(self.combo[k][1], self.combo[k][0].currentText())
        c["res_scale"] = get("res")
        c["msaa"], c["fxaa"] = get("aa")
        c["fps"], c["aspect"], c["hdr"], c["window_scale"] = get("fps"), get("aspect"), get("hdr"), get("window")
        c["renderer"], c["fx_scale"], c["aniso"] = get("renderer"), get("fx"), get("aniso")
        c["gpu_api"] = get("api")
        c["sharpen"] = round(self.sharpen.value() / 100, 2)
        for k, ch in self.chk.items():
            c[k] = ch.isChecked()
        c["texture_pack"] = self.tpack.text()
        if hasattr(self, "tpack_lbl"):
            self.update_tpack()
        c["preset"] = self.preset
        c["game_folder"] = self.folder.text()
        c["reduce_motion"] = self.reduce_motion
        self.L.save_config(c)
        return c

    def on_option(self, *_):
        if self.applying:
            return
        self.preset = "Custom"
        self.collect()
        self.update_presets()

    def apply_preset(self, name):
        p = PRESETS[name]
        self.applying = True
        self.combo["res"][0].setCurrentText(label_of(RES, p["res_scale"]))
        self.combo["aa"][0].setCurrentText(label_of(AA, (p["msaa"], p["fxaa"])))
        self.combo["aniso"][0].setCurrentText(label_of(ANISO, p["aniso"]))
        self.applying = False
        self.preset = name
        self.collect()
        self.update_presets()

    def update_presets(self):
        for n, b in self.preset_btn.items():
            b.setProperty("on", "true" if n == self.preset else "false")
            b.style().unpolish(b)
            b.style().polish(b)
        self.custom_lbl.setStyleSheet(f"color: {GOLD if self.preset == 'Custom' else '#555'};")

    def auto_preset(self):
        discrete, hz = detect_pc()
        self.apply_preset("High" if discrete else "Balanced")
        self.applying = True
        self.combo["hdr"][0].setCurrentText("Auto")
        # the frame rate stays at the original 60 even on a high-refresh display: above 60, a PC that can't hold the
        # rate makes the game skip ahead, which costs sound (issue #3); players can raise it themselves
        self.applying = False
        self.collect()

    # --- games -------------------------------------------------------------------------------------------
    def game_state(self, g):
        disc = self.cfg["disc"].get(g, "")
        if not disc or not Path(disc).is_file():
            return "No disc image found"
        return "Ready to play" if self.L.exe_path(g).exists() else "Needs building"

    def load_found_discs(self):
        for g in self.L.GAMES:
            d = self.cfg["disc"].get(g, "")
            if d and Path(d).is_file() and not self.no_art and self.L.disc_tool().exists():
                self.load_art(g, d)

    def load_art(self, g, disc):
        files = self.L.extract_art(g, disc)
        c = self.cards[g]
        c.pix = QPixmap(str(files["PIC1.PNG"])) if "PIC1.PNG" in files else None
        c.icon = QPixmap(str(files["ICON0.PNG"])) if "ICON0.PNG" in files else None

    def pick(self, g):
        self.cfg["game"] = g
        self.collect()
        self.set_bg_game(g)
        self.refresh()

    def cycle_game(self, d, focus=False):
        keys = list(self.L.GAMES)
        g = keys[(keys.index(self.cfg.get("game", "coo")) + d) % len(keys)]
        self.pick(g)
        if focus:
            self.cards[g].setFocus()

    def play_reason(self):
        g = self.cfg.get("game", "coo")
        st = self.game_state(g)
        if self.busy:
            return "Wait for the build to finish."
        if self.game_running():
            return "The game is running."
        if st == "No disc image found":
            return f"No disc image of {self.L.GAMES[g]['title']} in the game folder."
        if st == "Needs building":
            return "Press Build first (one time, about 10 minutes)."
        return ""

    def refresh(self):
        sel = self.cfg.get("game", "coo")
        for g, c in self.cards.items():
            c.selected = g == sel
            c.status = self.game_state(g)
            c.set_lift(c._lift)
        reason = self.play_reason()
        self.play.setEnabled(not reason)
        self.play.setToolTip(reason or f"Play {self.L.GAMES[sel]['title']} (Enter)")
        st = self.game_state(sel)
        self.b_build.setEnabled(st != "No disc image found" and not self.busy)
        self.b_build.setText("Rebuild" if st == "Ready to play" else "Build")
        self.b_build.setVisible(not self.busy)
        self.prog.setVisible(self.busy)
        for b in (self.b_steam, self.b_lnk):
            b.setEnabled(st == "Ready to play")
            b.setText(b.text().split("  (")[0] + ("" if st == "Ready to play" else ""))

    # --- actions -----------------------------------------------------------------------------------------
    def log(self, line):
        self.log_lines.append(line)
        if self.log_win is not None:
            self.bridge.call.emit(lambda: self.log_text.appendPlainText(line))

    def show_log(self):
        if self.log_win is None:
            self.log_win = QWidget()
            self.log_win.setWindowTitle("Launcher log")
            self.log_win.setStyleSheet(STYLE + f"QWidget {{ background: {BG}; }}")
            self.log_win.resize(1000, 600)
            lay = QVBoxLayout(self.log_win)
            self.log_text = QPlainTextEdit()
            self.log_text.setReadOnly(True)
            lay.addWidget(self.log_text)
            self.log_text.setPlainText("\n".join(self.log_lines))
        self.log_win.show()
        self.log_win.raise_()

    def ask(self, text):
        answer, done = [False], threading.Event()

        def show():
            answer[0] = QMessageBox.question(self, "Install build tools", text) == QMessageBox.StandardButton.Yes
            done.set()
        self.bridge.call.emit(show)
        done.wait()
        return answer[0]

    def choose_folder(self):
        if self.busy:
            return
        d = QFileDialog.getExistingDirectory(self, "Choose the folder with your games", self.folder.text())
        if not d:
            return
        self.folder.setText(d)
        self.collect()
        first = self.view == "welcome"
        self.busy = True
        if not self.L.disc_tool().exists():
            self.prep("Preparing the disc reader (one time, about 2 minutes)")
            self.prep_prog.set(0.05, "Building gow_disc")
        threading.Thread(target=self.scan_thread, args=(d, first), daemon=True).start()

    def scan_thread(self, folder, first):
        ok = self.L.ensure_disc_tool(self.log, self.ask)
        found = self.L.scan_folder(folder, self.log) if ok else {}
        self.bridge.call.emit(lambda: self.scan_done(found, ok, first))

    def scan_done(self, found, ok, first):
        self.busy = False
        if not ok:
            self.show_view("welcome" if first else "main")
            QMessageBox.critical(self, "Game folder", "The disc reader could not be built. See Show log.")
            return
        for g in self.L.GAMES:
            if g in found:
                self.cfg["disc"][g] = found[g]
                if not self.no_art:
                    self.load_art(g, found[g])
            else:
                self.cfg["disc"].pop(g, None)
                self.cards[g].pix = self.cards[g].icon = None
        if found and self.cfg.get("game") not in found:
            self.cfg["game"] = next(iter(found))
        self.collect()
        if not found:
            QMessageBox.warning(self, "Game folder", "No disc image of either game was found in this folder (.chd or .iso).")
        g = self.cfg.get("game", "coo")
        if first and found and self.game_state(g) == "Needs building":
            self.do_build(first_run=True)
            return
        self.show_view("main")
        self.refresh()

    def prep(self, title):
        self.prep_title.setText(title)
        self.prep_prog.set(0, "Starting")
        self.show_view("prep")

    def do_build(self, first_run=False):
        if self.busy and not first_run:
            return
        g = self.cfg.get("game", "coo")
        c = self.collect()
        try:
            self.L.check_disc(c, g)
        except ValueError as e:
            QMessageBox.critical(self, "Build", str(e))
            return
        self.busy = True
        if first_run or self.view == "prep":
            self.prep(f"Preparing {self.L.GAMES[g]['title']} (one time, a few minutes)")
        gen = len(list(self.L.gen_dir(g).glob("*.c"))) or 120
        self.pstate = {"stage": "Starting", "n": 0, "total": gen + len(list((self.L.ROOT / "runtime").glob("*.c*"))), "p": 0.0}
        self.refresh()
        threading.Thread(target=lambda: self.build_done(self.L.run_build(c, g, self.build_line, self.ask), g), daemon=True).start()

    def build_line(self, line):
        self.log(line)
        st = self.pstate
        if line.startswith("> "):
            if "gow_disc.exe" in line:
                st["stage"], st["p"] = "Decrypting", 0.01
            elif "--recomp" in line or "recomp.py" in line:
                st["stage"], st["p"] = "Recompiling", 0.04
            elif " --build " in line:
                st["stage"], st["p"] = "Compiling", 0.2
            elif " -S " in line:
                st["stage"], st["p"] = "Configuring", 0.16
        elif line.startswith("downloading "):
            st["stage"], st["p"] = "Downloading build tools", 0.005
        elif st["stage"] == "Compiling" and re.match(r"^\[(\d+)/(\d+)\] ", line):  # Ninja (portable toolchain)
            done, total = map(int, re.match(r"^\[(\d+)/(\d+)\] ", line).groups())
            st["p"] = 0.2 + 0.74 * done / max(1, total)
        elif st["stage"] == "Compiling" and re.match(r"^\s+[\w.\-]+\.(c|cpp)$", line):
            st["n"] += 1
            st["p"] = 0.2 + 0.74 * min(1.0, st["n"] / max(1, st["total"]))
            if st["n"] >= st["total"]:
                st["stage"] = "Linking"
        elif "Generating code" in line or ("gow_rt" in line and "->" in line):
            st["stage"], st["p"] = "Linking", 0.96
        elif "skipping recomp" in line:
            st["p"] = 0.15
        text = f"Compiling {st['n']}/{st['total']}" if st["stage"] == "Compiling" else st["stage"]
        p = st["p"]
        self.bridge.call.emit(lambda: (self.prog.set(p, text), self.prep_prog.set(p, text)))

    def build_done(self, ok, g):
        def done():
            self.prog.set(1.0, "Done" if ok else "Failed")
            self.prep_prog.set(1.0, "Done" if ok else "Failed")
            self.busy = False
            self.show_view("main")
            self.refresh()
            if not ok:
                QMessageBox.critical(self, "Build", "The build failed. See Show log for the details.")
            else:
                self.maybe_render_menus()
        self.bridge.call.emit(done)

    def do_play(self):
        if self.play_reason():
            return
        g = self.cfg.get("game", "coo")
        try:
            self.game_proc = self.L.start_game(self.collect(), g)
            self.log("started: " + subprocess.list2cmdline(self.L.play_command(self.cfg, g)))
            self.log(f"game log (attach it to bug reports): {self.L.game_log(g)}")
        except ValueError as e:
            QMessageBox.critical(self, "Play", str(e))
            return
        # While the game runs the launcher costs nothing: no animation, minimized (a maximized launcher animating
        # its background competed with the game on a weaker PC: movie stutter). It comes back when the game exits.
        self.was_maximized = self.isMaximized()
        self.timer.stop()
        self.showMinimized()
        self.refresh()
        self.game_watch = QTimer(self)
        self.game_watch.timeout.connect(self.check_game)
        self.game_watch.start(500)

    def game_running(self):
        return getattr(self, "game_proc", None) is not None and self.game_proc.poll() is None

    def check_game(self):
        if self.game_running():
            return
        code = self.game_proc.returncode if getattr(self, "game_proc", None) else 0
        self.game_watch.stop()
        self.game_proc = None
        self.log(f"game exited (code {code})")
        self.showMaximized() if getattr(self, "was_maximized", False) else self.showNormal()
        self.raise_()
        self.activateWindow()
        if not self.timer.isActive():
            self.timer.start(FRAME_MS)
        self.cache_dirty = True
        self.refresh()
        if code:
            self.game_failed(code)

    def game_failed(self, code):
        """The game stopped with an error or crashed: say so, show why (from its log) and offer what may help."""
        g = self.cfg.get("game", "coo")
        why = self.L.crash_summary(g)
        for line in why:
            self.log("game log: " + line)
        text = f"{self.L.GAMES[g]['title']} closed unexpectedly (exit code {code & 0xFFFFFFFF:#x})."
        if why:
            text += "\n\nFrom its log:\n" + "\n".join(why)
        box = QMessageBox(QMessageBox.Icon.Warning, "The game closed", text, parent=self)
        b_aniso = b_api = None
        if self.cfg.get("renderer") == "gpu" and int(self.cfg.get("aniso", 1)) > 1:
            b_aniso = box.addButton("Turn off anisotropic filtering", QMessageBox.ButtonRole.AcceptRole)
        elif self.cfg.get("renderer") == "gpu":
            b_api = box.addButton("Try " + ("Direct3D 12" if self.cfg.get("gpu_api") == "vulkan" else "Vulkan"),
                                  QMessageBox.ButtonRole.AcceptRole)
        b_log = box.addButton("Open the log", QMessageBox.ButtonRole.ActionRole)
        b_bug = box.addButton("Report a bug", QMessageBox.ButtonRole.ActionRole)
        box.addButton(QMessageBox.StandardButton.Close)
        box.exec()
        hit = box.clickedButton()
        if hit is b_aniso:
            self.combo["aniso"][0].setCurrentText(label_of(ANISO, 1))
        elif hit is b_api:
            self.combo["api"][0].setCurrentText(label_of(GPU_API, "direct3d12" if self.cfg.get("gpu_api") == "vulkan" else "vulkan"))
        elif hit is b_log:
            QDesktopServices.openUrl(QUrl.fromLocalFile(str(self.L.game_log(g))))
        elif hit is b_bug:
            self.report_bug()

    def do_shortcut(self):
        g = self.cfg.get("game", "coo")
        try:
            p = self.L.create_desktop_shortcut(g)
        except (OSError, subprocess.TimeoutExpired) as e:
            QMessageBox.critical(self, "Desktop shortcut", f"The shortcut could not be created:\n{e}")
            return
        self.log(f"desktop shortcut: {p}")
        QMessageBox.information(self, "Desktop shortcut", f"Created {p}.\nIt starts {self.L.GAMES[g]['title']} directly, with "
                                "the settings chosen here.")

    def do_add_steam(self):
        import steam_shortcuts
        g = self.cfg.get("game", "coo")
        while True:
            try:
                vdf, backup, state = self.L.add_to_steam(g)
                break
            except steam_shortcuts.SteamError as e:
                if QMessageBox.warning(self, "Add to Steam", str(e), QMessageBox.StandardButton.Retry |
                                       QMessageBox.StandardButton.Cancel) != QMessageBox.StandardButton.Retry:
                    return
            except (OSError, ValueError) as e:
                QMessageBox.critical(self, "Add to Steam", f"Steam's list of non-Steam games could not be changed:\n{e}")
                return
        self.log(f"Steam: {state} {self.L.shortcut_name(g)} in {vdf} (backup: {backup or 'none, new file'})")
        QMessageBox.information(self, "Add to Steam", f"{self.L.GAMES[g]['title']} was {state} in Steam's library. Start Steam "
                                "to see it.\n\n" + (f"The previous list was saved as {backup.name}." if backup else ""))

    def check_update(self, manual=False):
        """Asks GitHub for the latest release in the background; offline or failing stays quiet unless asked."""
        import launcher_update as U

        def work():
            try:
                info, err = U.check(self.L.VERSION), None
            except U.UpdateError as e:
                info, err = None, str(e)
            self.bridge.call.emit(lambda: self.update_found(info, err, manual))
        threading.Thread(target=work, daemon=True).start()

    def update_found(self, info, err, manual):
        if err:
            self.log(f"update check: {err}")
            if manual:
                QMessageBox.warning(self, "Updates", f"Couldn't check for updates: {err}")
            return
        if not info:
            self.log(f"update check: up to date (v{self.L.VERSION})")
            if manual:
                QMessageBox.information(self, "Updates", f"This launcher (v{self.L.VERSION}) is the latest.")
            return
        if not manual and self.cfg.get("skip_version") == info["version"]:
            return
        self.log(f"update check: v{info['version']} available")
        d = QDialog(self)
        d.setWindowTitle(f"Launcher v{info['version']}")
        d.setStyleSheet(STYLE + f"QDialog {{ background: {BG}; }}")
        d.resize(720, 520)
        lay = QVBoxLayout(d)
        l = QLabel(f"Version {info['version']} is out (you have {self.L.VERSION}). What's new:")
        l.setObjectName("head")
        lay.addWidget(l)
        notes = QPlainTextEdit(info["notes"])
        notes.setReadOnly(True)
        lay.addWidget(notes, 1)
        l = QLabel("Updating downloads the release ZIP, checks it against the release's SHA256SUMS.txt, replaces this "
                   "launcher and restarts it. GoW_PSP_data (builds, saves, settings) stays as it is."
                   if self.L.FROZEN else "Running from source: update with git pull, or download the release.")
        l.setObjectName("dim")
        l.setWordWrap(True)
        lay.addWidget(l)
        self.upd_prog = Progress(self)
        self.upd_prog.hide()
        lay.addWidget(self.upd_prog)
        h = QHBoxLayout()
        skip = QPushButton("Skip this version")
        h.addWidget(skip)
        h.addStretch()
        later = QPushButton("Later")
        h.addWidget(later)
        go = QPushButton("Update now" if self.L.FROZEN else "Open the release page")
        go.setObjectName("big")
        h.addWidget(go)
        lay.addLayout(h)
        skip.clicked.connect(lambda: (self.cfg.__setitem__("skip_version", info["version"]), self.collect(), d.reject()))
        later.clicked.connect(d.reject)
        go.clicked.connect(lambda: self.do_update(info, d, (go, skip, later)))
        self.upd_dialog = d
        d.exec()

    def do_update(self, info, dialog, buttons):
        import launcher_update as U
        if not self.L.FROZEN:
            webbrowser.open(info["page"] or REPO_URL + "/releases/latest")
            dialog.accept()
            return
        if self.busy or self.game_running():
            QMessageBox.warning(self, "Update", "Wait for the build or the game to finish first.")
            return
        for b in buttons:
            b.setEnabled(False)
        self.upd_prog.show()
        self.upd_prog.set(0.02, "Downloading")

        def prog(done, total):
            p = done / total if total else 0.5
            self.bridge.call.emit(lambda: (self.upd_prog.set(min(0.97, p), f"Downloading {done / 2**20:.0f} MB"),
                                           self.upd_prog.tick(0)))

        def work():
            try:
                new = U.download(info, self.L.update_dir(), prog)
                self.bridge.call.emit(lambda: self.finish_update(new, dialog, buttons, None))
            except U.UpdateError as e:
                self.bridge.call.emit(lambda: self.finish_update(None, dialog, buttons, str(e)))
        threading.Thread(target=work, daemon=True).start()

    def finish_update(self, new, dialog, buttons, err):
        import launcher_update as U
        if not err:
            self.upd_prog.set(1.0, "Verified; restarting")
            self.collect()
            try:
                U.apply(new, sys.executable)
            except U.UpdateError as e:
                err = str(e)
        if err:
            self.log(f"update failed: {err}")
            for b in buttons:
                b.setEnabled(True)
            self.upd_prog.hide()
            QMessageBox.critical(self, "Update", f"The update failed: {err}\nThe current launcher keeps working.")
            return
        self.log(f"updated to v{dialog.windowTitle().split('v')[-1]}; restarting")
        dialog.accept()
        QApplication.instance().quit()

    def open_logs(self):
        d = self.L.logs_dir()
        d.mkdir(parents=True, exist_ok=True)
        QDesktopServices.openUrl(QUrl.fromLocalFile(str(d)))

    def report_bug(self):
        """Zips the last Play's log, recorded input, settings and PC info (no game files), shows the zip in Explorer
        and opens GitHub's new-issue form, filled in with the game, version and PC info."""
        g = self.cfg.get("game", "coo")
        self.setCursor(Qt.CursorShape.WaitCursor)
        try:
            z = self.L.make_bug_report(self.collect(), g, self.log_lines[-400:])
            info = self.L.pc_info()
        finally:
            self.unsetCursor()
        self.log(f"bug report: {z}")
        if os.name == "nt":
            subprocess.Popen(["explorer", "/select,", str(z)])
        has_log = self.L.game_log(g).exists()
        QMessageBox.information(self, "Report a bug",
                                f"The report is ready:\n{z}\n\n"
                                + ("" if has_log else f"(No game log yet: play {self.L.GAMES[g]['title']} once and report right "
                                   "after the problem.)\n\n")
                                + "Your browser opens GitHub's bug form next. Describe what happened and drag the zip into the "
                                  "form. It holds the game's log, your button presses, the settings and PC info; no game files.")
        webbrowser.open(self.L.new_issue_url(g, info))

    def open_license(self):
        f = self.L.ROOT / "LICENSE"
        QDesktopServices.openUrl(QUrl.fromLocalFile(str(f)) if f.exists() else QUrl(REPO_URL + "/blob/main/LICENSE"))

    def open_notices(self):
        QDesktopServices.openUrl(QUrl.fromLocalFile(str(assets_dir() / "THIRD_PARTY_NOTICES.txt")))

    def closeEvent(self, e):
        g = self.geometry()
        self.cfg["geometry"] = f"{g.width()}x{g.height()}+{g.x()}+{g.y()}"
        self.collect()
        if self.log_win:
            self.log_win.close()
        super().closeEvent(e)


def run(L):
    if sys.platform == "win32":  # its own taskbar entry and icon, not grouped with python.exe
        import ctypes
        ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID("DrummingBird1.GoWPSP.Launcher")
    app = QApplication.instance() or QApplication(sys.argv)
    app.setWindowIcon(QIcon(str(assets_dir() / "icon.ico")))
    for f in assets_dir().glob("Cinzel*.ttf"):
        QFontDatabase.addApplicationFont(str(f))
    w = Window(L)
    w.show()
    shot = os.environ.get("GOW_UI_SHOT_OPTIONS")
    if shot:
        QTimer.singleShot(600, w.toggle_options)
        if shot == "advanced":
            QTimer.singleShot(700, lambda: w.adv_btn.setChecked(True))
    rec = os.environ.get("GOW_UI_RECORD")  # dir: grab the window itself (offscreen render) for a short clip
    if rec:
        Path(rec).mkdir(parents=True, exist_ok=True)
        frames, t_start = [], [0.0]
        c2 = w.cards[list(w.cards)[1]]
        length = float(os.environ.get("GOW_UI_RECORD_SECONDS", "9"))
        script = [(2.0, lambda: c2.hover_to(1.0)), (3.3, lambda: c2.hover_to(0.0)), (4.2, w.toggle_options),
                  (6.0, lambda: w.adv_btn.setChecked(True)), (7.4, w.toggle_options)]
        if length > 9:  # a longer tour, repeated: cards, switching games, options, the welcome page
            cycle = [(1.5, lambda: c2.hover_to(1.0)), (3.0, lambda: c2.hover_to(0.0)), (4.0, lambda: w.cycle_game(1)),
                     (7.0, lambda: w.cycle_game(-1)), (9.5, w.toggle_options), (11.5, lambda: w.adv_btn.setChecked(True)),
                     (14.0, lambda: w.adv_btn.setChecked(False)), (15.0, w.toggle_options),
                     (17.0, lambda: w.show_view("welcome")), (21.0, lambda: w.show_view("main"))]
            script = [(k * 23.0 + t, f) for k in range(int(length // 23) + 1) for t, f in cycle if k * 23.0 + t < length]

        def grab():
            el = time.monotonic() - t_start[0]
            while script and el >= script[0][0]:
                script.pop(0)[1]()
            frames.append((el, w.grab().toImage().scaledToWidth(1100, Qt.TransformationMode.SmoothTransformation)))
            if el >= length:
                tm.stop()
                with open(Path(rec) / "times.txt", "w") as f:
                    for i, (e, im) in enumerate(frames):
                        im.save(str(Path(rec) / f"f{i:04d}.png"))
                        f.write(f"file 'f{i:04d}.png'\nduration {((frames[i + 1][0] if i + 1 < len(frames) else e + 0.033) - e):.4f}\n")
                app.quit()
        tm = QTimer(w)
        tm.timeout.connect(grab)
        QTimer.singleShot(800, lambda: (t_start.__setitem__(0, time.monotonic()), tm.start(16 if length <= 9 else 40)))
    app.exec()