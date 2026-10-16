"""Which HLE functions a stretch of the game calls: runs gow_rt with --trace-hle (every call is
logged) and keeps only the distinct functions with their call counts, so the log never hits the disk.

  .venv/Scripts/python -I tools/hle_calls.py --out build/tour/hle_calls.json \
      [--levels a,b,...] [--seconds 40] [--jobs 2] [--baseline-input build/rec_v9/input.txt --baseline-frames 6800]

Each level is reached with --warp (runtime/rt_debug.c) and played with tools/level_tour.py's generic
input; nothing is drawn (--fast-forward past the end). The baseline is a normal boot with an input
file (the first level as played so far). tools/hle_audit.py --calls uses the JSON."""
import argparse
import concurrent.futures as cf
import json
import re
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import level_tour as lt  # noqa: E402

CALL = re.compile(r"\[hle\] (?:UNIMPLEMENTED )?(\w+)::(\w+)\(")


def run(exe, image, disc, args, frames):
    with tempfile.TemporaryDirectory() as ms0:
        cmd = [str(exe), "--image", str(image), "--boot", "--disc", str(disc), "--ms0", ms0, "--trace-hle",
               "--max-frames", str(frames), "--fast-forward", str(frames + 1), "--cpu-ms", "3600000",
               "--wall-ms", "3600000"] + args
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
        calls, tail = Counter(), []
        for line in p.stdout:
            m = CALL.search(line)
            if m:
                calls[f"{m.group(1)}::{m.group(2)}"] += 1
            elif not line.startswith("[") or "[sched] stopped" in line:
                tail.append(line.rstrip())
        p.wait()
    return {"calls": dict(calls), "exit": p.returncode, "tail": tail[-5:]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--levels", default="pi_king_fight,mar_02_village,ht07_throne_room,charon_galley_1,"
                                        "tar_caves_04_maintitan,poh_persephone_chamber,ef_03_crash_landing,"
                                        "foa_deathknight_arena,ec_3_eos_chamber,cog_01")
    ap.add_argument("--seconds", type=float, default=40)
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--baseline-input")
    ap.add_argument("--baseline-frames", type=int, default=6800)
    ap.add_argument("--exe", default=str(lt.DEF_EXE))
    ap.add_argument("--image", default=str(lt.DEF_IMAGE))
    ap.add_argument("--disc", default=str(lt.DEF_DISC))
    a = ap.parse_args()
    inp = Path(a.out).with_suffix(".input.txt")
    inp.write_text(lt.make_input(lt.INPUT_START_S, a.seconds).replace(",", "\n") + "\n")
    frames = lt.WARP_FRAME + int((lt.INPUT_START_S - 16.9 + a.seconds) * lt.FPS)
    jobs = {lv: ["--warp", lv, "--god", "--input", str(inp)] for lv in a.levels.split(",")}
    if a.baseline_input:
        jobs["(baseline: boot, menu, new game, first level)"] = ["--input", a.baseline_input]
    res = {}
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        futs = {ex.submit(run, a.exe, a.image, a.disc, args, a.baseline_frames if lv.startswith("(") else frames): lv
                for lv, args in jobs.items()}
        for f in cf.as_completed(futs):
            res[futs[f]] = f.result()
            print(f"{futs[f]}: {len(res[futs[f]]['calls'])} functions, exit {res[futs[f]]['exit']}", flush=True)
    Path(a.out).write_text(json.dumps(res, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
