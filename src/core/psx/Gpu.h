// Software GPU: 1024x512 15-bit VRAM, GP0 drawing commands, GP1 display
// control. Rendering is at native resolution and not pixel-exact against
// hardware in edge rules and dithering.
#pragma once

#include <vector>

#include "psx/Machine.h"

namespace fe {

struct Gpu {
    Gpu() : vram(1024 * 512, 0) {}

    void gp0(u32 word);
    void gp1(u32 word);
    u32 read();
    u32 status(const Machine& m) const;

    // The visible picture as 8-bit RGB, width*height*3 bytes.
    void display(std::vector<u8>& rgb, int& width, int& height) const;

    std::vector<u16> vram;
    u64 primitives = 0;
    bool displayEnabled = false;
    int dispX = 0, dispY = 0;
    u32 displayMode = 0;
    u32 dmaDirection = 0;

private:
    struct Vertex {
        int x, y;
        int r, g, b;
        int u, v;
    };
    int commandLength(u32 word) const;
    void execute();
    void polygon();
    void line();
    void rect();
    void triangle(Vertex a, Vertex b, Vertex c, bool textured, bool gouraud, bool semi, bool raw);
    void drawLine(Vertex a, Vertex b, bool semi);
    u16 texel(int u, int v) const;
    void plot(int x, int y, u16 color, bool semi);
    void setTexpage(u32 bits);

    std::vector<u32> cmd;
    int cmdNeed = 0;
    bool polyline = false;
    // CPU<->VRAM transfers
    int tx = 0, ty = 0, tw = 0, th = 0, tcx = 0, tcy = 0;
    bool loading = false, storing = false;
    u32 readLatch = 0;

    u32 texpage = 0;
    int tpx = 0, tpy = 0, blend = 0, depth = 0;
    int clutX = 0, clutY = 0;
    int twMaskX = 0, twMaskY = 0, twOffX = 0, twOffY = 0;
    int areaX1 = 0, areaY1 = 0, areaX2 = 1023, areaY2 = 511;
    int offX = 0, offY = 0;
    u16 setMask = 0;
    bool checkMask = false;
    u32 hRange = 0, vRange = 0;
};

}  // namespace fe
