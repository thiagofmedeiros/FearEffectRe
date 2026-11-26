// The PlayStation machine state the recompiled game code runs against:
// CPU registers, memory, the interrupt controller, HLE BIOS and hardware.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fe {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

struct Machine;
struct Cdrom;
struct Gpu;
struct Mdec;
struct Spu;
class StateIO;

struct Cpu {
    u32 r[32]{};
    u32 hi{}, lo{};
    u32 pc{};
    u32 cop0[32]{};
    u32 gteData[32]{};
    u32 gteCtrl[32]{};
    s64 cycles{};  // cycles left in the current time slice
    Machine* m{};
};

using GuestFn = void (*)(Cpu&);

// A block of statically recompiled code: one entry per instruction slot,
// null where no function starts.
struct RecompModule {
    const char* name;
    u32 base;
    u32 size;
    const GuestFn* table;
};

// How a `j` or `jr` that the recompiler classified leaves its function.
// The interpreter uses this in differential tests to stop where the
// recompiled function stops.
struct JumpMeta {
    u32 pc;
    u8 kind;  // 1 = jump table inside the function, 2 = tail call
};

enum : u32 {
    kRamSize = 0x200000,
    kIrqVblank = 1u << 0,
    kIrqGpu = 1u << 1,
    kIrqCdrom = 1u << 2,
    kIrqDma = 1u << 3,
    kIrqTimer0 = 1u << 4,
    kIrqTimer1 = 1u << 5,
    kIrqTimer2 = 1u << 6,
    kIrqSio = 1u << 7,
    kIrqSpu = 1u << 9,
    kCpuHz = 33868800,
    kCyclesPerFrame = 565045,  // NTSC, 59.94 Hz
    // Average cost of an instruction. The R3000A has no data cache and RAM
    // access takes several cycles; library timeout loops (VSync) are
    // calibrated against that, so one cycle per instruction is too fast.
    kCyclesPerInstruction = 2,
};

// Thrown to unwind host frames of guest code: ReturnFromException ends an
// interrupt handler, Stop ends a run (budget exhausted, quit requested).
struct ExceptionReturn {};
struct Stop {
    std::string reason;
};
// Thrown to load a save state: unwinds every host frame of guest code, then
// Machine::run() restores `data` and carries on from the saved pc.
struct Restart {
    std::vector<u8> data;
};

struct Event {
    u32 cls{}, spec{}, mode{}, func{};
    u32 status{};  // 0 free, 0x1000 disabled, 0x2000 enabled/busy, 0x4000 ready
};

struct Hooks {
    // Differential-test hook: when set, every call and tail jump is reported
    // here instead of being executed.
    std::function<void(Cpu&, u32 target)> call;
    // Reads and writes outside RAM/scratchpad, for tests and tracing.
    std::function<bool(u32 addr, int size, u32& value)> ioRead;
    std::function<bool(u32 addr, int size, u32 value)> ioWrite;
};

struct Machine {
    Machine();

    // ---- memory ----------------------------------------------------------
    std::unique_ptr<u8[]> ram;
    std::array<u8, 0x400> scratch{};
    std::array<u8, 0x2000> ioShadow{};  // last value written to each I/O byte

    u32 read(u32 addr, int size);
    void write(u32 addr, int size, u32 value);
    u8* ramPtr(u32 addr) { return ram.get() + (addr & (kRamSize - 1)); }
    std::string cstring(u32 addr, size_t max = 256);

    // ---- execution -------------------------------------------------------
    Cpu cpu;
    std::vector<RecompModule> modules;
    std::vector<JumpMeta> jumpMeta;
    Hooks hooks;
    bool nativeEnabled = true;

    GuestFn lookup(u32 pc) const;
    const JumpMeta* findJumpMeta(u32 pc) const;
    void loadExe(const std::vector<u8>& exe);
    void run();                                   // runs from cpu.pc until Stop (loads states on Restart)
    void call(Cpu& c, u32 target);                // guest call, returns when callee returns
    void jump(Cpu& c, u32 target);                // unresolved jr: BIOS vector or tail call
    void resume(Cpu& c, u32 untilPc);             // callee came back at c.pc instead of untilPc
    void interpret(Cpu& c, u32 untilPc, u32 untilSp, bool honourMeta = false);
    void onSlice(Cpu& c);                         // time slice used up
    void requestSlice(Cpu& c);                    // re-evaluate interrupts soon

    // ---- time and interrupts --------------------------------------------
    u64 clock = 0;           // cycles since power on
    s64 sliceLen = 0;
    u64 nextVblank = kCyclesPerFrame;
    u64 frame = 0;
    u64 cycleLimit = ~0ull;  // Stop when the clock passes this
    u32 iStat = 0, iMask = 0;
    bool inException = false;

    std::function<void()> onVblank;  // called once per frame, before the interrupt is delivered
    // Save states. A state can only be taken where every guest register is in
    // `cpu` and no host frame holds guest state: at the start of a time slice,
    // outside interrupt handlers and event callbacks. Set `safePointWanted`
    // and `onSafePoint` is called at the next such point (cpu.pc is where the
    // game resumes); it can call saveState().
    bool safePointWanted = false;
    std::function<void()> onSafePoint;
    int hostFrames = 0;  // guest code running on behalf of the BIOS (event callbacks)
    std::vector<u8> saveState();
    void loadState(const std::vector<u8>& data);
    void state(StateIO& io);
    // Rotation matrices the game loaded into the GTE (row-major, 4096 = 1.0),
    // collected only while `recordRotations` is set; the reader clears the
    // list. The modern controls look for the camera among them.
    bool recordRotations = false;
    std::vector<std::array<s16, 9>> rotations;
    void raise(u32 irq) { iStat |= irq; }
    // Current time including the part of the slice already executed.
    u64 now() const { return clock + static_cast<u64>(sliceLen - cpu.cycles); }
    void deliverInterrupt(Cpu& c);

    // ---- HLE BIOS --------------------------------------------------------
    u32 entryIntHook = 0;                         // jmp_buf from HookEntryInt
    std::array<u32, 4> intChains{};               // SysEnqIntRP list heads
    std::array<Event, 32> events{};
    u32 padBuf[2]{};
    u32 padLen[2]{};
    bool padStarted = false;
    u16 padState = 0xFFFF;                        // active-low digital pad buttons
    // DualShock in analog mode: the pad answers with id 73h and the four
    // stick bytes (right x, right y, left x, left y; 00h left/up, 80h centre, FFh right/down).
    bool padAnalog = false;
    u8 padAxes[4] = {0x80, 0x80, 0x80, 0x80};
    u32 heapBase = 0, heapSize = 0;
    u32 randSeed = 0;
    // Memory card in slot 1, kept as one host file per save in `cardDir`
    // (empty = no card inserted).
    std::string cardDir;
    struct CardFile {
        std::string path;
        u32 pos = 0;
        bool open = false;
        bool noWait = false;  // opened with 0x8000: read/write return 0 and finish with an event
    };
    std::vector<CardFile> cardFiles;
    std::vector<std::string> cardListing;
    size_t cardListPos = 0;
    // Memory card operations finish "later", like the hardware: the
    // completion events are delivered at the next frame, after the game has
    // returned from the call and started waiting for them.
    std::vector<u32> cardPending;  // event specs (EvSpIOE, EvSpTIMOUT, ...)
    std::string ttyLine;
    bool logBios = false;
    std::vector<std::string> log;                 // notable events, for the boot report

    void biosCall(Cpu& c, u32 vector);
    void deliverEvent(Cpu& c, u32 cls, u32 spec);
    void note(std::string text);

    // ---- hardware --------------------------------------------------------
    u32 ioRead(u32 phys, int size);
    u32 ioReadInner(u32 phys, int size);
    int sioTrace = 0;  // controller port accesses still to be logged
    void ioWrite(u32 phys, int size, u32 value);
    void runDma(int channel);
    std::shared_ptr<Cdrom> cd;
    std::shared_ptr<Gpu> gpu;
    std::shared_ptr<Mdec> mdec;
    std::shared_ptr<Spu> spu;
    std::vector<s16> audio;   // stereo 44100 Hz output not yet collected by the front end
    u64 audioClock = 0;
    // Controller port (SIO0): a digital pad in port 1, nothing else attached.
    struct Sio {
        u16 ctrl = 0, mode = 0, baud = 0;
        u8 rx = 0xFF;
        bool rxFull = false, irq = false;
        int step = 0;  // position in the current pad transfer, -1 = device not answering
        // The byte coming back and the pad's acknowledge arrive after the transfer time.
        bool pending = false, pendingAck = false;
        u8 pendingRx = 0xFF;
        u64 at = 0;
    } sio;
    void sioTransmit(u8 value);
    void sioUpdate();
    u32 dma[7][3]{};
    u32 dpcr = 0x07654321, dicr = 0;
    u32 gpuRead = 0;
    u64 gp0Words = 0, gp1Words = 0;
    struct Timer {
        u32 mode = 0, target = 0;
        u64 base = 0;  // clock value at which the counter read zero
    } timers[3];
    std::array<u64, 0x2000 / 4> ioAccess{};       // per-register access counts
};

// ---- helpers used by generated code -------------------------------------
inline u32 rd32(Cpu& c, u32 a) {
    u32 p = a & 0x1FFFFFFF;
    if (p < 0x800000 && !(p & 3)) [[likely]] {
        u32 v;
        std::memcpy(&v, c.m->ram.get() + (p & (kRamSize - 1)), 4);
        return v;
    }
    return c.m->read(a, 4);
}
inline u32 rd16(Cpu& c, u32 a) {
    u32 p = a & 0x1FFFFFFF;
    if (p < 0x800000 && !(p & 1)) [[likely]] {
        u16 v;
        std::memcpy(&v, c.m->ram.get() + (p & (kRamSize - 1)), 2);
        return v;
    }
    return c.m->read(a, 2);
}
inline u32 rd8(Cpu& c, u32 a) {
    u32 p = a & 0x1FFFFFFF;
    if (p < 0x800000) [[likely]] return c.m->ram[p & (kRamSize - 1)];
    return c.m->read(a, 1);
}
inline void wr32(Cpu& c, u32 a, u32 v) {
    u32 p = a & 0x1FFFFFFF;
    if (p < 0x800000 && !(p & 3) && !(c.cop0[12] & 0x10000)) [[likely]] {
        std::memcpy(c.m->ram.get() + (p & (kRamSize - 1)), &v, 4);
        return;
    }
    c.m->write(a, 4, v);
}
inline void wr16(Cpu& c, u32 a, u32 v) {
    u32 p = a & 0x1FFFFFFF;
    if (p < 0x800000 && !(p & 1) && !(c.cop0[12] & 0x10000)) [[likely]] {
        u16 h = static_cast<u16>(v);
        std::memcpy(c.m->ram.get() + (p & (kRamSize - 1)), &h, 2);
        return;
    }
    c.m->write(a, 2, v);
}
inline void wr8(Cpu& c, u32 a, u32 v) {
    u32 p = a & 0x1FFFFFFF;
    if (p < 0x800000 && !(c.cop0[12] & 0x10000)) [[likely]] {
        c.m->ram[p & (kRamSize - 1)] = static_cast<u8>(v);
        return;
    }
    c.m->write(a, 1, v);
}

inline u32 lwl(Cpu& c, u32 a, u32 old) {
    u32 w = rd32(c, a & ~3u);
    switch (a & 3) {
        case 0: return (old & 0x00FFFFFF) | (w << 24);
        case 1: return (old & 0x0000FFFF) | (w << 16);
        case 2: return (old & 0x000000FF) | (w << 8);
        default: return w;
    }
}
inline u32 lwr(Cpu& c, u32 a, u32 old) {
    u32 w = rd32(c, a & ~3u);
    switch (a & 3) {
        case 0: return w;
        case 1: return (old & 0xFF000000) | (w >> 8);
        case 2: return (old & 0xFFFF0000) | (w >> 16);
        default: return (old & 0xFFFFFF00) | (w >> 24);
    }
}
inline void swl(Cpu& c, u32 a, u32 v) {
    u32 w = rd32(c, a & ~3u);
    switch (a & 3) {
        case 0: w = (w & 0xFFFFFF00) | (v >> 24); break;
        case 1: w = (w & 0xFFFF0000) | (v >> 16); break;
        case 2: w = (w & 0xFF000000) | (v >> 8); break;
        default: w = v; break;
    }
    wr32(c, a & ~3u, w);
}
inline void swr(Cpu& c, u32 a, u32 v) {
    u32 w = rd32(c, a & ~3u);
    switch (a & 3) {
        case 0: w = v; break;
        case 1: w = (w & 0x000000FF) | (v << 8); break;
        case 2: w = (w & 0x0000FFFF) | (v << 16); break;
        default: w = (w & 0x00FFFFFF) | (v << 24); break;
    }
    wr32(c, a & ~3u, w);
}

inline void opMult(Cpu& c, u32 a, u32 b) {
    s64 t = static_cast<s64>(static_cast<s32>(a)) * static_cast<s64>(static_cast<s32>(b));
    c.lo = static_cast<u32>(t);
    c.hi = static_cast<u32>(static_cast<u64>(t) >> 32);
}
inline void opMultu(Cpu& c, u32 a, u32 b) {
    u64 t = static_cast<u64>(a) * b;
    c.lo = static_cast<u32>(t);
    c.hi = static_cast<u32>(t >> 32);
}
inline void opDiv(Cpu& c, u32 a, u32 b) {
    s32 n = static_cast<s32>(a), d = static_cast<s32>(b);
    if (d == 0) {
        c.lo = n >= 0 ? 0xFFFFFFFFu : 1u;
        c.hi = a;
    } else if (a == 0x80000000u && d == -1) {
        c.lo = 0x80000000u;
        c.hi = 0;
    } else {
        c.lo = static_cast<u32>(n / d);
        c.hi = static_cast<u32>(n % d);
    }
}
inline void opDivu(Cpu& c, u32 a, u32 b) {
    if (b == 0) {
        c.lo = 0xFFFFFFFFu;
        c.hi = a;
    } else {
        c.lo = a / b;
        c.hi = a % b;
    }
}

// Coprocessor and trap entry points (Cop.cpp / Gte.cpp)
u32 mfc0(Cpu& c, u32 reg);
void mtc0(Cpu& c, u32 reg, u32 value);
void rfe(Cpu& c);
void opSyscall(Cpu& c, u32 pc);
void opBreak(Cpu& c, u32 pc, u32 word);
u32 gteMfc2(Cpu& c, u32 reg);
u32 gteCfc2(Cpu& c, u32 reg);
void gteMtc2(Cpu& c, u32 reg, u32 value);
void gteCtc2(Cpu& c, u32 reg, u32 value);
void gteCommand(Cpu& c, u32 word);

}  // namespace fe
