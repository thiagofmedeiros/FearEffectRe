"""Function discovery for a PS-X EXE or a raw overlay image.

Functions are found from the entry point, every `jal` target, and words in
the image that point at a function prologue. Each function's body is the set
of instructions reachable from its entry through branches and jump tables.
"""
from __future__ import annotations

import os
import struct
import sys
from dataclasses import dataclass, field

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mips import Insn, decode  # noqa: E402


@dataclass
class Image:
    base: int
    data: bytes
    entry: int | None = None
    name: str = "image"

    @property
    def end(self) -> int:
        return self.base + len(self.data)

    def contains(self, addr: int) -> bool:
        return self.base <= addr < self.end and not addr & 3

    def word(self, addr: int) -> int:
        return struct.unpack_from("<I", self.data, addr - self.base)[0]

    def insn(self, addr: int) -> Insn:
        return decode(self.word(addr), addr)


def load_psx_exe(path: str) -> Image:
    raw = open(path, "rb").read()
    if raw[:8] != b"PS-X EXE":
        raise ValueError(f"{path}: not a PS-X EXE")
    pc, _gp, t_addr, t_size = struct.unpack_from("<4I", raw, 0x10)
    return Image(t_addr, raw[0x800:0x800 + t_size], pc, "exe")


@dataclass
class Function:
    addr: int
    insns: dict[int, Insn] = field(default_factory=dict)
    labels: set[int] = field(default_factory=set)       # in-function branch targets
    calls: set[int] = field(default_factory=set)        # direct jal targets
    tail_calls: set[int] = field(default_factory=set)   # j to another function
    jump_tables: dict[int, list[int]] = field(default_factory=dict)  # jr addr -> targets
    indirect_jumps: set[int] = field(default_factory=set)            # unresolved jr reg
    has_jalr: bool = False
    bad: str | None = None

    @property
    def end(self) -> int:
        return max(self.insns) + 4 if self.insns else self.addr


def _looks_like_prologue(img: Image, addr: int) -> bool:
    """addiu sp,sp,-N within the first few instructions."""
    for i in range(4):
        a = addr + 4 * i
        if not img.contains(a):
            return False
        ins = img.insn(a)
        if ins.op == "invalid":
            return False
        if ins.op == "addiu" and ins.rs == 29 and ins.rt == 29 and ins.simm < 0:
            return True
        if ins.has_delay_slot:
            return False
    return False


def _find_jump_table(img: Image, func: Function, jr: Insn, order: list[int]) -> list[int] | None:
    """Recognise the compiler's switch idiom ending in `jr reg`.

        sltiu at,idx,N ; beqz at,default ; sll idx,idx,2
        lui at,hi ; addu at,at,idx ; lw reg,lo(at) ; jr reg
    """
    reg = jr.rs
    table = None
    bound = None
    hi: dict[int, int] = {}
    window = [a for a in order if a < jr.addr][-24:]
    # resolve the table address: last `lw reg, lo(base)` where base came from lui
    for a in window:
        ins = func.insns[a]
        if ins.op == "lui":
            hi[ins.rt] = ins.imm << 16
        elif ins.op in ("addiu", "ori") and ins.rs in hi and ins.rt != 0:
            hi[ins.rt] = (hi[ins.rs] + (ins.simm if ins.op == "addiu" else ins.imm)) & 0xFFFFFFFF
        elif ins.op == "addu" and (ins.rs in hi or ins.rt in hi):
            src = ins.rs if ins.rs in hi else ins.rt
            hi[ins.rd] = hi[src]
        elif ins.op == "lw" and ins.rt == reg and ins.rs in hi:
            table = (hi[ins.rs] + ins.simm) & 0xFFFFFFFF
        elif ins.op == "sltiu":
            bound = ins.imm
        else:
            # any other write invalidates what we knew about the register
            if ins.op in ("sll", "srl", "sra", "sllv", "srlv", "srav", "add", "addu", "sub", "subu",
                          "and", "or", "xor", "nor", "slt", "sltu", "mfhi", "mflo", "jalr"):
                hi.pop(ins.rd, None)
            elif ins.op in ("addi", "addiu", "slti", "andi", "ori", "xori", "lb", "lh", "lwl", "lw",
                            "lbu", "lhu", "lwr", "mfc0", "mfc2", "cfc2"):
                hi.pop(ins.rt, None)
            elif ins.op == "jal":
                for r in (2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25):
                    hi.pop(r, None)
    if table is None or not img.contains(table):
        return None
    targets = []
    limit = bound if bound else 1024
    for i in range(limit):
        a = table + 4 * i
        if not img.contains(a):
            break
        t = img.word(a)
        if not img.contains(t) or img.insn(t).op == "invalid":
            break
        if bound is None and abs(t - jr.addr) > 0x4000:
            break
        targets.append(t)
    return targets or None


def trace_function(img: Image, addr: int, known_entries: set[int]) -> Function:
    func = Function(addr)
    work = [addr]
    pending_tables: list[Insn] = []
    # Addresses decoded as part of a straight-line walk. A delay slot is in
    # `insns` as soon as its branch is seen, but when something also branches
    # to it the walk still has to continue through it.
    walked: set[int] = set()
    while work or pending_tables:
        if not work:
            jr = pending_tables.pop()
            order = sorted(func.insns)
            targets = _find_jump_table(img, func, jr, order)
            if targets is None:
                func.indirect_jumps.add(jr.addr)
                continue
            func.jump_tables[jr.addr] = targets
            for t in targets:
                func.labels.add(t)
                if t not in func.insns:
                    work.append(t)
            continue
        pc = work.pop()
        while True:
            if pc in walked:
                break
            walked.add(pc)
            if not img.contains(pc):
                func.bad = f"runs out of image at {pc:08x}"
                break
            ins = img.insn(pc)
            if ins.op == "invalid":
                func.bad = f"invalid instruction at {pc:08x}"
                break
            func.insns[pc] = ins
            if not ins.has_delay_slot:
                pc += 4
                continue
            # delay slot always belongs to the function
            if img.contains(pc + 4):
                slot = img.insn(pc + 4)
                if slot.op == "invalid" or slot.has_delay_slot:
                    func.bad = f"bad delay slot at {pc + 4:08x}"
                    break
                func.insns.setdefault(pc + 4, slot)
            if ins.is_branch:
                func.labels.add(ins.target)
                work.append(ins.target)
                unconditional = (ins.op == "beq" and ins.rs == ins.rt) or (ins.op == "bgez" and ins.rs == 0)
                if unconditional:
                    break
                func.labels.add(pc + 8)
                pc += 8
                continue
            if ins.op == "jal":
                func.calls.add(ins.target)
                pc += 8
                continue
            if ins.op == "jalr":
                func.has_jalr = True
                pc += 8
                continue
            if ins.op == "j":
                # a jump to another function's entry is a tail call; anything
                # else stays inside this function
                if ins.target in known_entries and ins.target != addr:
                    func.tail_calls.add(ins.target)
                elif img.contains(ins.target):
                    func.labels.add(ins.target)
                    work.append(ins.target)
                else:
                    func.tail_calls.add(ins.target)
                break
            if ins.op == "jr":
                if ins.rs != 31:
                    pending_tables.append(ins)
                break
    return func


def discover(img: Image, extra_entries: list[int] | None = None) -> dict[int, Function]:
    entries: set[int] = set(extra_entries or [])
    if img.entry is not None:
        entries.add(img.entry)

    # every jal in the image whose target lands inside it
    for off in range(0, len(img.data) - 3, 4):
        w = struct.unpack_from("<I", img.data, off)[0]
        if w >> 26 == 3:
            t = 0x80000000 | ((w & 0x3FFFFFF) << 2)
            if img.contains(t) and _plausible_code(img, t):
                entries.add(t)
    # data words that point at a prologue (callbacks, vtables, handler tables)
    for off in range(0, len(img.data) - 3, 4):
        w = struct.unpack_from("<I", img.data, off)[0]
        if img.contains(w) and w not in entries and _looks_like_prologue(img, w):
            entries.add(w)

    funcs: dict[int, Function] = {}
    work = sorted(entries)
    while work:
        a = work.pop()
        if a in funcs:
            continue
        f = trace_function(img, a, entries)
        funcs[a] = f
        for t in f.calls | f.tail_calls:
            if img.contains(t) and t not in entries:
                entries.add(t)
                work.append(t)
    # drop entries that turned out to be data
    return {a: f for a, f in funcs.items() if f.bad is None and f.insns}


def _plausible_code(img: Image, addr: int) -> bool:
    for i in range(6):
        a = addr + 4 * i
        if not img.contains(a):
            return False
        ins = img.insn(a)
        if ins.op == "invalid":
            return False
        if ins.op == "jr":
            return True
    return True


if __name__ == "__main__":
    import sys
    from collections import Counter

    img = load_psx_exe(sys.argv[1])
    funcs = discover(img)
    total = sum(len(f.insns) for f in funcs.values())
    ops = Counter(i.op for f in funcs.values() for i in f.insns.values())
    lo, hi = min(funcs), max(f.end for f in funcs.values())
    print(f"image {img.base:08x}-{img.end:08x} entry {img.entry:08x}")
    print(f"functions {len(funcs)}  instructions {total}  code span {lo:08x}-{hi:08x}")
    print(f"jump tables {sum(len(f.jump_tables) for f in funcs.values())}  "
          f"unresolved jr {sum(len(f.indirect_jumps) for f in funcs.values())}  "
          f"functions with jalr {sum(f.has_jalr for f in funcs.values())}")
    print("cop2 ops", {k: v for k, v in ops.items() if "c2" in k or k == "cop2"})
    print("cop0 ops", {k: v for k, v in ops.items() if "c0" in k or k in ("rfe", "syscall", "break")})
    for f in sorted(funcs.values(), key=lambda f: f.addr):
        for a in sorted(f.indirect_jumps):
            ctx = " ; ".join(f.insns[x].text() for x in sorted(f.insns) if a - 20 <= x <= a + 4)
            print(f"  unresolved {f.addr:08x} @{a:08x}: {ctx}")
