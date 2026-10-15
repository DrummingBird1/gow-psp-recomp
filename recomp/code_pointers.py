"""Finds code addresses materialized in code by relocated lui/addiu (HI16/LO16) pairs
that Ghidra has not made function entries, and writes them as seeds for
re/scripts/SeedFunctionsFromRelocations.java (addresses at Ghidra's base).

Usage: python -I code_pointers.py <EBOOT.BIN> <program.recomp.json> <out missing_entries.txt>
"""
import json
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from pspelf import PspModule  # noqa: E402


def main() -> int:
    elf, ghidra_json, out = sys.argv[1:4]
    g = json.loads(Path(ghidra_json).read_text(encoding="utf-8"))
    gbase = int(g["image_base"], 16)
    mod = PspModule.parse(Path(elf).read_bytes())
    mod.load(gbase)
    text = mod.section(".text")
    t_lo, t_hi = gbase + text.addr, gbase + text.addr + text.size
    entries = {int(f["addr"], 16) for f in g["functions"]}
    interior = set()
    for f in g["functions"]:
        for lo, hi in f["ranges"]:
            interior.update(range(int(lo, 16), int(hi, 16) + 1, 4))
    kinds, seeds = Counter(), set()
    for _hi, lo_addr, value in mod.hilo_values:
        if not (t_lo <= value < t_hi) or value & 3:
            continue
        if not (t_lo <= lo_addr < t_hi):
            kinds["pair outside .text"] += 1
            continue
        if value in entries:
            kinds["function entry"] += 1
        elif value in interior:
            kinds["inside a function"] += 1
        else:
            kinds["not disassembled"] += 1
            seeds.add(value)
    Path(out).write_text("".join(f"{a:08x}\n" for a in sorted(seeds)), encoding="utf-8")
    print(f"{Path(elf).name}: code pointers from HI16/LO16 pairs in .text: {dict(kinds)}; {len(seeds)} seeds written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
