"""Builds GoWLauncher.exe with PyInstaller: tools/launcher.pyw + tools/toolchain.py + recomp/ (run in-process
with --recomp, so recompiling needs no Python), the window (launcher_qt.py on PySide6/Qt, LGPL-3.0, linked
dynamically as separate DLLs; launcher_art.py, Pillow,
the Cinzel font with its OFL text from tools/launcher_assets), plus the sources a game build needs, taken from git (only
tracked files, so never game data): CMakeLists.txt, LICENSE, cmake/, runtime/, recomp/runtime_funcs.json and
the re/out tables recomp.py reads. The launcher unpacks them next to itself on the first Build.

    .venv/Scripts/python.exe -m pip install -r tools/requirements-dev.txt
    .venv/Scripts/python.exe tools/build_launcher.py      -> build/launcher_dist/GoWLauncher.exe
"""
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "build" / "launcher_work"
DIST = ROOT / "build" / "launcher_dist"
SOURCES = ["CMakeLists.txt", "LICENSE", "cmake", "runtime", "recomp/runtime_funcs.json",
           "re/out/psp_nids_ppsspp.tsv", "re/out/recomp/*.recomp.json"]
BANNED = (".chd", ".iso", ".cso", ".pbp", ".prx", ".bin")


def main():
    files = subprocess.run(["git", "ls-files", "--"] + SOURCES, cwd=ROOT, capture_output=True, text=True, check=True).stdout.split()
    bad = [f for f in files if f.lower().endswith(BANNED) or f.startswith(("build/", "media/", "re/dumps/", "re/ghidra/"))]
    if bad or not files:
        sys.exit(f"refusing to bundle: {bad or 'no files'}")
    src = WORK / "src"
    if src.exists():
        shutil.rmtree(src)
    lines = []
    for f in files:
        data = (ROOT / f).read_bytes()
        (src / f).parent.mkdir(parents=True, exist_ok=True)
        (src / f).write_bytes(data)
        lines.append(f"{hashlib.sha256(data).hexdigest()}  {f}")
    (src / "manifest.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"bundling {len(files)} source files")
    sep = ";" if sys.platform == "win32" else ":"
    cmd = [sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--onefile", "--console", "--name", "GoWLauncher",
           "--distpath", str(DIST), "--workpath", str(WORK / "pyi"), "--specpath", str(WORK),
           "--paths", str(ROOT / "tools"), "--paths", str(ROOT / "recomp"),
           "--hidden-import", "recomp", "--hidden-import", "toolchain", "--hidden-import", "launcher_qt",
           "--hidden-import", "launcher_art", "--exclude-module", "tkinter",
           "--add-data", f"{ROOT / 'tools' / 'launcher_assets'}{sep}launcher_assets",
           "--exclude-module", "unicorn", "--exclude-module", "difftest", "--exclude-module", "numpy",
           "--add-data", f"{src}{sep}src", "--add-data", f"{ROOT / 'recomp' / 'runtime_funcs.json'}{sep}.",
           str(ROOT / "tools" / "launcher.pyw")]
    subprocess.run(cmd, cwd=ROOT, check=True)
    exe = DIST / "GoWLauncher.exe"
    print(f"built {exe} ({exe.stat().st_size / 2**20:.1f} MB)")


if __name__ == "__main__":
    main()