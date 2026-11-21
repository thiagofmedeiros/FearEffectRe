#include "Spu.h"

#include <algorithm>

namespace fe {

namespace {
const s32 kFilter[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
s32 clamp16(s32 v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

// Volume register to a signed gain; sweep modes keep the previous gain.
void setVolume(s32& gain, u16 value) {
    if (!(value & 0x8000)) gain = static_cast<s16>(value << 1);
    else if (gain == 0) gain = 0x3FFF;
}
}  // namespace

Spu::Spu() : ram(0x80000, 0) {}

u16 Spu::read(u32 offset) const {
    offset &= 0x3FE;
    if (offset < 0x180 && (offset & 0xF) == 0xC) return static_cast<u16>(voices[offset >> 4].level);
    switch (offset) {
        case 0x19C: return static_cast<u16>(endx);
        case 0x19E: return static_cast<u16>(endx >> 16);
        case 0x1AE: return static_cast<u16>((regc(0x1AA) & 0x3F) | (irqFlag ? 0x40 : 0));
        default: return regc(offset);
    }
}

void Spu::write(Machine& m, u32 offset, u16 value) {
    (void)m;
    offset &= 0x3FE;
    reg(offset) = value;
    if (offset < 0x180) {
        Voice& v = voices[offset >> 4];
        switch (offset & 0xF) {
            case 0x0: setVolume(v.volL, value); break;
            case 0x2: setVolume(v.volR, value); break;
            case 0xE: v.repeat = static_cast<u32>(value) << 3; break;
            default: break;
        }
        return;
    }
    switch (offset) {
        case 0x180: setVolume(mainL, value); break;
        case 0x182: setVolume(mainR, value); break;
        case 0x188: case 0x18A: {  // key on
            u32 bits = static_cast<u32>(value) << (offset == 0x18A ? 16 : 0);
            for (int i = 0; i < 24; ++i) {
                if (!(bits & (1u << i))) continue;
                Voice& v = voices[i];
                v.addr = static_cast<u32>(regc(i * 16 + 6)) << 3;
                v.repeat = v.addr;
                v.pos = 28;
                v.counter = 0;
                v.hist1 = v.hist2 = 0;
                v.prev = v.cur = 0;
                v.level = 0;
                v.phase = 1;
                v.wait = 0;
                endx &= ~(1u << i);
                ++keyOns;
            }
            break;
        }
        case 0x18C: case 0x18E: {  // key off
            u32 bits = static_cast<u32>(value) << (offset == 0x18E ? 16 : 0);
            for (int i = 0; i < 24; ++i)
                if ((bits & (1u << i)) && voices[i].phase) {
                    voices[i].phase = 4;
                    voices[i].wait = 0;
                }
            break;
        }
        case 0x1A6: transferAddr = static_cast<u32>(value) << 3; break;
        case 0x1A8:
            ram[transferAddr & 0x7FFFE] = static_cast<u8>(value);
            ram[(transferAddr + 1) & 0x7FFFF] = static_cast<u8>(value >> 8);
            transferAddr = (transferAddr + 2) & 0x7FFFF;
            break;
        case 0x1AA:
            if (!(value & 0x40)) irqFlag = false;
            break;
        default: break;
    }
}

void Spu::dma(Machine& m, u32 addr, u32 words, bool toSpu) {
    for (u32 i = 0; i < words * 4; ++i) {
        u8& mem = m.ram[(addr + i) & (kRamSize - 1)];
        u8& snd = ram[transferAddr];
        if (toSpu) snd = mem;
        else mem = snd;
        transferAddr = (transferAddr + 1) & 0x7FFFF;
    }
}

void Spu::decodeBlock(Machine& m, int vi) {
    Voice& v = voices[vi];
    u32 a = v.addr & 0x7FFF0;
    // interrupt when the decoder reaches the address the game is watching
    u32 irqAddr = static_cast<u32>(regc(0x1A4)) << 3;
    if ((regc(0x1AA) & 0x40) && irqAddr >= a && irqAddr < a + 16 && !irqFlag) {
        irqFlag = true;
        m.raise(kIrqSpu);
    }
    const u8* b = &ram[a];
    int shift = b[0] & 15, filter = (b[0] >> 4) & 7;
    if (filter > 4) filter = 4;
    if (shift > 12) shift = 9;
    u8 flags = b[1];
    if (flags & 4) v.repeat = a;
    for (int i = 0; i < 28; ++i) {
        int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 15;
        s32 s = static_cast<s16>(nib << 12) >> shift;
        s += (v.hist1 * kFilter[filter][0] + v.hist2 * kFilter[filter][1] + 32) >> 6;
        s = clamp16(s);
        v.hist2 = v.hist1;
        v.hist1 = s;
        v.block[i] = static_cast<s16>(s);
    }
    v.pos = 0;
    v.addr = (a + 16) & 0x7FFFF;
    if (flags & 1) {
        endx |= 1u << vi;
        v.addr = v.repeat;
        if (!(flags & 2)) {
            v.phase = 0;  // end without repeat: voice stops after this block
            v.level = 0;
        }
    }
}

void Spu::envelope(int vi) {
    Voice& v = voices[vi];
    if (!v.phase) return;
    if (v.wait) {
        --v.wait;
        return;
    }
    u16 a1 = regc(vi * 16 + 8), a2 = regc(vi * 16 + 10);
    int shift = 0, step = 0;
    bool expo = false, decrease = false;
    switch (v.phase) {
        case 1: expo = a1 & 0x8000; shift = (a1 >> 10) & 31; step = 7 - ((a1 >> 8) & 3); break;
        case 2: expo = true; decrease = true; shift = (a1 >> 4) & 15; step = -8; break;
        case 3:
            expo = a2 & 0x8000;
            decrease = a2 & 0x4000;
            shift = (a2 >> 8) & 31;
            step = decrease ? -8 + ((a2 >> 6) & 3) : 7 - ((a2 >> 6) & 3);
            break;
        default: expo = a2 & 0x20; decrease = true; shift = a2 & 31; step = -8; break;
    }
    u32 cycles = 1u << std::max(0, shift - 11);
    s32 delta = step << std::max(0, 11 - shift);
    if (expo && !decrease && v.level > 0x6000) cycles *= 4;
    if (expo && decrease) delta = delta * v.level / 0x8000;
    v.wait = cycles - 1;
    v.level = std::clamp(v.level + delta, 0, 0x7FFF);

    if (v.phase == 1 && v.level >= 0x7FFF) {
        v.phase = 2;
        v.wait = 0;
    } else if (v.phase == 2 && v.level <= static_cast<s32>(((a1 & 15) + 1) * 0x800)) {
        v.phase = 3;
        v.wait = 0;
    } else if (v.phase == 4 && v.level <= 0) {
        v.phase = 0;
    }
}

void Spu::generate(Machine& m, size_t frames, std::vector<s16>& out) {
    bool enabled = regc(0x1AA) & 0x8000, unmuted = regc(0x1AA) & 0x4000;
    bool cdOn = regc(0x1AA) & 1;
    s32 cdL = static_cast<s16>(regc(0x1B0)), cdR = static_cast<s16>(regc(0x1B2));
    for (size_t f = 0; f < frames; ++f) {
        s32 l = 0, r = 0;
        if (enabled) {
            for (int i = 0; i < 24; ++i) {
                Voice& v = voices[i];
                if (!v.phase) continue;
                u32 pitch = std::min<u32>(regc(i * 16 + 4), 0x4000);
                v.counter += pitch;
                while (v.counter >= 0x1000 && v.phase) {
                    v.counter -= 0x1000;
                    if (v.pos >= 28) decodeBlock(m, i);
                    v.prev = v.cur;
                    v.cur = v.block[v.pos++];
                }
                if (!v.phase) continue;
                envelope(i);
                s32 frac = static_cast<s32>(v.counter & 0xFFF);
                s32 s = v.prev + (((v.cur - v.prev) * frac) >> 12);
                s = (s * v.level) >> 15;
                l += (s * v.volL) >> 15;
                r += (s * v.volR) >> 15;
            }
        }
        if (xa.size() >= 2) {
            s32 xl = xa[0], xr = xa[1];
            xa.pop_front();
            xa.pop_front();
            if (cdOn) {
                l += (xl * cdL) >> 15;
                r += (xr * cdR) >> 15;
            }
        }
        if (!unmuted) l = r = 0;
        l = (l * mainL) >> 15;
        r = (r * mainR) >> 15;
        out.push_back(static_cast<s16>(clamp16(l)));
        out.push_back(static_cast<s16>(clamp16(r)));
    }
}

void Spu::xaSector(const u8* raw) {
    u8 coding = raw[19];
    bool stereo = coding & 1, halfRate = coding & 4;
    if (coding & 0x10) return;  // 8-bit samples: not used by this game's streams
    std::vector<s16> pcm[2];
    for (int g = 0; g < 18; ++g) {
        const u8* grp = raw + 24 + g * 128;
        for (int blk = 0; blk < 8; ++blk) {
            int ch = stereo ? (blk & 1) : 0;
            int shift = grp[4 + blk] & 15, filter = (grp[4 + blk] >> 4) & 3;
            if (shift > 12) shift = 9;
            for (int j = 0; j < 28; ++j) {
                int nib = (grp[16 + j * 4 + blk / 2] >> ((blk & 1) * 4)) & 15;
                s32 s = static_cast<s16>(nib << 12) >> shift;
                s += (xaHist[ch][0] * kFilter[filter][0] + xaHist[ch][1] * kFilter[filter][1] + 32) >> 6;
                s = clamp16(s);
                xaHist[ch][1] = xaHist[ch][0];
                xaHist[ch][0] = s;
                pcm[ch].push_back(static_cast<s16>(s));
            }
        }
    }
    if (!stereo) pcm[1] = pcm[0];
    // resample to 44100 Hz with linear interpolation
    u32 step = (halfRate ? 18900u : 37800u) * 4096 / 44100;
    size_t n = pcm[0].size();
    if (xa.size() > 44100 * 2 * 2) xa.clear();  // nobody is consuming
    for (;;) {
        size_t idx = xaPhase >> 12;
        if (idx >= n) break;
        s32 frac = static_cast<s32>(xaPhase & 0xFFF);
        for (int ch = 0; ch < 2; ++ch) {
            s32 a = idx ? pcm[ch][idx - 1] : xaLast[ch], b = pcm[ch][idx];
            xa.push_back(static_cast<s16>(a + (((b - a) * frac) >> 12)));
        }
        xaPhase += step;
    }
    xaPhase -= static_cast<u32>(n) << 12;
    xaLast[0] = pcm[0][n - 1];
    xaLast[1] = pcm[1][n - 1];
}

}  // namespace fe
