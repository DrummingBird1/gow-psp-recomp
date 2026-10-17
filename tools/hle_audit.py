"""Static audit of the HLE layer against the games' imports (docs/phase11-level-tour.md).

For every import of a game (build/gen/<GAME>_full/module.json) it finds the runtime's handler
(the HleEntry tables in runtime/*.c, matched by name like rt_hle.c does) and classifies it:
  real     - a handler that does work (a one-line handler that returns state, e.g.
             sceKernelGetThreadId, counts as real)
  stub     - a handler whose whole body is RET(number): hle_Zero and the like
  missing  - no handler: rt_hle() logs "UNIMPLEMENTED" once and returns 0
It also counts the call sites of each import stub in the recompiled code and, with --calls
(tools/hle_calls.py output), which runs called it.

  .venv/Scripts/python -I tools/hle_audit.py [--games UCUS98653,UCUS98737] [--calls build/tour/hle_calls.json]
      [--calls-game UCUS98653] [--md out.md] [--json out.json]"""
import argparse
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def handler_tables():
    """name -> (handler, file); handler bodies: handler -> body text."""
    names, bodies = {}, {}
    for path in sorted(glob.glob(os.path.join(ROOT, "runtime", "*.c"))):
        src = open(path, encoding="utf-8", errors="replace").read()
        for m in re.finditer(r"const HleEntry rt_hle_\w+\[\]\s*=\s*\{(.*?)\{NULL,\s*NULL\}", src, re.S):
            for e in re.finditer(r'\{"(\w+)",\s*(\w+)\}', m.group(1)):
                names.setdefault(e.group(1), (e.group(2), os.path.basename(path)))
        for m in re.finditer(r"static\s+void\s+(\w+)\s*\(\s*RtCtx\s*\*\s*c\s*\)\s*\{", src):
            i, depth = m.end(), 1
            while depth and i < len(src):
                depth += {"{": 1, "}": -1}.get(src[i], 0)
                i += 1
            bodies[m.group(1)] = src[m.end():i - 1]
    return names, bodies


def classify(handler, bodies):
    if handler is None:
        return "missing", ""
    body = bodies.get(handler)
    if body is None:
        return "real", ""  # defined through a macro or elsewhere
    stripped = re.sub(r"/\*.*?\*/|//[^\n]*", "", body, flags=re.S).strip()
    stripped = re.sub(r"\(void\)\s*c\s*;", "", stripped).strip()
    m = re.fullmatch(r"RET\(([^;]*)\);", stripped)
    if m and re.fullmatch(r"-?(0x[0-9A-Fa-f]+|\d+)u?", m.group(1).strip()):
        return "stub", m.group(1).strip()
    return "real", ""


def call_sites(game):
    """stub address (upper hex) -> number of call sites in the recompiled code."""
    counts = {}
    for path in glob.glob(os.path.join(ROOT, "build", "gen", game + "_full", "code_*.c")):
        for m in re.finditer(r"f_(08[0-9A-F]{6})\(c\);", open(path, encoding="utf-8").read()):
            counts[m.group(1)] = counts.get(m.group(1), 0) + 1
    return counts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--games", default="UCUS98653,UCUS98737")
    ap.add_argument("--calls", help="tools/hle_calls.py output")
    ap.add_argument("--calls-game", default="UCUS98653", help="the game whose runs --calls holds")
    ap.add_argument("--md", help="write a markdown table of the stubs and missing functions here")
    ap.add_argument("--json", help="write the whole classification as JSON here")
    a = ap.parse_args()
    names, bodies = handler_tables()
    runs = json.load(open(a.calls)) if a.calls else {}
    out = {}
    for game in a.games.split(","):
        mod = json.load(open(os.path.join(ROOT, "build", "gen", game + "_full", "module.json")))
        sites = call_sites(game)
        rows = []
        for imp in mod["imports"]:
            h, f = names.get(imp["name"], (None, ""))
            kind, val = classify(h, bodies)
            key = f"{imp['library']}::{imp['name']}"
            called = {lv: r["calls"][key] for lv, r in runs.items() if key in r["calls"]} if game == a.calls_game else {}
            rows.append({"library": imp["library"], "name": imp["name"], "nid": imp["nid"], "stub": imp["stub"],
                         "kind": kind, "returns": val, "handler": h or "", "file": f,
                         "call_sites": sites.get(imp["stub"][2:].upper(), 0), "called_in": called})
        out[game] = rows
        n = {k: sum(1 for r in rows if r["kind"] == k) for k in ("real", "stub", "missing")}
        print(f"{game}: {len(rows)} imports: {n['real']} real, {n['stub']} stubs returning a constant, {n['missing']} missing")
        for r in rows:
            if r["kind"] != "real":
                where = ", ".join(f"{k} x{v}" for k, v in r["called_in"].items()) if r["called_in"] else ""
                print(f"  {r['kind']:7} {r['library']:20} {r['name']:40} -> {r['returns'] or '0 (logged)':10} "
                      f"sites={r['call_sites']:2} {where}")
    if a.json:
        json.dump(out, open(a.json, "w"), indent=1)
    if a.md:
        with open(a.md, "w", encoding="utf-8") as f:
            f.write("| game | library | function | kind | returns | call sites | called in |\n|---|---|---|---|---|---|---|\n")
            for game, rows in out.items():
                for r in rows:
                    if r["kind"] != "real":
                        f.write(f"| {game} | {r['library']} | `{r['name']}` | {r['kind']} | {r['returns'] or '0'} | "
                                f"{r['call_sites']} | {', '.join(r['called_in']) or '-'} |\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
