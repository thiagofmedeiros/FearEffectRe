#include "Machine.h"

#include "Cdrom.h"
#include "Gpu.h"
#include "Mdec.h"
#include "Spu.h"

#include <algorithm>
#include <stdexcept>

namespace fe {

namespace {
constexpr u32 kReturnSentinel = 0xBFC0FFF0;

bool isBiosVector(u32 pc) {
    u32 p = pc & 0x1FFFFFFF;
    return p == 0xA0 || p == 0xB0 || p == 0xC0;
}
}  // namespace

Machine::Machine() : ram(new u8[kRamSize]()), gpu(std::make_shared<Gpu>()), mdec(std::make_shared<Mdec>()), spu(std::make_shared<Spu>()) {
    cpu.m = this;
    cpu.cop0[12] = 0x10900000;  // SR as the BIOS leaves it for the boot executable
    cpu.cop0[15] = 0x00000002;  // PRId
}

void Machine::note(std::string text) {
    if (log.size() < 20000) log.push_back(std::move(text));
}

std::string Machine::cstring(u32 addr, size_t max) {
    std::string s;
    for (size_t i = 0; i < max; ++i) {
        char ch = static_cast<char>(read(addr + static_cast<u32>(i), 1));
        if (!ch) break;
        s.push_back(ch);
    }
    return s;
}

u32 Machine::read(u32 addr, int size) {
    u32 p = addr & 0x1FFFFFFF;
    if (addr >= 0xFFFE0000) return 0;  // cache control
    if (p < 0x800000) {
        u32 v = 0;
        std::memcpy(&v, ram.get() + (p & (kRamSize - 1)), static_cast<size_t>(size));
        return v;
    }
    if (p >= 0x1F800000 && p < 0x1F800400) {
        u32 v = 0;
        std::memcpy(&v, scratch.data() + (p - 0x1F800000), static_cast<size_t>(size));
        return v;
    }
    u32 v = 0;
    if (hooks.ioRead && hooks.ioRead(addr, size, v)) return v;
    if (p >= 0x1F801000 && p < 0x1F803000) return ioRead(p, size);
    return 0;
}

void Machine::write(u32 addr, int size, u32 value) {
    u32 p = addr & 0x1FFFFFFF;
    if (addr >= 0xFFFE0000) return;
    if (p < 0x800000) {
        if (cpu.cop0[12] & 0x10000) return;  // cache isolated
        std::memcpy(ram.get() + (p & (kRamSize - 1)), &value, static_cast<size_t>(size));
        return;
    }
    if (p >= 0x1F800000 && p < 0x1F800400) {
        std::memcpy(scratch.data() + (p - 0x1F800000), &value, static_cast<size_t>(size));
        return;
    }
    if (hooks.ioWrite && hooks.ioWrite(addr, size, value)) return;
    if (p >= 0x1F801000 && p < 0x1F803000) ioWrite(p, size, value);
}

void Machine::loadExe(const std::vector<u8>& exe) {
    if (exe.size() < 0x800 || std::memcmp(exe.data(), "PS-X EXE", 8) != 0)
        throw std::runtime_error("not a PS-X EXE");
    auto w = [&](size_t off) {
        u32 v;
        std::memcpy(&v, exe.data() + off, 4);
        return v;
    };
    u32 pc = w(0x10), gp = w(0x14), tAddr = w(0x18), tSize = w(0x1C);
    u32 bAddr = w(0x28), bSize = w(0x2C), sAddr = w(0x30), sSize = w(0x34);
    if (exe.size() < 0x800 + static_cast<size_t>(tSize)) throw std::runtime_error("truncated PS-X EXE");
    std::memcpy(ramPtr(tAddr), exe.data() + 0x800, tSize);
    if (bSize) std::memset(ramPtr(bAddr), 0, bSize);
    // Kernel structures the game's libraries look at directly. The memory
    // card library finds the "bu" device in the device table (pointer and
    // size at 150h/154h) before it uses the BIOS file functions.
    const u32 dcb = 0xB000, name = 0xB0F0;
    write(0x80000150, 4, 0x80000000 | dcb);
    write(0x80000154, 4, 0x50);
    write(0x80000000 | dcb, 4, 0x80000000 | name);
    write(0x80000000 | name, 4, 0x00007562);  // "bu"

    cpu.pc = pc;
    cpu.r[28] = gp;
    cpu.r[29] = sAddr ? sAddr + sSize : 0x801FFFF0;
    cpu.r[30] = cpu.r[29];
    cpu.r[31] = kReturnSentinel;
}

GuestFn Machine::lookup(u32 pc) const {
    for (const auto& mod : modules) {
        u32 off = pc - mod.base;
        if (off < mod.size && !(off & 3)) return mod.table[off >> 2];
    }
    return nullptr;
}

const JumpMeta* Machine::findJumpMeta(u32 pc) const {
    auto it = std::lower_bound(jumpMeta.begin(), jumpMeta.end(), pc,
                               [](const JumpMeta& m, u32 v) { return m.pc < v; });
    return it != jumpMeta.end() && it->pc == pc ? &*it : nullptr;
}

void Machine::run() {
    for (;;) {
        try {
            interpret(cpu, 0xFFFFFFFF, 0);
            return;
        } catch (Restart& r) {
            // every host frame of guest code is gone: the restored game
            // carries on from its saved pc in the interpreter, which hands
            // over to recompiled code at the next function it calls
            loadState(r.data);
        }
    }
}

// Every path leaves c.pc at the address the guest continues at.
void Machine::call(Cpu& c, u32 target) {
    if (hooks.call) {
        hooks.call(c, target);
        return;
    }
    if (nativeEnabled) {
        if (GuestFn fn = lookup(target)) {
            fn(c);
            return;
        }
    }
    if (isBiosVector(target)) {
        biosCall(c, target & 0xFF);
        c.pc = c.r[31];
        return;
    }
    u32 ra = c.r[31], sp = c.r[29];
    c.pc = target;
    interpret(c, ra, sp);
}

void Machine::jump(Cpu& c, u32 target) { call(c, target); }

void Machine::resume(Cpu& c, u32 untilPc) { interpret(c, untilPc, 0); }

void Machine::requestSlice(Cpu& c) {
    if (c.cycles > 0) {
        sliceLen -= c.cycles;
        c.cycles = 0;
    }
}

void Machine::onSlice(Cpu& c) {
    s64 elapsed = sliceLen - c.cycles;
    if (elapsed > 0) clock += static_cast<u64>(elapsed);
    if (clock >= cycleLimit) throw Stop{"cycle limit"};
    if (safePointWanted && !inException && hostFrames == 0 && &c == &cpu && onSafePoint) {
        safePointWanted = false;
        onSafePoint();
    }
    while (clock >= nextVblank) {
        nextVblank += kCyclesPerFrame;
        ++frame;
        raise(kIrqVblank);
        if (onVblank) onVblank();
        if (!cardPending.empty()) {
            std::vector<u32> specs;
            specs.swap(cardPending);
            for (u32 spec : specs) {
                deliverEvent(c, 0xF0000011, spec);
                deliverEvent(c, 0xF4000001, spec);
            }
        }
        if (padStarted) {
            for (int port = 0; port < 2; ++port) {
                if (!padBuf[port] || padLen[port] < 4) continue;
                if (port == 0) {
                    write(padBuf[0] + 0, 1, 0x00);
                    write(padBuf[0] + 1, 1, 0x41);
                    write(padBuf[0] + 2, 1, padState & 0xFF);
                    write(padBuf[0] + 3, 1, padState >> 8);
                } else {
                    write(padBuf[1], 1, 0xFF);
                }
            }
        }
    }
    if (cd) cd->tick(*this);
    if (u64 frames = (clock - audioClock) / 768) {
        audioClock += frames * 768;
        if (audio.size() > 44100 * 2) audio.clear();  // nobody is listening
        spu->generate(*this, static_cast<size_t>(frames), audio);
    }
    sioUpdate();
    u64 next = std::min<u64>(nextVblank, cycleLimit);
    if (cd) next = std::min(next, cd->nextEvent());
    sliceLen = next > clock ? static_cast<s64>(std::min<u64>(next - clock, 16384)) : 1;
    if (sliceLen < 1) sliceLen = 1;
    c.cycles = sliceLen;
    if (!inException && (iStat & iMask) && (c.cop0[12] & 0x401) == 0x401) deliverInterrupt(c);
}

void Machine::deliverInterrupt(Cpu& c) {
    if (!entryIntHook && !intChains[0] && !intChains[1] && !intChains[2] && !intChains[3]) return;
    // Everything the BIOS exception handler would save in the thread's TCB.
    Cpu saved = c;
    s64 savedSlice = sliceLen;
    inException = true;
    c.cop0[13] = 0x400;
    c.cop0[14] = c.pc;
    c.cop0[12] = (c.cop0[12] & ~0x3Fu) | ((c.cop0[12] << 2) & 0x3F);
    try {
        if (iStat & iMask & kIrqVblank) deliverEvent(c, 0xF2000003, 0x0002);
        for (u32 prio = 0; prio < 4; ++prio) {
            for (u32 el = intChains[prio], guard = 0; el && guard < 64; ++guard) {
                u32 next = read(el, 4), handler = read(el + 4, 4), verifier = read(el + 8, 4);
                if (verifier) {
                    c.r[31] = kReturnSentinel;
                    call(c, verifier);
                    if (c.r[2] && handler) {
                        c.r[4] = c.r[2];
                        c.r[31] = kReturnSentinel;
                        call(c, handler);
                    }
                }
                el = next;
            }
        }
        if (entryIntHook) {
            u32 buf = entryIntHook;
            c.r[31] = read(buf, 4);
            c.r[29] = read(buf + 4, 4);
            c.r[30] = read(buf + 8, 4);
            for (u32 i = 0; i < 8; ++i) c.r[16 + i] = read(buf + 12 + 4 * i, 4);
            c.r[28] = read(buf + 44, 4);
            c.r[2] = 1;
            c.pc = c.r[31];
            interpret(c, 0xFFFFFFFF, 0);
        }
    } catch (const ExceptionReturn&) {
    }
    inException = false;
    // Time spent in the handler stays spent; registers come back from the TCB.
    s64 cycles = c.cycles;
    s64 slice = sliceLen;
    c = saved;
    c.cycles = cycles;
    sliceLen = slice;
    (void)savedSlice;
}

// ---- interpreter ----------------------------------------------------------
namespace {

struct Step {
    bool branch = false;
    bool link = false;
    u32 target = 0;
};

// Executes one non-control-flow instruction, or evaluates a branch.
inline Step execute(Cpu& c, u32 pc, u32 w) {
    u32 op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31;
    u32 imm = w & 0xFFFF;
    u32 simm = static_cast<u32>(static_cast<s32>(static_cast<s16>(imm)));
    u32* R = c.r;
    Step st;
    auto setr = [&](u32 reg, u32 v) {
        if (reg) R[reg] = v;
    };
    auto branchIf = [&](bool cond) {
        st.branch = true;
        st.target = cond ? pc + 4 + (simm << 2) : pc + 8;
    };
    switch (op) {
        case 0x00:
            switch (w & 0x3F) {
                case 0x00: setr(rd, R[rt] << sa); break;
                case 0x02: setr(rd, R[rt] >> sa); break;
                case 0x03: setr(rd, static_cast<u32>(static_cast<s32>(R[rt]) >> sa)); break;
                case 0x04: setr(rd, R[rt] << (R[rs] & 31)); break;
                case 0x06: setr(rd, R[rt] >> (R[rs] & 31)); break;
                case 0x07: setr(rd, static_cast<u32>(static_cast<s32>(R[rt]) >> (R[rs] & 31))); break;
                case 0x08: st.branch = true; st.target = R[rs]; break;
                case 0x09: st.branch = true; st.link = true; st.target = R[rs]; setr(rd, pc + 8); break;
                case 0x0C: opSyscall(c, pc); break;
                case 0x0D: opBreak(c, pc, w); break;
                case 0x10: setr(rd, c.hi); break;
                case 0x11: c.hi = R[rs]; break;
                case 0x12: setr(rd, c.lo); break;
                case 0x13: c.lo = R[rs]; break;
                case 0x18: opMult(c, R[rs], R[rt]); break;
                case 0x19: opMultu(c, R[rs], R[rt]); break;
                case 0x1A: opDiv(c, R[rs], R[rt]); break;
                case 0x1B: opDivu(c, R[rs], R[rt]); break;
                case 0x20: case 0x21: setr(rd, R[rs] + R[rt]); break;
                case 0x22: case 0x23: setr(rd, R[rs] - R[rt]); break;
                case 0x24: setr(rd, R[rs] & R[rt]); break;
                case 0x25: setr(rd, R[rs] | R[rt]); break;
                case 0x26: setr(rd, R[rs] ^ R[rt]); break;
                case 0x27: setr(rd, ~(R[rs] | R[rt])); break;
                case 0x2A: setr(rd, static_cast<s32>(R[rs]) < static_cast<s32>(R[rt])); break;
                case 0x2B: setr(rd, R[rs] < R[rt]); break;
                default: throw Stop{"invalid instruction"};
            }
            break;
        case 0x01: {
            bool cond = (rt & 1) ? static_cast<s32>(R[rs]) >= 0 : static_cast<s32>(R[rs]) < 0;
            if ((rt & 0x1E) == 0x10) {
                R[31] = pc + 8;
                st.link = cond;
            }
            branchIf(cond);
            break;
        }
        case 0x02: st.branch = true; st.target = ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2); break;
        case 0x03:
            st.branch = true; st.link = true;
            st.target = ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2);
            R[31] = pc + 8;
            break;
        case 0x04: branchIf(R[rs] == R[rt]); break;
        case 0x05: branchIf(R[rs] != R[rt]); break;
        case 0x06: branchIf(static_cast<s32>(R[rs]) <= 0); break;
        case 0x07: branchIf(static_cast<s32>(R[rs]) > 0); break;
        case 0x08: case 0x09: setr(rt, R[rs] + simm); break;
        case 0x0A: setr(rt, static_cast<s32>(R[rs]) < static_cast<s32>(simm)); break;
        case 0x0B: setr(rt, R[rs] < simm); break;
        case 0x0C: setr(rt, R[rs] & imm); break;
        case 0x0D: setr(rt, R[rs] | imm); break;
        case 0x0E: setr(rt, R[rs] ^ imm); break;
        case 0x0F: setr(rt, imm << 16); break;
        case 0x10:
            if (rs == 0) setr(rt, mfc0(c, rd));
            else if (rs == 4) mtc0(c, rd, R[rt]);
            else if (rs == 0x10) rfe(c);
            else throw Stop{"invalid cop0 instruction"};
            break;
        case 0x12:
            if (w & (1u << 25)) gteCommand(c, w);
            else if (rs == 0) setr(rt, gteMfc2(c, rd));
            else if (rs == 2) setr(rt, gteCfc2(c, rd));
            else if (rs == 4) gteMtc2(c, rd, R[rt]);
            else if (rs == 6) gteCtc2(c, rd, R[rt]);
            else throw Stop{"invalid cop2 instruction"};
            break;
        case 0x20: setr(rt, static_cast<u32>(static_cast<s32>(static_cast<s8>(rd8(c, R[rs] + simm))))); break;
        case 0x21: setr(rt, static_cast<u32>(static_cast<s32>(static_cast<s16>(rd16(c, R[rs] + simm))))); break;
        case 0x22: setr(rt, lwl(c, R[rs] + simm, R[rt])); break;
        case 0x23: setr(rt, rd32(c, R[rs] + simm)); break;
        case 0x24: setr(rt, rd8(c, R[rs] + simm)); break;
        case 0x25: setr(rt, rd16(c, R[rs] + simm)); break;
        case 0x26: setr(rt, lwr(c, R[rs] + simm, R[rt])); break;
        case 0x28: wr8(c, R[rs] + simm, R[rt]); break;
        case 0x29: wr16(c, R[rs] + simm, R[rt]); break;
        case 0x2A: swl(c, R[rs] + simm, R[rt]); break;
        case 0x2B: wr32(c, R[rs] + simm, R[rt]); break;
        case 0x2E: swr(c, R[rs] + simm, R[rt]); break;
        case 0x32: gteMtc2(c, rt, rd32(c, R[rs] + simm)); break;
        case 0x3A: wr32(c, R[rs] + simm, gteMfc2(c, rt)); break;
        default: throw Stop{"invalid instruction"};
    }
    return st;
}

}  // namespace

void Machine::interpret(Cpu& c, u32 untilPc, u32 untilSp, bool honourMeta) {
    const bool hooked = static_cast<bool>(hooks.call);
    for (;;) {
        u32 pc = c.pc;
        if (pc == untilPc && (untilSp == 0 || c.r[29] == untilSp)) return;
        if (!hooked) {
            if (nativeEnabled) {
                if (GuestFn fn = lookup(pc)) {
                    fn(c);  // leaves c.pc where the guest continues
                    continue;
                }
            }
            if (isBiosVector(pc)) {
                biosCall(c, pc & 0xFF);
                c.pc = c.r[31];
                continue;
            }
        }
        if ((c.cycles -= kCyclesPerInstruction) <= 0) {
            onSlice(c);
            if (c.pc != pc) continue;
        }
        u32 p = pc & 0x1FFFFFFF;
        if (p >= 0x800000 || (pc & 3)) throw Stop{"pc outside RAM"};
        u32 w;
        std::memcpy(&w, ram.get() + (p & (kRamSize - 1)), 4);
        Step st = execute(c, pc, w);
        if (!st.branch) {
            c.pc = pc + 4;
            continue;
        }
        // delay slot
        u32 sw;
        std::memcpy(&sw, ram.get() + ((p + 4) & (kRamSize - 1)), 4);
        c.cycles -= kCyclesPerInstruction;
        Step slot = execute(c, pc + 4, sw);
        if (slot.branch) throw Stop{"branch in delay slot"};
        if (hooked) {
            u32 opc = w >> 26;
            bool isJr = opc == 0 && (w & 0x3F) == 0x08;
            if (st.link) {
                hooks.call(c, st.target);
                c.pc = pc + 8;
                continue;
            }
            if (honourMeta && isJr) {
                if (((w >> 21) & 31) == 31) return;
                const JumpMeta* meta = findJumpMeta(pc);
                if (!meta || meta->kind != 1) {
                    hooks.call(c, st.target);
                    return;
                }
            }
            if (honourMeta && opc == 2) {
                const JumpMeta* meta = findJumpMeta(pc);
                if (meta && meta->kind == 2) {
                    hooks.call(c, st.target);
                    return;
                }
            }
        }
        c.pc = st.target;
    }
}

}  // namespace fe
