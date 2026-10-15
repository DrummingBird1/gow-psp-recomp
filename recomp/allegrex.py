"""Allegrex (PSP CPU) instruction decoder for the recompiler.

Encodings follow PPSSPP's MIPSTables.cpp (v1.20.4). VFPU instructions are recognized
(kind "vfpu") but not decoded further; the prototype recompiler does not translate them.
"""
from __future__ import annotations

from dataclasses import dataclass

# Control-flow kinds
NONE, BRANCH, JUMP, CALL, JR, JALR, TRAP = "none", "branch", "jump", "call", "jr", "jalr", "trap"

SPECIAL = {0: "sll", 2: "srl", 3: "sra", 4: "sllv", 6: "srlv", 7: "srav", 8: "jr", 9: "jalr", 10: "movz",
           11: "movn", 12: "syscall", 13: "break", 15: "sync", 16: "mfhi", 17: "mthi", 18: "mflo", 19: "mtlo",
           22: "clz", 23: "clo", 24: "mult", 25: "multu", 26: "div", 27: "divu", 28: "madd", 29: "maddu",
           32: "add", 33: "addu", 34: "sub", 35: "subu", 36: "and", 37: "or", 38: "xor", 39: "nor", 42: "slt",
           43: "sltu", 44: "max", 45: "min", 46: "msub", 47: "msubu", 48: "tge", 49: "tgeu", 50: "tlt",
           51: "tltu", 52: "teq", 54: "tne"}
REGIMM = {0: "bltz", 1: "bgez", 2: "bltzl", 3: "bgezl", 16: "bltzal", 17: "bgezal", 18: "bltzall", 19: "bgezall"}
IMMEDIATE = {2: "j", 3: "jal", 4: "beq", 5: "bne", 6: "blez", 7: "bgtz", 8: "addi", 9: "addiu", 10: "slti",
             11: "sltiu", 12: "andi", 13: "ori", 14: "xori", 15: "lui", 20: "beql", 21: "bnel", 22: "blezl",
             23: "bgtzl", 32: "lb", 33: "lh", 34: "lwl", 35: "lw", 36: "lbu", 37: "lhu", 38: "lwr", 40: "sb",
             41: "sh", 42: "swl", 43: "sw", 46: "swr", 47: "cache", 48: "ll", 49: "lwc1", 56: "sc", 57: "swc1"}
VFPU_OPCODES = {18, 24, 25, 27, 50, 52, 53, 54, 55, 58, 60, 61, 62, 63}
COP1S = {0: "add.s", 1: "sub.s", 2: "mul.s", 3: "div.s", 4: "sqrt.s", 5: "abs.s", 6: "mov.s", 7: "neg.s",
         12: "round.w.s", 13: "trunc.w.s", 14: "ceil.w.s", 15: "floor.w.s", 36: "cvt.w.s"}
FPU_CONDS = ["f", "un", "eq", "ueq", "olt", "ult", "ole", "ule", "sf", "ngle", "seq", "ngl", "lt", "nge", "le", "ngt"]
BSHFL = {2: "wsbh", 3: "wsbw", 16: "seb", 20: "bitrev", 24: "seh"}
LIKELY = {"beql", "bnel", "blezl", "bgtzl", "bltzl", "bgezl", "bltzall", "bgezall", "bc1fl", "bc1tl"}


@dataclass(frozen=True)
class Insn:
    pc: int
    word: int
    op: str          # mnemonic, "vfpu" or "invalid"
    rs: int = 0
    rt: int = 0
    rd: int = 0
    sa: int = 0
    imm: int = 0     # sign-extended 16-bit immediate
    uimm: int = 0    # zero-extended 16-bit immediate
    target: int = 0  # absolute branch/jump target
    flow: str = NONE
    likely: bool = False
    link: bool = False

    @property
    def fs(self) -> int: return self.rd

    @property
    def ft(self) -> int: return self.rt

    @property
    def fd(self) -> int: return self.sa

    @property
    def has_delay_slot(self) -> bool:
        return self.flow in (BRANCH, JUMP, CALL, JR, JALR)

    @property
    def is_vfpu(self) -> bool:
        return (self.word >> 26) in VFPU_OPCODES


def decode(word: int, pc: int) -> Insn:
    opc = word >> 26
    rs, rt, rd, sa, fn = (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31, (word >> 6) & 31, word & 63
    uimm = word & 0xFFFF
    imm = uimm - 0x10000 if uimm & 0x8000 else uimm
    btarget = (pc + 4 + (imm << 2)) & 0xFFFFFFFF
    base = dict(pc=pc, word=word, rs=rs, rt=rt, rd=rd, sa=sa, imm=imm, uimm=uimm)

    if opc == 0:
        name = SPECIAL.get(fn)
        if name == "srl" and rs == 1: name = "rotr"
        elif name == "srl" and rs != 0: name = None
        elif name == "srlv" and sa == 1: name = "rotrv"
        elif name == "srlv" and sa != 0: name = None
        if name is None: return Insn(op="invalid", **base)
        if name == "jr": return Insn(op=name, flow=JR, **base)
        if name == "jalr": return Insn(op=name, flow=JALR, link=True, **base)
        if name in ("syscall", "break") or name.startswith("t"): return Insn(op=name, flow=TRAP, **base)
        return Insn(op=name, **base)
    if opc == 1:
        name = REGIMM.get(rt)
        if name is None: return Insn(op="invalid", **base)
        return Insn(op=name, target=btarget, flow=BRANCH, likely=name in LIKELY, link="al" in name, **base)
    if opc in (2, 3):
        target = ((pc + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
        return Insn(op=IMMEDIATE[opc], target=target, flow=CALL if opc == 3 else JUMP, link=opc == 3, **base)
    if opc == 16:  # COP0 (kernel only)
        return Insn(op="cop0", flow=TRAP, **base)
    if opc == 17:  # COP1
        if rs == 0: return Insn(op="mfc1", **base)
        if rs == 2: return Insn(op="cfc1", **base)
        if rs == 4: return Insn(op="mtc1", **base)
        if rs == 6: return Insn(op="ctc1", **base)
        if rs == 8 and rt < 4:
            name = ["bc1f", "bc1t", "bc1fl", "bc1tl"][rt]
            return Insn(op=name, target=btarget, flow=BRANCH, likely=name in LIKELY, **base)
        if rs == 16:
            if fn >= 48: return Insn(op="c." + FPU_CONDS[fn & 15] + ".s", **base)
            name = COP1S.get(fn)
            return Insn(op=name or "invalid", **base)
        if rs == 20 and fn == 32: return Insn(op="cvt.s.w", **base)
        return Insn(op="invalid", **base)
    if opc == 18 and rs == 8:  # COP2 BC2: VFPU condition branches, cc index in bits 18-20
        name = ["bvf", "bvt", "bvfl", "bvtl"][(word >> 16) & 3]
        return Insn(op=name, target=btarget, flow=BRANCH, likely=name in LIKELY, **base)
    if opc in VFPU_OPCODES:
        return Insn(op="vfpu", **base)
    if opc == 28:  # SPECIAL2
        name = {0: "halt", 36: "mfic", 38: "mtic"}.get(fn)
        return Insn(op=name or "invalid", flow=TRAP if name == "halt" else NONE, **base)
    if opc == 31:  # SPECIAL3
        if fn == 0: return Insn(op="ext", **base)
        if fn == 4: return Insn(op="ins", **base)
        if fn in (24, 32):
            name = BSHFL.get(sa)
            return Insn(op=name or "invalid", **base)
        return Insn(op="invalid", **base)
    name = IMMEDIATE.get(opc)
    if name is None: return Insn(op="invalid", **base)
    if name in ("beq", "bne", "blez", "bgtz", "beql", "bnel", "blezl", "bgtzl"):
        return Insn(op=name, target=btarget, flow=BRANCH, likely=name in LIKELY, **base)
    return Insn(op=name, **base)
