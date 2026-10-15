"""Debug helper: print a function's decoded instructions next to its generated C.

Usage: python -I show.py <gen dir> <EBOOT.BIN> <ghidra recomp.json> <hex address>
"""
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from allegrex import decode  # noqa: E402
from pspelf import PspModule  # noqa: E402

REG = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
       "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]


def main() -> int:
    gen, elf, gj, addr = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4], 16)
    meta = json.loads((Path(gen) / "module.json").read_text())
    base = int(meta["base"], 16)
    mod = PspModule.parse(Path(elf).read_bytes())
    mod.load(base)
    g = json.loads(Path(gj).read_text())
    delta = base - int(g["image_base"], 16)
    f = next(f for f in g["functions"] if int(f["addr"], 16) + delta == addr)
    print(f"{f['name']} ranges {[(hex(int(a, 16) + delta), hex(int(b, 16) + delta)) for a, b in f['ranges']]}")
    for lo, hi in f["ranges"]:
        for pc in range(int(lo, 16) + delta, int(hi, 16) + delta + 1, 4):
            i = decode(mod.u32(pc), pc)
            extra = f" -> {i.target:08X}" if i.target else ""
            print(f"  {pc:08X} {i.word:08X} {i.op:8s} rs={REG[i.rs]} rt={REG[i.rt]} rd={REG[i.rd]} sa={i.sa} imm={i.imm}{extra}")
    for p in sorted(Path(gen).glob("code_*.c")):
        m = re.search(rf"void f_{addr:08X}\(RtCtx \*c\) \{{.*?\n\}}\n", p.read_text(), re.S)
        if m:
            print(m.group(0))
            break
    return 0


if __name__ == "__main__":
    sys.exit(main())
