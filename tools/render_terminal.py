"""Renders real command output as a terminal-style PNG, for the project's progress record.

The image shows the command that produced the output, the date and the git commit, so a
screenshot can always be traced back to the code that made it.

Usage:
  .venv/Scripts/python -I tools/render_terminal.py --title T --cmd "shown command" --log out.txt --png shot.png
         [--grep REGEX] [--exclude REGEX] [--head N] [--tail N]

Without --cmd, log lines that start with "PS> " are drawn as prompts (for a log that records
several commands with their output, as tools/run_logged.sh writes it).
"""
import argparse
import datetime
import re
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

BG, FG, DIM = (12, 12, 12), (204, 204, 204), (118, 118, 118)
COLORS = [
    (re.compile(r"FAIL|MISMATCH|error|Error|stopped|Exception"), (241, 76, 76)),
    (re.compile(r"\b0 failed|\b0 mismatch|\b0 differ|passed|all-matching|\bmatch\b|100\.00%"), (35, 209, 139)),
    (re.compile(r"UNIMPLEMENTED"), (229, 165, 46)),
    (re.compile(r"^\[boot\]|\[sched\]"), (41, 184, 219)),
    (re.compile(r"\[thread\]"), (229, 229, 16)),
    (re.compile(r"\[io\]|\[module\]"), (59, 142, 234)),
    (re.compile(r"\[ge\]|\[display\]"), (214, 112, 214)),
    (re.compile(r"guest printf"), (255, 255, 255)),
]


def git_rev() -> str:
    try:
        rev = subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "status", "--porcelain", "--untracked-files=no"], capture_output=True, text=True).stdout.strip()
        return rev + (" + uncommitted changes" if dirty else "")
    except Exception:
        return "?"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--title", required=True)
    ap.add_argument("--cmd")
    ap.add_argument("--log", required=True, nargs="+")
    ap.add_argument("--png", required=True)
    ap.add_argument("--grep")
    ap.add_argument("--exclude")
    ap.add_argument("--head", type=int, default=0)
    ap.add_argument("--tail", type=int, default=0)
    ap.add_argument("--cols", type=int, default=150)
    ap.add_argument("--wrap", action="store_true", help="wrap long lines instead of cutting them")
    args = ap.parse_args()

    lines = []
    for f in args.log:
        lines += Path(f).read_text(encoding="utf-8", errors="replace").replace("\r", "").split("\n")
    if args.grep:
        lines = [l for l in lines if re.search(args.grep, l)]
    if args.exclude:
        lines = [l for l in lines if not re.search(args.exclude, l)]
    lines = [l for l in lines if l.strip()]
    skipped = ""
    if args.head and args.tail and len(lines) > args.head + args.tail:
        skipped = f"... ({len(lines) - args.head - args.tail} lines omitted) ..."
        lines = lines[:args.head] + [skipped] + lines[-args.tail:]
    elif args.head:
        lines = lines[:args.head]
    elif args.tail:
        lines = lines[-args.tail:]
    def fit(l):
        cols = args.cols - (len("PS E:\\Projects\\GoW_PSP> ") - 4 if not args.cmd and l.startswith("PS> ") else 0)
        if len(l) <= cols:
            return [l]
        if not args.wrap:
            return [l[:cols - 1] + "…"]
        indent, out, first = len(l) - len(l.lstrip()) + 4, [], True
        while l:
            room = cols if first else cols - indent
            if len(l) <= room:
                cut = len(l)
            else:
                cut = l.rfind(" ", 0, room + 1)
                cut = cut if cut > room // 2 else room
            out.append(("" if first else " " * indent) + l[:cut].rstrip())
            l, first = l[cut:].lstrip(), False
        return out

    lines = [part for l in lines for part in fit(l)]

    font = ImageFont.truetype("C:/Windows/Fonts/CascadiaMono.ttf", 15)
    bold = ImageFont.truetype("C:/Windows/Fonts/consolab.ttf", 15)
    cw = font.getbbox("M")[2]
    lh = 19
    header = f"{args.title}"
    meta = f"{datetime.datetime.now():%Y-%m-%d %H:%M}  ·  commit {git_rev()}  ·  GoW_PSP"
    width = int(max(cw * args.cols + 40, 80 + bold.getlength(header) + 40 + font.getlength(meta) + 20))
    height = 30 + 34 + lh * (len(lines) + (2 if args.cmd else 0)) + 8 * sum(l.startswith("PS> ") for l in lines) + 20
    img = Image.new("RGB", (width, height), BG)
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, width, 30], fill=(36, 36, 36))
    for i, c in enumerate([(255, 95, 86), (255, 189, 46), (39, 201, 63)]):
        d.ellipse([12 + i * 20, 9, 24 + i * 20, 21], fill=c)
    d.text((80, 6), header, font=bold, fill=(230, 230, 230))
    d.text((width - 20 - d.textlength(meta, font=font), 7), meta, font=font, fill=DIM)
    prompt = "PS E:\\Projects\\GoW_PSP> "
    y = 44

    def draw_prompt(cmd):
        d.text((20, y), prompt, font=font, fill=(35, 209, 139))
        d.text((20 + d.textlength(prompt, font=font), y), cmd, font=font, fill=(255, 255, 255))

    if args.cmd:
        draw_prompt(args.cmd)
        y += lh + 6
    for l in lines:
        if not args.cmd and l.startswith("PS> "):
            if y > 44:
                y += 8
            draw_prompt(l[4:])
            y += lh
            continue
        color = DIM if l == skipped else FG
        for rx, c in COLORS:
            if rx.search(l):
                color = c
                break
        d.text((20, y), l, font=font, fill=color)
        y += lh
    Path(args.png).parent.mkdir(parents=True, exist_ok=True)
    img.save(args.png, optimize=True)
    print(f"{args.png}: {width}x{height}, {len(lines)} lines")
    return 0


if __name__ == "__main__":
    sys.exit(main())
