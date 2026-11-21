"""Translate discovered MIPS functions to C++ against src/core/recomp/Runtime.h.

Every guest function becomes `void fn_XXXXXXXX(fe::Cpu& c)`. Branches inside
a function are gotos, delay slots are folded into the instruction that owns
them, calls are direct C++ calls, and jumps the analysis could not resolve
go through the runtime dispatcher.
"""
from __future__ import annotations

import zlib

from analyze import Function, Image
from mips import Insn


def _r(n: int) -> str:
    return f"R[{n}]"


def _u(v: int) -> str:
    return f"0x{v & 0xFFFFFFFF:X}u"


def _addr(ins: Insn) -> str:
    if ins.simm == 0:
        return _r(ins.rs)
    if ins.rs == 0:
        return _u(ins.simm)
    return f"{_r(ins.rs)} + {_u(ins.simm)}"


def _set(reg: int, expr: str) -> str:
    return f"{_r(reg)} = {expr};" if reg else f"(void)({expr});"


def simple(ins: Insn) -> str:
    """C++ for an instruction without control flow."""
    o = ins.op
    rs, rt, rd = _r(ins.rs), _r(ins.rt), _r(ins.rd)
    if ins.word == 0:
        return ";"
    if o == "sll":
        return _set(ins.rd, f"{rt} << {ins.sa}")
    if o == "srl":
        return _set(ins.rd, f"{rt} >> {ins.sa}")
    if o == "sra":
        return _set(ins.rd, f"(u32)((s32){rt} >> {ins.sa})")
    if o == "sllv":
        return _set(ins.rd, f"{rt} << ({rs} & 31)")
    if o == "srlv":
        return _set(ins.rd, f"{rt} >> ({rs} & 31)")
    if o == "srav":
        return _set(ins.rd, f"(u32)((s32){rt} >> ({rs} & 31))")
    if o in ("add", "addu"):
        return _set(ins.rd, f"{rs} + {rt}")
    if o in ("sub", "subu"):
        return _set(ins.rd, f"{rs} - {rt}")
    if o == "and":
        return _set(ins.rd, f"{rs} & {rt}")
    if o == "or":
        return _set(ins.rd, f"{rs} | {rt}")
    if o == "xor":
        return _set(ins.rd, f"{rs} ^ {rt}")
    if o == "nor":
        return _set(ins.rd, f"~({rs} | {rt})")
    if o == "slt":
        return _set(ins.rd, f"(s32){rs} < (s32){rt}")
    if o == "sltu":
        return _set(ins.rd, f"{rs} < {rt}")
    if o == "mfhi":
        return _set(ins.rd, "c.hi")
    if o == "mflo":
        return _set(ins.rd, "c.lo")
    if o == "mthi":
        return f"c.hi = {rs};"
    if o == "mtlo":
        return f"c.lo = {rs};"
    if o == "mult":
        return f"opMult(c, {rs}, {rt});"
    if o == "multu":
        return f"opMultu(c, {rs}, {rt});"
    if o == "div":
        return f"opDiv(c, {rs}, {rt});"
    if o == "divu":
        return f"opDivu(c, {rs}, {rt});"
    if o in ("addi", "addiu"):
        return _set(ins.rt, f"{rs} + {_u(ins.simm)}")
    if o == "slti":
        return _set(ins.rt, f"(s32){rs} < (s32){_u(ins.simm)}")
    if o == "sltiu":
        return _set(ins.rt, f"{rs} < {_u(ins.simm)}")
    if o == "andi":
        return _set(ins.rt, f"{rs} & {_u(ins.imm)}")
    if o == "ori":
        return _set(ins.rt, f"{rs} | {_u(ins.imm)}")
    if o == "xori":
        return _set(ins.rt, f"{rs} ^ {_u(ins.imm)}")
    if o == "lui":
        return _set(ins.rt, _u(ins.imm << 16))
    a = _addr(ins)
    if o == "lb":
        return _set(ins.rt, f"(u32)(s32)(s8)rd8(c, {a})")
    if o == "lbu":
        return _set(ins.rt, f"rd8(c, {a})")
    if o == "lh":
        return _set(ins.rt, f"(u32)(s32)(s16)rd16(c, {a})")
    if o == "lhu":
        return _set(ins.rt, f"rd16(c, {a})")
    if o == "lw":
        return _set(ins.rt, f"rd32(c, {a})")
    if o == "lwl":
        return _set(ins.rt, f"lwl(c, {a}, {rt})")
    if o == "lwr":
        return _set(ins.rt, f"lwr(c, {a}, {rt})")
    if o == "sb":
        return f"wr8(c, {a}, {rt});"
    if o == "sh":
        return f"wr16(c, {a}, {rt});"
    if o == "sw":
        return f"wr32(c, {a}, {rt});"
    if o == "swl":
        return f"swl(c, {a}, {rt});"
    if o == "swr":
        return f"swr(c, {a}, {rt});"
    if o == "lwc2":
        return f"gteMtc2(c, {ins.rt}, rd32(c, {a}));"
    if o == "swc2":
        return f"wr32(c, {a}, gteMfc2(c, {ins.rt}));"
    if o == "mfc0":
        return _set(ins.rt, f"mfc0(c, {ins.rd})")
    if o == "mtc0":
        return f"mtc0(c, {ins.rd}, {rt});"
    if o == "rfe":
        return "rfe(c);"
    if o == "mfc2":
        return _set(ins.rt, f"gteMfc2(c, {ins.rd})")
    if o == "cfc2":
        return _set(ins.rt, f"gteCfc2(c, {ins.rd})")
    if o == "mtc2":
        return f"gteMtc2(c, {ins.rd}, {rt});"
    if o == "ctc2":
        return f"gteCtc2(c, {ins.rd}, {rt});"
    if o == "cop2":
        return f"gteCommand(c, {_u(ins.word)});"
    if o == "syscall":
        return f"opSyscall(c, {_u(ins.addr)});"
    if o == "break":
        return f"opBreak(c, {_u(ins.addr)}, {_u(ins.word)});"
    raise ValueError(f"cannot translate {o} at {ins.addr:08x}")


def _condition(ins: Insn) -> str:
    rs, rt = _r(ins.rs), _r(ins.rt)
    o = ins.op
    if o == "beq":
        return "true" if ins.rs == ins.rt else f"{rs} == {rt}"
    if o == "bne":
        return "false" if ins.rs == ins.rt else f"{rs} != {rt}"
    if o == "blez":
        return f"(s32){rs} <= 0"
    if o == "bgtz":
        return f"(s32){rs} > 0"
    if o in ("bltz", "bltzal"):
        return f"(s32){rs} < 0"
    if o in ("bgez", "bgezal"):
        return f"(s32){rs} >= 0"
    raise ValueError(o)


def fn_name(addr: int) -> str:
    return f"fn_{addr:08X}"


def emit_function(func: Function, known: set[int]) -> list[str]:
    insns = func.insns
    order = sorted(insns)
    slots = {a + 4 for a in order if insns[a].has_delay_slot}
    leaders = {func.addr} | {a for a in func.labels if a in insns}

    def call(target: int, ret: int) -> str:
        if target in known:
            return f"FE_CALL({fn_name(target)}, {_u(target)}, {_u(ret)});"
        return f"c.m->call(c, {_u(target)}); FE_RETURNED({_u(ret)});"

    def tail(target: int) -> str:
        if target in known:
            return f"FE_TAIL({fn_name(target)}, {_u(target)});"
        return f"c.m->call(c, {_u(target)});"

    def block_len(start: int) -> int:
        n, a = 0, start
        while a in insns:
            n += 1
            a += 4
            if a in leaders:
                break
        return n

    out = [f"void {fn_name(func.addr)}(Cpu& c) {{", "    u32* const R = c.r;"]
    if order[0] != func.addr:
        out.append(f"    goto L_{func.addr:08X};")

    for a in order:
        ins = insns[a]
        if a in slots:
            # executed as part of the preceding branch; only reachable on its
            # own when something branches straight to it
            if a in leaders:
                out.append(f"    if (0) {{ L_{a:08X}: FE_BLOCK({block_len(a)}); {simple(ins)} }}")
            continue
        prefix = f"    L_{a:08X}: FE_BLOCK({block_len(a)}); " if a in leaders else "    "
        if not ins.has_delay_slot:
            out.append(prefix + simple(ins))
            continue
        slot = simple(insns[a + 4])
        o = ins.op
        if ins.is_branch:
            cond = _condition(ins)
            if o in ("bltzal", "bgezal"):
                out.append(f"{prefix}{{ bool t = {cond}; R[31] = {_u(a + 8)}; {slot} if (t) {{ {call(ins.target, a + 8)} }} }}")
            elif cond == "true":
                out.append(f"{prefix}{{ {slot} goto L_{ins.target:08X}; }}")
            elif cond == "false":
                out.append(f"{prefix}{{ {slot} }}")
            else:
                out.append(f"{prefix}{{ bool t = {cond}; {slot} if (t) goto L_{ins.target:08X}; }}")
        elif o == "jal":
            out.append(f"{prefix}{{ R[31] = {_u(a + 8)}; {slot} {call(ins.target, a + 8)} }}")
        elif o == "jalr":
            link = f"R[{ins.rd}] = {_u(a + 8)}; " if ins.rd else ""
            out.append(f"{prefix}{{ u32 t = {_r(ins.rs)}; {link}{slot} c.m->call(c, t); FE_RETURNED({_u(a + 8)}); }}")
        elif o == "j":
            if ins.target in func.tail_calls:
                out.append(f"{prefix}{{ {slot} {tail(ins.target)} return; }}")
            else:
                out.append(f"{prefix}{{ {slot} goto L_{ins.target:08X}; }}")
        elif o == "jr":
            if ins.rs == 31:
                out.append(f"{prefix}{{ u32 t = R[31]; {slot} c.pc = t; return; }}")
            elif a in func.jump_tables:
                cases = " ".join(f"case {_u(t)}: goto L_{t:08X};" for t in sorted(set(func.jump_tables[a])))
                out.append(f"{prefix}{{ u32 t = {_r(ins.rs)}; {slot} switch (t) {{ {cases} "
                           f"default: c.m->jump(c, t); return; }} }}")
            else:
                out.append(f"{prefix}{{ u32 t = {_r(ins.rs)}; {slot} c.m->jump(c, t); return; }}")
        else:
            raise ValueError(o)
    out.append("}")
    return out


def emit_module(img: Image, funcs: dict[int, Function], outdir: str, shards: int, module: str = "exe") -> None:
    import os

    os.makedirs(outdir, exist_ok=True)
    known = set(funcs)
    ordered = [funcs[a] for a in sorted(funcs)]
    total = sum(len(f.insns) for f in ordered)
    per_shard = total // shards + 1

    def write(name: str, lines: list[str]) -> None:
        path = os.path.join(outdir, name)
        text = "\n".join(lines) + "\n"
        if os.path.exists(path) and open(path, encoding="utf-8").read() == text:
            return  # keep the timestamp so unchanged shards are not recompiled
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)

    banner = "// Generated from the game executable by tools/recomp. Do not edit, do not commit."
    write(f"fe_{module}_decl.h", [banner, "#pragma once", '#include "recomp/Runtime.h"']
          + [f"void {fn_name(f.addr)}(fe::Cpu& c);" for f in ordered])

    bucket: list[list[Function]] = [[] for _ in range(shards)]
    acc, idx = 0, 0
    for f in ordered:
        bucket[idx].append(f)
        acc += len(f.insns)
        if acc >= per_shard and idx < shards - 1:
            acc, idx = 0, idx + 1
    for i, fs in enumerate(bucket):
        lines = [banner, f'#include "fe_{module}_decl.h"', "using namespace fe;", ""]
        for f in fs:
            lines += emit_function(f, known)
            lines.append("")
        write(f"fe_{module}_{i:02d}.cpp", lines)

    meta: dict[int, int] = {}
    for f in ordered:
        for a in f.jump_tables:
            meta[a] = 1
        for a, ins in f.insns.items():
            if ins.op == "j" and ins.target in f.tail_calls:
                meta[a] = 2
    lines = [banner, f'#include "fe_{module}_decl.h"', "#include <vector>", "namespace fe {",
             "namespace {", "struct Entry { u32 addr; GuestFn fn; };", "const Entry kEntries[] = {"]
    lines += [f"    {{{_u(f.addr)}, {fn_name(f.addr)}}}," for f in ordered]
    lines += ["};", "const JumpMeta kMeta[] = {"]
    lines += [f"    {{{_u(a)}, {k}}}," for a, k in sorted(meta.items())] or ["    {0, 0},"]
    lines += ["};", "}  // namespace", "",
              f"// {len(ordered)} functions, {total} instructions",
              f"RecompModule {module}Module() {{",
              f"    static std::vector<GuestFn> table({_u(len(img.data) // 4)});",
              "    for (const Entry& e : kEntries) table[(e.addr - " + _u(img.base) + ") >> 2] = e.fn;",
              f'    return RecompModule{{"{module}", {_u(img.base)}, {_u(len(img.data))}, table.data()}};',
              "}",
              f"const JumpMeta* {module}JumpMeta(size_t& count) {{",
              "    count = sizeof(kMeta) / sizeof(kMeta[0]);",
              "    return kMeta;",
              "}",
              f"u32 {module}ImageCrc() {{ return {_u(zlib.crc32(img.data))}; }}",
              "}  // namespace fe"]
    write(f"fe_{module}_table.cpp", lines)
