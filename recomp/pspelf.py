"""PSP ELF/PRX loading: segments, sections, SHT_PSPREL relocations, module info, imports.

Relocation semantics follow PPSSPP's ElfReader::LoadRelocations (v1.20.4):
each SHT_PSPREL section is processed on its own, original words are read before any
write, and an R_MIPS_HI16 pairs with the next non-HI16 relocation in the same section.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

SHT_PSPREL = 0x700000A0
R_MIPS_NONE, R_MIPS_16, R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16, R_MIPS_GPREL16 = 0, 1, 2, 4, 5, 6, 7


@dataclass
class Segment:
    type: int
    offset: int
    vaddr: int
    filesz: int
    memsz: int


@dataclass
class Section:
    name: str
    type: int
    flags: int
    addr: int
    offset: int
    size: int
    info: int


@dataclass
class Import:
    library: str
    nid: int
    stub: int  # relocated address of the 8-byte import stub


@dataclass
class PspModule:
    data: bytes
    entry: int  # unrelocated e_entry
    segments: list[Segment]
    sections: list[Section]
    base: int = 0
    image: bytearray = field(default_factory=bytearray)  # relocated, starts at `base`
    reloc_stats: dict = field(default_factory=dict)
    hilo_values: list = field(default_factory=list)  # (hi16 addr, lo16 addr, full relocated value)
    addr32_values: dict = field(default_factory=dict)  # addr -> relocated 32-bit value

    # --- parsing -------------------------------------------------------------
    @classmethod
    def parse(cls, data: bytes) -> "PspModule":
        if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
            raise ValueError("not a little-endian ELF32 file")
        e_type, e_machine = struct.unpack_from("<HH", data, 0x10)
        if e_machine != 8:
            raise ValueError("not a MIPS ELF")
        entry, phoff, shoff = struct.unpack_from("<III", data, 0x18)
        phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from("<HHHHH", data, 0x2A)
        segments = []
        for i in range(phnum):
            p_type, p_offset, p_vaddr, _paddr, p_filesz, p_memsz = struct.unpack_from("<IIIIII", data, phoff + i * phentsize)
            segments.append(Segment(p_type, p_offset, p_vaddr, p_filesz, p_memsz))
        raw = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
        stroff = raw[shstrndx][4] if shnum else 0
        sections = []
        for s in raw:
            end = data.index(b"\0", stroff + s[0])
            sections.append(Section(data[stroff + s[0]:end].decode("latin1"), s[1], s[2], s[3], s[4], s[5], s[7]))
        return cls(data=data, entry=entry, segments=segments, sections=sections)

    def section(self, name: str) -> Section:
        return next(s for s in self.sections if s.name == name)

    # --- loading -------------------------------------------------------------
    def load(self, base: int) -> None:
        """Copy loadable segments to `base` and apply all PSP relocations."""
        loads = [s for s in self.segments if s.type == 1]
        size = max(s.vaddr + s.memsz for s in loads)
        img = bytearray(size)
        for s in loads:
            img[s.vaddr:s.vaddr + s.filesz] = self.data[s.offset:s.offset + s.filesz]
        self.base, self.image = base, img
        self.hilo_values = []
        self.addr32_values = {}
        seg_vaddr = [base + s.vaddr for s in self.segments]
        stats = {"sections": 0, "relocs": 0, "by_type": {}, "hi_without_lo": 0, "skipped": 0}

        def rd(addr: int) -> int:
            return struct.unpack_from("<I", img, addr - base)[0]

        for sec in self.sections:
            if sec.type != SHT_PSPREL:
                continue
            stats["sections"] += 1
            rels = [struct.unpack_from("<II", self.data, sec.offset + i * 8) for i in range(sec.size // 8)]
            targets, ops = [], []
            for off, info in rels:  # pass 1: original words
                rw = (info >> 8) & 0xFF
                addr = off + seg_vaddr[rw] if rw < len(seg_vaddr) else None
                typ = info & 0xF
                if addr is None or ((addr & 3) and typ != R_MIPS_32) or not (base <= addr < base + size - 3):
                    targets.append(None); ops.append(0); stats["skipped"] += 1
                    continue
                targets.append(addr); ops.append(rd(addr))
            for r, (off, info) in enumerate(rels):  # pass 2: apply
                addr = targets[r]
                if addr is None:
                    continue
                typ, rel = info & 0xF, (info >> 16) & 0xFF
                to = seg_vaddr[rel] if rel < len(seg_vaddr) else 0
                op = ops[r]
                if typ == R_MIPS_32:
                    op = (op + to) & 0xFFFFFFFF
                    if not (addr & 3):
                        self.addr32_values[addr] = op
                elif typ == R_MIPS_26:
                    op = (op & 0xFC000000) | (((op & 0x03FFFFFF) + (to >> 2)) & 0x03FFFFFF)
                elif typ == R_MIPS_HI16:
                    cur, hi, found = (op & 0xFFFF) << 16, 0, False
                    for t in range(r + 1, len(rels)):
                        t_type = rels[t][1] & 0xF
                        if t_type == R_MIPS_HI16:
                            continue
                        if t_type not in (R_MIPS_LO16, R_MIPS_16) or targets[t] is None:
                            continue
                        lo = ops[t] & 0xFFFF
                        lo = lo - 0x10000 if lo & 0x8000 else lo
                        cur = (cur + lo + to) & 0xFFFFFFFF
                        hi = ((cur >> 16) + (1 if cur & 0x8000 else 0)) & 0xFFFF
                        self.hilo_values.append((addr, targets[t], cur))
                        found = True
                        break
                    if not found:
                        stats["hi_without_lo"] += 1
                    op = (op & 0xFFFF0000) | hi
                elif typ == R_MIPS_LO16:
                    op = (op & 0xFFFF0000) | (((op & 0xFFFF) + to) & 0xFFFF)
                elif typ == R_MIPS_16:
                    op = (op & 0xFFFF0000) | (((op & 0xFFFF) + to) & 0xFFFF)
                elif typ in (R_MIPS_NONE, R_MIPS_GPREL16):
                    pass
                else:
                    raise ValueError(f"unknown relocation type {typ} at {addr:#x}")
                struct.pack_into("<I", img, addr - base, op)
                stats["relocs"] += 1
                stats["by_type"][typ] = stats["by_type"].get(typ, 0) + 1
        self.reloc_stats = stats

    # --- relocated accessors -------------------------------------------------
    def u32(self, addr: int) -> int:
        return struct.unpack_from("<I", self.image, addr - self.base)[0]

    def u16(self, addr: int) -> int:
        return struct.unpack_from("<H", self.image, addr - self.base)[0]

    def cstr(self, addr: int) -> str:
        o = addr - self.base
        return self.image[o:self.image.index(b"\0", o)].decode("latin1")

    @property
    def entry_addr(self) -> int:
        return self.base + self.entry

    def module_info(self) -> dict:
        """sceModuleInfo: attributes, version, name, gp, export/import table bounds."""
        s = self.section(".rodata.sceModuleInfo")
        a = self.base + s.addr
        attr, ver = self.u16(a), self.u16(a + 2)
        name = bytes(self.image[a + 4 - self.base:a + 32 - self.base]).split(b"\0")[0].decode("latin1")
        gp, ent_top, ent_end, stub_top, stub_end = (self.u32(a + 32 + 4 * i) for i in range(5))
        return {"attr": attr, "version": ver, "name": name, "gp": gp, "ent_top": ent_top,
                "ent_end": ent_end, "stub_top": stub_top, "stub_end": stub_end}

    def imports(self) -> list[Import]:
        mi, out = self.module_info(), []
        a = mi["stub_top"]
        while a < mi["stub_end"]:
            lib = self.cstr(self.u32(a)) if self.u32(a) else "?"
            ent_len = self.image[a + 8 - self.base]
            nfunc = self.u16(a + 10)
            nids, stubs = self.u32(a + 12), self.u32(a + 16)
            for i in range(nfunc):
                out.append(Import(lib, self.u32(nids + 4 * i), stubs + 8 * i))
            a += ent_len * 4
        return out
