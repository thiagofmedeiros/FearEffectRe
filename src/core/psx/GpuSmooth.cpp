// 60 fps: frames in between the game's own (see Gpu::recordPasses).
#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "psx/Gpu.h"

namespace fe {

namespace {

constexpr size_t kPassesKept = 4;
constexpr float kMaxMove = 40.0f;  // native pixels: a vertex moving further than this is not the same point

s32 sext11(u32 v) { return static_cast<s32>(v << 21) >> 21; }

// A polygon of a recorded picture: where its vertices are and what it looks like.
struct Poly {
    u32 sig;                 // command and texture coordinates: what must match
    int n;                   // vertices
    size_t xy[4];            // word indices of the vertex positions
    float x[4], y[4];        // positions (precise where known)
    float cx, cy;            // centre
    bool is3d;               // has precise vertices (drawn from the GTE)
};

std::vector<Poly> polygons(const std::vector<u32>& w, const std::vector<std::array<float, 2>>& p, int (*length)(u32)) {
    std::vector<Poly> out;
    size_t i = 0;
    while (i < w.size()) {
        const u32 op = w[i] >> 24;
        if (op >= 0x20 && op < 0x40) {
            const bool gouraud = op & 0x10, quad = op & 0x08, textured = op & 0x04;
            Poly q{};
            q.n = quad ? 4 : 3;
            q.sig = op * 2654435761u;
            size_t j = i;
            bool ok = true;
            for (int k = 0; k < q.n; ++k) {
                if (k == 0 || gouraud) ++j;  // colour (the command word itself for the first)
                if (j >= w.size()) {
                    ok = false;
                    break;
                }
                q.xy[k] = j;
                const bool hasPrecise = j < p.size() && !std::isnan(p[j][0]);
                q.x[k] = hasPrecise ? p[j][0] : static_cast<float>(sext11(w[j]));
                q.y[k] = hasPrecise ? p[j][1] : static_cast<float>(sext11(w[j] >> 16));
                q.is3d = q.is3d || hasPrecise;
                ++j;
                if (textured) {
                    if (j >= w.size()) {
                        ok = false;
                        break;
                    }
                    q.sig = (q.sig ^ w[j]) * 16777619u;
                    ++j;
                }
            }
            if (!ok) break;
            q.cx = q.cy = 0;
            for (int k = 0; k < q.n; ++k) {
                q.cx += q.x[k] / q.n;
                q.cy += q.y[k] / q.n;
            }
            out.push_back(q);
            i += static_cast<size_t>(length(w[i]));
        } else if (op >= 0xA0 && op < 0xC0 && i + 2 < w.size()) {
            // an upload: the pixels follow the command
            const u32 size = w[i + 2];
            const u32 ww = ((size & 0xFFFF) - 1) % 1024 + 1, hh = ((size >> 16) - 1) % 512 + 1;
            i += 3 + (ww * hh + 1) / 2;
        } else if (op >= 0x48 && op < 0x60 && (op & 0x08)) {
            // a polyline: up to its terminator
            for (++i; i < w.size() && (w[i] & 0xF000F000) != 0x50005000; ++i) {}
            ++i;
        } else {
            i += static_cast<size_t>(std::max(1, length(w[i])));
        }
    }
    return out;
}

}  // namespace

Gpu::Regs Gpu::saveRegs() const {
    return Regs{texpage, tpx,    tpy,    blend,  depth,  clutX, clutY, twMaskX, twMaskY, twOffX,  twOffY,
                areaX1,  areaY1, areaX2, areaY2, offX,   offY,  setMask, checkMask};
}

void Gpu::loadRegs(const Regs& r) {
    texpage = r.texpage;
    tpx = r.tpx;
    tpy = r.tpy;
    blend = r.blend;
    depth = r.depth;
    clutX = r.clutX;
    clutY = r.clutY;
    twMaskX = r.twMaskX;
    twMaskY = r.twMaskY;
    twOffX = r.twOffX;
    twOffY = r.twOffY;
    areaX1 = r.areaX1;
    areaY1 = r.areaY1;
    areaX2 = r.areaX2;
    areaY2 = r.areaY2;
    offX = r.offX;
    offY = r.offY;
    setMask = r.setMask;
    checkMask = r.checkMask;
}

void Gpu::recordWord(u32 word, float x, float y) {
    Pass& pass = passes.back();
    if (pass.words.size() > 4000000) return;  // runaway: a pass this long is not a picture
    pass.words.push_back(word);
    pass.precise.push_back({x, y});
}

void Gpu::startPass() {
    if (!passes.empty()) passes.back().done = true;
    while (passes.size() >= kPassesKept) passes.pop_front();
    Pass pass;
    pass.x1 = areaX1;
    pass.y1 = areaY1;
    pass.x2 = areaX2;
    pass.y2 = areaY2;
    passes.push_back(std::move(pass));
}

bool Gpu::renderBetween(Gpu& source, float t, int& matched) {
    matched = 0;
    if (source.scale != scale || scale < 2) return false;
    // The picture on screen (B) is the newest finished one drawn where the
    // display is; the one before it (A) is the pass just before.
    const Pass* b = nullptr;
    const Pass* a = nullptr;
    for (size_t i = source.passes.size(); i-- > 0;) {
        const Pass& p = source.passes[i];
        if (!p.done || !p.snapped) continue;
        if (!b) {
            if (source.dispX >= p.x1 && source.dispX <= p.x2 && source.dispY >= p.y1 && source.dispY <= p.y2) b = &p;
            continue;
        }
        a = &p;
        break;
    }
    if (!a || !b) return false;

    // ---- which polygon of A each 3D polygon of B is ---------------------------
    static int (*length)(u32) = [](u32 word) {
        const u32 op = word >> 24;
        if (op >= 0x20 && op < 0x40) {
            const int n = (op & 0x08) ? 4 : 3;
            return 1 + n + ((op & 0x04) ? n : 0) + ((op & 0x10) ? n - 1 : 0);
        }
        if (op >= 0x40 && op < 0x60) return (op & 0x08) ? 1 : ((op & 0x10) ? 4 : 3);
        if (op >= 0x60 && op < 0x80) return 2 + ((op & 0x04) ? 1 : 0) + (((op >> 3) & 3) == 0 ? 1 : 0);
        if (op >= 0x80 && op < 0xA0) return 4;
        if (op >= 0xA0 && op < 0xE0) return 3;
        if (op == 0x02) return 3;
        return 1;
    };
    const std::vector<Poly> pa = polygons(a->words, a->precise, length);
    const std::vector<Poly> pb = polygons(b->words, b->precise, length);
    std::unordered_multimap<u32, size_t> bySig;
    for (size_t i = 0; i < pa.size(); ++i)
        if (pa[i].is3d) bySig.emplace(pa[i].sig, i);
    std::vector<bool> used(pa.size(), false);
    std::vector<std::array<float, 2>> moved(b->words.size(), {kNan, kNan});  // new position per vertex word
    int total = 0, found = 0;
    for (const Poly& q : pb) {
        if (!q.is3d) continue;
        ++total;
        // the nearest unused polygon of A that looks the same
        size_t best = SIZE_MAX;
        float bestD = kMaxMove * kMaxMove;
        auto range = bySig.equal_range(q.sig);
        for (auto it = range.first; it != range.second; ++it) {
            const Poly& r = pa[it->second];
            if (used[it->second] || r.n != q.n) continue;
            const float dx = r.cx - q.cx, dy = r.cy - q.cy, d = dx * dx + dy * dy;
            if (d < bestD) {
                bestD = d;
                best = it->second;
            }
        }
        if (best == SIZE_MAX) continue;
        used[best] = true;
        ++found;
        const Poly& r = pa[best];
        for (int k = 0; k < q.n; ++k)
            moved[q.xy[k]] = {r.x[k] + (q.x[k] - r.x[k]) * t, r.y[k] + (q.y[k] - r.y[k]) * t};
    }
    matched = total ? found * 100 / total : 100;
    if (total == 0 || matched < 60) return false;  // a cut, or nothing that moves

    // ---- draw B again, from its snapshot, with the matched vertices moved -----
    vram = source.vram;
    loadRegs(b->regs);
    growFrameBuffer(b->x1, b->y1, b->x2, b->y2);
    {
        const int S = scale, x1 = std::max(b->x1, 0), y1 = std::max(b->y1, 0);
        const int x2 = std::min(b->x2, 1023), y2 = std::min(b->y2, 511);
        const size_t w = static_cast<size_t>(x2 - x1 + 1) * S, stride = static_cast<size_t>(1024) * S;
        if (b->pixels.size() != w * static_cast<size_t>(y2 - y1 + 1) * S) return false;
        for (int y = y1 * S; y < (y2 + 1) * S; ++y)
            std::copy_n(&b->pixels[static_cast<size_t>(y - y1 * S) * w], w, &hires[static_cast<size_t>(y) * stride + static_cast<size_t>(x1) * S]);
    }
    dispX = source.dispX;
    dispY = source.dispY;
    displayMode = source.displayMode;
    displayEnabled = source.displayEnabled;
    hRange = source.hRange;
    vRange = source.vRange;
    cmd.clear();
    cmdPrecise.clear();
    loading = storing = polyline = false;
    preciseCache.clear();
    // the enlarged movie pictures are borrowed, not copied
    std::swap(video, source.video);
    std::swap(videoState, source.videoState);
    for (size_t i = 0; i < b->words.size(); ++i) {
        const auto& m = moved[i];
        if (!std::isnan(m[0])) gp0Precise(b->words[i], m[0], m[1]);
        else if (!std::isnan(b->precise[i][0])) gp0Precise(b->words[i], b->precise[i][0], b->precise[i][1]);
        else gp0(b->words[i]);
    }
    std::swap(video, source.video);
    std::swap(videoState, source.videoState);
    return true;
}

}  // namespace fe
