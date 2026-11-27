// Memory-mapped hardware at 1F801000h. Only what the boot path needs so far:
// interrupt controller, DMA, root counters and status registers. Devices
// that are not implemented yet keep a shadow of what was written and are
// counted in `ioAccess` so the boot report shows what the game touches.
#include <cstdio>

#include "Cdrom.h"
#include "Gpu.h"
#include "Mdec.h"
#include "Spu.h"
#include "Machine.h"

namespace fe {

namespace {
u32 timerValue(const Machine& m, int n) {
    const auto& t = m.timers[n];
    u64 ticks = m.clock - t.base;
    if (n == 1 && (t.mode & 0x100)) ticks = ticks * 15734 / kCpuHz;  // hblank clock
    if (n == 2 && (t.mode & 0x200)) ticks /= 8;
    return static_cast<u32>(ticks & 0xFFFF);
}
}  // namespace

u32 Machine::ioRead(u32 p, int size) {
    if (sioTrace > 0 && p >= 0x1F801040 && p < 0x1F801050) {
        --sioTrace;
        u32 v = ioReadInner(p, size);
        char t[80];
        std::snprintf(t, sizeof t, "sio R %04x = %04x  frame %llu istat %03x", p & 0xFFFF, v, static_cast<unsigned long long>(frame), iStat);
        log.push_back(t);
        return v;
    }
    return ioReadInner(p, size);
}

u32 Machine::ioReadInner(u32 p, int size) {
    if (p >= 0x1F801C00 && p < 0x1F802000) {
        u32 off = p - 0x1F801C00;
        u32 v = spu->read(off);
        if (size == 4) v |= static_cast<u32>(spu->read(off + 2)) << 16;
        if (size == 1) v = (v >> ((p & 1) * 8)) & 0xFF;
        return v;
    }
    if (sio.pending && ((p >= 0x1F801040 && p < 0x1F801050) || (p & ~3u) == 0x1F801070)) sioUpdate();
    u32 off = p - 0x1F801000;
    ++ioAccess[off >> 2];
    if (cd && p >= 0x1F801800 && p < 0x1F801804) {
        u32 v = 0;
        for (int i = 0; i < size; ++i) v |= static_cast<u32>(cd->read(*this, (p & 3) + (p == 0x1F801802 ? 0 : static_cast<u32>(i)))) << (8 * i);
        return v;
    }
    u32 reg = p & ~3u;
    u32 shift = (p & 3) * 8;
    u32 value;
    // bring the visible clock up to date for registers that depend on time
    u64 now = clock + static_cast<u64>(sliceLen - cpu.cycles);

    if (reg == 0x1F801070) value = iStat;
    else if (reg == 0x1F801074) value = iMask;
    else if (reg >= 0x1F801080 && reg < 0x1F8010F0) value = dma[(reg - 0x1F801080) >> 4][((reg & 0xF) >> 2) % 3];
    else if (reg == 0x1F8010F0) value = dpcr;
    else if (reg == 0x1F8010F4) {
        value = dicr & 0x7FFFFFFF;
        if ((dicr & 0x8000) || ((dicr & 0x800000) && (dicr & (dicr >> 8) & 0x7F0000))) value |= 0x80000000;
    } else if (reg >= 0x1F801100 && reg < 0x1F801130) {
        int n = (reg - 0x1F801100) >> 4;
        u64 saved = clock;
        clock = now;
        switch (reg & 0xF) {
            case 0: value = timerValue(*this, n); break;
            case 4: value = timers[n].mode; break;
            default: value = timers[n].target; break;
        }
        clock = saved;
    } else if (reg == 0x1F801810) value = gpu->read();
    else if (reg == 0x1F801820) value = mdec->readData();
    else if (reg == 0x1F801824) value = mdec->readStatus();
    else if (reg == 0x1F801814) {
        // ready for commands/DMA, display enabled; bit 31 toggles per field
        value = gpu->status(*this);
    } else if (p == 0x1F801040) {
        sio.rxFull = false;
        return sio.rx;
    } else if (reg == 0x1F801044) {
        value = 0x5 | (sio.rxFull ? 0x2 : 0) | (sio.irq ? 0x200 : 0);  // JOY_STAT
    } else if (p == 0x1F801048) return sio.mode;
    else if (p == 0x1F80104A) return sio.ctrl;
    else if (p == 0x1F80104E) return sio.baud;
    else {
        u32 v = 0;
        std::memcpy(&v, ioShadow.data() + off, static_cast<size_t>(size));
        return v;
    }
    value >>= shift;
    return size == 1 ? value & 0xFF : size == 2 ? value & 0xFFFF : value;
}

void Machine::ioWrite(u32 p, int size, u32 value) {
    if (sioTrace > 0 && p >= 0x1F801040 && p < 0x1F801050) {
        --sioTrace;
        char t[80];
        std::snprintf(t, sizeof t, "sio W %04x = %04x  frame %llu istat %03x", p & 0xFFFF, value, static_cast<unsigned long long>(frame), iStat);
        log.push_back(t);
    }
    u32 off = p - 0x1F801000;
    ++ioAccess[off >> 2];
    std::memcpy(ioShadow.data() + off, &value, static_cast<size_t>(size));
    if (p >= 0x1F801C00 && p < 0x1F802000) {
        u32 so = p - 0x1F801C00;
        if (size == 1) {
            u16 cur = spu->read(so);
            value = (p & 1) ? (cur & 0x00FF) | (value << 8) : (cur & 0xFF00) | (value & 0xFF);
        }
        spu->write(*this, so, static_cast<u16>(value));
        if (size == 4) spu->write(*this, so + 2, static_cast<u16>(value >> 16));
        return;
    }
    if (cd && p >= 0x1F801800 && p < 0x1F801804) {
        cd->write(*this, p & 3, static_cast<u8>(value));
        return;
    }
    u32 reg = p & ~3u;

    // Byte and halfword writes to the 32-bit DMA registers change only the
    // bytes they address.
    if (reg >= 0x1F801080 && reg < 0x1F8010F8 && (size < 4 || (p & 3))) {
        u32 shift = (p & 3) * 8;
        u32 mask = (size == 4 ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1)) << shift;
        u32 cur = reg == 0x1F8010F0 ? dpcr
                  : reg == 0x1F8010F4 ? (dicr & 0x00FFFFFF)  // flag bits are write-one-to-clear
                  : dma[(reg - 0x1F801080) >> 4][((reg & 0xF) >> 2) % 3];
        value = (cur & ~mask) | ((value << shift) & mask);
    }

    if (reg == 0x1F801070) {
        iStat &= value;
    } else if (reg == 0x1F801074) {
        iMask = value & 0x7FF;
        requestSlice(cpu);
    } else if (reg >= 0x1F801080 && reg < 0x1F8010F0) {
        int ch = (reg - 0x1F801080) >> 4;
        int idx = ((reg & 0xF) >> 2) % 3;
        dma[ch][idx] = value;
        if (idx == 2 && (value & 0x01000000) && (dpcr & (8u << (ch * 4)))) runDma(ch);
    } else if (reg == 0x1F8010F0) {
        dpcr = value;
    } else if (reg == 0x1F8010F4) {
        u32 ack = value & 0x7F000000;
        dicr = (value & 0x00FF803F) | ((dicr & 0x7F000000) & ~ack);
    } else if (reg >= 0x1F801100 && reg < 0x1F801130) {
        int n = (reg - 0x1F801100) >> 4;
        u64 now = clock + static_cast<u64>(sliceLen - cpu.cycles);
        switch (reg & 0xF) {
            case 0: timers[n].base = now; break;
            case 4:
                timers[n].mode = value & 0x3FF;
                timers[n].base = now;
                if (value & 0x30) note("timer " + std::to_string(n) + " interrupt requested (not implemented)");
                break;
            default: timers[n].target = value & 0xFFFF; break;
        }
    } else if (p == 0x1F801040) {
        sioTransmit(static_cast<u8>(value));
    } else if (p == 0x1F801048) {
        sio.mode = static_cast<u16>(value);
    } else if (p == 0x1F80104A) {
        sio.ctrl = static_cast<u16>(value) & ~0x10;
        if (value & 0x10) sio.irq = false;
        if (value & 0x40) sio = Sio{};
        if (!(value & 0x02)) sio.step = 0;  // device deselected
    } else if (p == 0x1F80104E) {
        sio.baud = static_cast<u16>(value);
    } else if (reg == 0x1F801820) {
        mdec->writeCommand(value);
    } else if (reg == 0x1F801824) {
        mdec->writeControl(value);
    } else if (reg == 0x1F801810) {
        ++gp0Words;
        gpu->gp0(value);
    } else if (reg == 0x1F801814) {
        ++gp1Words;
        gpu->gp1(value);
        if ((value >> 24) == 0x10) gpuRead = (value & 7) == 7 ? 2 : 0;
    }
}

void Machine::runDma(int ch) {
    u32 addr = dma[ch][0] & 0x1FFFFC;
    u32 bcr = dma[ch][1];
    u32 chcr = dma[ch][2];
    switch (ch) {
        case 6: {  // ordering table clear: reverse linked list
            u32 n = bcr & 0xFFFF;
            if (!n) n = 0x10000;
            for (u32 i = 0; i < n; ++i) {
                u32 v = i == n - 1 ? 0x00FFFFFF : (addr - 4) & 0x1FFFFC;
                std::memcpy(ram.get() + addr, &v, 4);
                addr = (addr - 4) & 0x1FFFFC;
            }
            break;
        }
        case 0: case 1: {  // MDEC in, MDEC out
            u32 words = bcr & 0xFFFF;
            if (!words) words = 0x10000;
            if (((chcr >> 9) & 3) == 1) words *= (bcr >> 16) ? (bcr >> 16) : 1;
            if (ch == 0) mdec->dmaIn(*this, addr, words);
            else mdec->dmaOut(*this, addr, words);
            break;
        }
        case 2:  // GPU
            if (((chcr >> 9) & 3) == 2) {
                for (u32 guard = 0; guard < 0x100000; ++guard) {
                    u32 head;
                    std::memcpy(&head, ram.get() + addr, 4);
                    u32 n = head >> 24;
                    gp0Words += n;
                    for (u32 i = 1; i <= n; ++i) {
                        const u32 at = (addr + 4 * i) & 0x1FFFFC;
                        u32 w;
                        std::memcpy(&w, ram.get() + at, 4);
                        if (const PreciseWord* p = pgxp ? preciseAt(at, w) : nullptr) {
                            ++pgxpUsed;
                            gpu->gp0Precise(w, p->x, p->y, p->z);
                        } else {
                            gpu->gp0(w);
                        }
                    }
                    if (head & 0x800000) break;
                    addr = head & 0x1FFFFC;
                }
            } else {
                u32 words = (bcr & 0xFFFF) * ((bcr >> 16) ? (bcr >> 16) : 1);
                gp0Words += words;
                for (u32 i = 0; i < words; ++i) {
                    u32 at = (addr + 4 * i) & 0x1FFFFC;
                    u32 w;
                    if (chcr & 1) {
                        std::memcpy(&w, ram.get() + at, 4);
                        gpu->gp0(w);
                    } else {
                        w = gpu->read();
                        std::memcpy(ram.get() + at, &w, 4);
                    }
                }
            }
            break;
        case 4: {  // SPU sound RAM
            u32 words = (bcr & 0xFFFF) * ((bcr >> 16) ? (bcr >> 16) : 1);
            spu->dma(*this, addr, words, chcr & 1);
            break;
        }
        case 3: {  // CD-ROM data FIFO to RAM
            u32 words = bcr & 0xFFFF;
            if (!words) words = 0x10000;
            if (((chcr >> 9) & 3) == 1) words *= (bcr >> 16) ? (bcr >> 16) : 1;
            if (cd) cd->dmaRead(*this, addr, words * 4);
            break;
        }
        default:
            note("dma channel " + std::to_string(ch) + " started (device not implemented)");
            break;
    }
    dma[ch][2] &= ~0x11000000u;
    if (dicr & (0x10000u << ch)) {
        dicr |= 0x01000000u << ch;
        if (dicr & 0x800000) raise(kIrqDma);
    }
}

void Machine::sioTransmit(u8 value) {
    u8 rx = 0xFF;
    bool port1 = !(sio.ctrl & 0x2000);
    bool ack = false;
    if (port1 && (sio.ctrl & 0x02) && sio.step >= 0) {
        switch (sio.step) {
            case 0: ack = value == 0x01; break;                   // pad access; 81h = memory card
            case 1: rx = padAnalog ? 0x73 : 0x41; ack = value == 0x42; break;  // pad id
            case 2: rx = 0x5A; ack = true; break;
            case 3: rx = padState & 0xFF; ack = true; break;
            case 4: rx = padState >> 8; ack = padAnalog; break;   // a digital pad's last byte: no acknowledge
            case 5: case 6: case 7: rx = padAxes[sio.step - 5]; ack = true; break;
            case 8: rx = padAxes[3]; break;
            default: break;
        }
        sio.step = ack ? sio.step + 1 : -1;
    }
    sio.pending = true;
    sio.pendingAck = ack;
    sio.pendingRx = rx;
    sio.at = now() + (sio.baud ? sio.baud * 8u : 1088u) + 100;
}

void Machine::sioUpdate() {
    if (!sio.pending || now() < sio.at) return;
    sio.pending = false;
    sio.rx = sio.pendingRx;
    sio.rxFull = true;
    if (sio.pendingAck) {
        sio.irq = true;
        raise(kIrqSio);
    }
}

}  // namespace fe
