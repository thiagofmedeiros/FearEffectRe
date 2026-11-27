// Where the high-resolution picture is drawn when it is not drawn by the CPU:
// the Gpu draws everything into video memory at the console's resolution
// (what the game sees), and hands each primitive to a backend as well, which
// draws it at the render scale (on the graphics card). Positions are in
// native pixels; the backend scales them.
#pragma once

#include "psx/Machine.h"

namespace fe {

struct HwVertex {
    float x, y;       // native pixels, drawing offset included; fractional where known precisely
    float w;          // perspective: depth relative to the triangle's nearest point (1 = flat, as the console draws)
    u8 r, g, b;       // colour (or texture modulation, 128 = unchanged)
    u8 pad;
    float u, v;       // texture coordinates, 0..255 within the texture page
};

struct HwState {
    bool textured = false, semi = false, raw = false;
    int blend = 0;            // semi-transparency: 0 B/2+F/2, 1 B+F, 2 B-F, 3 B+F/4
    int depth = 0;            // texture colours: 0 4-bit, 1 8-bit, 2 15-bit
    int tpx = 0, tpy = 0;     // texture page
    int clutX = 0, clutY = 0;
    int twMaskX = 0, twMaskY = 0, twOffX = 0, twOffY = 0;  // texture window
    bool setMask = false;
    bool precise = false;     // placed at precise positions: keep texture reads inside its own texture rectangle
    int areaX1 = 0, areaY1 = 0, areaX2 = 1023, areaY2 = 511;  // drawing area (inclusive)
};

class HiresBackend {
public:
    virtual ~HiresBackend() = default;
    virtual void triangle(const HwVertex v[3], const HwState& s) = 0;
    // Native rectangles; they never wrap (the Gpu splits them).
    virtual void fill(int x, int y, int w, int h, u16 colour) = 0;
    virtual void copy(int sx, int sy, int dx, int dy, int w, int h) = 0;
    // Pixels written to video memory by the CPU: drawn from video memory, enlarged.
    virtual void upload(int x, int y, int w, int h) = 0;
    // An enlarged picture (AI upscaling) for a native rectangle: `pixels` at
    // the render scale, `stride` pixels per row, 15-bit like video memory.
    virtual void uploadHires(int x, int y, int w, int h, const u16* pixels, size_t stride) = 0;
    // Rows of native video memory that changed (textures and palettes are read from it).
    virtual void dirty(int y1, int y2) = 0;
    // 60 fps: keep the high-resolution picture of a native rectangle aside
    // (snapshot), and later draw a frame starting from it; in between,
    // primitives go to that frame instead of the picture.
    virtual void snapshot(int x1, int y1, int x2, int y2) = 0;
    virtual void beginBetween(int x1, int y1, int x2, int y2) = 0;
    virtual void endBetween() = 0;
};

}  // namespace fe
