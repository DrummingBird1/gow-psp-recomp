"""The software-renderer check of AGENTS.md in one command, for a regular (e.g. nightly) run on the developer's PC.

Replays build/rec_v9/input.txt in Chains of Olympus with a fresh memory stick, compares the 34 frames with
build/rec_v9 and the WAV's SHA-256 with the reference, and optionally boots Ghost of Sparta headless for a few
thousand frames (no errors, no unimplemented functions). Writes everything into OUT (default build/regression/<date>)
and exits 1 when anything differs. The game data stays in build/: nothing here may leave the PC.

Usage: .venv/Scripts/python -I tools/regression_check.py [--exe-coo EXE] [--exe-gos EXE] [--out DIR] [--disc-dir DIR]
"""
import argparse
import datetime
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WAV_SHA256 = "1c1ad6762270d1dead70c3faa0997f9fb4cff264c111420b3221a621cdff5869"
COO_DISC = "God of War - Chains of Olympus (USA).chd"
GOS_DISC = "God of War - Ghost of Sparta (USA) (En,Fr,Es).chd"


def run(exe, image, disc, ms0, extra, log):
    ms0.mkdir(parents=True, exist_ok=True)
    cmd = [str(Path(exe).resolve()), "--image", str(image), "--boot", "--disc", disc, "--ms0", str(ms0),
           "--cpu-ms", "1200000", "--wall-ms", "1200000"] + extra
    with open(log, "w", encoding="utf-8", errors="replace") as f:
        return subprocess.run(cmd, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT).returncode


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe-coo", default=str(ROOT / "build/mingw_full/gow_rt_UCUS98653_full.exe"))
    ap.add_argument("--exe-gos", default="", help="also boot Ghost of Sparta with this exe")
    ap.add_argument("--out", default=str(ROOT / "build/regression" / datetime.date.today().isoformat()))
    ap.add_argument("--disc-dir", default=str(ROOT), help="folder with the two disc images (default: the repository)")
    a = ap.parse_args()
    out = Path(a.out).resolve()
    discs = Path(a.disc_dir)
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    failed = []
    coo = out / "coo"
    code = run(a.exe_coo, ROOT / "build/gen/UCUS98653_full/image.bin", str(discs / COO_DISC), coo / "ms0",
               ["--input", str(ROOT / "build/rec_v9/input.txt"), "--frames-dir", str(coo), "--frames-every", "200",
                "--max-frames", "6800", "--wav", str(coo / "mix.wav")], coo / "log.txt")
    if code:
        failed.append(f"Chains of Olympus exited with {code}")
    r = subprocess.run([sys.executable, "-I", str(ROOT / "tools/check_frames.py"), str(ROOT / "build/rec_v9"), str(coo)],
                       capture_output=True, text=True)
    same = r.stdout.count("identical")
    if same != 34:
        failed.append(f"frames: {same} of 34 identical")
    wav = hashlib.sha256((coo / "mix.wav").read_bytes()).hexdigest() if (coo / "mix.wav").exists() else "none"
    if wav != WAV_SHA256:
        failed.append(f"mix.wav SHA-256 {wav}")
    if a.exe_gos:
        gos = out / "gos"
        code = run(a.exe_gos, ROOT / "build/gen/UCUS98737_full/image.bin", str(discs / GOS_DISC), gos / "ms0",
                   ["--max-frames", "3000"], gos / "log.txt")
        log = (gos / "log.txt").read_text(encoding="utf-8", errors="replace")
        if code or "unimplemented functions that were called: none" not in log:
            failed.append(f"Ghost of Sparta: exit {code}, see {gos / 'log.txt'}")
    print(f"frames {same}/34 identical, mix.wav {'as expected' if wav == WAV_SHA256 else 'DIFFERENT'}")
    print("PASS" if not failed else "FAIL: " + "; ".join(failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
