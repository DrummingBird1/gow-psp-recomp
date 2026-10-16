"""Compares rendered frames of two runs (same frame numbers) and summarizes frame times.

Usage: .venv/Scripts/python -I tools/check_frames.py REF_DIR NEW_DIR [--perf NEW_DIR/perf.csv --from N --to M]
"""
import argparse
import csv
import sys
from pathlib import Path

from PIL import Image, ImageChops


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("ref")
    ap.add_argument("new")
    ap.add_argument("--perf")
    ap.add_argument("--from", dest="lo", type=int, default=0)
    ap.add_argument("--to", dest="hi", type=int, default=1 << 30)
    a = ap.parse_args()
    bad = 0
    for f in sorted(Path(a.new).glob("frame_*.png")):
        ref = Path(a.ref) / f.name
        if not ref.exists():
            continue
        d = ImageChops.difference(Image.open(ref).convert("RGB"), Image.open(f).convert("RGB")).getbbox()
        print(f"{f.name}: {'identical' if d is None else f'differs in {d}'}")
        bad += d is not None
    if a.perf:
        ft = sorted(float(r["host_frame_ms"]) for r in csv.DictReader(open(a.perf)) if a.lo <= int(r["frame"]) <= a.hi)
        if ft:
            print(f"frames {a.lo}-{a.hi}: n={len(ft)} avg {sum(ft) / len(ft):.1f} ms ({1000 * len(ft) / sum(ft):.1f} fps) "
                  f"p50 {ft[len(ft) // 2]:.1f} ms p99 {ft[max(0, int(len(ft) * 0.99) - 1)]:.1f} ms")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
