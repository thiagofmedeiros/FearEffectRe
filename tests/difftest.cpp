// Differential test: every recompiled function is run against the
// interpreter from the same pseudo-random machine state. Calls, tail jumps
// and hardware accesses are not executed; they are recorded, and both sides
// must record the same sequence and end with the same registers and memory.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "disc/DiscImage.h"
#include "psx/Machine.h"

namespace fe {
RecompModule exeModule();
const JumpMeta* exeJumpMeta(size_t& count);
}  // namespace fe

using namespace fe;

namespace {

constexpr u32 kSentinel = 0xBFC0FFF0;
constexpr u64 kBudget = 20000;

struct Rng {
    u64 s;
    u32 next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<u32>(s >> 16);
    }
};

u64 mix(u64 h, u64 v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
}

struct Outcome {
    std::string stop;  // empty when the function returned
    Cpu cpu;
    u64 trace = 0;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: fe_difftest <disc.cue> [seeds] [function-address]\n");
        return 2;
    }
    int seeds = argc > 2 ? std::atoi(argv[2]) : 3;
    u32 only = argc > 3 ? static_cast<u32>(std::strtoul(argv[3], nullptr, 16)) : 0;

    DiscImage disc(argv[1]);
    auto boot = disc.bootExecutable();
    if (!boot) {
        std::fprintf(stderr, "no boot executable\n");
        return 1;
    }
    auto exe = disc.readFile(*boot);

    Machine m;
    RecompModule mod = exeModule();
    m.modules.push_back(mod);
    size_t metaCount = 0;
    const JumpMeta* meta = exeJumpMeta(metaCount);
    m.jumpMeta.assign(meta, meta + metaCount);

    // Baseline memory: noise everywhere, then the executable on top.
    std::vector<u8> baseline(kRamSize);
    Rng fill{0x1234567887654321ull};
    for (size_t i = 0; i < kRamSize; i += 4) {
        u32 v = fill.next();
        std::memcpy(baseline.data() + i, &v, 4);
    }
    std::memcpy(m.ram.get(), baseline.data(), kRamSize);
    m.loadExe(exe);
    std::memcpy(baseline.data(), m.ram.get(), kRamSize);
    std::vector<u8> ramA(kRamSize);
    std::array<u8, 0x400> scratchA{};

    u64 trace = 0;
    m.slowCalls = true;
    m.hooks.call = [&](Cpu& c, u32 target) {
        trace = mix(trace, target);
        for (int i = 4; i < 8; ++i) trace = mix(trace, c.r[i]);
        trace = mix(trace, c.r[29]);
        c.r[2] = static_cast<u32>(trace >> 20);
    };
    m.hooks.ioRead = [&](u32 addr, int size, u32& value) {
        trace = mix(trace, addr * 8u + static_cast<u32>(size));
        value = static_cast<u32>(mix(addr, 0x5555) >> 24) & (size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu);
        return true;
    };
    m.hooks.ioWrite = [&](u32 addr, int size, u32 value) {
        trace = mix(mix(trace, addr * 8u + static_cast<u32>(size)), value);
        return true;
    };

    auto runOne = [&](u32 entry, u64 seed, bool native) {
        Outcome out;
        std::memcpy(m.ram.get(), baseline.data(), kRamSize);
        m.scratch.fill(0);
        Rng rng{seed * 0x9E3779B97F4A7C15ull + entry};
        Cpu& c = m.cpu;
        c = Cpu{};
        c.m = &m;
        for (int i = 1; i < 32; ++i) {
            u32 v = rng.next();
            c.r[i] = (rng.next() & 1) ? 0x80100000u + (v & 0xFFFFC) : (rng.next() & 1) ? v & 0xFF : v;
        }
        c.hi = rng.next();
        c.lo = rng.next();
        for (auto& v : c.gteData) v = rng.next();
        for (auto& v : c.gteCtrl) v = rng.next();
        c.r[29] = 0x801F0000;
        c.r[31] = kSentinel;
        c.pc = entry;
        m.clock = 0;
        m.cycleLimit = kBudget;
        m.sliceLen = c.cycles = static_cast<s64>(kBudget);
        m.nextVblank = ~0ull;
        m.iStat = m.iMask = 0;
        m.log.clear();
        trace = 0;
        try {
            if (native) mod.table[(entry - mod.base) >> 2](c);
            else m.interpret(c, kSentinel, 0, true);
        } catch (const Stop& s) {
            out.stop = s.reason;
        } catch (const ExceptionReturn&) {
            out.stop = "exception return";
        }
        out.cpu = c;
        out.trace = trace;
        return out;
    };

    size_t functions = 0, cases = 0, compared = 0, inconclusive = 0, failures = 0;
    for (u32 off = 0; off < mod.size; off += 4) {
        if (!mod.table[off >> 2]) continue;
        u32 entry = mod.base + off;
        if (only && entry != only) continue;
        ++functions;
        bool failed = false;
        for (int seed = 1; seed <= seeds && !failed; ++seed) {
            ++cases;
            Outcome a = runOne(entry, static_cast<u64>(seed), false);
            std::memcpy(ramA.data(), m.ram.get(), kRamSize);
            scratchA = m.scratch;
            Outcome b = runOne(entry, static_cast<u64>(seed), true);
            if (a.stop == "cycle limit" || b.stop == "cycle limit") {
                ++inconclusive;
                continue;
            }
            ++compared;
            const char* why = nullptr;
            if (a.stop != b.stop) why = "stop reason";
            else if (a.trace != b.trace) why = "call/io trace";
            else if (std::memcmp(a.cpu.r, b.cpu.r, sizeof a.cpu.r)) why = "registers";
            else if (a.cpu.hi != b.cpu.hi || a.cpu.lo != b.cpu.lo) why = "hi/lo";
            else if (std::memcmp(a.cpu.cop0, b.cpu.cop0, sizeof a.cpu.cop0)) why = "cop0";
            else if (std::memcmp(a.cpu.gteData, b.cpu.gteData, sizeof a.cpu.gteData) ||
                     std::memcmp(a.cpu.gteCtrl, b.cpu.gteCtrl, sizeof a.cpu.gteCtrl)) why = "gte registers";
            else if (std::memcmp(ramA.data(), m.ram.get(), kRamSize)) why = "memory";
            else if (scratchA != m.scratch) why = "scratchpad";
            if (!why) continue;
            failed = true;
            ++failures;
            if (failures <= 40) {
                std::printf("MISMATCH fn_%08X seed %d: %s (interp stop '%s', native stop '%s')\n", entry, seed, why,
                            a.stop.c_str(), b.stop.c_str());
                if (!std::strcmp(why, "registers"))
                    for (int i = 0; i < 32; ++i)
                        if (a.cpu.r[i] != b.cpu.r[i])
                            std::printf("    r%d interp %08x native %08x\n", i, a.cpu.r[i], b.cpu.r[i]);
            }
        }
    }
    std::printf("functions %zu  cases %zu  compared %zu  inconclusive %zu  mismatching functions %zu\n", functions,
                cases, compared, inconclusive, failures);
    return failures ? 1 : 0;
}
