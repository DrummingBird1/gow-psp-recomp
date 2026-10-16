"""Performance report from recorded runs: a frame-time chart and a side-by-side video.

Each run is a host-clock recording (gow_rt --record-host) and its --perf-csv log. The chart
plots host time per displayed frame; the video puts the recordings side by side with a live
overlay (fps over the last second, frame number, game/host speed) built from the CSV.

Usage:
  .venv/Scripts/python -I tools/perf_report.py --run "label=v1 1 thread,video=a.mp4,csv=a.csv" --run ... \
      [--chart out.png] [--video out.mp4] [--ffmpeg exe] [--title T]
"""
import argparse
import csv
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

COLORS = [(241, 76, 76), (35, 209, 139), (59, 142, 234), (229, 165, 46)]
FONT = "C:/Windows/Fonts/CascadiaMono.ttf"


def load(path):
    return [{k: float(v) for k, v in r.items()} for r in csv.DictReader(open(path))]


def chart(runs, out, title, avg_from, avg_label):
    W, H, L, R, T, B = 1400, 640, 80, 30, 70, 70
    img = Image.new("RGB", (W, H), (16, 16, 20))
    d = ImageDraw.Draw(img)
    font = ImageFont.truetype(FONT, 15)
    big = ImageFont.truetype(FONT, 20)
    nmax = max(int(r["frame"]) for run in runs for r in run["rows"])
    times = sorted(r["host_frame_ms"] for run in runs for r in run["rows"])
    ymax = max(100.0, 50.0 * -(-times[int(len(times) * 0.998) - 1] // 50))
    xstep = next(s for s in (100, 200, 250, 500, 1000, 2000, 5000, 10000) if nmax / s <= 14)
    ystep = 50 if ymax <= 500 else 100
    def X(f): return L + (W - L - R) * f / nmax
    def Y(ms): return H - B - (H - T - B) * min(ms, ymax) / ymax
    for ms in range(0, int(ymax) + 1, ystep):
        d.line([(L, Y(ms)), (W - R, Y(ms))], fill=(40, 40, 48))
        d.text((L - 10 - d.textlength(f"{ms}", font=font), Y(ms) - 9), f"{ms}", font=font, fill=(150, 150, 160))
    for f in range(0, nmax + 1, xstep):
        d.text((X(f) - d.textlength(str(f), font=font) / 2, H - B + 8), str(f), font=font, fill=(150, 150, 160))
    d.line([(L, Y(16.683)), (W - R, Y(16.683))], fill=(90, 90, 110))
    d.text((W - R - 190, Y(16.683) - 20), "59.94 fps (16.7 ms)", font=font, fill=(150, 150, 170))
    d.text((L, 20), title, font=big, fill=(235, 235, 240))
    d.text((W / 2 - 120, H - 30), "displayed frame number", font=font, fill=(150, 150, 160))
    d.text((8, T - 30), "host ms per frame", font=font, fill=(150, 150, 160))
    for i, run in enumerate(runs):
        c = COLORS[i % len(COLORS)]
        pts = [(X(r["frame"]), Y(r["host_frame_ms"])) for r in run["rows"]]
        d.line(pts, fill=c, width=1)
        seg = [r["host_frame_ms"] for r in run["rows"] if r["frame"] >= avg_from]
        total = run["rows"][-1]["host_ms"] / 1000
        note = f"{run['label']}: total {total:.1f} s"
        if seg:
            note += f", {avg_label} avg {sum(seg) / len(seg):.1f} ms ({1000 * len(seg) / sum(seg):.1f} fps)"
        d.rectangle([L + 20, T + 10 + 26 * i, L + 34, T + 24 + 26 * i], fill=c)
        d.text((L + 44, T + 7 + 26 * i), note, font=font, fill=(225, 225, 230))
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    img.save(out, optimize=True)
    print(f"{out}: chart of {len(runs)} runs")


def ts(sec):
    h, rem = divmod(sec, 3600)
    m, s = divmod(rem, 60)
    return f"{int(h)}:{int(m):02d}:{s:05.2f}"


def overlay_ass(run, path, until):
    """ASS subtitles: one event per 1/4 s of host time with the fps over the last second."""
    rows = run["rows"]
    end = rows[-1]["host_ms"] / 1000
    lines = ["[Script Info]", "ScriptType: v4.00+", "PlayResX: 960", "PlayResY: 544", "",
             "[V4+ Styles]",
             "Format: Name, Fontname, Fontsize, PrimaryColour, OutlineColour, BackColour, Bold, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV",
             "Style: Default,Cascadia Mono,22,&H00FFFFFF,&H00000000,&H80000000,1,3,2,0,7,12,12,10", "",
             "[Events]", "Format: Layer, Start, End, Style, Text"]
    j = 0
    t = 0.0
    while t < until:
        t1 = t + 0.25
        while j < len(rows) and rows[j]["host_ms"] / 1000 <= t1:
            j += 1
        recent = [r for r in rows[max(0, j - 400):j] if r["host_ms"] / 1000 > t1 - 1.0]
        last = rows[j - 1] if j else rows[0]
        done = t1 > end
        state = f"finished in {end:.1f} s" if done else f"{len(recent):3d} fps"
        speed = last["virtual_ms"] / last["host_ms"] if last["host_ms"] else 0
        text = f"{run['label']}\\N{state}  frame {int(last['frame'])}  game/host {speed:.2f}x"
        lines.append(f"Dialogue: 0,{ts(t)},{ts(t1)},Default,{text}")
        t = t1
    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")


def video(runs, out, ffmpeg):
    longest = max(r["rows"][-1]["host_ms"] for r in runs) / 1000 + 1.0
    with tempfile.TemporaryDirectory() as tmp:
        inputs, filters = [], []
        for i, run in enumerate(runs):
            overlay_ass(run, Path(tmp) / f"o{i}.ass", longest)
            inputs += ["-i", str(Path(run["video"]).resolve())]
            pad = longest - run["rows"][-1]["host_ms"] / 1000
            filters.append(f"[{i}:v]tpad=stop_mode=clone:stop_duration={pad:.2f},subtitles=o{i}.ass[v{i}]")
        stack = "".join(f"[v{i}]" for i in range(len(runs)))
        filters.append(f"{stack}hstack=inputs={len(runs)}[out]" if len(runs) > 1 else "[v0]null[out]")
        cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", *inputs, "-filter_complex", ";".join(filters),
               "-map", "[out]", "-t", f"{longest:.2f}", "-c:v", "libx264", "-preset", "veryfast", "-crf", "20",
               "-pix_fmt", "yuv420p", str(Path(out).resolve())]
        subprocess.run(cmd, check=True, cwd=tmp)
    print(f"{out}: side-by-side video of {len(runs)} runs, {longest:.1f} s")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", action="append", required=True)
    ap.add_argument("--chart")
    ap.add_argument("--video")
    ap.add_argument("--ffmpeg", default="ffmpeg")
    ap.add_argument("--title", default="Chains of Olympus: host time per frame")
    ap.add_argument("--avg-from", type=int, default=900, help="first frame of the averaged segment")
    ap.add_argument("--avg-label", default="main menu", help="name of the averaged segment")
    a = ap.parse_args()
    runs = []
    for spec in a.run:
        kv = dict(part.split("=", 1) for part in spec.split(","))
        kv["rows"] = load(kv["csv"])
        runs.append(kv)
    if a.chart:
        chart(runs, a.chart, a.title, a.avg_from, a.avg_label)
    if a.video:
        video(runs, a.video, a.ffmpeg)
    return 0


if __name__ == "__main__":
    sys.exit(main())
