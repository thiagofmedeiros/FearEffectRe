#include "Mdec.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fe {

namespace {
const u8 kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                        41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                        30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

int sext10(u32 v) { return static_cast<int>(static_cast<s32>(v << 22) >> 22); }

// The IDCT as the hardware does it, with the scale table the game uploaded.
void idct(s16* blk, const s16* scale) {
    s32 a[64], b[64];
    for (int i = 0; i < 64; ++i) a[i] = blk[i];
    s32* src = a;
    s32* dst = b;
    for (int pass = 0; pass < 2; ++pass) {
        for (int x = 0; x < 8; ++x)
            for (int y = 0; y < 8; ++y) {
                s32 sum = 0;
                for (int z = 0; z < 8; ++z) sum += src[y + z * 8] * (scale[x + z * 8] / 8);
                dst[x + y * 8] = (sum + 0xFFF) / 0x2000;
            }
        std::swap(src, dst);
    }
    for (int i = 0; i < 64; ++i) blk[i] = static_cast<s16>(src[i]);
}

int clampS8(int v) { return v < -128 ? -128 : v > 127 ? 127 : v; }
}  // namespace

bool Mdec::block(s16* blk, const u8* qt) {
    std::fill(blk, blk + 64, 0);
    while (pos < codeCount() && code(pos) == 0xFE00) ++pos;
    if (pos >= codeCount()) return false;
    u32 n = code(pos++);
    int q = static_cast<int>(n >> 10);
    int val = sext10(n) * qt[0];
    int k = 0;
    for (;;) {
        if (q == 0) val = sext10(n) * 2;
        val = std::clamp(val, -0x400, 0x3FF);
        if (q > 0) blk[kZigzag[k]] = static_cast<s16>(val);
        else blk[k] = static_cast<s16>(val);
        if (pos >= codeCount()) break;
        n = code(pos++);
        k += static_cast<int>(n >> 10) + 1;
        if (k > 63) break;
        val = (sext10(n) * qt[k] * q + 4) / 8;
    }
    idct(blk, scale);
    return true;
}

u16 Mdec::code(size_t i) const {
    if (!lazy) return in[i];
    u16 v;
    std::memcpy(&v, lazy + i * 2, 2);
    return v;
}

void Mdec::decode(size_t wantBytes) {
    u32 depth = (command >> 27) & 3;
    bool isSigned = command & (1u << 26), bit15 = command & (1u << 25);
    s16 cr[64], cb[64], yb[64];
    if (depth < 2) {
        while (out.size() - outPos < wantBytes && block(yb, qY)) {
            ++macroblocks;
            for (int i = 0; i < 64; ++i) {
                int v = clampS8(yb[i]);
                out.push_back(static_cast<u8>(isSigned ? v : v + 128));
            }
        }
        return;
    }
    while (out.size() - outPos < wantBytes) {
        if (!block(cr, qC) || !block(cb, qC)) break;
        u8 rgb[16 * 16][3];
        bool ok = true;
        for (int b = 0; b < 4 && ok; ++b) {
            ok = block(yb, qY);
            if (!ok) break;
            int bx = (b & 1) * 8, by = (b >> 1) * 8;
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    int ci = ((y + by) / 2) * 8 + (x + bx) / 2;
                    int Y = yb[y * 8 + x];
                    int R = static_cast<int>(std::lround(1.402f * cr[ci]));
                    int G = static_cast<int>(std::lround(-0.3437f * cb[ci] - 0.7143f * cr[ci]));
                    int B = static_cast<int>(std::lround(1.772f * cb[ci]));
                    u8* p = rgb[(y + by) * 16 + x + bx];
                    int add = isSigned ? 0 : 128;
                    p[0] = static_cast<u8>(clampS8(Y + R) + add);
                    p[1] = static_cast<u8>(clampS8(Y + G) + add);
                    p[2] = static_cast<u8>(clampS8(Y + B) + add);
                }
        }
        if (!ok) break;
        ++macroblocks;
        for (auto& p : rgb) {
            if (depth == 3) {
                u16 v = static_cast<u16>((p[0] >> 3) | ((p[1] >> 3) << 5) | ((p[2] >> 3) << 10) | (bit15 ? 0x8000 : 0));
                out.push_back(static_cast<u8>(v));
                out.push_back(static_cast<u8>(v >> 8));
            } else {
                out.push_back(p[0]);
                out.push_back(p[1]);
                out.push_back(p[2]);
            }
        }
    }
}

void Mdec::writeCommand(u32 word) {
    if (need == 0) {
        if (trace.size() < 40) {
            char t[64];
            std::snprintf(t, sizeof t, "mdec command %08x", word);
            trace.push_back(t);
        }
        command = word;
        in.clear();
        lazy = nullptr;
        pos = 0;
        switch (word >> 29) {
            case 1: need = word & 0xFFFF; break;
            case 2: need = (word & 1) ? 32 : 16; break;
            case 3: need = 32; break;
            default: break;
        }
        if ((word >> 29) == 1) {
            // a new decode discards output the game did not collect
            out.clear();
            outPos = 0;
        }
        return;
    }
    in.push_back(static_cast<u16>(word));
    in.push_back(static_cast<u16>(word >> 16));
    if (--need) return;
    switch (command >> 29) {
        case 1: pos = 0; break;  // decoded when the output is read
        case 2:
            for (int i = 0; i < 64; ++i) qY[i] = static_cast<u8>(in[i / 2] >> ((i & 1) * 8));
            if (command & 1)
                for (int i = 0; i < 64; ++i) qC[i] = static_cast<u8>(in[32 + i / 2] >> ((i & 1) * 8));
            break;
        case 3: {
            for (int i = 0; i < 64; ++i) scale[i] = static_cast<s16>(in[i]);
            char t[96];
            std::snprintf(t, sizeof t, "mdec scale table %04x %04x %04x %04x ... %04x", in[0], in[1], in[2], in[3], in[9]);
            trace.push_back(t);
            break;
        }
        default: break;
    }
}

void Mdec::writeControl(u32 word) {
    if (word & 0x80000000u) {
        command = 0;
        need = 0;
        in.clear();
        out.clear();
        outPos = 0;
    }
    control = word;
}

u32 Mdec::readStatus() const {
    u32 s = 0;
    if (outPos >= out.size()) s |= 0x80000000u;        // output FIFO empty
    if (need) s |= 1u << 29;                           // busy receiving parameters
    if (control & (1u << 30)) s |= 1u << 28;           // data-in request
    if ((control & (1u << 29)) && outPos < out.size()) s |= 1u << 27;  // data-out request
    s |= ((command >> 25) & 0xF) << 23;
    s |= (need - 1) & 0xFFFF;
    return s;
}

u32 Mdec::readData() {
    if ((command >> 29) == 1 && need == 0 && outPos + 4 > out.size()) decode(4);
    u32 v = 0;
    for (int i = 0; i < 4; ++i)
        if (outPos < out.size()) v |= static_cast<u32>(out[outPos++]) << (8 * i);
    return v;
}

void Mdec::dmaIn(Machine& m, u32 addr, u32 words) {
    if (lazyInput && (command >> 29) == 1 && need >= words && !lazy && in.empty()) {
        lazy = m.ram.get() + (addr & (kRamSize - 4));
        lazyCodes = static_cast<size_t>(words) * 2;
        need -= words;
        pos = 0;
        return;
    }
    for (u32 i = 0; i < words; ++i) {
        u32 w;
        std::memcpy(&w, m.ram.get() + ((addr + i * 4) & (kRamSize - 4)), 4);
        writeCommand(w);
    }
}

void Mdec::dmaOut(Machine& m, u32 addr, u32 words) {
    if ((command >> 29) == 1 && need == 0) decode(static_cast<size_t>(words) * 4);
    if (trace.size() < 12) {
        char t[96];
        std::snprintf(t, sizeof t, "mdec dma out %u words to %06x, %zu bytes available, code %zu/%zu", words, addr, out.size() - outPos, pos, codeCount());
        trace.push_back(t);
    }
    for (u32 i = 0; i < words; ++i) {
        u32 w = 0;
        for (int k = 0; k < 4; ++k)
            if (outPos < out.size()) w |= static_cast<u32>(out[outPos++]) << (8 * k);
        std::memcpy(m.ram.get() + ((addr + i * 4) & (kRamSize - 4)), &w, 4);
    }
    if (outPos >= out.size()) {
        out.clear();
        outPos = 0;
    }
}

}  // namespace fe
