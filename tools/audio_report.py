"""Checks and pictures for the port's sound output (gow_rt --wav), for the progress record.

Usage:
  .venv/Scripts/python -I tools/audio_report.py levels run.wav [--step 1]
      RMS and peak per step (seconds): where there is sound at all.
  .venv/Scripts/python -I tools/audio_report.py match ours.wav reference.wav --at 20 [--dur 1]
      Finds where a stretch of our output occurs in a reference decode (e.g. ffmpeg's decode of the
      same .AT3) and prints the offset, the gain and the match after gain (dB). A decoder that is
      right gives ~80 dB or more; a gain other than 1 is the game's own volume.
  .venv/Scripts/python -I tools/audio_report.py picture run.wav --png out.png [--title T] [--mark SEC:label ...]
      Spectrogram (ffmpeg showspectrumpic) above a level strip, with labelled marks on the time axis.

Needs numpy and pillow in the venv, and ffmpeg on PATH for "picture".
"""
import argparse
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

import numpy as np


def load(path):
    with wave.open(str(path)) as w:
        if w.getsampwidth() != 2:
            sys.exit(f"{path}: 16-bit PCM WAV expected")
        rate, ch = w.getframerate(), w.getnchannels()
        data = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").reshape(-1, ch).astype(np.float64)
    return data, rate


def levels(args):
    a, rate = load(args.wav)
    n = int(args.step * rate)
    print(f"{args.wav}: {len(a)} frames, {len(a) / rate:.2f} s")
    for i in range(len(a) // n):
        seg = a[i * n:(i + 1) * n]
        print(f"{i * args.step:8.2f} s  rms {np.sqrt((seg ** 2).mean()):8.1f}  peak {int(np.abs(seg).max()):6d}")


def match(args):
    ours, rate = load(args.ours)
    ref, _ = load(args.reference)
    s0 = int(args.at * rate)
    seg = ours[s0:s0 + int(args.dur * rate)]
    x, y = ref[:, 0], seg[:, 0]
    n = 1 << int(np.ceil(np.log2(len(x) + len(y))))
    corr = np.fft.irfft(np.fft.rfft(x, n) * np.conj(np.fft.rfft(y, n)), n)[:len(x) - len(y)]
    lag = int(np.argmax(corr))
    m = ref[lag:lag + len(seg)]
    gain = (seg * m).sum() / max((m * m).sum(), 1e-9)
    err = seg - gain * m
    db = 10 * np.log10((gain * gain * (m * m).sum()) / max((err * err).sum(), 1e-9))
    print(f"ours {args.at:.2f} s (+{args.dur} s) = reference frame {lag} ({lag / rate:.4f} s): "
          f"gain {gain:.4f}, match {db:.1f} dB, largest difference {np.abs(err).max():.0f}")


def picture(args):
    from PIL import Image, ImageDraw, ImageFont
    a, rate = load(args.wav)
    dur = len(a) / rate
    W, SPEC_H, LEV_H, PAD = 1600, 420, 90, 60
    with tempfile.TemporaryDirectory() as tmp:
        spec = Path(tmp) / "spec.png"
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(args.wav), "-lavfi",
                        f"showspectrumpic=s={W}x{SPEC_H}:legend=0:scale=log:stop=16000:color=intensity",
                        str(spec)], check=True)
        sp = Image.open(spec).convert("RGB").resize((W, SPEC_H))
    img = Image.new("RGB", (W + 2 * PAD, SPEC_H + LEV_H + 110 + 22 * max(1, len(args.mark or []))), (12, 12, 12))
    d = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype("consola.ttf", 16)
        big = ImageFont.truetype("consola.ttf", 20)
    except OSError:
        font = big = ImageFont.load_default()
    d.text((PAD, 14), args.title or Path(args.wav).name, fill=(230, 230, 230), font=big)
    top = 50
    img.paste(sp, (PAD, top))
    for khz in (0, 4, 8, 12, 16):
        y = top + SPEC_H - int(khz / 16 * SPEC_H)
        d.text((8, y - 8), f"{khz}k", fill=(150, 150, 150), font=font)
    # level strip: RMS per pixel column, both channels
    ly = top + SPEC_H + 10
    d.rectangle((PAD, ly, PAD + W, ly + LEV_H), fill=(24, 24, 24))
    per = max(1, len(a) // W)
    for x in range(W):
        seg = a[x * per:(x + 1) * per]
        if not len(seg):
            break
        r = np.sqrt((seg ** 2).mean()) / 32768
        h = int(min(1.0, r * 4) * LEV_H)
        d.line((PAD + x, ly + LEV_H, PAD + x, ly + LEV_H - h), fill=(90, 170, 240))
    d.text((8, ly + LEV_H // 2 - 8), "lvl", fill=(150, 150, 150), font=font)
    # time axis and marks
    ay = ly + LEV_H + 6
    for t in range(0, int(dur) + 1, 10):
        x = PAD + int(t / dur * W)
        d.line((x, ay, x, ay + 6), fill=(150, 150, 150))
        d.text((x - 10, ay + 8), f"{t}s", fill=(150, 150, 150), font=font)
    row_end = []  # right edge of the last label on each row
    for m in args.mark or []:
        sec, label = m.split(":", 1)
        x = PAD + int(float(sec) / dur * W)
        d.line((x, top, x, ay), fill=(255, 210, 80))
        w = d.textlength(label, font=font)
        row = next((r for r, end in enumerate(row_end) if end < x), len(row_end))
        if row == len(row_end):
            row_end.append(0)
        row_end[row] = x + 4 + w + 12
        d.text((x + 4, ay + 30 + row * 22), label, fill=(255, 210, 80), font=font)
    img.save(args.png)
    print(f"wrote {args.png} ({dur:.1f} s of audio)")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("levels"); p.add_argument("wav"); p.add_argument("--step", type=float, default=1.0)
    p = sub.add_parser("match"); p.add_argument("ours"); p.add_argument("reference")
    p.add_argument("--at", type=float, required=True); p.add_argument("--dur", type=float, default=1.0)
    p = sub.add_parser("picture"); p.add_argument("wav"); p.add_argument("--png", required=True)
    p.add_argument("--title"); p.add_argument("--mark", nargs="*")
    args = ap.parse_args()
    {"levels": levels, "match": match, "picture": picture}[args.cmd](args)


if __name__ == "__main__":
    main()
