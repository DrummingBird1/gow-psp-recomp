"""Automated level tour of Chains of Olympus and Ghost of Sparta (docs/phase11-level-tour.md,
docs/phase12-gos-level-tour.md).

For every level (a zone file in disc0:/PSP_GAME/USRDIR/data), boots the port headless, warps
into the level from the main menu (gow_rt --warp, runtime/rt_debug.c), plays a generic input
script (running in all directions, combos, jumps, grabs, rolls, block/use) with god mode on,
and collects:
  - how the run ended: frame limit (ok), deadlock, trap, abort, crash (exit code), timeout;
  - "[hle] UNIMPLEMENTED" functions and files the game could not open;
  - whether the level loaded and became current, and which levels the game streamed in;
  - PSNR of the software renderer against the GPU renderer at several frames (--renderer both);
  - frame rate of the GPU renderer headless (host time per frame, a separate --renderer gpu run);
  - a contact sheet of frames.

  .venv/Scripts/python -I tools/level_tour.py --out build/tour/run1 [--game coo|gos] [--levels a,b,...]
      [--jobs 6] [--seconds 90] [--modes gpu,both] [--exe ...] [--disc ...]

Results: <out>/<level>/{gpu,both}/run.log, frames, perf.csv; <out>/<level>/sheet.png;
<out>/summary.json, <out>/summary.md (one table row per level), <out>/overview.png."""
import argparse
import concurrent.futures as cf
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

from PIL import Image, ImageDraw, ImageStat

ROOT = Path(__file__).resolve().parent.parent
FPS = 60
LANG = re.compile(r"_(eng|fre|ger|ita|spa)$")


class Game:
    """What the tour needs to know about a game: its build, when --warp happens after a boot with an
    empty ms0 (the warp frame, and its virtual time), and which data files are levels."""

    def __init__(self, **kw):
        self.__dict__.update(kw)


GAMES = {
    "coo": Game(
        exe=ROOT / "build/cmake_tour/Release/gow_rt_UCUS98653_full.exe",
        image=ROOT / "build/gen/UCUS98653_full/image.bin",
        disc=ROOT / "God of War - Chains of Olympus (USA).chd",
        warp_frame=1000,     # the main menu after a boot with an empty ms0
        warp_s=16.9,
        input_start_s=20.0,  # the level is up by ~18 s
        # not levels: menus, fonts, Kratos's models, splash screens
        is_level=lambda n: n not in {"engine_assets", "frontend_assets", "gallery_screens", "kratos_anims", "kratos_model",
                                     "kratos_costume_01", "kratos_costume_02", "kratos_costume_03", "splash_assets",
                                     "splash_screens", "start_screen_demo"},
        # loaded by a main zone next to it (enemy waves, push boxes); warping into one alone is not meaningful
        addon=re.compile(r"(_enemies|_enc(_\d+)?|_pushbox)$"),
        menu_levels=("start_screen_demo", "splash_screens", "pi_beach_00"),  # loaded by the menu, not by a level
        expected_missing=(),
        warp_via=None,
    ),
    "gos": Game(
        exe=ROOT / "build/cmake_gos_tour/Release/gow_rt_UCUS98737_full.exe",
        image=ROOT / "build/gen/UCUS98737_full/image.bin",
        disc=ROOT / "God of War - Ghost of Sparta (USA) (En,Fr,Es).chd",
        warp_frame=1461,     # the warp waits for the main menu to be idle: frame 1461 after a boot with an empty ms0
        warp_s=24.81,
        input_start_s=27.0,  # the level is up by ~25.5 s
        # levels are AREA_NNN_NAME, plus the menus' arenas and challenges; the rest are enemies, Kratos,
        # menus and galleries. ATT_000_STARTSCREEN is the disc check (PGD), not a level.
        is_level=lambda n: n != "att_000_startscreen" and bool(
            re.match(r"^[a-z]{2,3}_\d{3}", n) or n.startswith(("arena_", "toa_"))
            or n in ("challenge_of_gods_a", "atb_nis_statueactivate")),
        addon=re.compile(r"(_enemies|_enemy|_encounter)$"),
        menu_levels=("start_screen", "splash_screens"),
        # the game looks for its two DLC packs on the memory stick (docs/phase10-ghost-of-sparta.md)
        expected_missing=("ms0:/PSP/GAME/UCUS98737/dlcarena.EDAT", "ms0:/PSP/GAME/UCUS98737/dlccostume.EDAT"),
        # a level file bigger than this does not fit next to the main menu's start_screen (4.3 MB): the game
        # starts in a small level first and changes to it the way Load Game does (gow_rt --warp-via)
        warp_via=("att_020_bow", 6_500_000),
    ),
}
G = GAMES["coo"]  # set by main()
SIZES = {}        # level -> file size, from list_levels
VIA = set()       # levels always warped into through G.warp_via (--via)


def list_levels(exe, disc):
    out = subprocess.run([str(exe), "--disc", str(disc), "--extract", "disc0:/PSP_GAME/USRDIR/data", "-"],
                         capture_output=True, text=True).stdout
    files = re.findall(r"^\s+(\S+)\.bin\s+file\s.*\((\d+) bytes\)", out, re.M | re.I)
    SIZES.update((n.lower(), int(size)) for n, size in files)
    names = [n.lower() for n, _ in files]
    main, addons = [], []
    for n in names:
        if not G.is_level(n) or LANG.search(n):
            continue
        (addons if G.addon.search(n) else main).append(n)
    return main, addons


def make_input(start_s, seconds):
    """A generic script by virtual time: runs in 8 directions in turn, light/heavy combos, jumps and
    double jumps, grabs, rolls (L+R+stick), block/use (R). Cross also closes tutorial text boxes."""
    dirs = [("lx=128+ly=0", "up"), ("lx=255+ly=128", "right"), ("lx=128+ly=255", "down"), ("lx=0+ly=128", "left"),
            ("lx=230+ly=30", "up-right"), ("lx=30+ly=230", "down-left"), ("lx=230+ly=230", "down-right"),
            ("lx=30+ly=30", "up-left")]
    items, t, k = [], start_s, 0
    while t < start_s + seconds:
        stick, _ = dirs[k % len(dirs)]
        nxt, _ = dirs[(k + 3) % len(dirs)]
        items += [f"t{t:.2f}:{stick}:2.0",                       # run
                  f"t{t + 2.0:.2f}:square:0.1", f"t{t + 2.3:.2f}:square:0.1", f"t{t + 2.6:.2f}:triangle:0.15",
                  f"t{t + 3.2:.2f}:{nxt}:2.0",                    # run elsewhere, jumping
                  f"t{t + 3.4:.2f}:cross+{nxt}:0.1", f"t{t + 3.8:.2f}:cross+{nxt}:0.1",
                  f"t{t + 5.4:.2f}:circle:0.1",                   # grab
                  f"t{t + 5.9:.2f}:{stick}:1.6", f"t{t + 6.2:.2f}:square+{stick}:0.1", f"t{t + 6.6:.2f}:triangle+{stick}:0.3",
                  f"t{t + 7.6:.2f}:l+r+{nxt}:0.15",               # roll
                  f"t{t + 8.0:.2f}:r:0.4",                        # block / use (doors, chests, levers)
                  f"t{t + 8.6:.2f}:cross:0.1",                    # close a text box, or jump
                  f"t{t + 9.0:.2f}:{nxt}:2.5", f"t{t + 10.0:.2f}:square+{nxt}:0.1", f"t{t + 10.3:.2f}:square+{nxt}:0.1",
                  f"t{t + 10.6:.2f}:square+{nxt}:0.1", f"t{t + 11.0:.2f}:triangle+{nxt}:0.4"]
        t += 12.0
        k += 1
    return ",".join(items)


def run_one(exe, image, disc, level, mode, d, seconds, input_file, timeout_s, both_from):
    """One run of a level. "gpu": frames, perf.csv; "both": software and GPU renderers side by side,
    compared at the shots from frame both_from on (nothing is drawn before it: --fast-forward)."""
    d.mkdir(parents=True, exist_ok=True)
    ms0 = d / "ms0"
    if ms0.exists():
        shutil.rmtree(ms0)
    ms0.mkdir()
    last = G.warp_frame + int((G.input_start_s - G.warp_s + seconds) * FPS) + 60
    shots = list(range(G.warp_frame + 200, last + 1, 600))
    ff = G.warp_frame - 10
    if mode == "both" and both_from:
        shots = [f for f in shots if f >= both_from]
        ff = max(ff, both_from - 60)
    cmd = [str(exe), "--image", str(image), "--boot", "--disc", str(disc), "--ms0", str(ms0), "--warp", level, "--god",
           "--input", str(input_file), "--max-frames", str(last), "--fast-forward", str(ff),
           "--frames-dir", str(d), "--frames-at", ",".join(map(str, shots)), "--cpu-ms", str(timeout_s * 1000),
           "--wall-ms", str(timeout_s * 1000), "--renderer", mode]
    if G.warp_via and (SIZES.get(zone(level), 0) > G.warp_via[1] or zone(level) in VIA or "all" in VIA):
        cmd += ["--warp-via", G.warp_via[0]]
    if mode == "gpu":
        cmd += ["--perf-csv", str(d / "perf.csv")]
    else:
        cmd += ["--gpu-compare-at", ",".join(map(str, shots if both_from else shots[1::2]))]
    t0 = time.time()
    with open(d / "run.log", "w", encoding="utf-8", errors="replace") as log:
        try:
            p = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, timeout=timeout_s + 60, cwd=str(ROOT))
            code = p.returncode
        except subprocess.TimeoutExpired:
            code = "timeout"
    return {"exit": code, "wall_s": round(time.time() - t0, 1), "shots": shots, "last": last, "cmd": " ".join(cmd)}


def parse_log(path):
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    r = {}
    m = re.search(r"\[sched\] stopped: (.*?) \(at ([0-9A-F]+), extra ([0-9A-F]+)\); virtual time ([0-9.]+) s", text)
    r["stopped"] = m.group(1) if m else None
    r["stop_at"] = m.group(2) if m else None
    r["virtual_s"] = float(m.group(4)) if m else None
    r["traps"] = re.findall(r"^\[trap\].*$", text, re.M)[:5]
    r["unimplemented"] = sorted(set(re.findall(r"\[hle\] UNIMPLEMENTED (\S+?)\(", text)))
    r["not_found"] = sorted(set(re.findall(r'\[io\] open(?: async)? "([^"]+)" -> not found', text)) - set(G.expected_missing))
    r["ioctl"] = sorted(set(re.findall(r"\[io\] ioctl fd \d+ cmd ([0-9A-F]+)", text)))
    r["savedata_unimpl"] = sorted(set(re.findall(r"\[savedata\] mode (\S+) \(\d+\) is not implemented", text)))
    r["errors"] = [l for l in re.findall(r"^.*(?:\brt: |bad jump|missing function|unknown|abort).*$", text, re.M)
                   if "[io] chdir" not in l][:5]
    levels = re.findall(r"\[level\] loaded: (.*)$", text, re.M)
    seen, current = {}, None
    for line in levels:
        lv, _, game = line.partition(" | game: ")
        for tok in lv.split():
            mm = re.match(r"(\*?)(\S+)\((\d+)\)", tok)
            if mm:
                seen[mm.group(2)] = max(seen.get(mm.group(2), 0), int(mm.group(3)))
                if mm.group(1):
                    current = mm.group(2)
    r["levels_seen"] = seen
    r["current_at_end"] = current
    r["last_levels_line"] = levels[-1] if levels else None
    m = re.search(r"god mode refilled Kratos's health (\d+) times(?:; health reached 0 (\d+) times)?", text)
    r["god_refills"] = int(m.group(1)) if m else None
    r["deaths"] = int(m.group(2)) if m and m.group(2) else 0
    # spawn points of the level the game is in, once it is the current level ("game: NAME spawn S of N")
    r["spawn_points"] = None
    for line in levels:
        m = re.search(r"\*(\S+)\(\d+\).*\| game: (\S+) spawn \d+ of (\d+)", line)
        if m and m.group(1) == m.group(2):
            r["spawn_points"] = max(r["spawn_points"] or 0, int(m.group(3)))
    r["psnr"] = {int(f): float(p) for f, p in re.findall(r"\[compare\] frame (\d+):.*PSNR ([0-9.]+) dB", text)}
    r["compare_missing"] = re.findall(r"\[compare\] frame (\d+): the GPU has no target", text)
    r["warp"] = bool(re.search(r"\[debug\] warp to ", text))
    m = re.search(r"\[hle\] unimplemented functions that were called:(.*)$", text, re.M)
    r["unimplemented_summary"] = m.group(1).strip() if m else None
    return r


def perf(path, start_frame):
    try:
        rows = [r for r in csv.DictReader(open(path)) if int(r["frame"]) >= start_frame]
    except OSError:
        return None
    ft = sorted(float(r["host_frame_ms"]) for r in rows)
    if not ft:
        return None
    vt = [float(r["virtual_ms"]) for r in rows]
    return {"frames": len(ft), "host_fps": round(1000 * len(ft) / sum(ft), 1), "p50_ms": round(ft[len(ft) // 2], 2),
            "p99_ms": round(ft[max(0, int(len(ft) * 0.99) - 1)], 2), "max_ms": round(ft[-1], 1),
            "over_16_7ms": sum(1 for x in ft if x > 16.7),
            "game_fps": round(1000 * (len(vt) - 1) / (vt[-1] - vt[0]), 2) if len(vt) > 1 and vt[-1] > vt[0] else None}


def frame_stats(d, shots):
    """Mean brightness of each shot and how many shots are identical to the one before (a stuck picture)."""
    out, prev, same = [], None, 0
    for f in shots:
        p = d / f"frame_{f:06d}.png"
        if not p.exists():
            out.append(None)
            continue
        im = Image.open(p).convert("RGB")
        out.append(round(sum(ImageStat.Stat(im.convert("L")).mean), 1))
        if prev is not None and im.tobytes() == prev:
            same += 1
        prev = im.tobytes()
    return out, same


def sheet(level, d, shots, path):
    ims = [(f, d / f"frame_{f:06d}.png") for f in shots]
    ims = [(f, Image.open(p).convert("RGB")) for f, p in ims if p.exists()]
    if not ims:
        return None
    w, h, cols = 240, 136, 5
    rows = (len(ims) + cols - 1) // cols
    s = Image.new("RGB", (w * cols, h * rows + 18), (24, 24, 24))
    dr = ImageDraw.Draw(s)
    dr.text((4, 3), level, fill=(255, 255, 255))
    for i, (f, im) in enumerate(ims):
        x, y = (i % cols) * w, 18 + (i // cols) * h
        s.paste(im.resize((w, h)), (x, y))
        dr.text((x + 3, y + 3), str(f), fill=(255, 255, 0))
    s.save(path)
    return path


def zone(level):
    """"name" or "name:spawn" (--warp's spawn point): the zone's name."""
    return level.split(":")[0]


def tour_level(a, level, input_file):
    base = Path(a.out) / level.replace(":", "__s")
    res = {"level": level}
    for mode in ("gpu", "both"):
        d = base / mode
        done = (d / "run.json").exists()
        if mode in a.modes and not (a.reuse and done):
            info = run_one(a.exe, a.image, a.disc, level, mode, d, a.seconds, input_file, a.timeout, a.both_from)
            (d / "run.json").write_text(json.dumps(info, indent=1))
        elif done:  # a run of an earlier pass (e.g. --modes both, then --modes gpu --jobs 1 for frame rates)
            info = json.loads((d / "run.json").read_text())
        else:
            continue
        info.update(parse_log(d / "run.log"))
        shots = info.get("shots") or []
        info["brightness"], info["stuck_shots"] = frame_stats(d, shots)
        if mode == "gpu":
            info["perf"] = perf(d / "perf.csv", G.warp_frame + 200)
        res[mode] = info
    ref = "gpu" if "gpu" in res else "both"  # the GPU run has shots over the whole run
    res["sheet"] = sheet(level, base / ref, res[ref].get("shots") or [], base / "sheet.png") if ref in res else None
    return res


def verdict(r):
    probs = []
    for mode in ("gpu", "both"):
        i = r.get(mode)
        if not i:
            continue
        if i["exit"] not in (0,):
            probs.append(f"{mode}: exit {i['exit']}")
        if i["stopped"] and i["stopped"] != "frame limit reached":
            probs.append(f"{mode}: stopped: {i['stopped']} at {i['stop_at']}")
        if not i["stopped"]:
            probs.append(f"{mode}: no scheduler stop line (crash?)")
        if i["traps"]:
            probs.append(f"{mode}: trap")
    any_i = r.get("gpu") or r.get("both")
    lv = any_i["levels_seen"].get(zone(r["level"]), 0) if any_i else 0
    if lv < 5:
        probs.append(f"level state {lv} (never loaded)")
    return probs


def write_summary(a, results):
    out = Path(a.out)
    (out / "summary.json").write_text(json.dumps(results, indent=1, default=str))
    lines = ["| level | result | loaded | current at end | other levels streamed | unimplemented HLE | PSNR soft/GPU (dB) | GPU host FPS (p99 ms) | game FPS |",
             "|---|---|---|---|---|---|---|---|---|"]
    for r in results:
        g, b = r.get("gpu") or {}, r.get("both") or {}
        i = g or b
        probs = verdict(r)
        seen = i.get("levels_seen", {})
        others = [n for n in seen if n not in (zone(r["level"]),) + G.menu_levels]
        psnr = b.get("psnr") or {}
        pf = g.get("perf") or {}
        lines.append("| {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
            r["level"], "ok" if not probs else "; ".join(probs), seen.get(zone(r["level"]), 0), i.get("current_at_end"),
            ", ".join(others) or "-", ", ".join(sorted(set(g.get("unimplemented", []) + b.get("unimplemented", [])))) or "-",
            " / ".join(f"{v:.1f}" for v in psnr.values()) or "-",
            f"{pf.get('host_fps')} ({pf.get('p99_ms')})" if pf else "-", pf.get("game_fps", "-") if pf else "-"))
    (out / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    sheets = [Image.open(r["sheet"]) for r in results if r.get("sheet")]
    if sheets:
        # one picture per level: the middle shot
        w, h, cols = 240, 136, 6
        rows = (len(results) + cols - 1) // cols
        ov = Image.new("RGB", (w * cols, h * rows), (24, 24, 24))
        dr = ImageDraw.Draw(ov)
        for k, r in enumerate(results):
            i = r.get("gpu") or r.get("both") or {}
            shots = i.get("shots") or []
            d = out / r["level"].replace(":", "__s") / ("gpu" if r.get("gpu") else "both")
            pick = None
            for f in shots[len(shots) // 2:] + shots[:len(shots) // 2]:
                if (d / f"frame_{f:06d}.png").exists():
                    pick = d / f"frame_{f:06d}.png"
                    break
            x, y = (k % cols) * w, (k // cols) * h
            if pick:
                ov.paste(Image.open(pick).convert("RGB").resize((w, h)), (x, y))
            dr.rectangle([x, y, x + w - 1, y + 13], fill=(0, 0, 0))
            dr.text((x + 3, y + 1), r["level"], fill=(255, 255, 0) if not verdict(r) else (255, 80, 80))
        ov.save(out / "overview.png")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--levels", help="comma-separated; default: every main zone on the disc")
    ap.add_argument("--addons", action="store_true", help="also warp into the add-on zones (_enemies, _enc, _pushbox)")
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--seconds", type=float, default=90)
    ap.add_argument("--modes", default="gpu,both")
    ap.add_argument("--timeout", type=int, default=1800, help="per run, wall seconds")
    ap.add_argument("--both-from", type=int, default=0,
                    help="in --renderer both runs, draw and compare only from this frame on (the software "
                         "renderer is the slow part; 0 = the whole run, compared at every other shot)")
    ap.add_argument("--via", default="", help="GoS: levels to start through --warp-via even when they fit next to "
                                              "the main menu, or \"all\": the hard level change (the one Load Game "
                                              "takes) starts them in a clean heap, with the save system set up")
    ap.add_argument("--reuse", action="store_true", help="keep runs that already finished (the summary always "
                                                          "includes the other mode's runs found in --out)")
    ap.add_argument("--game", choices=sorted(GAMES), default="coo")
    ap.add_argument("--exe")
    ap.add_argument("--image")
    ap.add_argument("--disc")
    a = ap.parse_args()
    global G
    G = GAMES[a.game]
    a.exe, a.image, a.disc = (str(Path(x).resolve()) for x in (a.exe or G.exe, a.image or G.image, a.disc or G.disc))
    a.modes = a.modes.split(",")
    VIA.update(x for x in a.via.split(",") if x)
    main_levels, addons = list_levels(a.exe, a.disc)
    levels = a.levels.split(",") if a.levels else main_levels + (addons if a.addons else [])
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "levels.json").write_text(json.dumps({"main": main_levels, "addons": addons}, indent=1))
    input_file = out / "input.txt"
    input_file.write_text(make_input(G.input_start_s, a.seconds).replace(",", "\n") + "\n")
    print(f"{len(levels)} levels, {a.jobs} at a time, modes {a.modes}, {a.seconds:.0f} s each -> {out}", flush=True)
    results = {}
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        futs = {ex.submit(tour_level, a, lv, input_file): lv for lv in levels}
        for f in cf.as_completed(futs):
            lv = futs[f]
            try:
                results[lv] = f.result()
            except Exception as e:  # keep going; the level shows as failed
                results[lv] = {"level": lv, "error": repr(e)}
            r = results[lv]
            msg = r["error"] if "error" in r else ("; ".join(verdict(r)) or "ok")
            print(f"{len(results):3}/{len(levels)} {lv}: {msg}", flush=True)
    write_summary(a, [results[lv] for lv in levels if "error" not in results[lv]])
    return 0


if __name__ == "__main__":
    sys.exit(main())
