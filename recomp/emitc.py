"""C code generation for Allegrex functions.

Each recompiled function becomes `void f_XXXXXXXX(RtCtx *c)`. Control flow inside a
function uses labels/goto; calls are direct C calls when the target is recompiled,
otherwise `rt_call(c, addr)` (runtime lookup). `jr $ra` returns. Branch conditions and
jump-register targets are captured before the delay slot executes, as on hardware.
"""
from __future__ import annotations

from dataclasses import dataclass, field

from allegrex import BRANCH, CALL, JALR, JR, JUMP, TRAP, Insn, decode

FCMP = {  # PPSSPP Int_FPUComp semantics; a, b are floats
    "f": "0", "sf": "0",
    "un": "(isnan(a) || isnan(b))", "ngle": "(isnan(a) || isnan(b))",
    "eq": "(a == b)", "seq": "(a == b)",
    "ueq": "(a == b || isnan(a) || isnan(b))", "ngl": "(a == b || isnan(a) || isnan(b))",
    "olt": "(a < b)", "lt": "(a < b)",
    "ult": "(a < b || isnan(a) || isnan(b))", "nge": "(a < b || isnan(a) || isnan(b))",
    "ole": "(a <= b)", "le": "(a <= b)",
    "ule": "(a <= b || isnan(a) || isnan(b))", "ngt": "(a <= b || isnan(a) || isnan(b))",
}


class Untranslatable(Exception):
    pass


def R(n: int) -> str:
    return "0u" if n == 0 else f"c->r[{n}]"


def S(n: int) -> str:
    return "0" if n == 0 else f"(int32_t)c->r[{n}]"


def F(n: int) -> str:
    return f"c->fpr[{n}].f"


def FI(n: int) -> str:
    return f"c->fpr[{n}].i"


def addr_expr(i: Insn) -> str:
    return f"({R(i.rs)} + 0x{i.imm & 0xFFFFFFFF:X}u)" if i.imm else R(i.rs)


def set_r(n: int, expr: str) -> str:
    return "" if n == 0 else f"c->r[{n}] = {expr};"


def simple(i: Insn) -> str:
    """Translate a non-control-flow instruction to a C statement."""
    op, rs, rt, rd, sa = i.op, i.rs, i.rt, i.rd, i.sa
    a = addr_expr(i)
    if op == "sll":
        return "" if i.word == 0 else set_r(rd, f"{R(rt)} << {sa}")
    table = {
        "srl": lambda: set_r(rd, f"{R(rt)} >> {sa}"),
        "sra": lambda: set_r(rd, f"(uint32_t)({S(rt)} >> {sa})"),
        "rotr": lambda: set_r(rd, f"rt_rotr({R(rt)}, {sa})"),
        "sllv": lambda: set_r(rd, f"{R(rt)} << ({R(rs)} & 31)"),
        "srlv": lambda: set_r(rd, f"{R(rt)} >> ({R(rs)} & 31)"),
        "srav": lambda: set_r(rd, f"(uint32_t)({S(rt)} >> ({R(rs)} & 31))"),
        "rotrv": lambda: set_r(rd, f"rt_rotr({R(rt)}, {R(rs)} & 31)"),
        "movz": lambda: "" if rd == 0 else f"if ({R(rt)} == 0) c->r[{rd}] = {R(rs)};",
        "movn": lambda: "" if rd == 0 else f"if ({R(rt)} != 0) c->r[{rd}] = {R(rs)};",
        "sync": lambda: "/* sync */",
        "mfhi": lambda: set_r(rd, "c->hi"), "mflo": lambda: set_r(rd, "c->lo"),
        "mthi": lambda: f"c->hi = {R(rs)};", "mtlo": lambda: f"c->lo = {R(rs)};",
        "clz": lambda: set_r(rd, f"rt_clz32({R(rs)})"),
        "clo": lambda: set_r(rd, f"rt_clz32(~{R(rs)})"),
        "mult": lambda: f"rt_mult(c, {R(rs)}, {R(rt)});", "multu": lambda: f"rt_multu(c, {R(rs)}, {R(rt)});",
        "div": lambda: f"rt_div(c, {R(rs)}, {R(rt)});", "divu": lambda: f"rt_divu(c, {R(rs)}, {R(rt)});",
        "madd": lambda: f"rt_madd(c, {R(rs)}, {R(rt)});", "maddu": lambda: f"rt_maddu(c, {R(rs)}, {R(rt)});",
        "msub": lambda: f"rt_msub(c, {R(rs)}, {R(rt)});", "msubu": lambda: f"rt_msubu(c, {R(rs)}, {R(rt)});",
        "add": lambda: set_r(rd, f"{R(rs)} + {R(rt)}"), "addu": lambda: set_r(rd, f"{R(rs)} + {R(rt)}"),
        "sub": lambda: set_r(rd, f"{R(rs)} - {R(rt)}"), "subu": lambda: set_r(rd, f"{R(rs)} - {R(rt)}"),
        "and": lambda: set_r(rd, f"{R(rs)} & {R(rt)}"), "or": lambda: set_r(rd, f"{R(rs)} | {R(rt)}"),
        "xor": lambda: set_r(rd, f"{R(rs)} ^ {R(rt)}"), "nor": lambda: set_r(rd, f"~({R(rs)} | {R(rt)})"),
        "slt": lambda: set_r(rd, f"({S(rs)} < {S(rt)})"), "sltu": lambda: set_r(rd, f"({R(rs)} < {R(rt)})"),
        "max": lambda: set_r(rd, f"({S(rs)} > {S(rt)}) ? {R(rs)} : {R(rt)}"),
        "min": lambda: set_r(rd, f"({S(rs)} < {S(rt)}) ? {R(rs)} : {R(rt)}"),
        "addi": lambda: set_r(rt, f"{R(rs)} + 0x{i.imm & 0xFFFFFFFF:X}u"),
        "addiu": lambda: set_r(rt, f"{R(rs)} + 0x{i.imm & 0xFFFFFFFF:X}u"),
        "slti": lambda: set_r(rt, f"({S(rs)} < {i.imm})"),
        "sltiu": lambda: set_r(rt, f"({R(rs)} < 0x{i.imm & 0xFFFFFFFF:X}u)"),
        "andi": lambda: set_r(rt, f"{R(rs)} & 0x{i.uimm:X}u"),
        "ori": lambda: set_r(rt, f"{R(rs)} | 0x{i.uimm:X}u"),
        "xori": lambda: set_r(rt, f"{R(rs)} ^ 0x{i.uimm:X}u"),
        "lui": lambda: set_r(rt, f"0x{(i.uimm << 16):X}u"),
        "lb": lambda: set_r(rt, f"(uint32_t)(int32_t)(int8_t)rt_r8({a})"),
        "lbu": lambda: set_r(rt, f"rt_r8({a})"),
        "lh": lambda: set_r(rt, f"(uint32_t)(int32_t)(int16_t)rt_r16({a})"),
        "lhu": lambda: set_r(rt, f"rt_r16({a})"),
        "lw": lambda: set_r(rt, f"rt_r32({a})"), "ll": lambda: set_r(rt, f"rt_r32({a})"),
        "lwl": lambda: set_r(rt, f"rt_lwl({a}, {R(rt)})"),
        "lwr": lambda: set_r(rt, f"rt_lwr({a}, {R(rt)})"),
        "sb": lambda: f"rt_w8({a}, (uint8_t){R(rt)});", "sh": lambda: f"rt_w16({a}, (uint16_t){R(rt)});",
        "sw": lambda: f"rt_w32({a}, {R(rt)});",
        "sc": lambda: f"rt_w32({a}, {R(rt)}); " + set_r(rt, "1"),
        "swl": lambda: f"rt_swl({a}, {R(rt)});", "swr": lambda: f"rt_swr({a}, {R(rt)});",
        "cache": lambda: "/* cache */",
        "lwc1": lambda: f"{FI(rt)} = rt_r32({a});", "swc1": lambda: f"rt_w32({a}, {FI(rt)});",
        "mfc1": lambda: set_r(rt, FI(i.fs)), "mtc1": lambda: f"{FI(i.fs)} = {R(rt)};",
        "cfc1": lambda: set_r(rt, f"rt_cfc1(c, {i.fs})"), "ctc1": lambda: f"rt_ctc1(c, {i.fs}, {R(rt)});",
        "add.s": lambda: f"{F(i.fd)} = {F(i.fs)} + {F(i.ft)};", "sub.s": lambda: f"{F(i.fd)} = {F(i.fs)} - {F(i.ft)};",
        "mul.s": lambda: f"{F(i.fd)} = {F(i.fs)} * {F(i.ft)};", "div.s": lambda: f"{F(i.fd)} = {F(i.fs)} / {F(i.ft)};",
        "sqrt.s": lambda: f"{F(i.fd)} = sqrtf({F(i.fs)});", "abs.s": lambda: f"{F(i.fd)} = fabsf({F(i.fs)});",
        "mov.s": lambda: f"{FI(i.fd)} = {FI(i.fs)};", "neg.s": lambda: f"{F(i.fd)} = -{F(i.fs)};",
        "round.w.s": lambda: f"{FI(i.fd)} = rt_round_w_s({F(i.fs)});",
        "trunc.w.s": lambda: f"{FI(i.fd)} = rt_trunc_w_s({F(i.fs)});",
        "ceil.w.s": lambda: f"{FI(i.fd)} = rt_ceil_w_s({F(i.fs)});",
        "floor.w.s": lambda: f"{FI(i.fd)} = rt_floor_w_s({F(i.fs)});",
        "cvt.w.s": lambda: f"{FI(i.fd)} = rt_cvt_w_s(c, {F(i.fs)});",
        "cvt.s.w": lambda: f"{F(i.fd)} = (float)(int32_t){FI(i.fs)};",
        "ext": lambda: set_r(rt, f"({R(rs)} >> {sa}) & 0x{(0xFFFFFFFF >> (31 - rd)):X}u"),
        "wsbh": lambda: set_r(rd, f"(({R(rt)} & 0xFF00FF00u) >> 8) | (({R(rt)} & 0x00FF00FFu) << 8)"),
        "wsbw": lambda: set_r(rd, f"rt_bswap32({R(rt)})"),
        "seb": lambda: set_r(rd, f"(uint32_t)(int32_t)(int8_t){R(rt)}"),
        "seh": lambda: set_r(rd, f"(uint32_t)(int32_t)(int16_t){R(rt)}"),
        "bitrev": lambda: set_r(rd, f"rt_bitrev({R(rt)})"),
        "mfic": lambda: set_r(rt, "c->ic"), "mtic": lambda: f"c->ic = {R(rt)};",
    }
    if op in table:
        return table[op]()
    if op == "ins":
        size = rd + 1 - sa
        if size <= 0:
            raise Untranslatable(f"ins with size {size} at {i.pc:#x}")
        mask = (0xFFFFFFFF >> (32 - size)) << sa
        return set_r(rt, f"({R(rt)} & 0x{(~mask) & 0xFFFFFFFF:X}u) | (({R(rs)} << {sa}) & 0x{mask:X}u)")
    if op.startswith("c.") and op.endswith(".s"):
        cond = FCMP[op[2:-2]]
        return f"{{ float a = {F(i.fs)}, b = {F(i.ft)}; (void)a; (void)b; c->fpcond = {cond}; }}"
    if op in ("teq", "tne", "tge", "tgeu", "tlt", "tltu"):
        cmp = {"teq": f"{R(rs)} == {R(rt)}", "tne": f"{R(rs)} != {R(rt)}", "tge": f"{S(rs)} >= {S(rt)}",
               "tgeu": f"{R(rs)} >= {R(rt)}", "tlt": f"{S(rs)} < {S(rt)}", "tltu": f"{R(rs)} < {R(rt)}"}[op]
        return f"if ({cmp}) rt_trap(c, 0x{i.pc:08X}u, 0x{i.word:08X}u, \"{op}\");"
    if op in ("break", "syscall", "halt", "cop0"):
        return f"rt_trap(c, 0x{i.pc:08X}u, 0x{i.word:08X}u, \"{op}\");"
    if op == "vfpu":  # executed by the runtime's VFPU interpreter (rt_vfpu.c)
        return f"rt_vfpu(c, 0x{i.word:08X}u, 0x{i.pc:08X}u);"
    raise Untranslatable(f"{op} at {i.pc:#x} ({i.word:08x})")


def branch_cond(i: Insn) -> str:
    rs, rt = i.rs, i.rt
    if i.op in ("bvf", "bvt", "bvfl", "bvtl"):
        cc = (i.word >> 18) & 7
        return f"RT_VFPU_CC(c, {cc})" if i.op.startswith("bvt") else f"!RT_VFPU_CC(c, {cc})"
    base = i.op.rstrip("l") if i.op not in ("bltzal", "bgezal") else i.op
    base = {"bltzal": "bltz", "bgezal": "bgez", "bltzall": "bltz", "bgezall": "bgez"}.get(i.op, base)
    return {
        "beq": f"{R(rs)} == {R(rt)}", "bne": f"{R(rs)} != {R(rt)}",
        "blez": f"{S(rs)} <= 0", "bgtz": f"{S(rs)} > 0", "bltz": f"{S(rs)} < 0", "bgez": f"{S(rs)} >= 0",
        "bc1f": "!c->fpcond", "bc1t": "c->fpcond",
    }[base]


@dataclass
class FuncInfo:
    entry: int
    name: str
    addrs: list[int]                 # instruction addresses in body, ascending
    insns: dict[int, Insn] = field(default_factory=dict)
    calls: set[int] = field(default_factory=set)
    has_vfpu: bool = False
    untranslatable: str | None = None
    has_jalr: bool = False
    has_indirect_jr: bool = False


class Program:
    """What the emitter needs to know about the whole module."""

    def __init__(self, read_u32, functions: dict[int, FuncInfo], jumptables: dict[int, list[int]],
                 stubs: dict[int, tuple[int, str, str]], generated: set[int]):
        self.read_u32 = read_u32
        self.functions = functions
        self.jumptables = jumptables
        self.stubs = stubs          # stub addr -> (nid, library, name)
        self.generated = generated  # entries emitted as C in this build

    def call(self, target: int) -> str:
        if target in self.stubs or target in self.generated:
            return f"f_{target:08X}(c);"
        return f"rt_call(c, 0x{target:08X}u);"


def emit_function(fn: FuncInfo, prog: Program) -> str:
    inset = set(fn.addrs)
    dec = fn.insns

    def insn_at(pc: int) -> Insn:
        return dec[pc] if pc in dec else decode(prog.read_u32(pc), pc)

    labels: set[int] = set()
    for pc in fn.addrs:
        i = dec[pc]
        if i.flow in (BRANCH, JUMP) and i.target in inset:
            labels.add(i.target)
        if i.flow == JR and pc in prog.jumptables:
            labels.update(t for t in prog.jumptables[pc] if t in inset)
    for pc in fn.addrs:  # code jumping into a delay slot needs a standalone copy
        i = dec[pc]
        if i.has_delay_slot and pc + 4 in labels:
            labels.add(pc + 8)

    out = [f"void f_{fn.entry:08X}(RtCtx *c) {{"]
    expected = None   # address that sequential execution reaches next
    consumed = set()
    idx = 0
    while idx < len(fn.addrs):
        pc = fn.addrs[idx]
        idx += 1
        if pc in consumed:  # delay slot already emitted inline (plus a standalone copy if it is a label)
            continue
        if expected is not None and pc != expected:
            out.append("  " + fallthrough(expected, inset, labels, prog))
        if pc in labels:
            out.append(f"L_{pc:08X}:;")
        i = dec[pc]
        if not i.has_delay_slot:
            stmt = simple(i)
            if stmt:
                out.append("  " + stmt)
            expected = pc + 4
            continue
        ds = insn_at(pc + 4)
        if ds.has_delay_slot:
            raise Untranslatable(f"control flow in delay slot at {pc + 4:#x}")
        dcode = simple(ds)
        out.append("  " + control(i, dcode, fn, inset, prog))
        consumed.add(pc + 4)
        expected = None if i.flow in (JUMP, JR) else pc + 8
        if pc + 4 in labels:  # standalone delay-slot copy for jumps into it
            if expected is not None:
                out.append(f"  goto L_{pc + 8:08X};")
            out.append(f"L_{pc + 4:08X}:;")
            if dcode:
                out.append("  " + dcode)
            expected = pc + 8
    if expected is not None:
        out.append("  " + fallthrough(expected, inset, labels, prog))
    out.append("}")
    return "\n".join(out) + "\n"


def fallthrough(addr: int, inset: set[int], labels: set[int], prog: Program) -> str:
    if addr in inset:
        labels.add(addr)  # already emitted labels are fine; later ones are picked up
        return f"goto L_{addr:08X};"
    return f"{prog.call(addr)} return; /* falls into next function */"


def control(i: Insn, dcode: str, fn: FuncInfo, inset: set[int], prog: Program) -> str:
    ret = f"0x{i.pc + 8:08X}u"

    def go(target: int) -> str:
        if target in inset:
            return f"goto L_{target:08X};"
        return f"{{ {prog.call(target)} return; }}"

    if i.flow == BRANCH:
        cond = branch_cond(i)
        link = f"c->r[31] = {ret}; " if i.link else ""
        if i.link:  # bltzal/bgezal: a conditional call
            if i.target in inset:
                raise Untranslatable(f"local {i.op} (subroutine inside function) at {i.pc:#x}")
            call = prog.call(i.target)
            if i.likely:
                return f"{{ int t_ = {cond}; {link}if (t_) {{ {dcode} {call} }} }}"
            return f"{{ int t_ = {cond}; {link}{dcode} if (t_) {{ {call} }} }}"
        if i.likely:
            return f"if ({cond}) {{ {dcode} {go(i.target)} }}"
        return f"{{ int t_ = {cond}; {dcode} if (t_) {go(i.target)} }}"
    if i.flow == JUMP:
        return f"{dcode} {go(i.target)}"
    if i.flow == CALL:
        return f"c->r[31] = {ret}; {dcode} {prog.call(i.target)}"
    if i.flow == JR:
        if i.rs == 31:
            return f"{{ {dcode} return; }}"
        targets = prog.jumptables.get(i.pc, [])
        if targets and all(t in inset for t in targets):
            cases = " ".join(f"case 0x{t:08X}u: goto L_{t:08X};" for t in sorted(set(targets)))
            return (f"{{ uint32_t t_ = {R(i.rs)}; {dcode} switch (t_) {{ {cases} "
                    f"default: rt_bad_jump(c, 0x{i.pc:08X}u, t_); return; }} }}")
        return f"{{ uint32_t t_ = {R(i.rs)}; {dcode} rt_call(c, t_); return; }}"
    if i.flow == JALR:
        link = "" if i.rd == 0 else f"c->r[{i.rd}] = {ret}; "
        return f"{{ uint32_t t_ = {R(i.rs)}; {link}{dcode} rt_call(c, t_); }}"
    raise Untranslatable(f"unexpected control flow {i.op} at {i.pc:#x}")
