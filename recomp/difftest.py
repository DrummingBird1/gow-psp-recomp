"""Differential test: recompiled functions (gow_rt --difftest) vs Unicorn (MIPS32 24Kf).

For each difftest root in module.json, random initial states are generated (registers,
FPU, a scratch heap full of plausible pointers, a stack), executed in Unicorn until the
function returns, and replayed in the recompiled build. Final registers, HI/LO, FPU
registers/condition and CRCs of the image, scratch and stack regions are compared.

Allegrex-only encodings (clz/clo/madd/max/min/... in SPECIAL) and Allegrex-specific
results (division by zero, float->int overflow) are classified, not counted as bugs.

Usage: .venv/Scripts/python -I recomp/difftest.py <gen dir> <gow_rt exe> [--cases 8] [--seed 1]
"""
from __future__ import annotations

import argparse
import json
import math
import random
import struct
import subprocess
import sys
import tempfile
import zlib
from collections import Counter
from pathlib import Path

from unicorn import (UC_ARCH_MIPS, UC_HOOK_CODE, UC_MODE_LITTLE_ENDIAN, UC_MODE_MIPS32, UC_PROT_EXEC, UC_PROT_READ, Uc,
                     UcError)
from unicorn.mips_const import (UC_CPU_MIPS32_24KF, UC_MIPS_REG_0, UC_MIPS_REG_F0, UC_MIPS_REG_FCSR, UC_MIPS_REG_HI,
                                UC_MIPS_REG_LO, UC_MIPS_REG_PC)

SCRATCH, SCRATCH_SIZE = 0x09800000, 0x10000
STACK, STACK_SIZE = 0x09E00000, 0x10000
RET_MAGIC = 0x09D00000
MAX_INSNS = 300_000
F32 = struct.Struct("<f")


def f2u(f: float) -> int:
    return struct.unpack("<I", F32.pack(f))[0]


def is_nan_or_inf_bits(w: int) -> bool:
    return (w >> 23) & 0xFF == 0xFF


def rand_word(rng: random.Random) -> int:
    p = rng.random()
    if p < 0.30:
        return SCRATCH + rng.randrange(0, SCRATCH_SIZE - 0x400, 4)
    if p < 0.55:
        return rng.randrange(0, 64)
    if p < 0.70:
        return f2u(rng.uniform(-1000.0, 1000.0))
    if p < 0.80:
        return (-rng.randrange(1, 64)) & 0xFFFFFFFF
    w = rng.getrandbits(32)
    return w ^ 0x40000000 if is_nan_or_inf_bits(w) else w  # keep random data float-finite


def make_pool(rng: random.Random, size: int) -> bytes:
    return struct.pack(f"<{size // 4}I", *(rand_word(rng) for _ in range(size // 4)))


class CaseFactory:
    """Registers are fresh per case; memory comes from a few pre-generated pools (speed)."""

    def __init__(self, rng: random.Random, pools: int = 6):
        self.rng = rng
        self.scratch = [make_pool(rng, SCRATCH_SIZE) for _ in range(pools)]
        self.stack = [make_pool(rng, STACK_SIZE) for _ in range(pools)]

    def make(self, func: int) -> dict:
        rng = self.rng
        r = [0] + [rand_word(rng) for _ in range(31)]
        r[26] = r[27] = 0
        r[28] = 0
        r[29] = STACK + STACK_SIZE - 0x100
        r[31] = RET_MAGIC
        fpr = [f2u(rng.choice([rng.uniform(-100, 100), rng.uniform(-1e6, 1e6), float(rng.randrange(-8, 8))]))
               for _ in range(32)]
        return {"func": func, "r": r, "hi": rng.getrandbits(32), "lo": rng.getrandbits(32), "fpr": fpr, "fcr31": 0,
                "fpcond": rng.randrange(2), "scratch": rng.choice(self.scratch), "stack": rng.choice(self.stack)}


class Reference:
    def __init__(self, base: int, image: bytes, text_size: int):
        self.base, self.image, self.text_size = base, image, text_size
        self.mu = Uc(UC_ARCH_MIPS, UC_MODE_MIPS32 + UC_MODE_LITTLE_ENDIAN)
        self.mu.ctl_set_cpu_model(UC_CPU_MIPS32_24KF)
        # Code is read-only (a wild write into it is an error, not silent corruption);
        # the rest of the image (data, bss, stubs) is writable.
        code_pages = text_size & ~0xFFF
        total = (len(image) + 0xFFF) & ~0xFFF
        self.mu.mem_map(base, code_pages, UC_PROT_READ | UC_PROT_EXEC)
        self.mu.mem_map(base + code_pages, total - code_pages)
        self.code_pages = code_pages
        self.mu.mem_map(SCRATCH, SCRATCH_SIZE)
        self.mu.mem_map(STACK, STACK_SIZE)
        self.mu.mem_map(RET_MAGIC, 0x1000)
        self.mu.mem_write(base, image)

    def run(self, case: dict, diagnose: bool = False) -> dict:
        mu = self.mu
        mu.mem_write(self.base + self.code_pages, self.image[self.code_pages:])
        mu.ctl_flush_tb()  # never reuse translations across cases
        mu.mem_write(SCRATCH, case["scratch"])
        mu.mem_write(STACK, case["stack"])
        for i in range(1, 32):
            mu.reg_write(UC_MIPS_REG_0 + i, case["r"][i])
        mu.reg_write(UC_MIPS_REG_HI, case["hi"])
        mu.reg_write(UC_MIPS_REG_LO, case["lo"])
        for i in range(32):
            mu.reg_write(UC_MIPS_REG_F0 + i, case["fpr"][i])
        mu.reg_write(UC_MIPS_REG_FCSR, case["fcr31"] | (case["fpcond"] << 23))
        notes = set()
        # A per-instruction hook is always installed: without one, Unicorn 2.1.4 can report
        # success and leave stale registers when a memory fault happens in a delay slot.
        def on_code(uc, addr, size, _):
            if diagnose:
                w = struct.unpack("<I", uc.mem_read(addr, 4))[0]
                op, fn = w >> 26, w & 63
                if op == 0 and fn in (26, 27):
                    a, b = uc.reg_read(UC_MIPS_REG_0 + ((w >> 21) & 31)), uc.reg_read(UC_MIPS_REG_0 + ((w >> 16) & 31))
                    if b == 0 or (fn == 26 and a == 0x80000000 and b == 0xFFFFFFFF):
                        notes.add("div by zero/overflow (Allegrex-specific result)")
                if op == 17 and (w >> 21) & 31 == 16 and fn in (12, 13, 14, 15, 36):
                    x = F32.unpack(struct.pack("<I", uc.reg_read(UC_MIPS_REG_F0 + ((w >> 11) & 31)) & 0xFFFFFFFF))[0]
                    if not math.isfinite(x) or abs(x) >= 2147483648.0 or (fn == 12 and x - math.floor(x) == 0.5):
                        notes.add("float->int edge case (Allegrex-specific result)")
                if op == 17 and (w >> 21) & 31 == 16 and fn < 8:
                    for reg in ((w >> 11) & 31, (w >> 16) & 31):
                        if is_nan_or_inf_bits(uc.reg_read(UC_MIPS_REG_F0 + reg) & 0xFFFFFFFF):
                            notes.add("NaN/Inf operand (host NaN encoding differs)")

        hook = mu.hook_add(UC_HOOK_CODE, on_code)
        try:
            mu.emu_start(case["func"], RET_MAGIC, count=MAX_INSNS)
            status = "ok" if mu.reg_read(UC_MIPS_REG_PC) == RET_MAGIC else "timeout"
        except UcError as e:
            status = f"unicorn: {e}"
            # A fault inside a delay slot leaves Unicorn's branch state pending, which corrupts
            # the next run (it jumps to the stale target after one instruction): start fresh.
            self.__init__(self.base, self.image, self.text_size)
            return {"status": status, "notes": notes}
        except OSError:  # native crash inside Unicorn; its state is unusable afterwards
            self.__init__(self.base, self.image, self.text_size)
            return {"status": "reference crashed (Unicorn bug)", "notes": notes}
        finally:
            try:
                mu.hook_del(hook)
            except (UcError, OSError):
                pass
        out = {"status": status, "notes": notes}
        if status == "ok":
            out["r"] = [0] + [mu.reg_read(UC_MIPS_REG_0 + i) for i in range(1, 32)]
            out["hi"], out["lo"] = mu.reg_read(UC_MIPS_REG_HI), mu.reg_read(UC_MIPS_REG_LO)
            out["fpr"] = [mu.reg_read(UC_MIPS_REG_F0 + i) & 0xFFFFFFFF for i in range(32)]
            out["fpcond"] = (mu.reg_read(UC_MIPS_REG_FCSR) >> 23) & 1
            out["h_image"] = zlib.crc32(mu.mem_read(self.base, len(self.image)))
            out["h_scratch"] = zlib.crc32(mu.mem_read(SCRATCH, SCRATCH_SIZE))
            out["h_stack"] = zlib.crc32(mu.mem_read(STACK, STACK_SIZE))
        return out


def pack_cases(cases: list[dict]) -> bytes:
    out = [struct.pack("<6I", 0x31435444, len(cases), SCRATCH, SCRATCH_SIZE, STACK, STACK_SIZE)]
    for c in cases:
        out.append(struct.pack("<I32I2I32I2I", c["func"], *c["r"], c["hi"], c["lo"], *c["fpr"], c["fcr31"], c["fpcond"]))
        out.append(c["scratch"])
        out.append(c["stack"])
    return b"".join(out)


def unpack_results(data: bytes) -> list[dict]:
    rec = struct.Struct("<2I32I2I32I2I3I")
    res = []
    for k in range(len(data) // rec.size):
        v = rec.unpack_from(data, k * rec.size)
        res.append({"status": v[0], "abort_addr": v[1], "r": list(v[2:34]), "hi": v[34], "lo": v[35],
                    "fpr": list(v[36:68]), "fpcond": v[68], "h_image": v[70], "h_scratch": v[71], "h_stack": v[72]})
    return res


def compare(ref: dict, ours: dict) -> list[str]:
    diffs = [f"r{i}: ref {ref['r'][i]:08x} ours {ours['r'][i]:08x}" for i in range(1, 32) if ref["r"][i] != ours["r"][i]]
    for k in ("hi", "lo", "fpcond"):
        if ref[k] != ours[k]:
            diffs.append(f"{k}: ref {ref[k]:08x} ours {ours[k]:08x}")
    for i in range(32):
        a, b = ref["fpr"][i], ours["fpr"][i]
        if a != b and not (is_nan_or_inf_bits(a) and is_nan_or_inf_bits(b) and a & 0x7FFFFF and b & 0x7FFFFF):
            diffs.append(f"f{i}: ref {a:08x} ours {b:08x}")
    for k in ("h_image", "h_scratch", "h_stack"):
        if ref[k] != ours[k]:
            diffs.append(f"memory {k[2:]} differs")
    return diffs


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("gen")
    ap.add_argument("exe")
    ap.add_argument("--cases", type=int, default=8)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--report")
    args = ap.parse_args()
    gen = Path(args.gen)
    meta = json.loads((gen / "module.json").read_text(encoding="utf-8"))
    base = int(meta["base"], 16)
    image = (gen / "image.bin").read_bytes()
    roots = [int(r, 16) for r in meta["selection"]["difftest_roots"]]
    rng = random.Random(args.seed)
    ref = Reference(base, image, int(meta["text_size"]))
    factory = CaseFactory(rng)

    cases, refs = [], []
    for func in roots:
        for _ in range(args.cases):
            c = factory.make(func)
            cases.append(c)
            refs.append(ref.run(c))
    print(f"reference runs done: {len(cases)} cases", flush=True)
    # Only replay cases the reference finished; a case that hangs ends the runtime with
    # exit code 3 (watchdog), is recorded as hung, and the run resumes after it.
    todo = [k for k, r in enumerate(refs) if r["status"] == "ok"]
    ours: list[dict | None] = [None] * len(cases)
    exe = str(Path(args.exe).resolve())
    image_path = str((gen / "image.bin").resolve())
    with tempfile.TemporaryDirectory() as td:
        cpath, rpath = Path(td) / "cases.bin", Path(td) / "results.bin"
        while todo:
            cpath.write_bytes(pack_cases([cases[k] for k in todo]))
            proc = subprocess.run([exe, "--image", image_path, "--difftest", str(cpath), str(rpath)],
                                  capture_output=True, text=True)
            got = unpack_results(rpath.read_bytes()) if rpath.exists() else []
            for k, res in zip(todo, got):
                ours[k] = res
            if proc.returncode == 0:
                break
            if proc.returncode != 3 or len(got) >= len(todo):
                raise RuntimeError(f"runtime failed ({proc.returncode}): {proc.stderr[-500:]}")
            ours[todo[len(got)]] = {"status": -1, "abort_addr": 0}
            todo = todo[len(got) + 1:]

    tally, per_func, examples = Counter(), {}, []
    for c, r, o in zip(cases, refs, ours):
        f = c["func"]
        st = per_func.setdefault(f, Counter())
        if r["status"] != "ok":
            kind = "reference: unsupported Allegrex encoding or trap" if "INSN_INVALID" in r["status"] or "EXCEPTION" in r["status"] \
                else f"reference: {r['status'].split(' (')[0]}"
            tally[kind] += 1; st["skipped"] += 1
            continue
        if o["status"] == -1:
            tally["ours hung (reference finished)"] += 1; st["fail"] += 1
            examples.append((f, "hung although the reference returned"))
            continue
        if o["status"] != 0:
            tally[f"ours aborted ({o['status']})"] += 1; st["fail"] += 1
            examples.append((f, f"aborted status {o['status']} at {o['abort_addr']:08x}"))
            continue
        d = compare(r, o)
        if not d:
            tally["match"] += 1; st["match"] += 1
            continue
        notes = ref.run(c, diagnose=True)["notes"]
        if notes:
            tally["differs: " + "; ".join(sorted(notes))] += 1; st["explained"] += 1
        else:
            tally["MISMATCH"] += 1; st["fail"] += 1
            examples.append((f, "; ".join(d[:6])))
    funcs_ok = sum(1 for s in per_func.values() if s["match"] and not s["fail"])
    funcs_fail = sum(1 for s in per_func.values() if s["fail"])
    funcs_untested = sum(1 for s in per_func.values() if not s["match"] and not s["fail"])
    print(f"cases: {len(cases)} ({len(roots)} root functions x {args.cases})")
    for k, v in tally.most_common():
        print(f"  {v:5d}  {k}")
    print(f"functions: {funcs_ok} all-matching, {funcs_fail} with mismatches, {funcs_untested} not exercised by the reference")
    for f, msg in examples[:15]:
        print(f"  {f:08X}: {msg}")
    if args.report:
        Path(args.report).write_text(json.dumps({
            "cases": len(cases), "roots": len(roots), "tally": dict(tally), "functions_all_matching": funcs_ok,
            "functions_with_mismatches": funcs_fail, "functions_not_exercised": funcs_untested,
            "mismatch_examples": [{"func": f"0x{f:08X}", "detail": m} for f, m in examples[:50]]}, indent=1), encoding="utf-8")
    return 1 if funcs_fail else 0


if __name__ == "__main__":
    sys.exit(main())
