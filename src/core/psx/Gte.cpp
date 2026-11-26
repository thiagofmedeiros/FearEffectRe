// Geometry transformation engine (COP2): register access rules and all
// commands, including the flag register and the hardware division.
#include <algorithm>
#include <cmath>

#include "Machine.h"

namespace fe {

namespace {

struct Gte {
    Cpu& c;
    u32* d;   // data registers
    u32* k;   // control registers
    u32 flag = 0;
    int sf = 0;
    bool lm = false;

    explicit Gte(Cpu& cpu) : c(cpu), d(cpu.gteData), k(cpu.gteCtrl) {}

    static s64 sext44(s64 v) { return static_cast<s64>(static_cast<u64>(v) << 20) >> 20; }
    s32 D(int i) const { return static_cast<s32>(d[i]); }
    s32 K(int i) const { return static_cast<s32>(k[i]); }
    s16 lo16(u32 v) const { return static_cast<s16>(v); }
    s16 hi16(u32 v) const { return static_cast<s16>(v >> 16); }

    // matrix m (0 rotation, 1 light, 2 colour), row r, column col
    s32 M(int m, int r, int col) const {
        static const int base[3] = {0, 8, 16};
        int idx = r * 3 + col;
        u32 w = k[base[m] + idx / 2];
        return (idx & 1) ? hi16(w) : lo16(w);
    }
    s32 V(int v, int comp) const {
        if (v == 3) return static_cast<s16>(d[9 + comp]);
        if (comp == 2) return static_cast<s16>(d[v * 2 + 1]);
        return comp ? hi16(d[v * 2]) : lo16(d[v * 2]);
    }

    s64 A(int i, s64 v) {  // 44-bit accumulator overflow of MAC1..3
        if (v > 0x7FFFFFFFFFFLL) flag |= 1u << (31 - i);
        else if (v < -0x80000000000LL) flag |= 1u << (28 - i);
        return sext44(v);
    }
    s64 F(s64 v) {  // 32-bit overflow of MAC0
        if (v > 0x7FFFFFFFLL) flag |= 1u << 16;
        else if (v < -0x80000000LL) flag |= 1u << 15;
        return v;
    }
    s32 limB(int i, s32 v, bool lmFlag) {
        s32 lowest = lmFlag ? 0 : -0x8000;
        if (v > 0x7FFF) { flag |= 1u << (25 - i); return 0x7FFF; }
        if (v < lowest) { flag |= 1u << (25 - i); return lowest; }
        return v;
    }
    u32 limC(int i, s32 v) {
        if (v > 255) { flag |= 1u << (22 - i); return 255; }
        if (v < 0) { flag |= 1u << (22 - i); return 0; }
        return static_cast<u32>(v);
    }
    u32 limD(s64 v) {
        if (v > 0xFFFF) { flag |= 1u << 18; return 0xFFFF; }
        if (v < 0) { flag |= 1u << 18; return 0; }
        return static_cast<u32>(v);
    }
    s32 limG(int i, s64 v) {
        if (v > 0x3FF) { flag |= 1u << (15 - i); return 0x3FF; }
        if (v < -0x400) { flag |= 1u << (15 - i); return -0x400; }
        return static_cast<s32>(v);
    }
    s32 limH(s64 v) {
        if (v > 0x1000) { flag |= 1u << 12; return 0x1000; }
        if (v < 0) { flag |= 1u << 12; return 0; }
        return static_cast<s32>(v);
    }

    void setMac(int i, s64 v) { d[24 + i] = static_cast<u32>(static_cast<s32>(v >> (i ? sf : 0))); }
    void setIr(int i, s32 v, bool lmFlag) { d[8 + i] = static_cast<u32>(limB(i, v, lmFlag)); }
    void macToIr() {
        for (int i = 1; i <= 3; ++i) setIr(i, D(24 + i), lm);
    }
    void setMacIr(s64 m1, s64 m2, s64 m3) {
        setMac(1, m1);
        setMac(2, m2);
        setMac(3, m3);
        macToIr();
    }
    void pushColour() {
        d[20] = d[21];
        d[21] = d[22];
        d[22] = limC(1, D(25) >> 4) | (limC(2, D(26) >> 4) << 8) | (limC(3, D(27) >> 4) << 16) | (d[6] & 0xFF000000);
    }
    void pushSxy(s32 x, s32 y) {
        d[12] = d[13];
        d[13] = d[14];
        d[14] = (static_cast<u32>(x) & 0xFFFF) | (static_cast<u32>(y) << 16);
        if (Machine* m = c.m; m && m->pgxp) {
            for (int i = 0; i < 2; ++i) {
                m->pgxpSxy[i][0] = m->pgxpSxy[i + 1][0];
                m->pgxpSxy[i][1] = m->pgxpSxy[i + 1][1];
                m->pgxpSxyValid[i] = m->pgxpSxyValid[i + 1];
            }
            m->pgxpSxyValid[2] = false;
        }
    }

    // row i of (T<<12) + M * vec, with the overflow check after every addition
    s64 row(int m, int i, const s32* t, s32 x, s32 y, s32 z) {
        s64 acc = t ? (static_cast<s64>(t[i]) << 12) : 0;
        acc = A(i + 1, acc + static_cast<s64>(M(m, i, 0)) * x);
        acc = A(i + 1, acc + static_cast<s64>(M(m, i, 1)) * y);
        acc = A(i + 1, acc + static_cast<s64>(M(m, i, 2)) * z);
        return acc;
    }

    u32 divide(u32 h, u32 sz3) {
        static u8 table[257];
        static bool ready = false;
        if (!ready) {
            for (int i = 0; i < 257; ++i) table[i] = static_cast<u8>(std::max(0, (0x40000 / (i + 0x100) + 1) / 2 - 0x101));
            ready = true;
        }
        if (h >= sz3 * 2) {
            flag |= 1u << 17;
            return 0x1FFFF;
        }
        int z = 0;
        while (z < 16 && !((sz3 << z) & 0x8000)) ++z;
        u32 n = h << z, dv = sz3 << z;
        u32 u = table[(dv - 0x7FC0) >> 7] + 0x101u;
        s32 t = static_cast<s32>(0x2000080 - dv * u) >> 8;
        t = static_cast<s32>(0x0000080 + static_cast<u32>(t) * u) >> 8;
        u64 q = (static_cast<u64>(n) * static_cast<u32>(t) + 0x8000) >> 16;
        return static_cast<u32>(std::min<u64>(q, 0x1FFFF));
    }

    void rtps(int v, bool last) {
        const s32 tr[3] = {K(5), K(6), K(7)};
        s64 m1 = row(0, 0, tr, V(v, 0), V(v, 1), V(v, 2));
        s64 m2 = row(0, 1, tr, V(v, 0), V(v, 1), V(v, 2));
        s64 m3 = row(0, 2, tr, V(v, 0), V(v, 1), V(v, 2));
        setMac(1, m1);
        setMac(2, m2);
        setMac(3, m3);
        setIr(1, D(25), lm);
        setIr(2, D(26), lm);
        // IR3 saturates on the shifted value but flags on the 12-bit shifted one
        s32 zs = static_cast<s32>(m3 >> 12);
        if (zs > 0x7FFF || zs < -0x8000) flag |= 1u << 22;
        d[11] = static_cast<u32>(std::clamp(D(27), lm ? 0 : -0x8000, 0x7FFF));
        d[16] = d[17];
        d[17] = d[18];
        d[18] = d[19];
        d[19] = limD(zs);
        const u32 flagBefore = flag;
        s64 n = divide(k[26] & 0xFFFF, d[19]);
        s32 sx = limG(1, F(n * static_cast<s16>(d[9]) + K(24)) >> 16);
        s32 sy = limG(2, F(n * static_cast<s16>(d[10]) + K(25)) >> 16);
        pushSxy(sx, sy);
        if (Machine* m = c.m; m && m->pgxp) {
            // The same projection without rounding: the transformed point
            // before it is cut to whole units, divided exactly. Used only where
            // the hardware's result is not clipped or saturated and the two
            // agree to the pixel, so the picture never moves by more than
            // what was rounded away.
            const double z = static_cast<double>(m3) / 4096.0;
            const bool clipped = (flag & ~flagBefore & ((1u << 17) | (1u << 14) | (1u << 13))) != 0;
            const s32 ir1 = static_cast<s16>(d[9]), ir2 = static_cast<s16>(d[10]);
            const bool saturated = ir1 == 0x7FFF || ir1 == -0x8000 || ir2 == 0x7FFF || ir2 == -0x8000;
            if (z >= 1.0 && !clipped && !saturated) {
                const double h = static_cast<double>(k[26] & 0xFFFF);
                const double fx = K(24) / 65536.0 + (static_cast<double>(m1) / 4096.0) * h / z;
                const double fy = K(25) / 65536.0 + (static_cast<double>(m2) / 4096.0) * h / z;
                if (std::abs(fx - (sx + 0.5)) < 1.0 && std::abs(fy - (sy + 0.5)) < 1.0) {
                    m->pgxpSxy[2][0] = static_cast<float>(fx);
                    m->pgxpSxy[2][1] = static_cast<float>(fy);
                    m->pgxpSxyValid[2] = true;
                }
            }
        }
        if (last) {
            s64 mac0 = F(n * static_cast<s16>(k[27]) + K(28));
            d[24] = static_cast<u32>(mac0);
            d[8] = static_cast<u32>(limH(mac0 >> 12));
        }
    }

    // [MAC] = ([FC] << 12 - [in]) >> sf, saturate, then [in] + IR0 * that
    void interpolate(s64 in1, s64 in2, s64 in3) {
        s64 in[3] = {in1, in2, in3};
        s32 ir0 = static_cast<s16>(d[8]);
        s32 tmp[3];
        for (int i = 0; i < 3; ++i) {
            s64 v = A(i + 1, (static_cast<s64>(K(21 + i)) << 12) - in[i]) >> sf;
            tmp[i] = limB(i + 1, static_cast<s32>(v), false);
        }
        for (int i = 0; i < 3; ++i) setMac(i + 1, A(i + 1, static_cast<s64>(tmp[i]) * ir0 + in[i]));
        macToIr();
    }

    void lightNormal(int v) {
        setMacIr(row(1, 0, nullptr, V(v, 0), V(v, 1), V(v, 2)), row(1, 1, nullptr, V(v, 0), V(v, 1), V(v, 2)),
                 row(1, 2, nullptr, V(v, 0), V(v, 1), V(v, 2)));
    }
    void lightColour() {
        const s32 bk[3] = {K(13), K(14), K(15)};
        s32 x = static_cast<s16>(d[9]), y = static_cast<s16>(d[10]), z = static_cast<s16>(d[11]);
        setMacIr(row(2, 0, bk, x, y, z), row(2, 1, bk, x, y, z), row(2, 2, bk, x, y, z));
    }
    s64 rgbIr(int i) const {  // (colour component * IR) << 4
        s64 col = (d[6] >> (8 * i)) & 0xFF;
        return (col * static_cast<s16>(d[9 + i])) << 4;
    }

    void ncs(int v) { lightNormal(v); lightColour(); pushColour(); }
    void nccs(int v) {
        lightNormal(v);
        lightColour();
        setMacIr(rgbIr(0), rgbIr(1), rgbIr(2));
        pushColour();
    }
    void ncds(int v) {
        lightNormal(v);
        lightColour();
        interpolate(rgbIr(0), rgbIr(1), rgbIr(2));
        pushColour();
    }

    void run(u32 w) {
        sf = (w & (1u << 19)) ? 12 : 0;
        lm = w & (1u << 10);
        flag = 0;
        switch (w & 0x3F) {
            case 0x01: rtps(0, true); break;
            case 0x06: {  // NCLIP
                s64 x0 = lo16(d[12]), y0 = hi16(d[12]), x1 = lo16(d[13]), y1 = hi16(d[13]), x2 = lo16(d[14]), y2 = hi16(d[14]);
                d[24] = static_cast<u32>(F(x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1));
                break;
            }
            case 0x0C: {  // OP
                s64 d1 = lo16(k[0]), d2 = lo16(k[2]), d3 = lo16(k[4]);
                s64 i1 = static_cast<s16>(d[9]), i2 = static_cast<s16>(d[10]), i3 = static_cast<s16>(d[11]);
                setMacIr(i3 * d2 - i2 * d3, i1 * d3 - i3 * d1, i2 * d1 - i1 * d2);
                break;
            }
            case 0x10:  // DPCS
                interpolate(static_cast<s64>(d[6] & 0xFF) << 16, static_cast<s64>((d[6] >> 8) & 0xFF) << 16,
                            static_cast<s64>((d[6] >> 16) & 0xFF) << 16);
                pushColour();
                break;
            case 0x11:  // INTPL
                interpolate(static_cast<s64>(static_cast<s16>(d[9])) << 12, static_cast<s64>(static_cast<s16>(d[10])) << 12,
                            static_cast<s64>(static_cast<s16>(d[11])) << 12);
                pushColour();
                break;
            case 0x12: {  // MVMVA
                int mx = (w >> 17) & 3, v = (w >> 15) & 3, cv = (w >> 13) & 3;
                s32 x = V(v, 0), y = V(v, 1), z = V(v, 2);
                if (mx == 3) mx = 0;
                if (cv == 2) {
                    // far colour: the hardware drops the first product from the result
                    for (int i = 0; i < 3; ++i) {
                        s64 first = A(i + 1, (static_cast<s64>(K(21 + i)) << 12) + static_cast<s64>(M(mx, i, 0)) * x);
                        limB(i + 1, static_cast<s32>(first >> sf), false);
                        s64 acc = A(i + 1, static_cast<s64>(M(mx, i, 1)) * y);
                        acc = A(i + 1, acc + static_cast<s64>(M(mx, i, 2)) * z);
                        setMac(i + 1, acc);
                    }
                    macToIr();
                } else {
                    const s32 tr[3] = {K(5), K(6), K(7)}, bk[3] = {K(13), K(14), K(15)};
                    const s32* t = cv == 0 ? tr : cv == 1 ? bk : nullptr;
                    setMacIr(row(mx, 0, t, x, y, z), row(mx, 1, t, x, y, z), row(mx, 2, t, x, y, z));
                }
                break;
            }
            case 0x13: ncds(0); break;
            case 0x14:  // CDP
                lightColour();
                interpolate(rgbIr(0), rgbIr(1), rgbIr(2));
                pushColour();
                break;
            case 0x16: ncds(0); ncds(1); ncds(2); break;
            case 0x1B: nccs(0); break;
            case 0x1C:  // CC
                lightColour();
                setMacIr(rgbIr(0), rgbIr(1), rgbIr(2));
                pushColour();
                break;
            case 0x1E: ncs(0); break;
            case 0x20: ncs(0); ncs(1); ncs(2); break;
            case 0x28: {  // SQR
                s64 i1 = static_cast<s16>(d[9]), i2 = static_cast<s16>(d[10]), i3 = static_cast<s16>(d[11]);
                setMacIr(i1 * i1, i2 * i2, i3 * i3);
                break;
            }
            case 0x29:  // DCPL
                interpolate(rgbIr(0), rgbIr(1), rgbIr(2));
                pushColour();
                break;
            case 0x2A:  // DPCT
                for (int i = 0; i < 3; ++i) {
                    interpolate(static_cast<s64>(d[20] & 0xFF) << 16, static_cast<s64>((d[20] >> 8) & 0xFF) << 16,
                                static_cast<s64>((d[20] >> 16) & 0xFF) << 16);
                    pushColour();
                }
                break;
            case 0x2D: {  // AVSZ3
                s64 v = F(static_cast<s64>(static_cast<s16>(k[29])) * (d[17] + d[18] + d[19]));
                d[24] = static_cast<u32>(v);
                d[7] = limD(v >> 12);
                break;
            }
            case 0x2E: {  // AVSZ4
                s64 v = F(static_cast<s64>(static_cast<s16>(k[30])) * (d[16] + d[17] + d[18] + d[19]));
                d[24] = static_cast<u32>(v);
                d[7] = limD(v >> 12);
                break;
            }
            case 0x30: rtps(0, false); rtps(1, false); rtps(2, true); break;
            case 0x3D: {  // GPF
                s64 ir0 = static_cast<s16>(d[8]);
                setMacIr(ir0 * static_cast<s16>(d[9]), ir0 * static_cast<s16>(d[10]), ir0 * static_cast<s16>(d[11]));
                pushColour();
                break;
            }
            case 0x3E: {  // GPL
                s64 ir0 = static_cast<s16>(d[8]);
                s64 m[3];
                for (int i = 0; i < 3; ++i)
                    m[i] = A(i + 1, (static_cast<s64>(D(25 + i)) << sf) + ir0 * static_cast<s16>(d[9 + i]));
                setMacIr(m[0], m[1], m[2]);
                pushColour();
                break;
            }
            case 0x3F: nccs(0); nccs(1); nccs(2); break;
            default: c.m->note("unknown GTE command " + std::to_string(w & 0x3F)); break;
        }
        if (flag & 0x7F87E000) flag |= 0x80000000u;
        k[31] = flag;
    }
};

u32 irgb(const u32* d) {
    auto ch = [](s32 v) { return static_cast<u32>(std::clamp(v >> 7, 0, 31)); };
    return ch(static_cast<s16>(d[9])) | (ch(static_cast<s16>(d[10])) << 5) | (ch(static_cast<s16>(d[11])) << 10);
}

}  // namespace

u32 gteMfc2(Cpu& c, u32 reg) {
    const u32* d = c.gteData;
    switch (reg) {
        case 1: case 3: case 5: case 8: case 9: case 10: case 11:
            return static_cast<u32>(static_cast<s32>(static_cast<s16>(d[reg])));
        case 7: case 16: case 17: case 18: case 19: return d[reg] & 0xFFFF;
        case 15: return d[14];
        case 28: case 29: return irgb(d);
        default: return d[reg];
    }
}

void gteMtc2(Cpu& c, u32 reg, u32 value) {
    u32* d = c.gteData;
    if (reg >= 12 && reg <= 15 && c.m && c.m->pgxp) {
        // screen positions written by the game: no precise value for them
        if (reg == 15) {
            for (int i = 0; i < 2; ++i) {
                c.m->pgxpSxy[i][0] = c.m->pgxpSxy[i + 1][0];
                c.m->pgxpSxy[i][1] = c.m->pgxpSxy[i + 1][1];
                c.m->pgxpSxyValid[i] = c.m->pgxpSxyValid[i + 1];
            }
            c.m->pgxpSxyValid[2] = false;
        } else {
            c.m->pgxpSxyValid[reg - 12] = false;
        }
    }
    switch (reg) {
        case 15:  // SXYP: push
            d[12] = d[13];
            d[13] = d[14];
            d[14] = value;
            break;
        case 28:  // IRGB
            d[9] = (value & 31) << 7;
            d[10] = ((value >> 5) & 31) << 7;
            d[11] = ((value >> 10) & 31) << 7;
            d[28] = value & 0x7FFF;
            break;
        case 29: case 31: break;  // read-only
        case 30: {  // LZCS: LZCR counts the leading bits equal to the sign
            d[30] = value;
            u32 v = (value & 0x80000000u) ? ~value : value;
            u32 n = 0;
            while (n < 32 && !(v & (0x80000000u >> n))) ++n;
            d[31] = n;
            break;
        }
        default: d[reg] = value; break;
    }
}

u32 gteCfc2(Cpu& c, u32 reg) {
    const u32* k = c.gteCtrl;
    switch (reg) {
        case 4: case 12: case 20: case 26: case 27: case 29: case 30:
            return static_cast<u32>(static_cast<s32>(static_cast<s16>(k[reg])));
        default: return k[reg];
    }
}

void gteCtc2(Cpu& c, u32 reg, u32 value) {
    if (reg == 31) {
        value &= 0x7FFFF000;
        if (value & 0x7F87E000) value |= 0x80000000u;
    }
    c.gteCtrl[reg] = value;
    // R33 is the last of the rotation registers a game writes
    if (reg == 4 && c.m && c.m->recordRotations && c.m->rotations.size() < 512) {
        const u32* k = c.gteCtrl;
        auto lo = [](u32 v) { return static_cast<s16>(v & 0xFFFF); };
        auto hi = [](u32 v) { return static_cast<s16>(v >> 16); };
        c.m->rotations.push_back({lo(k[0]), hi(k[0]), lo(k[1]), hi(k[1]), lo(k[2]), hi(k[2]), lo(k[3]), hi(k[3]), lo(k[4])});
    }
}

void gteCommand(Cpu& c, u32 word) { Gte(c).run(word); }

void gteLwc2(Cpu& c, u32 addr, u32 reg) {
    const u32 value = rd32(c, addr);
    gteMtc2(c, reg, value);  // marks the position as not precise
    Machine* m = c.m;
    if (!m || !m->pgxp || reg < 12 || reg > 15) return;
    if (const Machine::PreciseWord* e = m->preciseAt(addr, value)) {
        const int i = reg == 15 ? 2 : static_cast<int>(reg) - 12;
        m->pgxpSxy[i][0] = e->x;
        m->pgxpSxy[i][1] = e->y;
        m->pgxpSxyValid[i] = true;
    }
}

void gteSwc2(Cpu& c, u32 addr, u32 reg) {
    const u32 value = gteMfc2(c, reg);
    wr32(c, addr, value);
    Machine* m = c.m;
    if (!m || !m->pgxp || m->pgxpMem.empty()) return;
    const long at = Machine::pgxpIndex(addr);
    if (at < 0) return;
    Machine::PreciseWord& e = m->pgxpMem[static_cast<size_t>(at)];
    const int i = reg == 15 ? 2 : static_cast<int>(reg) - 12;
    if (i >= 0 && i <= 2 && m->pgxpSxyValid[i]) {
        e.word = value;
        e.frame = static_cast<u32>(m->frame);
        e.x = m->pgxpSxy[i][0];
        e.y = m->pgxpSxy[i][1];
    } else {
        e = Machine::PreciseWord{};
    }
}

}  // namespace fe
