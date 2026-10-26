"""Builds GoWLauncher.exe with PyInstaller: tools/launcher.pyw + tools/toolchain.py + recomp/ (run in-process
with --recomp, so recompiling needs no Python), the window (launcher_qt.py on PySide6/Qt, LGPL-3.0, linked
dynamically as separate DLLs; launcher_art.py, Pillow,
the Cinzel font with its OFL text from tools/launcher_assets), plus the sources a game build needs, taken from git (only
tracked files, so never game data): CMakeLists.txt, LICENSE, cmake/, runtime/, recomp/runtime_funcs.json and
the re/out tables recomp.py reads. The launcher unpacks them into GoW_PSP_data/ next to itself on the first Build.
Also writes the release ZIP, GoW_PSP_Launcher-v<VERSION>.zip: one folder GoW_PSP/ with the exe and README.txt.

    .venv/Scripts/python.exe -m pip install -r tools/requirements-dev.txt
    .venv/Scripts/python.exe tools/build_launcher.py      -> build/launcher_dist/GoWLauncher.exe and the ZIP
"""
import hashlib
import re
import zipfile
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
    cmd = [sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--onefile", "--windowed", "--name", "GoWLauncher", "--icon", str(ROOT / "tools" / "launcher_assets" / "icon.ico"),
           "--distpath", str(DIST), "--workpath", str(WORK / "pyi"), "--specpath", str(WORK),
           "--paths", str(ROOT / "tools"), "--paths", str(ROOT / "recomp"),
           "--hidden-import", "recomp", "--hidden-import", "toolchain", "--hidden-import", "launcher_qt",
           "--hidden-import", "launcher_art", "--hidden-import", "launcher_update", "--hidden-import", "steam_shortcuts",
           "--hidden-import", "discord_presence", "--hidden-import", "texture_upscale", "--hidden-import", "launcher_i18n",
           "--exclude-module", "tkinter",
           "--add-data", f"{ROOT / 'tools' / 'launcher_assets'}{sep}launcher_assets",
           "--exclude-module", "unicorn", "--exclude-module", "difftest", "--exclude-module", "numpy",
           "--add-data", f"{src}{sep}src", "--add-data", f"{ROOT / 'recomp' / 'runtime_funcs.json'}{sep}.",
           str(ROOT / "tools" / "launcher.pyw")]
    subprocess.run(cmd, cwd=ROOT, check=True)
    exe = DIST / "GoWLauncher.exe"
    print(f"built {exe} ({exe.stat().st_size / 2**20:.1f} MB)")
    make_zip(exe)


README_TXT = """God of War PSP: native PC ports (unofficial), launcher v{version}
https://github.com/DrummingBird1/gow-psp-recomp

You need your own disc image (.chd or .iso) of the US release. No game content is included.

1. Run GoWLauncher.exe. It keeps everything it creates (builds, saves, settings) in the
   GoW_PSP_data folder next to it.
2. Choose the folder with your disc images.
3. Press Build (one time per game), then Play. The first time it asks to download its
   portable build tools (llvm-mingw, CMake, Ninja; about 246 MB, once) into GoW_PSP_data.
   No Visual Studio, no administrator rights, nothing installed in Windows.

Windows warning: GoWLauncher.exe is unsigned (built with PyInstaller), so SmartScreen may say
"Windows protected your PC": click "More info", then "Run anyway". Some antivirus programs flag
PyInstaller programs as a false positive. Compare the SHA-256 with the one in the release notes
(PowerShell: Get-FileHash GoWLauncher.exe).

Bug reports: attach GoW_PSP_data\\build\\launcher_coo.log (or launcher_gos.log) and, if you can,
a recorded input file. Never attach game files.
"""


def make_zip(exe):
    """The release download: GoW_PSP/GoWLauncher.exe + GoW_PSP/README.txt, so extracting it makes one folder."""
    version = re.search(r'^VERSION = "([^"]+)"', (ROOT / "tools" / "launcher.pyw").read_text(encoding="utf-8"), re.M)[1]
    out = DIST / f"GoW_PSP_Launcher-v{version}.zip"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.write(exe, "GoW_PSP/GoWLauncher.exe")
        z.writestr("GoW_PSP/README.txt", README_TXT.format(version=version).replace("\n", "\r\n"))
    print(f"built {out} ({out.stat().st_size / 2**20:.1f} MB)")


if __name__ == "__main__":
    main()