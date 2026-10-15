"""Cross-checks the runtime's VFPU operand decoding against Ghidra's disassembly.

For every VFPU instruction listed by re/scripts/ListVfpu.java, the register operands are
decoded with the same field/size rules as runtime/rt_vfpu.c and printed in PPSSPP's
notation (S/C/R vectors, M/E matrices); they must match Ghidra's operands in order.

Usage: python -I vfpu_check.py <program.vfpu.tsv>...
"""
import re
import sys
from collections import Counter
from pathlib import Path


def vlen(op):
    return (((op >> 7) & 1) | ((op >> 14) & 2)) + 1


def vname(reg, n):
    mtx, col, tr = (reg >> 2) & 7, reg & 3, (reg >> 5) & 1
    if n == 1:
        return f"S{mtx}{col}{(reg >> 5) & 3}"
    row = (reg >> 6) & 1 if n == 3 else (reg >> 5) & 2
    return f"R{mtx}{row}{col}" if tr else f"C{mtx}{col}{row}"


def mname(reg, n):
    mtx, col, tr = (reg >> 2) & 7, reg & 3, (reg >> 5) & 1
    row = (reg >> 6) & 1 if n == 3 else (reg >> 5) & 2
    return f"E{mtx}{row}{col}" if tr else f"M{mtx}{col}{row}"


def operands(op):
    """Register operands in disassembly order, or None for instructions without any."""
    opc, n = op >> 26, vlen(op)
    vd, vs, vt = op & 0x7F, (op >> 8) & 0x7F, (op >> 16) & 0x7F
    if opc in (50, 58):
        return [vname(((op >> 16) & 0x1F) | ((op & 3) << 5), 1)]
    if opc in (54, 62):
        return [vname(((op >> 16) & 0x1F) | ((op & 1) << 5), 4)]
    if opc == 18:
        rs, imm = (op >> 21) & 0x1F, op & 0xFF
        return [vname(imm, 1)] if rs in (3, 7) and imm < 128 else None
    if opc in (24, 27):
        if opc == 27 and (op >> 23) & 7 == 0:  # vcmp cond, vs, vt
            return [vname(vs, n), vname(vt, n)]
        return [vname(vd, n), vname(vs, n), vname(vt, n)]
    if opc == 25:
        s = (op >> 23) & 7
        if s == 1:
            return [vname(vd, 1), vname(vs, n), vname(vt, n)]
        if s == 2:
            return [vname(vd, n), vname(vs, n), vname(vt, 1)]
        return [vname(vd, n), vname(vs, n), vname(vt, n)]
    if opc == 52:
        sub, t = (op >> 21) & 0x1F, (op >> 16) & 0x1F
        if sub == 0 and t in (3, 6, 7):
            return [vname(vd, n)]
        if sub == 1 and t >= 28:
            oz = 1 if (t & 3) < 2 else (2 if n >= 3 else 1)
            return [vname(vd, oz), vname(vs, n)]
        return [vname(vd, n), vname(vs, n)]
    if opc == 55:
        return [vname(vt, 1)] if (op >> 23) & 7 >= 6 else None
    if opc == 60:
        s = (op >> 21) & 0x1F
        if s <= 3:
            return [mname(vd, n), mname(vs, n), mname(vt, n)]
        if s <= 15:
            ins = (op >> 23) & 3
            return [vname(vd, ins + 1), mname(vs, ins + 1), vname(vt, n)]
        if 20 <= s <= 23:
            return [vname(vd, n), vname(vs, n), vname(vt, n)]
        if s == 28:
            k = (op >> 16) & 0xF
            return [mname(vd, n)] if k in (3, 6, 7) else [mname(vd, n), mname(vs, n)]
    return None


def main() -> int:
    bad, total, kinds = 0, 0, Counter()
    for path in sys.argv[1:]:
        for line in Path(path).read_text(encoding="utf-8").splitlines()[1:]:
            addr, word, _fn, dis = line.split("\t")
            op = int(word, 16)
            ours = operands(op)
            if ours is None:
                continue
            ghidra = re.findall(r"\b[SCRME]\d{3}\b", dis)
            total += 1
            mnem = dis.split()[0].lstrip("_")
            # Display conventions, not decoding differences:
            #  - vmmul encodes vs transposed; disassemblers print it with the bit flipped.
            #  - ghidra-allegrex prints vi2c's single-register destination as a quad
            #    (it prints vi2uc's correctly); PPSSPP writes a single register.
            if mnem.startswith("vmmul") and ghidra:
                ours = [ours[0], mname(((op >> 8) & 0x7F) ^ 0x20, vlen(op)), ours[2]]
            if mnem.startswith("vi2c.") and ghidra and ghidra[0][0] == "R":
                ghidra = [ours[0]] + ghidra[1:]
            if ghidra != ours:
                bad += 1
                kinds[mnem] += 1
                if kinds[mnem] <= 2:
                    print(f"MISMATCH {Path(path).name.split('_')[0]} {addr} {word} {dis!r}: ours {ours}")
    print(f"checked {total} VFPU instructions with register operands: {total - bad} match, {bad} differ {dict(kinds)}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
