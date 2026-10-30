"""Checks of the launcher that need no game data and no screen, for CI and before a release:

1. every language table in launcher_i18n.py has the same texts;
2. the window opens in every language (laid out but never shown, with a fresh settings folder, so no disc and no
   art) and each label, button, check box and combo on the welcome page, the main page, Options and Advanced shows
   its whole text inside the window;
3. "Report a bug" zips the game log, the recorded input and the frame timings (cut to the last PERF_ROWS rows).

Exits 1 when anything fails. Usage: python tools/check_launcher.py [language ...]
"""
import importlib.machinery
import importlib.util
import json
import os
import sys
import tempfile
import zipfile
from pathlib import Path

# Windows' own platform plugin, for its real fonts (the offscreen plugin's font metrics are about twice too wide);
# the window is laid out but never shown (WA_DontShowOnScreen). Elsewhere: offscreen.
os.environ.setdefault("QT_QPA_PLATFORM", "windows" if sys.platform == "win32" else "offscreen")
TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
_loader = importlib.machinery.SourceFileLoader("launcher", str(TOOLS / "launcher.pyw"))
L = importlib.util.module_from_spec(importlib.util.spec_from_loader("launcher", _loader))
sys.modules["launcher"] = L
_loader.exec_module(L)
import launcher_i18n  # noqa: E402

SLACK = 4  # px: flat buttons draw their text with less padding than estimated here


def check_tables():
    keys = set(launcher_i18n.STRINGS["he"])
    bad = []
    for lang, table in launcher_i18n.STRINGS.items():
        missing, extra = keys - set(table), set(table) - keys
        if missing or extra:
            bad.append(f"{lang}: missing {sorted(missing)}, not in the Hebrew table {sorted(extra)}")
    codes = [c for _, c in launcher_i18n.LANGS]
    bad += [f"{c}: in LANGS but has no table" for c in codes if c != "en" and c not in launcher_i18n.STRINGS]
    bad += [f"{c}: has a table but is not in LANGS" for c in launcher_i18n.STRINGS if c not in codes]
    bad += [f"{c}: not an allowed ui_language in launcher.pyw (ALLOWED)" for c in codes if c not in L.ALLOWED["ui_language"]]
    return bad


def cut_off(w, seen):
    from PySide6.QtCore import QPoint
    from PySide6.QtWidgets import QAbstractButton, QCheckBox, QComboBox, QLabel
    out = []
    for cls in (QLabel, QAbstractButton, QComboBox):
        for x in w.findChildren(cls):
            if not x.isVisible():
                continue
            seen.add(id(x))
            fm = x.fontMetrics()
            if isinstance(x, QComboBox):
                text = x.currentText()
                need, have = fm.horizontalAdvance(text) + 30, x.width()
            elif isinstance(x, QLabel):
                if x.wordWrap() or not x.text() or "<" in x.text() or not x.pixmap().isNull():
                    continue
                text = max(x.text().splitlines(), key=fm.horizontalAdvance)
                need, have = fm.horizontalAdvance(text), x.contentsRect().width()
            else:
                if not x.text():
                    continue
                text = x.text().replace("&", "")
                need, have = fm.horizontalAdvance(text) + (24 if isinstance(x, QCheckBox) else 12), x.width()
            if need > have + SLACK:
                out.append(f"{type(x).__name__} {need - have}px too narrow: {text}")
            left = x.mapTo(w, QPoint(0, 0)).x()
            if left < -SLACK or left + x.width() > w.width() + SLACK:  # a widget that grew past the window's edge
                out.append(f"{type(x).__name__} outside the window (x {left}, width {x.width()} of {w.width()}): {text}")
    return out


def check_window(langs):
    from PySide6.QtCore import Qt
    from PySide6.QtGui import QFontDatabase
    from PySide6.QtWidgets import QApplication
    app = QApplication.instance() or QApplication([])
    import launcher_qt
    for f in launcher_qt.assets_dir().glob("Cinzel*.ttf"):
        QFontDatabase.addApplicationFont(str(f))
    bad = []
    for lang in langs:
        with tempfile.TemporaryDirectory() as d:
            L.ROOT = Path(d)
            L.CONFIG = L.ROOT / "build" / "launcher.json"
            L.CONFIG.parent.mkdir(parents=True)
            L.CONFIG.write_text(json.dumps({"ui_language": lang}), encoding="utf-8")
            w = launcher_qt.Window(L)
            w.setAttribute(Qt.WidgetAttribute.WA_DontShowOnScreen, True)
            w.reduce_motion = True  # the pages and the Options panel open at once, in their final place
            w.show()
            seen, found = set(), []
            for step in ("welcome", "main", "options", "advanced"):
                if step in ("welcome", "main"):
                    w.show_view(step, animate=False)
                elif step == "options":
                    w.toggle_options()
                else:
                    w.adv_btn.setChecked(True)
                for _ in range(5):
                    app.processEvents()
                found += [p for p in cut_off(w, seen) if p not in found]  # each widget again: a page can widen
            rtl = app.layoutDirection() == Qt.LayoutDirection.RightToLeft
            if launcher_i18n.language() != lang:
                found.append(f"the window is in {launcher_i18n.language()!r}")
            if rtl != (lang in launcher_i18n.RTL):
                found.append(f"layout direction {'right to left' if rtl else 'left to right'}")
            print(f"  {lang}: {len(seen)} widgets, {len(found)} problems")
            bad += [f"{lang}: {p}" for p in found]
            w.close()
            w.deleteLater()
            app.processEvents()
            app.setLayoutDirection(Qt.LayoutDirection.LeftToRight)
    return bad


def check_bug_report():
    bad = []
    with tempfile.TemporaryDirectory() as d:
        L.ROOT = Path(d)
        (L.ROOT / "build").mkdir()
        L.game_log("coo").write_text("[host] test log\n")
        L.game_input_log("coo").write_text("0:0:1\n")
        rows = ["frame,virtual_ms"] + [f"{i},{i * 16.7:.3f}" for i in range(L.PERF_ROWS + 500)]
        L.game_perf_log("coo").write_text("\n".join(rows) + "\n")
        with zipfile.ZipFile(L.make_bug_report({}, "coo", ["launcher line"])) as z:
            names = set(z.namelist())
            perf = z.read("launcher_coo_perf.csv").decode().splitlines() if "launcher_coo_perf.csv" in names else []
    want = {"launcher_coo.log", "launcher_coo_input.txt", "launcher_coo_perf.csv", "settings.json", "pc_info.txt",
            "launcher_log.txt", "README.txt"}
    if names != want:
        bad.append(f"bug report: files {sorted(names)}, expected {sorted(want)}")
    if perf[:1] != ["frame,virtual_ms"] or len(perf) != L.PERF_ROWS + 1 or not perf[1].startswith("500,"):
        bad.append(f"bug report: frame timings not cut to the header and the last {L.PERF_ROWS} rows")
    return bad


def main(argv):
    langs = argv or [c for _, c in launcher_i18n.LANGS]
    print("language tables")
    bad = check_tables()
    print("window in each language")
    bad += check_window(langs)
    print("bug report zip")
    bad += check_bug_report()
    for b in bad:
        print("FAIL", b)
    print("PASS" if not bad else f"{len(bad)} problems")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
