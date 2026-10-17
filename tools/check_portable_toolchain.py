"""Downloads the launcher's portable toolchain (tools/toolchain.py: llvm-mingw, CMake, Ninja; pinned SHA-256) and
builds gow_disc with it: a check that needs no game data (CI runs it; the game itself needs the recompiled C).

    python tools/check_portable_toolchain.py [DIR]      default DIR: build/toolchain
"""
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "toolchain"
    if not toolchain.find_portable(root) and not toolchain.download_portable(root):
        return 1
    t = toolchain.find_portable(root)
    build = ROOT / "build" / "mingw_tools"
    for cmd in ([t["cmake"], "-S", str(ROOT), "-B", str(build), "-DGOW_TOOLS_ONLY=ON", "-DCMAKE_DISABLE_FIND_PACKAGE_Git=ON", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                 f"-DCMAKE_MAKE_PROGRAM={t['ninja']}", f"-DCMAKE_C_COMPILER={t['cc']}", f"-DCMAKE_CXX_COMPILER={t['cxx']}"],
                [t["cmake"], "--build", str(build), "--target", "gow_disc"],
                [str(build / "gow_disc.exe")]):
        print("> " + subprocess.list2cmdline(cmd), flush=True)
        code = subprocess.run(cmd, env=t["env"]).returncode
        if code not in (0, 2) or (code == 2 and not cmd[0].endswith("gow_disc.exe")):  # gow_disc without args: usage, 2
            return 1
    print("portable toolchain ok: gow_disc built and runs")
    return 0


if __name__ == "__main__":
    sys.exit(main())