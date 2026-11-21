"""MIPS I (R3000A) instruction decoder for the PlayStation CPU.

Decodes one 32-bit word into an `Insn`. Only what the R3000A in the
PlayStation executes is recognised: MIPS I, COP0 system control and the
COP2 geometry engine. Anything else decodes as mnemonic "invalid".
"""
from __future__ import annotations

from dataclasses import dataclass

REG = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
       "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
       "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
       "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]

SPECIAL = {
    0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv", 0x06: "srlv", 0x07: "srav",
    0x08: "jr", 0x09: "jalr", 0x0C: "syscall", 0x0D: "break",
    0x10: "mfhi", 0x11: "mthi", 0x12: "mflo", 0x13: "mtlo",
    0x18: "mult", 0x19: "multu", 0x1A: "div", 0x1B: "divu",
    0x20: "add", 0x21: "addu", 0x22: "sub", 0x23: "subu",
    0x24: "and", 0x25: "or", 0x26: "xor", 0x27: "nor", 0x2A: "slt", 0x2B: "sltu",
}
REGIMM = {0x00: "bltz", 0x01: "bgez", 0x10: "bltzal", 0x11: "bgezal"}
OPCODE = {
    0x02: "j", 0x03: "jal", 0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz",
    0x08: "addi", 0x09: "addiu", 0x0A: "slti", 0x0B: "sltiu",
    0x0C: "andi", 0x0D: "ori", 0x0E: "xori", 0x0F: "lui",
    0x20: "lb", 0x21: "lh", 0x22: "lwl", 0x23: "lw", 0x24: "lbu", 0x25: "lhu", 0x26: "lwr",
    0x28: "sb", 0x29: "sh", 0x2A: "swl", 0x2B: "sw", 0x2E: "swr",
    0x32: "lwc2", 0x3A: "swc2",
}

BRANCHES = {"beq", "bne", "blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal"}
LINKING = {"jal", "jalr", "bltzal", "bgezal"}
LOADS = {"lb", "lh", "lwl", "lw", "lbu", "lhu", "lwr", "lwc2"}
STORES = {"sb", "sh", "swl", "sw", "swr", "swc2"}


@dataclass(frozen=True)
class Insn:
    addr: int
    word: int
    op: str
    rs: int = 0
    rt: int = 0
    rd: int = 0
    sa: int = 0
    imm: int = 0      # zero-extended 16-bit immediate
    simm: int = 0     # sign-extended 16-bit immediate
    target: int = 0   # absolute branch / jump target

    @property
    def is_branch(self) -> bool:
        return self.op in BRANCHES

    @property
    def is_jump(self) -> bool:
        return self.op in ("j", "jal", "jr", "jalr")

    @property
    def has_delay_slot(self) -> bool:
        return self.is_branch or self.is_jump

    @property
    def is_nop(self) -> bool:
        return self.word == 0

    def text(self) -> str:
        o, r = self.op, REG
        if self.word == 0:
            return "nop"
        if o in ("sll", "srl", "sra"):
            return f"{o} {r[self.rd]},{r[self.rt]},{self.sa}"
        if o in ("sllv", "srlv", "srav"):
            return f"{o} {r[self.rd]},{r[self.rt]},{r[self.rs]}"
        if o in ("add", "addu", "sub", "subu", "and", "or", "xor", "nor", "slt", "sltu"):
            return f"{o} {r[self.rd]},{r[self.rs]},{r[self.rt]}"
        if o in ("mult", "multu", "div", "divu"):
            return f"{o} {r[self.rs]},{r[self.rt]}"
        if o in ("mfhi", "mflo"):
            return f"{o} {r[self.rd]}"
        if o in ("mthi", "mtlo", "jr"):
            return f"{o} {r[self.rs]}"
        if o == "jalr":
            return f"jalr {r[self.rd]},{r[self.rs]}"
        if o in ("j", "jal"):
            return f"{o} 0x{self.target:08x}"
        if o in ("beq", "bne"):
            return f"{o} {r[self.rs]},{r[self.rt]},0x{self.target:08x}"
        if o in BRANCHES:
            return f"{o} {r[self.rs]},0x{self.target:08x}"
        if o in ("addi", "addiu", "slti", "sltiu"):
            return f"{o} {r[self.rt]},{r[self.rs]},{self.simm}"
        if o in ("andi", "ori", "xori"):
            return f"{o} {r[self.rt]},{r[self.rs]},0x{self.imm:x}"
        if o == "lui":
            return f"lui {r[self.rt]},0x{self.imm:x}"
        if o in LOADS or o in STORES:
            return f"{o} {r[self.rt]},{self.simm}({r[self.rs]})"
        if o in ("mfc0", "mtc0", "mfc2", "mtc2", "cfc2", "ctc2"):
            return f"{o} {r[self.rt]},${self.rd}"
        if o == "cop2":
            return f"cop2 0x{self.word & 0x1FFFFFF:07x}"
        return o


def _s16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


def decode(word: int, addr: int) -> Insn:
    opc = word >> 26
    rs, rt, rd, sa = (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31, (word >> 6) & 31
    imm = word & 0xFFFF
    simm = _s16(imm)
    kw = dict(rs=rs, rt=rt, rd=rd, sa=sa, imm=imm, simm=simm)
    btarget = (addr + 4 + (simm << 2)) & 0xFFFFFFFF
    jtarget = ((addr + 4) & 0xF0000000) | ((word & 0x3FFFFFF) << 2)

    if opc == 0:
        name = SPECIAL.get(word & 0x3F)
        if name is None:
            return Insn(addr, word, "invalid")
        return Insn(addr, word, name, **kw)
    if opc == 1:
        name = REGIMM.get(rt)
        if name is None:
            # the R3000A decodes only bit 0 and bits 4..1 == 1000 of rt
            name = ("bgez" if rt & 1 else "bltz") + ("al" if (rt & 0x1E) == 0x10 else "")
        return Insn(addr, word, name, target=btarget, **kw)
    if opc in (2, 3):
        return Insn(addr, word, OPCODE[opc], target=jtarget, **kw)
    if opc in (4, 5, 6, 7):
        return Insn(addr, word, OPCODE[opc], target=btarget, **kw)
    if opc == 0x10:
        if rs == 0x00:
            return Insn(addr, word, "mfc0", **kw)
        if rs == 0x04:
            return Insn(addr, word, "mtc0", **kw)
        if rs == 0x10 and (word & 0x3F) == 0x10:
            return Insn(addr, word, "rfe", **kw)
        return Insn(addr, word, "invalid")
    if opc == 0x12:
        if word & (1 << 25):
            return Insn(addr, word, "cop2", **kw)
        name = {0x00: "mfc2", 0x02: "cfc2", 0x04: "mtc2", 0x06: "ctc2"}.get(rs)
        return Insn(addr, word, name or "invalid", **kw)
    name = OPCODE.get(opc)
    if name is None:
        return Insn(addr, word, "invalid")
    return Insn(addr, word, name, **kw)
