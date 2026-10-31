"""Static recompiler driver: decrypted PSP EBOOT (PRX) + Ghidra export -> C sources.

Usage:
  python -I recomp.py --elf EBOOT.BIN --ghidra X.recomp.json --nids psp_nids.tsv --out DIR
                      [--base 0x08804000] [--difftest 300] [--boot 200] [--seed 1]

Every function is translated once in memory (coverage report); only the selected
subset is written as C. Output: funcs.h, code_NN.c, stubs.c, table.c, module_info.c,
image.bin (relocated image) and module.json (metadata for tests and tooling).
"""
from __future__ import annotations

import argparse
import bisect
import json
import random
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from allegrex import BRANCH, CALL, JALR, JR, JUMP, TRAP, decode  # noqa: E402
from emitc import FuncInfo, Program, Untranslatable, emit_function  # noqa: E402
from pspelf import PspModule  # noqa: E402

CHUNK = 150


class RecordingProgram(Program):
    """Program that records every external control-transfer target per function."""

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.refs: dict[int, None] = {}  # ordered set, in call-site order

    def call(self, target: int) -> str:
        self.refs.setdefault(target)
        return super().call(target)


def load_nids(path: Path) -> dict[int, str]:
    out = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        f = line.split("\t")
        if len(f) >= 2:
            out[int(f[0], 16)] = f[1]
    return out


def auto_analyze(mod: PspModule) -> tuple[dict[int, FuncInfo], dict[int, list[int]]]:
    base = mod.base
    text = mod.section(".text")
    t_lo, t_hi = base + text.addr, base + text.addr + text.size
    rodata = mod.section(".rodata")
    r_lo, r_hi = base + rodata.addr, base + rodata.addr + rodata.size
    hilo_by_lo = {lo: val for _hi, lo, val in mod.hilo_values}
    rodata_refs = sorted(set(val for _hi, _lo, val in mod.hilo_values if r_lo <= val < r_hi))
    jumptables: dict[int, list[int]] = {}
    jt_rodata_addrs: set[int] = set()
    for pc in range(t_lo, t_hi, 4):
        ins = decode(mod.u32(pc), pc)
        if ins.flow == JR and ins.rs != 31:
            tbl = next((hilo_by_lo[pc - off] for off in range(4, 32, 4)
                        if pc - off in hilo_by_lo and r_lo <= hilo_by_lo[pc - off] < r_hi), None)
            if not tbl:
                continue
            n_bound = None
            for off in range(4, 64, 4):
                bi = decode(mod.u32(pc - off), pc - off)
                prev = decode(mod.u32(pc - off - 4), pc - off - 4)
                if prev.flow == BRANCH and prev.likely:
                    continue
                if bi.op in ("sltiu", "slti") and 0 < bi.uimm < 2048:
                    n_bound = bi.uimm
                    break
            idx = bisect.bisect_right(rodata_refs, tbl)
            next_ref = rodata_refs[idx] if idx < len(rodata_refs) else r_hi
            pts, addr = [], tbl
            while addr < min(next_ref, r_hi):
                val = mod.addr32_values.get(addr, 0)
                if not (t_lo <= val < t_hi) or (val & 3):
                    break
                pts.append(val)
                jt_rodata_addrs.add(addr)
                addr += 4
                if n_bound and len(pts) >= n_bound:
                    break
            if pts:
                jumptables[pc] = sorted(set(pts))
    entries = {mod.entry_addr}
    for pc in range(t_lo, t_hi, 4):
        ins = decode(mod.u32(pc), pc)
        if ins.flow == CALL and t_lo <= ins.target < t_hi:
            entries.add(ins.target)
        elif ins.flow == BRANCH and ins.link and t_lo <= ins.target < t_hi:
            entries.add(ins.target)
    for _hi_a, _lo_a, val in mod.hilo_values:
        if t_lo <= val < t_hi and not (val & 3):
            entries.add(val)
    for a, val in mod.addr32_values.items():
        if a not in jt_rodata_addrs and t_lo <= val < t_hi and not (val & 3):
            entries.add(val)
    funcs: dict[int, FuncInfo] = {}
    for entry in sorted(entries):
        fi = FuncInfo(entry=entry, name=f"FUN_{entry - base:08x}", addrs=[entry])
        i = decode(mod.u32(entry), entry)
        fi.insns[entry] = i
        fi.has_vfpu = i.is_vfpu
        fi.has_jalr = i.flow == JALR
        fi.has_indirect_jr = i.flow == JR and i.rs != 31
        funcs[entry] = fi
    return funcs, jumptables


def detect_co_switch(mod: PspModule) -> tuple[int, int, int] | None:
    base = mod.base
    text = mod.section(".text")
    t_lo, t_hi = base + text.addr, base + text.addr + text.size
    for pc in range(t_lo, t_hi - 0x70, 4):
        if mod.u32(pc) == 0x27BDFFA0 and mod.u32(pc + 4) == 0xAFA40000 and mod.u32(pc + 8) == 0xAFBF0004:
            i_lui = decode(mod.u32(pc + 0x68), pc + 0x68)
            i_lw = decode(mod.u32(pc + 0x6C), pc + 0x6C)
            if i_lui.op == "lui" and i_lw.op == "lw":
                swap_slot = ((i_lui.uimm << 16) + i_lw.imm) & 0xFFFFFFFF
                return pc, swap_slot, (swap_slot + 8) & 0xFFFFFFFF
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", required=True)
    ap.add_argument("--ghidra", required=True)
    ap.add_argument("--nids", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--base", default="0x08804000")
    ap.add_argument("--difftest", type=int, default=300, help="number of leaf-closure roots for differential tests")
    ap.add_argument("--boot", type=int, default=200, help="max functions reachable from the entry point")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--all", action="store_true", help="emit every translatable function")
    args = ap.parse_args()
    base = int(args.base, 16)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    mod = PspModule.parse(Path(args.elf).read_bytes())
    mod.load(base)
    g = json.loads(Path(args.ghidra).read_text(encoding="utf-8"))
    gbase = int(g["image_base"], 16)
    delta = base - gbase
    nids = load_nids(Path(args.nids))
    stubs = {imp.stub: (imp.nid, imp.library, nids.get(imp.nid, f"{imp.library}_{imp.nid:08X}")) for imp in mod.imports()}

    funcs: dict[int, FuncInfo] = {}
    for f in g["functions"]:
        entry = int(f["addr"], 16) + delta
        addrs = []
        for lo, hi in f["ranges"]:
            addrs.extend(range(int(lo, 16) + delta, int(hi, 16) + delta + 1, 4))
        fi = FuncInfo(entry=entry, name=f["name"], addrs=sorted(set(addrs)))
        for pc in fi.addrs:
            i = decode(mod.u32(pc), pc)
            fi.insns[pc] = i
            if i.is_vfpu:
                fi.has_vfpu = True
            if i.flow == JALR:
                fi.has_jalr = True
            if i.flow == JR and i.rs != 31:
                fi.has_indirect_jr = True
        funcs[entry] = fi
    jumptables = {int(j["at"], 16) + delta: [int(t, 16) + delta for t in j["targets"]] for j in g["jumptables"]}
    if mod.section(".text").size != int(g["text"][1], 16) - int(g["text"][0], 16) + 1:
        funcs, jumptables = auto_analyze(mod)

    # Body completion: follow control flow from each entry ourselves and add reachable code
    # that Ghidra left out of the function (e.g. a fall-through it stopped at). Other
    # function entries end the walk (they become tail calls).
    text = mod.section(".text")
    t_lo, t_hi = base + text.addr, base + text.addr + text.size
    extended = 0
    for e, fi in funcs.items():
        body, work = set(fi.addrs), [e]
        seen = set()
        while work:
            pc = work.pop()
            if pc in seen or not (t_lo <= pc < t_hi) or (pc != e and pc in funcs):
                continue
            seen.add(pc)
            i = fi.insns.get(pc) or decode(mod.u32(pc), pc)
            if i.op == "invalid":
                continue
            body.add(pc)
            if i.has_delay_slot:
                body.add(pc + 4)
                if i.flow == BRANCH:
                    work += [i.target, pc + 8]
                elif i.flow == JUMP:
                    work.append(i.target)
                elif i.flow in (CALL, JALR):
                    work.append(pc + 8)
                elif i.flow == JR and i.rs != 31:
                    work += jumptables.get(pc, [])
            elif i.flow != TRAP:
                work.append(pc + 4)
        if len(body) > len(fi.addrs):
            extended += 1
            for pc in body - set(fi.addrs):
                i = decode(mod.u32(pc), pc)
                fi.insns[pc] = i
                fi.has_vfpu |= i.is_vfpu
                fi.has_jalr |= i.flow == JALR
                fi.has_indirect_jr |= i.flow == JR and i.rs != 31
            fi.addrs = sorted(body)

    # Pass 1: translate everything in memory, record references and failures
    all_entries = set(funcs)
    prog = RecordingProgram(mod.u32, funcs, jumptables, stubs, all_entries)
    refs: dict[int, list[int]] = {}
    reasons = Counter()
    ok: set[int] = set()
    ok_insns = 0
    for e, fi in funcs.items():
        prog.refs = {}
        try:
            emit_function(fi, prog)
            ok.add(e)
            ok_insns += len(fi.addrs)
        except Untranslatable as ex:
            fi.untranslatable = str(ex)
            reasons[str(ex).split(" at ")[0]] += 1
        refs[e] = list(prog.refs)
    total_insns = sum(len(f.addrs) for f in funcs.values())
    unresolved_jr = sum(1 for e in ok for pc in funcs[e].addrs
                        if funcs[e].insns[pc].flow == JR and funcs[e].insns[pc].rs != 31 and not jumptables.get(pc))

    # Selection A: leaf closures for differential testing (no HLE, no indirect flow, no VFPU)
    def clean(e: int) -> bool:
        f = funcs.get(e)
        # Unicorn has no VFPU, so VFPU functions cannot be differential-tested against it
        return f is not None and e in ok and not f.has_jalr and not f.has_indirect_jr and not f.has_vfpu

    def closure(root: int, limit: int = 12) -> set[int] | None:
        seen, todo = {root}, [root]
        while todo:
            e = todo.pop()
            for t in refs[e]:
                if t in stubs or not clean(t):
                    return None
                if t not in seen:
                    seen.add(t)
                    todo.append(t)
                    if len(seen) > limit:
                        return None
        return seen

    candidates = sorted(e for e in ok if clean(e) and len(funcs[e].addrs) >= 4)
    rng = random.Random(args.seed)
    rng.shuffle(candidates)
    diff_roots, diff_set = [], set()
    for e in candidates:
        if len(diff_roots) >= args.difftest:
            break
        c = closure(e)
        if c is not None and sum(len(funcs[x].addrs) for x in c) <= 800:
            diff_roots.append(e)
            diff_set |= c

    # Selection B: breadth-first from the module entry point, following direct transfers and
    # function addresses materialized with lui+addiu/ori (thread entries, callbacks, vtables)
    # C++ static initializers: .cplinit holds (function, priority) pairs run by libc startup
    cpl = next((s for s in mod.sections if s.name == ".cplinit"), None)
    cpl_lo = cpl_hi = 0
    cpl_targets: list[int] = []
    if cpl:
        cpl_lo, cpl_hi = base + cpl.addr, base + cpl.addr + cpl.size
        pairs = [(mod.u32(a + 4), mod.u32(a)) for a in range(cpl_lo, cpl_hi - 7, 8)]
        cpl_targets = [f for _, f in sorted(pairs, key=lambda p: p[0]) if f in funcs]

    def code_constants(fi: FuncInfo) -> list[int]:
        hi, found = {}, {}
        for pc in fi.addrs:
            i = fi.insns[pc]
            if i.op == "lui":
                hi[i.rt] = i.uimm << 16
            elif i.op in ("addiu", "ori") and i.rs in hi:
                v = (hi[i.rs] + i.imm) & 0xFFFFFFFF if i.op == "addiu" else hi[i.rs] | i.uimm
                if v in funcs:
                    found.setdefault(v)
                elif cpl_lo <= v < cpl_hi:  # this function walks the initializer table
                    for t in cpl_targets:
                        found.setdefault(t)
        return list(found)

    # Depth-first in call-site order approximates the order in which boot code runs
    entry = mod.entry_addr
    boot_set, stack = set(), [entry]
    while stack and len(boot_set) < args.boot:
        e = stack.pop()
        if e in boot_set or e not in ok:
            continue
        boot_set.add(e)
        nxt = [t for t in dict.fromkeys(refs[e] + code_constants(funcs[e])) if t in funcs and t not in boot_set]
        stack.extend(reversed(nxt))
    generated = set(ok) if args.all else diff_set | boot_set

    # Pass 2: emit selected functions
    prog = Program(mod.u32, funcs, jumptables, stubs, generated)
    gen = sorted(generated)
    # Functions the runtime provides (e.g. a context switch that static code cannot express)
    game_id = Path(args.elf).name.split("_")[0]
    rt_cfg = json.loads((Path(__file__).parent / "runtime_funcs.json").read_text(encoding="utf-8"))
    runtime_funcs = {int(a, 16): call for a, call in rt_cfg.get(game_id, {}).items()}
    co_info = detect_co_switch(mod)
    if co_info is not None:
        co_pc, swap_slot, cur_slot = co_info
        runtime_funcs = {co_pc: f"rt_co_switch(c, 0x{swap_slot:08X}u, 0x{cur_slot:08X}u)"}
        known_ids = {0x08B36148: "UCUS98653", 0x08B36158: "UCES00842", 0x08C15D88: "UCUS98737", 0x08C173D0: "UCES01473"}
        game_id = known_ids.get(swap_slot, game_id)
    for a in runtime_funcs:
        if a not in generated:
            print(f"{game_id}: runtime function 0x{a:08X} is not a generated function")

    def emit(e: int) -> str:
        if e in runtime_funcs:
            return (f"/* {funcs[e].name}: provided by the runtime (recomp/runtime_funcs.json) */\n"
                    f"void f_{e:08X}(RtCtx *c) {{ {runtime_funcs[e]}; }}\n")
        return f"/* {funcs[e].name} */\n" + emit_function(funcs[e], prog)
    (out / "funcs.h").write_text(
        "#pragma once\n#include \"psp_rt.h\"\n"
        + "".join(f"void f_{e:08X}(RtCtx *c);\n" for e in gen)
        + "".join(f"void f_{s:08X}(RtCtx *c); /* {v[1]}::{v[2]} */\n" for s, v in sorted(stubs.items())),
        encoding="utf-8")
    for old in out.glob("code_*.c"):
        old.unlink()
    for n in range(0, len(gen), CHUNK):
        body = [emit(e) for e in gen[n:n + CHUNK]]
        (out / f"code_{n // CHUNK:03d}.c").write_text(
            "/* Generated by recomp.py from the user's own game dump - personal use only. */\n"
            "#include \"funcs.h\"\n#ifdef _MSC_VER\n#pragma warning(disable: 4102 4100)\n#endif\n\n"
            + "\n".join(body), encoding="utf-8")
    (out / "stubs.c").write_text(
        "#include \"funcs.h\"\n\n" + "".join(
            f"void f_{s:08X}(RtCtx *c) {{ rt_hle(c, 0x{nid:08X}u, \"{lib}\", \"{name}\"); }}\n"
            for s, (nid, lib, name) in sorted(stubs.items())), encoding="utf-8")
    table = sorted([(e, f"f_{e:08X}") for e in gen] + [(s, f"f_{s:08X}") for s in stubs])
    (out / "table.c").write_text(
        "#include \"funcs.h\"\n\nconst RtFuncEntry rt_funcs[] = {\n"
        + "".join(f"  {{0x{a:08X}u, {n}}},\n" for a, n in table)
        + f"}};\nconst size_t rt_funcs_count = {len(table)};\n", encoding="utf-8")
    mi = mod.module_info()
    (out / "module_info.c").write_text(
        "#include \"psp_rt.h\"\n"
        f"const char *const RT_GAME_ID = \"{game_id}\";\n"
        f"const char *const RT_MODULE_NAME = \"{mi['name']}\";\n"
        f"const uint32_t RT_IMAGE_BASE = 0x{base:08X}u;\n"
        f"const uint32_t RT_IMAGE_SIZE = 0x{len(mod.image):X}u;\n"
        f"const uint32_t RT_ENTRY = 0x{entry:08X}u;\n"
        f"const uint32_t RT_GP = 0x{mi['gp']:08X}u;\n", encoding="utf-8")
    (out / "image.bin").write_bytes(bytes(mod.image))
    report = {
        "game_id": game_id, "base": f"0x{base:08X}", "entry": f"0x{entry:08X}", "gp": f"0x{mi['gp']:08X}",
        "module_name": mi["name"], "image_size": len(mod.image), "text_size": mod.section(".text").size,
        "relocations": mod.reloc_stats,
        "coverage": {
            "functions": len(funcs), "translatable": len(ok), "bodies_extended_by_cfg": extended,
            "functions_with_vfpu": sum(1 for f in funcs.values() if f.has_vfpu),
            "instructions": total_insns, "translatable_instructions": ok_insns,
            "unresolved_indirect_jr_in_translatable": unresolved_jr,
            "untranslatable_reasons": dict(reasons.most_common()),
        },
        "selection": {"difftest_roots": [f"0x{e:08X}" for e in diff_roots], "difftest_functions": len(diff_set),
                      "boot_functions": len(boot_set), "generated": len(gen),
                      "generated_instructions": sum(len(funcs[e].addrs) for e in gen)},
        "imports": [{"stub": f"0x{s:08X}", "nid": f"0x{v[0]:08X}", "library": v[1], "name": v[2]} for s, v in sorted(stubs.items())],
        "functions": {f"0x{e:08X}": {"name": funcs[e].name, "instructions": len(funcs[e].addrs)} for e in gen},
    }
    (out / "module.json").write_text(json.dumps(report, indent=1), encoding="utf-8")
    c = report["coverage"]
    print(f"{game_id}: {extended} function bodies extended by own control-flow walk")
    print(f"{game_id}: {c['translatable']}/{c['functions']} functions translatable "
          f"({100 * c['translatable_instructions'] / c['instructions']:.2f}% of instructions); "
          f"untranslatable: {c['untranslatable_reasons']}")
    print(f"selected: {len(diff_roots)} difftest roots ({len(diff_set)} functions), {len(boot_set)} boot functions, "
          f"{len(gen)} generated, {report['selection']['generated_instructions']} instructions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
