#include "Gpu.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
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

// Below this many output pixels a primitive is drawn on the calling thread:
// waking the workers would cost more than it saves.
constexpr s64 kParallelPixels = 40000;

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

void Gpu::setUpscaler(std::shared_ptr<VideoUpscaler> u) {
    upscaler = std::move(u);
    video.clear();
    videoState.clear();
    videoDirty.clear();
    if (!upscaler) return;
    if (scale == 1) {
        upscaler.reset();
        return;
    }
    video.assign(static_cast<size_t>(1024) * scale * 512 * scale, 0);
    videoState.assign(static_cast<size_t>(1024) * 512, 0);
}

Gpu::~Gpu() = default;

void Gpu::videoInvalidate(int x, int y, int w, int h) {
    if (videoState.empty() || ghost) return;
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) videoState[static_cast<size_t>((y + j) & 511) * 1024 + ((x + i) & 1023)] = 0;
}

void Gpu::videoUploaded(int x, int y, int w, int h) {
    if (videoState.empty() || scale == 1 || ghost) return;
    // pictures come as slices 16 pixels wide; small uploads are textures and CLUTs
    if (w * h < 1024 || x + w > 1024 || y + h > 512) {
        videoInvalidate(x, y, w, h);
        return;
    }
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) videoState[static_cast<size_t>(y + j) * 1024 + x + i] = 1;
    if (videoBreak || videoDirty.empty() || videoDirty.back().y2 - videoDirty.back().y1 != h - 1) ++videoPicture;
    videoBreak = false;
    videoDirty.push_back({x, y, x + w - 1, y + h - 1, videoPicture});
}

void Gpu::videoFlush(int tx1, int ty1, int tx2, int ty2) {
    videoBreak = true;
    if (videoDirty.empty()) return;
    // A picture arrives as slices 16 pixels wide, left to right, but its parts
    // can sit in separate areas of video memory (a 320-pixel picture as 256 +
    // 64, not always on the same rows). The slices uploaded back to back are
    // put side by side again so the model sees the whole picture and the
    // parts meet without a seam.
    std::vector<std::vector<VideoRect>> pictures;
    for (const VideoRect& r : videoDirty) {
        if (pictures.empty() || pictures.back().front().picture != r.picture) pictures.push_back({});
        pictures.back().push_back(r);
    }
    // only the pictures the texture being drawn uses: the next picture may
    // still be arriving in another buffer
    videoDirty.clear();
    auto used = [&](const std::vector<VideoRect>& g) {
        for (const VideoRect& r : g)
            if (r.x1 <= tx2 && r.x2 >= tx1 && r.y1 <= ty2 && r.y2 >= ty1) return true;
        return false;
    };
    for (auto it = pictures.begin(); it != pictures.end();) {
        if (used(*it)) {
            ++it;
            continue;
        }
        videoDirty.insert(videoDirty.end(), it->begin(), it->end());
        it = pictures.erase(it);
    }
    // Each picture is enlarged before it is drawn, so the frame on screen is
    // always the one the game decoded (the upscaler must answer quickly: a
    // cache lookup or the fast model).
    const int K = VideoUpscaler::kFactor, S = scale;
    for (const auto& slices : pictures) {
        const int h = slices.front().y2 - slices.front().y1 + 1;
        int w = 0;
        for (const VideoRect& r : slices) w += r.x2 - r.x1 + 1;
        videoRgb.resize(static_cast<size_t>(w) * h * 3);
        for (int y = 0; y < h; ++y) {
            u8* o = &videoRgb[static_cast<size_t>(y) * w * 3];
            for (const VideoRect& r : slices)
                for (int x = r.x1; x <= r.x2; ++x, o += 3) {
                    u16 p = vram[static_cast<size_t>(r.y1 + y) * 1024 + x];
                    int c0 = p & 31, c1 = (p >> 5) & 31, c2 = (p >> 10) & 31;
                    o[0] = static_cast<u8>((c0 << 3) | (c0 >> 2));
                    o[1] = static_cast<u8>((c1 << 3) | (c1 >> 2));
                    o[2] = static_cast<u8>((c2 << 3) | (c2 >> 2));
                }
        }
        const bool ok = upscaler->upscale(videoRgb.data(), w, h, videoOut) && videoOut.size() == videoRgb.size() * K * K;
        // where each output column lands: slice by slice in video memory
        std::vector<int> column(static_cast<size_t>(w));
        {
            int c = 0;
            for (const VideoRect& r : slices)
                for (int x = r.x1; x <= r.x2; ++x) column[static_cast<size_t>(c++)] = x;
        }
        if (ok) {
            // the 4x picture brought to the render scale, rows spread over the worker threads
            const int iw = w * K, ih = h * K, ow = w * S, oh = h * S;
            const size_t vstride = static_cast<size_t>(1024) * S;
            // per output column: where it reads in the 4x picture and where it
            // lands in video memory (the slices may sit apart)
            struct Col {
                int c0, c1, fx;
                size_t dst;  // offset of the column within a scaled row
                int y1;      // first native row of its slice
            };
            std::vector<Col> cols(static_cast<size_t>(ow));
            for (int x = 0; x < ow; ++x) {
                const int nx = column[static_cast<size_t>(x / S)];
                int y1 = slices.front().y1;
                for (const VideoRect& r : slices)
                    if (nx >= r.x1 && nx <= r.x2) y1 = r.y1;
                const int sx = ((2 * x + 1) * K * 128) / S - 128;  // source position, 8 bits of fraction
                const int c0 = std::clamp(sx >> 8, 0, iw - 1);
                cols[static_cast<size_t>(x)] = {c0, std::min(c0 + 1, iw - 1), sx < 0 ? 0 : sx & 255,
                                                static_cast<size_t>(nx) * S + static_cast<size_t>(x % S), y1};
            }
            auto job = [&](int band) {
                const int bands = rowPool().bands();
                for (int y = oh * band / bands; y < oh * (band + 1) / bands; ++y) {
                    const int sy = ((2 * y + 1) * K * 128) / S - 128;
                    const int r0 = std::clamp(sy >> 8, 0, ih - 1), r1 = std::min(r0 + 1, ih - 1), fy = sy < 0 ? 0 : sy & 255;
                    const u8* top = &videoOut[static_cast<size_t>(r0) * iw * 3];
                    const u8* bot = &videoOut[static_cast<size_t>(r1) * iw * 3];
                    for (const Col& c : cols) {
                        const u8 *a = top + c.c0 * 3, *b = top + c.c1 * 3, *cc = bot + c.c0 * 3, *d = bot + c.c1 * 3;
                        u16 v = 0;
                        for (int ch = 0; ch < 3; ++ch) {
                            const int t = a[ch] * (256 - c.fx) + b[ch] * c.fx, u = cc[ch] * (256 - c.fx) + d[ch] * c.fx;
                            const int p = (t * (256 - fy) + u * fy + (1 << 15)) >> 16;
                            v |= static_cast<u16>(std::min((p + 4) >> 3, 31) << (ch * 5));
                        }
                        video[(static_cast<size_t>(c.y1) * S + y) * vstride + c.dst] = v;
                    }
                }
            };
            rowPool().run(job);
        }
        for (const VideoRect& r : slices)
            for (int y = r.y1; y <= r.y2; ++y)
                for (int x = r.x1; x <= r.x2; ++x) {
                    u8& st = videoState[static_cast<size_t>(y) * 1024 + x];
                    if (st == 1) st = ok ? 2 : 0;
                }
    }
}

bool Gpu::videoTexel(s64 x, s64 y, u16& out) const {
    const int nx = static_cast<int>(x >> 16) & 1023, ny = static_cast<int>(y >> 16) & 511;
    if (videoState[static_cast<size_t>(ny) * 1024 + nx] != 2) return false;
    // `video` is already at the render scale: the nearest of its pixels
    const int hx = std::min(static_cast<int>((x * scale) >> 16), 1024 * scale - 1);
    const int hy = std::min(static_cast<int>((y * scale) >> 16), 512 * scale - 1);
    out = video[static_cast<size_t>(hy) * 1024 * scale + hx];
    return true;
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
    // A triangle placed at precise positions no longer lines up with its
    // texture to the texel: keep the texture coordinates inside its own
    // range, or the edges pick up the texels next to it (lines across faces).
    const bool precise = a.hx != kNoPrecise || b.hx != kNoPrecise || c.hx != kNoPrecise;
    const s32 uLo = std::min({a.u, b.u, c.u}) << 16, vLo = std::min({a.v, b.v, c.v}) << 16;
    const s32 uHi = std::max(uLo, (std::max({a.u, b.u, c.u}) << 16) - 1), vHi = std::max(vLo, (std::max({a.v, b.v, c.v}) << 16) - 1);
    auto clampU = [&](s32 u) { return precise ? std::clamp(u, uLo, uHi) : u; };
    auto clampV = [&](s32 v) { return precise ? std::clamp(v, vLo, vHi) : v; };
    for (Vertex* v : {&a, &b, &c}) {
        if (v->hx != kNoPrecise) {
            v->x = v->hx;
            v->y = v->hy;
        } else {
            v->x *= S;
            v->y *= S;
        }
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

    if (direct && !videoDirty.empty()) videoFlush(tpx, tpy, tpx + 255, tpy + 255);
    const bool vid = direct && !videoState.empty();

    // The columns of row y inside all three edges: each edge value is linear
    // in x, so its sign changes at most once along the row. Only that span is
    // walked, instead of testing every pixel of the bounding box.
    auto span = [&](int y, int& xs, int& xe) {
        xs = minX;
        xe = maxX;
        const s64 w[3] = {row0 + dy0 * (y - minY), row1 + dy1 * (y - minY), row2 + dy2 * (y - minY)};
        const s64 d[3] = {dx0, dx1, dx2};
        for (int e = 0; e < 3 && xs <= xe; ++e) {
            if (d[e] == 0) {
                if (w[e] < 0) xe = xs - 1;
            } else if (d[e] > 0) {
                if (w[e] < 0) {
                    const s64 first = minX + (-w[e] + d[e] - 1) / d[e];  // first step where the value is >= 0
                    xs = first > xe ? xe + 1 : std::max(xs, static_cast<int>(first));
                }
            } else {
                const s64 last = w[e] < 0 ? -1 : w[e] / -d[e];  // last step where the value is still >= 0
                if (last < 0) xe = xs - 1;
                else if (minX + last < xe) xe = static_cast<int>(minX + last);
            }
        }
    };

    // Fast path: textured, one colour, no blending and no mask test (the
    // title screen's full-screen layers). Same results as texel() + plotHi().
    const bool opaqueFlat = textured && !gouraud && !semi && !checkMask;
    const int winKeepX = ~(twMaskX * 8), winSetX = (twOffX & twMaskX) * 8;
    const int winKeepY = ~(twMaskY * 8), winSetY = (twOffY & twMaskY) * 8;
    u16 clut[256];
    u16 modR[32], modG[32], modB[32];
    if (opaqueFlat) {
        if (depth < 2)
            for (int i = 0; i < 256; ++i) clut[i] = vram[static_cast<size_t>(clutY) * 1024 + ((clutX + i) & 1023)];
        for (int i = 0; i < 32; ++i) {
            modR[i] = static_cast<u16>(clamp255((i * a.r) >> 4) >> 3);
            modG[i] = static_cast<u16>((clamp255((i * a.g) >> 4) >> 3) << 5);
            modB[i] = static_cast<u16>((clamp255((i * a.b) >> 4) >> 3) << 10);
        }
    }

    auto rows = [&](int yFrom, int yTo, int yStep) {
    for (int y = yFrom; y <= yTo; y += yStep) {
        int xs, xe;
        span(y, xs, xe);
        if (xs > xe) continue;
        const int skip = xs - minX;
        u16* out = &hires[static_cast<size_t>(y) * stride + xs];
        if (direct) {
            // 15-bit texture drawn as it is: the streamed backgrounds
            s32 u = start(pu, y, half) + pu.step * skip, v = start(pv, y, half) + pv.step * skip;
            for (int x = xs; x <= xe; ++x, ++out, u += pu.step, v += pv.step) {
                const s32 cu = clampU(u), cv = clampV(v);
                const int ty = (tpy + ((cv >> 16) & 255)) & 511, tx = (tpx + ((cu >> 16) & 255)) & 1023;
                u16 t = vram[static_cast<size_t>(ty) * 1024 + tx];
                if (!t) continue;
                u16 e;
                if (vid && videoTexel((static_cast<s64>(tx) << 16) | (cu & 0xFFFF), (static_cast<s64>(ty) << 16) | (cv & 0xFFFF), e))
                    t = static_cast<u16>((t & 0x8000) | e);
                *out = t | setMask;
            }
            continue;
        }
        if (!textured && !gouraud && !semi && !checkMask) {
            const u16 colour = flat | setMask;
            std::fill(out, out + (xe - xs + 1), colour);
            continue;
        }
        if (opaqueFlat) {
            // one colour for the whole triangle and nothing to blend: the
            // colour is applied through the tables, and the texture lookup
            // has its texture-window and depth decisions made once
            s32 u = start(pu, y, half) + pu.step * skip, v = start(pv, y, half) + pv.step * skip;
            for (int x = xs; x <= xe; ++x, ++out, u += pu.step, v += pv.step) {
                const int tu = (((clampU(u) >> 16) & winKeepX) | winSetX) & 255, tv = (((clampV(v) >> 16) & winKeepY) | winSetY) & 255;
                const u16* texRow = &vram[static_cast<size_t>((tpy + tv) & 511) * 1024];
                u16 t;
                if (depth == 0) t = clut[(texRow[(tpx + tu / 4) & 1023] >> ((tu & 3) * 4)) & 15];
                else if (depth == 1) t = clut[(texRow[(tpx + tu / 2) & 1023] >> ((tu & 1) * 8)) & 255];
                else t = texRow[(tpx + tu) & 1023];
                if (!t) continue;
                if (!raw) t = static_cast<u16>((t & 0x8000) | modR[t & 31] | modG[(t >> 5) & 31] | modB[(t >> 10) & 31]);
                *out = t | setMask;
            }
            continue;
        }
        s32 u = start(pu, y, half) + pu.step * skip, v = start(pv, y, half) + pv.step * skip;
        s32 r = start(pr, y, 0) + pr.step * skip, g = start(pg, y, 0) + pg.step * skip, bl = start(pb, y, 0) + pb.step * skip;
        for (int x = xs; x <= xe; ++x, u += pu.step, v += pv.step, r += pr.step, g += pg.step, bl += pb.step) {
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
            u16 t = texel(clampU(u) >> 16, clampV(v) >> 16);
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
    if (pixels < kParallelPixels || pool.bands() == 1) {
        rows(minY, maxY, 1);
        return;
    }
    // rows dealt out in turn, so a thread does not get only the narrow tip
    // of the triangle while another gets its wide base
    const int n = pool.bands();
    pool.run([&](int band) { rows(minY + band, maxY, n); });
}

void Gpu::rectHi(int x, int y, int w, int h, int u0, int v0, int r, int g, int b, bool textured, bool semi, bool raw) {
    const int S = scale;
    u16 flat = to15(r, g, b);
    int y0 = std::max(y, areaY1), y1 = std::min(y + h - 1, areaY2);
    int x0 = std::max(x, areaX1), x1 = std::min(x + w - 1, areaX2);
    const bool neutral = r == 0x80 && g == 0x80 && b == 0x80;
    const bool vid = textured && depth >= 2 && (raw || neutral) && !semi && !checkMask && !twMaskX && !twMaskY &&
                     !videoState.empty();
    if (vid && !videoDirty.empty()) videoFlush(tpx, tpy, tpx + 255, tpy + 255);
    if (vid) {
        // a 15-bit texture drawn unchanged: rows of the enlarged sprite in parallel
        if (y0 > y1 || x0 > x1) return;
        const size_t stride = static_cast<size_t>(1024) * S;
        const int rows = (y1 - y0 + 1) * S;
        auto job = [&](int band) {
            const int bands = rowPool().bands();
            for (int r = rows * band / bands; r < rows * (band + 1) / bands; ++r) {
                const int py = y0 + r / S, j = r % S;
                const int tv = (tpy + ((v0 + (py - y)) & 255)) & 511;
                const s64 fy = (static_cast<s64>(tv) << 16) + ((2 * j + 1) << 15) / S;
                u16* row = &hires[static_cast<size_t>(py * S + j) * stride];
                for (int px = x0; px <= x1; ++px) {
                    const int tu = (tpx + ((u0 + (px - x)) & 255)) & 1023;
                    const u16 t = vram[static_cast<size_t>(tv) * 1024 + tu];
                    if (!t) continue;
                    u16* out = row + static_cast<size_t>(px) * S;
                    if (videoState[static_cast<size_t>(tv) * 1024 + tu] != 2) {
                        for (int i = 0; i < S; ++i) out[i] = t | setMask;
                        continue;
                    }
                    for (int i = 0; i < S; ++i) {
                        u16 e;
                        const s64 fx = (static_cast<s64>(tu) << 16) + ((2 * i + 1) << 15) / S;
                        out[i] = static_cast<u16>((videoTexel(fx, fy, e) ? e : t & 0x7FFF) | (t & 0x8000) | setMask);
                    }
                }
            }
        };
        if (rows * (x1 - x0 + 1) * S >= 4096) rowPool().run(job);
        else
            for (int band = 0; band < rowPool().bands(); ++band) job(band);
        return;
    }
    if (y0 > y1 || x0 > x1) return;
    // each native row writes only its own S output rows, so bands of rows can
    // be drawn in parallel
    auto body = [&](int from, int to) {
    for (int py = from; py <= to; ++py)
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
    };
    const int total = y1 - y0 + 1;
    RowPool& pool = rowPool();
    if (static_cast<s64>(total) * (x1 - x0 + 1) * S * S < kParallelPixels || pool.bands() == 1 || total < 2) {
        body(y0, y1);
        return;
    }
    const int n = pool.bands();
    pool.run([&](int band) {
        const int from = y0 + static_cast<int>(static_cast<s64>(total) * band / n);
        const int to = y0 + static_cast<int>(static_cast<s64>(total) * (band + 1) / n) - 1;
        if (from <= to) body(from, to);
    });
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
    if (!videoState.empty() && !ghost) videoState[static_cast<size_t>(y) * 1024 + x] = 0;
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

    // The edge values step by a constant along a row, so they and the
    // weighted attribute sums are updated by addition. Inside the triangle
    // every weight is >= 0 and they add up to `area` (< 2^21 for the sizes
    // let through above), so the sums of weight * 8-bit attribute stay below
    // 2^29 and 32-bit unsigned division gives the same quotient.
    const s64 ex0 = -(c.y - b.y), ex1 = -(a.y - c.y), ex2 = -(b.y - a.y);
    // n / area as a multiply and shift (Granlund and Montgomery): with
    // k = 29 + ceil(log2 area) and m = ceil(2^k / area), (n * m) >> k equals
    // n / area for every n < 2^29.
    int lg = 0;
    while ((s64{1} << lg) < area) ++lg;
    const int k = 29 + lg;
    const u64 m = ((u64{1} << k) + static_cast<u64>(area) - 1) / static_cast<u64>(area);
    auto div = [&](u32 n) { return static_cast<int>((static_cast<u64>(n) * m) >> k); };
    for (int y = minY; y <= maxY; ++y) {
        s64 w0 = edge(b, c, minX, y), w1 = edge(c, a, minX, y), w2 = edge(a, b, minX, y);
        for (int x = minX; x <= maxX; ++x, w0 += ex0, w1 += ex1, w2 += ex2) {
            if (w0 + b0 < 0 || w1 + b1 < 0 || w2 + b2 < 0) continue;
            const u32 q0 = static_cast<u32>(w0), q1 = static_cast<u32>(w1), q2 = static_cast<u32>(w2);
            int r = a.r, g = a.g, bl = a.b;
            if (gouraud) {
                r = div(q0 * a.r + q1 * b.r + q2 * c.r);
                g = div(q0 * a.g + q1 * b.g + q2 * c.g);
                bl = div(q0 * a.b + q1 * b.b + q2 * c.b);
            }
            if (!textured) {
                plot(x, y, to15(r, g, bl), semi);
                continue;
            }
            int u = div(q0 * a.u + q1 * b.u + q2 * c.u);
            int v = div(q0 * a.v + q1 * b.v + q2 * c.v);
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
        const size_t at = i;
        ++vertices;
        u32 xy = cmd[i++];
        v[k].x = sext11(xy) + offX;
        v[k].y = sext11(xy >> 16) + offY;
        // All vertices that round to the same native pixel in a frame share
        // one precise position: the first one drawn there. A model is built
        // from parts transformed one by one, so where two parts meet the same
        // point is computed twice with slightly different results; rounded
        // they landed on one pixel, precise they would leave a crack.
        const u32 key = (static_cast<u32>(v[k].x) & 0xFFFF) | (static_cast<u32>(v[k].y) << 16);
        const bool hasPrecise = at < cmdPrecise.size() && !std::isnan(cmdPrecise[at][0]);
        if (hasPrecise || !preciseCache.empty()) {
            auto it = preciseCache.find(key);
            if (it != preciseCache.end() && it->second.frame + 1 >= frameTag) {
                v[k].hx = it->second.x;
                v[k].hy = it->second.y;
                if (hasPrecise) ++preciseVertices;
            } else if (hasPrecise) {
                v[k].hx = static_cast<int>(std::lround((cmdPrecise[at][0] + offX) * scale));
                v[k].hy = static_cast<int>(std::lround((cmdPrecise[at][1] + offY) * scale));
                ++preciseVertices;
                if (preciseCache.size() > 65536) preciseCache.clear();
                preciseCache[key] = {v[k].hx, v[k].hy, frameTag};
            }
        }
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
    if (recordPasses && !passes.empty() && !passes.back().snapped && !passes.back().done && scale > 1) {
        bool is3d = false;
        for (int k = 0; k < n; ++k) is3d = is3d || v[k].hx != kNoPrecise;
        if (is3d) {
            Pass& pass = passes.back();
            const int S = scale, x1 = std::max(pass.x1, 0), y1 = std::max(pass.y1, 0);
            const int x2 = std::min(pass.x2, 1023), y2 = std::min(pass.y2, 511);
            if (x2 >= x1 && y2 >= y1) {
                const size_t w = static_cast<size_t>(x2 - x1 + 1) * S, stride = static_cast<size_t>(1024) * S;
                pass.pixels.resize(w * static_cast<size_t>(y2 - y1 + 1) * S);
                for (int y = y1 * S; y < (y2 + 1) * S; ++y)
                    std::copy_n(&hires[static_cast<size_t>(y) * stride + static_cast<size_t>(x1) * S], w,
                                &pass.pixels[static_cast<size_t>(y - y1 * S) * w]);
                pass.regs = saveRegs();
                pass.snapped = true;
                // this command's words came before the snapshot: they start the record
                for (size_t i = 0; i < cmd.size(); ++i)
                    recordWord(cmd[i], i < cmdPrecise.size() ? cmdPrecise[i][0] : kNan, i < cmdPrecise.size() ? cmdPrecise[i][1] : kNan);
            }
        }
    }
    if (!ghost) {
        triangle(v[0], v[1], v[2], textured, gouraud, semi, raw);
        if (quad) triangle(v[1], v[2], v[3], textured, gouraud, semi, raw);
    }
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
    if (recordPasses && !passes.empty() && passes.back().snapped && !passes.back().done)
        recordWord(word, pendingPrecise ? pendingX : kNan, pendingY);
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
                    videoUploaded(tx, ty, tw, th);
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
    if (!cmdPrecise.empty() || pendingPrecise) {
        // keep the precise positions lined up with the words
        cmdPrecise.resize(cmd.size(), {kNan, kNan});
        if (pendingPrecise) cmdPrecise.back() = {pendingX, pendingY};
    }
    pendingPrecise = false;
    if (!polyline && static_cast<int>(cmd.size()) >= cmdNeed) execute();
}

void Gpu::gp0Precise(u32 word, float x, float y) {
    pendingPrecise = true;
    pendingX = x;
    pendingY = y;
    gp0(word);
    pendingPrecise = false;
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
        videoInvalidate(dx, dy, w, h);
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
                        if (!videoState.empty() && !ghost) videoState[static_cast<size_t>((y0 + y) & 511) * 1024 + ((x0 + x) & 1023)] = 0;
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
            case 0xE3:
                areaX1 = cmd[0] & 1023;
                areaY1 = (cmd[0] >> 10) & 511;
                if (recordPasses && (passes.empty() || passes.back().x1 != areaX1 || passes.back().y1 != areaY1)) startPass();
                break;
            case 0xE4:
                areaX2 = cmd[0] & 1023;
                areaY2 = (cmd[0] >> 10) & 511;
                growFrameBuffer(areaX1, areaY1, areaX2, areaY2);
                if (recordPasses && !passes.empty() && !passes.back().snapped) {
                    passes.back().x2 = areaX2;
                    passes.back().y2 = areaY2;
                }
                break;
            case 0xE5: offX = sext11(cmd[0]); offY = sext11(cmd[0] >> 11); break;
            case 0xE6: setMask = (cmd[0] & 1) ? 0x8000 : 0; checkMask = cmd[0] & 2; break;
            default: --primitives; break;
        }
    }
    cmd.clear();
    cmdPrecise.clear();
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
            cmdPrecise.clear();
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
        case 0x01: cmd.clear(); cmdPrecise.clear(); loading = false; break;
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
