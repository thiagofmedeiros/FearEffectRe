#include "Gpu.h"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>

namespace fe {

namespace {
int sext11(u32 v) { return static_cast<int>(static_cast<s32>(v << 21) >> 21); }
u16 to15(int r, int g, int b) { return static_cast<u16>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)); }
int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// Worker threads for large high-resolution triangles: the rows are split
// into bands, one per thread. Bands write disjoint rows and only read video
// memory, so they do not need to synchronise with each other.
class RowPool {
public:
    RowPool() {
        unsigned n = std::thread::hardware_concurrency();
        count_ = static_cast<int>(std::clamp(n, 1u, 12u));
        for (int i = 1; i < count_; ++i) threads_.emplace_back([this, i] { work(i); });
    }
    ~RowPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
            ++generation_;
        }
        wake_.notify_all();
        for (auto& t : threads_) t.join();
    }
    int bands() const { return count_; }
    // Calls job(band) for band 0..bands()-1 and returns when all are done.
    void run(const std::function<void(int)>& job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_ = &job;
            pending_ = count_ - 1;
            ++generation_;
        }
        wake_.notify_all();
        job(0);
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [this] { return pending_ == 0; });
    }

private:
    void work(int band) {
        unsigned seen = 0;
        for (;;) {
            const std::function<void(int)>* job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return generation_ != seen; });
                seen = generation_;
                if (quit_) return;
                job = job_;
            }
            (*job)(band);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                --pending_;
            }
            done_.notify_one();
        }
    }
    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable wake_, done_;
    const std::function<void(int)>* job_ = nullptr;
    unsigned generation_ = 0;
    int pending_ = 0, count_ = 1;
    bool quit_ = false;
};

RowPool& rowPool() {
    static RowPool pool;
    return pool;
}
}  // namespace

void Gpu::setScale(int factor) {
    scale = std::clamp(factor, 1, 16);
    hires.clear();
    if (scale == 1) return;
    hires.assign(static_cast<size_t>(1024) * scale * 512 * scale, 0);
    fbX1 = 1024;
    fbY1 = 512;
    fbX2 = fbY2 = -1;
    growFrameBuffer(areaX1, areaY1, areaX2, areaY2);
    trackDisplay();
}

void Gpu::fillHi(int x, int y, u16 color) {
    const size_t stride = static_cast<size_t>(1024) * scale;
    u16* row = &hires[static_cast<size_t>(y) * scale * stride + static_cast<size_t>(x) * scale];
    for (int j = 0; j < scale; ++j, row += stride)
        for (int i = 0; i < scale; ++i) row[i] = color;
}

void Gpu::plotHi(int x, int y, u16 color, bool semi) {
    u16& d = hires[static_cast<size_t>(y) * 1024 * scale + x];
    if (checkMask && (d & 0x8000)) return;
    if (semi) {
        int br = d & 31, bg = (d >> 5) & 31, bb = (d >> 10) & 31;
        int fr = color & 31, fg = (color >> 5) & 31, fb = (color >> 10) & 31;
        int r, g, b;
        switch (blend) {
            case 0: r = (br + fr) >> 1; g = (bg + fg) >> 1; b = (bb + fb) >> 1; break;
            case 1: r = br + fr; g = bg + fg; b = bb + fb; break;
            case 2: r = br - fr; g = bg - fg; b = bb - fb; break;
            default: r = br + (fr >> 2); g = bg + (fg >> 2); b = bb + (fb >> 2); break;
        }
        r = std::clamp(r, 0, 31);
        g = std::clamp(g, 0, 31);
        b = std::clamp(b, 0, 31);
        color = static_cast<u16>((color & 0x8000) | r | (g << 5) | (b << 10));
    }
    d = color | setMask;
}

void Gpu::growFrameBuffer(int x1, int y1, int x2, int y2) {
    x1 = std::clamp(x1, 0, 1023);
    x2 = std::clamp(x2, 0, 1023);
    y1 = std::clamp(y1, 0, 511);
    y2 = std::clamp(y2, 0, 511);
    if (scale == 1 || x1 > x2 || y1 > y2) return;
    if (x1 >= fbX1 && x2 <= fbX2 && y1 >= fbY1 && y2 <= fbY2) return;
    int nx1 = std::min(fbX1, x1), ny1 = std::min(fbY1, y1), nx2 = std::max(fbX2, x2), ny2 = std::max(fbY2, y2);
    // bring the newly covered part up to date from video memory
    for (int y = ny1; y <= ny2; ++y)
        for (int x = nx1; x <= nx2; ++x)
            if (!inFrameBuffer(x, y)) fillHi(x, y, vram[static_cast<size_t>(y) * 1024 + x]);
    fbX1 = nx1;
    fbY1 = ny1;
    fbX2 = nx2;
    fbY2 = ny2;
}

void Gpu::trackDisplay() {
    static const int widths[4] = {256, 320, 512, 640};
    int w = (displayMode & 0x40) ? 368 : widths[displayMode & 3];
    int h = (displayMode & 0x24) == 0x24 ? 480 : 240;
    growFrameBuffer(dispX, dispY, dispX + w - 1, dispY + h - 1);
}

// The same triangle as triangle(), `scale` times larger. Attributes step in
// 16.16 fixed point along each row; texture coordinates stay in native texels.
void Gpu::triangleHi(Vertex a, Vertex b, Vertex c, bool textured, bool gouraud, bool semi, bool raw) {
    if (std::max({a.x, b.x, c.x}) - std::min({a.x, b.x, c.x}) > 1023) return;
    if (std::max({a.y, b.y, c.y}) - std::min({a.y, b.y, c.y}) > 511) return;
    const int S = scale;
    for (Vertex* v : {&a, &b, &c}) {
        v->x *= S;
        v->y *= S;
    }
    s64 area = static_cast<s64>(b.x - a.x) * (c.y - a.y) - static_cast<s64>(b.y - a.y) * (c.x - a.x);
    if (area == 0) return;
    if (area < 0) {
        std::swap(b, c);
        area = -area;
    }
    int minX = std::max(std::min({a.x, b.x, c.x}), areaX1 * S), maxX = std::min(std::max({a.x, b.x, c.x}), (areaX2 + 1) * S - 1);
    int minY = std::max(std::min({a.y, b.y, c.y}), areaY1 * S), maxY = std::min(std::max({a.y, b.y, c.y}), (areaY2 + 1) * S - 1);
    if (minX > maxX || minY > maxY) return;

    auto edge = [](const Vertex& p, const Vertex& q, int x, int y) {
        return static_cast<s64>(q.x - p.x) * (y - p.y) - static_cast<s64>(q.y - p.y) * (x - p.x);
    };
    auto bias = [](const Vertex& p, const Vertex& q) {
        int dx = q.x - p.x, dy = q.y - p.y;
        return (dy < 0 || (dy == 0 && dx > 0)) ? 0 : -1;
    };
    s64 row0 = edge(b, c, minX, minY) + bias(b, c), row1 = edge(c, a, minX, minY) + bias(c, a),
        row2 = edge(a, b, minX, minY) + bias(a, b);
    const s64 dx0 = -(c.y - b.y), dx1 = -(a.y - c.y), dx2 = -(b.y - a.y);
    const s64 dy0 = c.x - b.x, dy1 = a.x - c.x, dy2 = b.x - a.x;

    // plane equation of an attribute: value = a + (x - a.x) * ddx + (y - a.y) * ddy
    const double inv = 1.0 / static_cast<double>(area);
    struct Plane {
        double base, ddx, ddy;
        s32 step;
    };
    auto plane = [&](int va, int vb, int vc) {
        Plane p;
        p.ddx = (static_cast<double>(vb - va) * (c.y - a.y) - static_cast<double>(vc - va) * (b.y - a.y)) * inv;
        p.ddy = (static_cast<double>(vc - va) * (b.x - a.x) - static_cast<double>(vb - va) * (c.x - a.x)) * inv;
        p.base = va;
        p.step = static_cast<s32>(p.ddx * 65536.0);
        return p;
    };
    // half a native texel so each texel covers the block of pixels it did before
    const double half = 0.5 / S;
    Plane pu = plane(a.u, b.u, c.u), pv = plane(a.v, b.v, c.v);
    Plane pr = plane(a.r, b.r, c.r), pg = plane(a.g, b.g, c.g), pb = plane(a.b, b.b, c.b);
    auto start = [&](const Plane& p, int y, double offset) {
        return static_cast<s32>((p.base + (minX - a.x) * p.ddx + (y - a.y) * p.ddy + offset) * 65536.0);
    };

    const bool neutral = !gouraud && a.r == 0x80 && a.g == 0x80 && a.b == 0x80;
    const bool direct = textured && depth >= 2 && !twMaskX && !twMaskY && (raw || neutral) && !semi && !checkMask;
    const u16 flat = to15(a.r, a.g, a.b);
    const size_t stride = static_cast<size_t>(1024) * S;

    auto rows = [&](int yFrom, int yTo) {
    for (int y = yFrom; y <= yTo; ++y) {
        s64 w0 = row0 + dy0 * (y - minY), w1 = row1 + dy1 * (y - minY), w2 = row2 + dy2 * (y - minY);
        u16* out = &hires[static_cast<size_t>(y) * stride + minX];
        if (direct) {
            // 15-bit texture drawn as it is: the streamed backgrounds
            s32 u = start(pu, y, half), v = start(pv, y, half);
            for (int x = minX; x <= maxX; ++x, ++out, w0 += dx0, w1 += dx1, w2 += dx2, u += pu.step, v += pv.step) {
                if ((w0 | w1 | w2) < 0) continue;
                u16 t = vram[static_cast<size_t>((tpy + ((v >> 16) & 255)) & 511) * 1024 + ((tpx + ((u >> 16) & 255)) & 1023)];
                if (t) *out = t | setMask;
            }
            continue;
        }
        if (!textured && !gouraud && !semi && !checkMask) {
            const u16 colour = flat | setMask;
            for (int x = minX; x <= maxX; ++x, ++out, w0 += dx0, w1 += dx1, w2 += dx2)
                if ((w0 | w1 | w2) >= 0) *out = colour;
            continue;
        }
        s32 u = start(pu, y, half), v = start(pv, y, half);
        s32 r = start(pr, y, 0), g = start(pg, y, 0), bl = start(pb, y, 0);
        for (int x = minX; x <= maxX; ++x, w0 += dx0, w1 += dx1, w2 += dx2, u += pu.step, v += pv.step, r += pr.step,
                 g += pg.step, bl += pb.step) {
            if ((w0 | w1 | w2) < 0) continue;
            int cr = a.r, cg = a.g, cb = a.b;
            if (gouraud) {
                cr = clamp255(r >> 16);
                cg = clamp255(g >> 16);
                cb = clamp255(bl >> 16);
            }
            if (!textured) {
                plotHi(x, y, to15(cr, cg, cb), semi);
                continue;
            }
            u16 t = texel(u >> 16, v >> 16);
            if (!t) continue;
            if (!raw) {
                int tr = clamp255(((t & 31) * cr) >> 4), tg = clamp255((((t >> 5) & 31) * cg) >> 4);
                int tb = clamp255((((t >> 10) & 31) * cb) >> 4);
                t = static_cast<u16>((t & 0x8000) | to15(tr, tg, tb));
            }
            plotHi(x, y, t, semi && (t & 0x8000));
        }
    }
    };

    const s64 pixels = static_cast<s64>(maxX - minX + 1) * (maxY - minY + 1);
    RowPool& pool = rowPool();
    if (pixels < 150000 || pool.bands() == 1) {
        rows(minY, maxY);
        return;
    }
    const int n = pool.bands(), total = maxY - minY + 1;
    pool.run([&](int band) {
        int from = minY + static_cast<int>(static_cast<s64>(total) * band / n);
        int to = minY + static_cast<int>(static_cast<s64>(total) * (band + 1) / n) - 1;
        if (from <= to) rows(from, to);
    });
}

void Gpu::rectHi(int x, int y, int w, int h, int u0, int v0, int r, int g, int b, bool textured, bool semi, bool raw) {
    const int S = scale;
    u16 flat = to15(r, g, b);
    int y0 = std::max(y, areaY1), y1 = std::min(y + h - 1, areaY2);
    int x0 = std::max(x, areaX1), x1 = std::min(x + w - 1, areaX2);
    for (int py = y0; py <= y1; ++py)
        for (int px = x0; px <= x1; ++px) {
            u16 t = flat;
            bool blendIt = semi;
            if (textured) {
                t = texel(u0 + (px - x), v0 + (py - y));
                if (!t) continue;
                if (!raw) {
                    int tr = clamp255(((t & 31) * r) >> 4), tg = clamp255((((t >> 5) & 31) * g) >> 4);
                    int tb = clamp255((((t >> 10) & 31) * b) >> 4);
                    t = static_cast<u16>((t & 0x8000) | to15(tr, tg, tb));
                }
                blendIt = semi && (t & 0x8000);
            }
            if (!blendIt && !checkMask) {
                fillHi(px, py, t | setMask);
                continue;
            }
            for (int j = 0; j < S; ++j)
                for (int i = 0; i < S; ++i) plotHi(px * S + i, py * S + j, t, blendIt);
        }
}

bool Gpu::frame(const u16*& pixels, int& stride, int& x, int& y, int& width, int& height) const {
    if (displayMode & 0x10) return false;
    static const int widths[4] = {256, 320, 512, 640};
    int w = (displayMode & 0x40) ? 368 : widths[displayMode & 3];
    int h = (displayMode & 0x24) == 0x24 ? 480 : 240;
    w = std::min(w, 1024 - dispX);
    h = std::min(h, 512 - dispY);
    pixels = scale > 1 ? hires.data() : vram.data();
    stride = 1024 * scale;
    x = dispX * scale;
    y = dispY * scale;
    width = w * scale;
    height = h * scale;
    return true;
}

void Gpu::setTexpage(u32 bits) {
    texpage = (texpage & ~0x1FFu) | (bits & 0x1FF);
    tpx = static_cast<int>(bits & 15) * 64;
    tpy = (bits & 0x10) ? 256 : 0;
    blend = static_cast<int>((bits >> 5) & 3);
    depth = static_cast<int>((bits >> 7) & 3);
}

void Gpu::plot(int x, int y, u16 color, bool semi) {
    u16& d = vram[static_cast<size_t>(y) * 1024 + x];
    if (checkMask && (d & 0x8000)) return;
    if (semi) {
        int br = d & 31, bg = (d >> 5) & 31, bb = (d >> 10) & 31;
        int fr = color & 31, fg = (color >> 5) & 31, fb = (color >> 10) & 31;
        int r, g, b;
        switch (blend) {
            case 0: r = (br + fr) >> 1; g = (bg + fg) >> 1; b = (bb + fb) >> 1; break;
            case 1: r = br + fr; g = bg + fg; b = bb + fb; break;
            case 2: r = br - fr; g = bg - fg; b = bb - fb; break;
            default: r = br + (fr >> 2); g = bg + (fg >> 2); b = bb + (fb >> 2); break;
        }
        r = std::clamp(r, 0, 31);
        g = std::clamp(g, 0, 31);
        b = std::clamp(b, 0, 31);
        color = static_cast<u16>((color & 0x8000) | r | (g << 5) | (b << 10));
    }
    d = color | setMask;
}

u16 Gpu::texel(int u, int v) const {
    u = (u & ~(twMaskX * 8)) | ((twOffX & twMaskX) * 8);
    v = (v & ~(twMaskY * 8)) | ((twOffY & twMaskY) * 8);
    u &= 255;
    v &= 255;
    int y = (tpy + v) & 511;
    switch (depth) {
        case 0: {
            u16 w = vram[static_cast<size_t>(y) * 1024 + ((tpx + u / 4) & 1023)];
            int idx = (w >> ((u & 3) * 4)) & 15;
            return vram[static_cast<size_t>(clutY) * 1024 + ((clutX + idx) & 1023)];
        }
        case 1: {
            u16 w = vram[static_cast<size_t>(y) * 1024 + ((tpx + u / 2) & 1023)];
            int idx = (w >> ((u & 1) * 8)) & 255;
            return vram[static_cast<size_t>(clutY) * 1024 + ((clutX + idx) & 1023)];
        }
        default: return vram[static_cast<size_t>(y) * 1024 + ((tpx + u) & 1023)];
    }
}

void Gpu::triangle(Vertex a, Vertex b, Vertex c, bool textured, bool gouraud, bool semi, bool raw) {
    s64 area = static_cast<s64>(b.x - a.x) * (c.y - a.y) - static_cast<s64>(b.y - a.y) * (c.x - a.x);
    if (area == 0) return;
    if (area < 0) {
        std::swap(b, c);
        area = -area;
    }
    int minX = std::min({a.x, b.x, c.x}), maxX = std::max({a.x, b.x, c.x});
    int minY = std::min({a.y, b.y, c.y}), maxY = std::max({a.y, b.y, c.y});
    if (maxX - minX > 1023 || maxY - minY > 511) return;
    minX = std::max(minX, areaX1);
    minY = std::max(minY, areaY1);
    maxX = std::min(maxX, areaX2);
    maxY = std::min(maxY, areaY2);
    if (minX > maxX || minY > maxY) return;

    auto edge = [](const Vertex& p, const Vertex& q, int x, int y) {
        return static_cast<s64>(q.x - p.x) * (y - p.y) - static_cast<s64>(q.y - p.y) * (x - p.x);
    };
    // top-left fill rule: pixels on a bottom or right edge are not drawn
    auto bias = [](const Vertex& p, const Vertex& q) {
        int dx = q.x - p.x, dy = q.y - p.y;
        return (dy < 0 || (dy == 0 && dx > 0)) ? 0 : -1;
    };
    int b0 = bias(b, c), b1 = bias(c, a), b2 = bias(a, b);

    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            s64 w0 = edge(b, c, x, y), w1 = edge(c, a, x, y), w2 = edge(a, b, x, y);
            if (w0 + b0 < 0 || w1 + b1 < 0 || w2 + b2 < 0) continue;
            int r = a.r, g = a.g, bl = a.b;
            if (gouraud) {
                r = static_cast<int>((w0 * a.r + w1 * b.r + w2 * c.r) / area);
                g = static_cast<int>((w0 * a.g + w1 * b.g + w2 * c.g) / area);
                bl = static_cast<int>((w0 * a.b + w1 * b.b + w2 * c.b) / area);
            }
            if (!textured) {
                plot(x, y, to15(r, g, bl), semi);
                continue;
            }
            int u = static_cast<int>((w0 * a.u + w1 * b.u + w2 * c.u) / area);
            int v = static_cast<int>((w0 * a.v + w1 * b.v + w2 * c.v) / area);
            u16 t = texel(u, v);
            if (!t) continue;
            if (!raw) {
                int tr = clamp255(((t & 31) * r) >> 4), tg = clamp255((((t >> 5) & 31) * g) >> 4);
                int tb = clamp255((((t >> 10) & 31) * bl) >> 4);
                t = static_cast<u16>((t & 0x8000) | to15(tr, tg, tb));
            }
            plot(x, y, t, semi && (t & 0x8000));
        }
    }
}

void Gpu::polygon() {
    u32 op = cmd[0] >> 24;
    bool gouraud = op & 0x10, quad = op & 0x08, textured = op & 0x04, semi = op & 0x02, raw = op & 0x01;
    int n = quad ? 4 : 3;
    Vertex v[4]{};
    size_t i = 0;
    u32 color = cmd[0];
    for (int k = 0; k < n; ++k) {
        if (k == 0 || gouraud) color = cmd[i++];
        v[k].r = color & 255;
        v[k].g = (color >> 8) & 255;
        v[k].b = (color >> 16) & 255;
        u32 xy = cmd[i++];
        v[k].x = sext11(xy) + offX;
        v[k].y = sext11(xy >> 16) + offY;
        if (textured) {
            u32 uv = cmd[i++];
            v[k].u = uv & 255;
            v[k].v = (uv >> 8) & 255;
            if (k == 0) {
                clutX = static_cast<int>((uv >> 16) & 63) * 16;
                clutY = static_cast<int>((uv >> 22) & 511);
            } else if (k == 1) {
                setTexpage((texpage & ~0x9FFu) | ((uv >> 16) & 0x9FF));
            }
        }
    }
    triangle(v[0], v[1], v[2], textured, gouraud, semi, raw);
    if (quad) triangle(v[1], v[2], v[3], textured, gouraud, semi, raw);
    if (scale > 1) {
        triangleHi(v[0], v[1], v[2], textured, gouraud, semi, raw);
        if (quad) triangleHi(v[1], v[2], v[3], textured, gouraud, semi, raw);
    }
}

void Gpu::drawLine(Vertex a, Vertex b, bool semi) {
    int dx = std::abs(b.x - a.x), dy = std::abs(b.y - a.y);
    if (dx > 1023 || dy > 511) return;
    int steps = std::max(dx, dy);
    for (int i = 0; i <= steps; ++i) {
        int x = steps ? a.x + (b.x - a.x) * i / steps : a.x;
        int y = steps ? a.y + (b.y - a.y) * i / steps : a.y;
        if (x < areaX1 || x > areaX2 || y < areaY1 || y > areaY2) continue;
        if (scale > 1) {
            int hr = steps ? a.r + (b.r - a.r) * i / steps : a.r, hg = steps ? a.g + (b.g - a.g) * i / steps : a.g;
            int hb = steps ? a.b + (b.b - a.b) * i / steps : a.b;
            for (int j = 0; j < scale; ++j)
                for (int k = 0; k < scale; ++k) plotHi(x * scale + k, y * scale + j, to15(hr, hg, hb), semi);
        }
        int r = steps ? a.r + (b.r - a.r) * i / steps : a.r;
        int g = steps ? a.g + (b.g - a.g) * i / steps : a.g;
        int bl = steps ? a.b + (b.b - a.b) * i / steps : a.b;
        plot(x, y, to15(r, g, bl), semi);
    }
}

void Gpu::line() {
    u32 op = cmd[0] >> 24;
    bool gouraud = op & 0x10, semi = op & 0x02;
    std::vector<Vertex> pts;
    size_t i = 0;
    u32 color = cmd[0];
    while (i < cmd.size()) {
        if (pts.empty() || gouraud) {
            if (!pts.empty() && (cmd[i] & 0xF000F000) == 0x50005000) break;
            color = cmd[i++];
        }
        if (i >= cmd.size() || (cmd[i] & 0xF000F000) == 0x50005000) break;
        u32 xy = cmd[i++];
        Vertex v{};
        v.r = color & 255;
        v.g = (color >> 8) & 255;
        v.b = (color >> 16) & 255;
        v.x = sext11(xy) + offX;
        v.y = sext11(xy >> 16) + offY;
        pts.push_back(v);
    }
    for (size_t k = 1; k < pts.size(); ++k) drawLine(pts[k - 1], pts[k], semi);
}

void Gpu::rect() {
    u32 op = cmd[0] >> 24;
    bool textured = op & 0x04, semi = op & 0x02, raw = op & 0x01;
    int r = cmd[0] & 255, g = (cmd[0] >> 8) & 255, b = (cmd[0] >> 16) & 255;
    size_t i = 1;
    int x = sext11(cmd[i]) + offX, y = sext11(cmd[i] >> 16) + offY;
    ++i;
    int u0 = 0, v0 = 0;
    if (textured) {
        u0 = cmd[i] & 255;
        v0 = (cmd[i] >> 8) & 255;
        clutX = static_cast<int>((cmd[i] >> 16) & 63) * 16;
        clutY = static_cast<int>((cmd[i] >> 22) & 511);
        ++i;
    }
    int w, h;
    switch ((op >> 3) & 3) {
        case 0: w = cmd[i] & 0x3FF; h = (cmd[i] >> 16) & 0x1FF; break;
        case 1: w = h = 1; break;
        case 2: w = h = 8; break;
        default: w = h = 16; break;
    }
    if (scale > 1) rectHi(x, y, w, h, u0, v0, r, g, b, textured, semi, raw);
    u16 flat = to15(r, g, b);
    for (int py = 0; py < h; ++py) {
        int yy = y + py;
        if (yy < areaY1 || yy > areaY2) continue;
        for (int px = 0; px < w; ++px) {
            int xx = x + px;
            if (xx < areaX1 || xx > areaX2) continue;
            if (!textured) {
                plot(xx, yy, flat, semi);
                continue;
            }
            u16 t = texel(u0 + px, v0 + py);
            if (!t) continue;
            if (!raw) {
                int tr = clamp255(((t & 31) * r) >> 4), tg = clamp255((((t >> 5) & 31) * g) >> 4);
                int tb = clamp255((((t >> 10) & 31) * b) >> 4);
                t = static_cast<u16>((t & 0x8000) | to15(tr, tg, tb));
            }
            plot(xx, yy, t, semi && (t & 0x8000));
        }
    }
}

int Gpu::commandLength(u32 word) const {
    u32 op = word >> 24;
    if (op >= 0x20 && op < 0x40) {
        int n = (op & 0x08) ? 4 : 3;
        return 1 + n + ((op & 0x04) ? n : 0) + ((op & 0x10) ? n - 1 : 0);
    }
    if (op >= 0x40 && op < 0x60) return (op & 0x08) ? -1 : ((op & 0x10) ? 4 : 3);
    if (op >= 0x60 && op < 0x80) return 2 + ((op & 0x04) ? 1 : 0) + (((op >> 3) & 3) == 0 ? 1 : 0);
    if (op >= 0x80 && op < 0xA0) return 4;
    if (op >= 0xA0 && op < 0xE0) return 3;
    if (op == 0x02) return 3;
    return 1;
}

void Gpu::gp0(u32 word) {
    if (loading) {
        for (int k = 0; k < 2; ++k) {
            u16 px = static_cast<u16>(word >> (16 * k));
            int x = (tx + tcx) & 1023, y = (ty + tcy) & 511;
            u16& d = vram[static_cast<size_t>(y) * 1024 + x];
            if (!(checkMask && (d & 0x8000))) {
                d = px | setMask;
                if (scale > 1 && inFrameBuffer(x, y)) fillHi(x, y, d);
            }
            if (++tcx == tw) {
                tcx = 0;
                if (++tcy == th) {
                    loading = false;
                    break;
                }
            }
        }
        return;
    }
    if (cmd.empty()) {
        cmdNeed = commandLength(word);
        polyline = cmdNeed < 0;
    } else if (polyline) {
        // terminator is only looked for where a vertex (or colour) would start
        bool gouraud = (cmd[0] >> 24) & 0x10;
        size_t n = cmd.size();
        bool boundary = gouraud ? (n >= 4 && n % 2 == 0) : n >= 3;
        if (boundary && (word & 0xF000F000) == 0x50005000) {
            execute();
            return;
        }
    }
    cmd.push_back(word);
    if (!polyline && static_cast<int>(cmd.size()) >= cmdNeed) execute();
}

void Gpu::execute() {
    u32 op = cmd[0] >> 24;
    ++primitives;
    if (op >= 0x20 && op < 0x40) polygon();
    else if (op >= 0x40 && op < 0x60) line();
    else if (op >= 0x60 && op < 0x80) rect();
    else if (op >= 0x80 && op < 0xA0) {
        int sx = cmd[1] & 1023, sy = (cmd[1] >> 16) & 511, dx = cmd[2] & 1023, dy = (cmd[2] >> 16) & 511;
        int w = static_cast<int>(((cmd[3] & 0xFFFF) - 1) & 1023) + 1;
        int h = static_cast<int>(((cmd[3] >> 16) - 1) & 511) + 1;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                u16 px = vram[static_cast<size_t>((sy + y) & 511) * 1024 + ((sx + x) & 1023)];
                u16& d = vram[static_cast<size_t>((dy + y) & 511) * 1024 + ((dx + x) & 1023)];
                if (!(checkMask && (d & 0x8000))) d = px | setMask;
            }
        bool srcIn = inFrameBuffer(sx, sy) && inFrameBuffer(std::min(sx + w - 1, 1023), std::min(sy + h - 1, 511));
        if (scale > 1 && !srcIn) {
            // the source is a texture area that is not mirrored: scale up the result
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    int px = (dx + x) & 1023, py = (dy + y) & 511;
                    if (inFrameBuffer(px, py)) fillHi(px, py, vram[static_cast<size_t>(py) * 1024 + px]);
                }
        } else if (scale > 1) {
            // copy the scaled pixels themselves so detail drawn at high resolution survives
            const size_t stride = static_cast<size_t>(1024) * scale;
            const int hm = 1024 * scale - 1, vm = 512 * scale - 1;
            std::vector<u16> line(static_cast<size_t>(w) * scale);
            for (int y = 0; y < h * scale; ++y) {
                for (int x = 0; x < w * scale; ++x) line[x] = hires[static_cast<size_t>((sy * scale + y) & vm) * stride + ((sx * scale + x) & hm)];
                for (int x = 0; x < w * scale; ++x) {
                    u16& d = hires[static_cast<size_t>((dy * scale + y) & vm) * stride + ((dx * scale + x) & hm)];
                    if (!(checkMask && (d & 0x8000))) d = line[x] | setMask;
                }
            }
        }
    } else if (op >= 0xA0 && op < 0xE0) {
        tx = cmd[1] & 1023;
        ty = (cmd[1] >> 16) & 511;
        tw = static_cast<int>(((cmd[2] & 0xFFFF) - 1) & 1023) + 1;
        th = static_cast<int>(((cmd[2] >> 16) - 1) & 511) + 1;
        tcx = tcy = 0;
        if (op < 0xC0) loading = true;
        else storing = true;
    } else {
        switch (op) {
            case 0x02: {
                u16 c = to15(cmd[0] & 255, (cmd[0] >> 8) & 255, (cmd[0] >> 16) & 255);
                int x0 = cmd[1] & 0x3F0, y0 = (cmd[1] >> 16) & 511;
                int w = static_cast<int>(((cmd[2] & 0x3FF) + 15) & ~15u), h = (cmd[2] >> 16) & 511;
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        vram[static_cast<size_t>((y0 + y) & 511) * 1024 + ((x0 + x) & 1023)] = c;
                        if (scale > 1 && inFrameBuffer((x0 + x) & 1023, (y0 + y) & 511)) fillHi((x0 + x) & 1023, (y0 + y) & 511, c);
                    }
                break;
            }
            case 0xE1:
                setTexpage(cmd[0] & 0x1FF);
                texpage = (texpage & ~0x3E00u) | (cmd[0] & 0x3E00);
                break;
            case 0xE2:
                twMaskX = cmd[0] & 31;
                twMaskY = (cmd[0] >> 5) & 31;
                twOffX = (cmd[0] >> 10) & 31;
                twOffY = (cmd[0] >> 15) & 31;
                break;
            case 0xE3: areaX1 = cmd[0] & 1023; areaY1 = (cmd[0] >> 10) & 511; break;
            case 0xE4:
                areaX2 = cmd[0] & 1023;
                areaY2 = (cmd[0] >> 10) & 511;
                growFrameBuffer(areaX1, areaY1, areaX2, areaY2);
                break;
            case 0xE5: offX = sext11(cmd[0]); offY = sext11(cmd[0] >> 11); break;
            case 0xE6: setMask = (cmd[0] & 1) ? 0x8000 : 0; checkMask = cmd[0] & 2; break;
            default: --primitives; break;
        }
    }
    cmd.clear();
    polyline = false;
}

u32 Gpu::read() {
    if (!storing) return readLatch;
    u32 v = 0;
    for (int k = 0; k < 2 && storing; ++k) {
        v |= static_cast<u32>(vram[static_cast<size_t>((ty + tcy) & 511) * 1024 + ((tx + tcx) & 1023)]) << (16 * k);
        if (++tcx == tw) {
            tcx = 0;
            if (++tcy == th) storing = false;
        }
    }
    return v;
}

void Gpu::gp1(u32 word) {
    switch ((word >> 24) & 0x3F) {
        case 0x00:
            cmd.clear();
            loading = storing = false;
            displayEnabled = false;
            dispX = dispY = 0;
            displayMode = 0;
            dmaDirection = 0;
            texpage = 0;
            setTexpage(0);
            twMaskX = twMaskY = twOffX = twOffY = 0;
            areaX1 = areaY1 = 0;
            areaX2 = 1023;
            areaY2 = 511;
            offX = offY = 0;
            setMask = 0;
            checkMask = false;
            break;
        case 0x01: cmd.clear(); loading = false; break;
        case 0x03: displayEnabled = !(word & 1); break;
        case 0x04: dmaDirection = word & 3; break;
        case 0x05: dispX = word & 1023; dispY = (word >> 10) & 511; trackDisplay(); break;
        case 0x06: hRange = word & 0xFFFFFF; break;
        case 0x07: vRange = word & 0xFFFFF; break;
        case 0x08: displayMode = word & 0xFF; trackDisplay(); break;
        case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
            switch (word & 7) {
                case 2: readLatch = static_cast<u32>(twMaskX | (twMaskY << 5) | (twOffX << 10) | (twOffY << 15)); break;
                case 3: readLatch = static_cast<u32>(areaX1 | (areaY1 << 10)); break;
                case 4: readLatch = static_cast<u32>(areaX2 | (areaY2 << 10)); break;
                case 5: readLatch = static_cast<u32>((offX & 0x7FF) | ((offY & 0x7FF) << 11)); break;
                case 7: readLatch = 2; break;
                default: break;
            }
            break;
        default: break;
    }
}

u32 Gpu::status(const Machine& m) const {
    u32 s = texpage & 0x7FF;
    if (setMask) s |= 1u << 11;
    if (checkMask) s |= 1u << 12;
    s |= (displayMode & 0x40) << 10;          // hres2
    s |= (displayMode & 0x03) << 17;          // hres1
    s |= (displayMode & 0x04) << 17;          // vres
    s |= (displayMode & 0x08) << 17;          // PAL
    s |= (displayMode & 0x10) << 17;          // 24-bit
    s |= (displayMode & 0x20) << 17;          // interlace
    if (!displayEnabled) s |= 1u << 23;
    s |= 0x1C000000;                          // ready for commands, VRAM reads and DMA
    s |= dmaDirection << 29;
    u64 inFrame = m.now() % kCyclesPerFrame;
    u32 scanline = static_cast<u32>(inFrame / (kCyclesPerFrame / 263));
    bool interlaced = (displayMode & 0x24) == 0x24;
    if (scanline < 240) {
        if (interlaced ? (m.frame & 1) : (scanline & 1)) s |= 0x80000000u;
    }
    if (interlaced && (m.frame & 1)) s |= 1u << 13;
    return s;
}

void Gpu::display(std::vector<u8>& rgb, int& width, int& height) const {
    static const int widths[4] = {256, 320, 512, 640};
    width = (displayMode & 0x40) ? 368 : widths[displayMode & 3];
    height = (displayMode & 0x24) == 0x24 ? 480 : 240;
    const u16* pixels;
    int stride, fx, fy, fw, fh;
    if (frame(pixels, stride, fx, fy, fw, fh)) {
        width *= scale;
        height *= scale;
        rgb.assign(static_cast<size_t>(width) * height * 3, 0);
        if (!displayEnabled) return;
        for (int y = 0; y < fh; ++y)
            for (int x = 0; x < fw; ++x) {
                u16 p = pixels[static_cast<size_t>(fy + y) * stride + fx + x];
                u8* out = &rgb[(static_cast<size_t>(y) * width + x) * 3];
                out[0] = static_cast<u8>((p & 31) << 3);
                out[1] = static_cast<u8>(((p >> 5) & 31) << 3);
                out[2] = static_cast<u8>(((p >> 10) & 31) << 3);
            }
        return;
    }
    rgb.assign(static_cast<size_t>(width) * height * 3, 0);
    if (!displayEnabled) return;
    const u8* bytes = reinterpret_cast<const u8*>(vram.data());
    for (int y = 0; y < height; ++y) {
        int vy = (dispY + y) & 511;
        for (int x = 0; x < width; ++x) {
            u8* out = &rgb[(static_cast<size_t>(y) * width + x) * 3];
            size_t off = static_cast<size_t>(vy) * 2048 + ((static_cast<size_t>(dispX) * 2 + static_cast<size_t>(x) * 3) & 2047);
            out[0] = bytes[off];
            out[1] = bytes[(off + 1) & (1024 * 512 * 2 - 1)];
            out[2] = bytes[(off + 2) & (1024 * 512 * 2 - 1)];
        }
    }
}

}  // namespace fe
