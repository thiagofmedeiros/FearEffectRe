// Software GPU: 1024x512 15-bit VRAM, GP0 drawing commands, GP1 display
// control. Rendering is at native resolution and not pixel-exact against
// hardware in edge rules and dithering.
#pragma once

#include <memory>
#include <vector>

#include "psx/Machine.h"
#include "psx/VideoUpscaler.h"

namespace fe {

struct Gpu {
    Gpu() : vram(1024 * 512, 0) {}
    ~Gpu();
    Gpu(const Gpu&) = delete;
    Gpu& operator=(const Gpu&) = delete;

    // Internal resolution multiplier (1..16). Above 1 every primitive is also
    // rasterised into `hires`, a copy of video memory `scale` times larger in
    // each direction, which is what gets displayed. Video memory itself stays
    // at the console's resolution so textures and read-back behave as before.
    void setScale(int factor);
    int scale = 1;
    std::vector<u16> hires;
    // Optional: when set (and scale > 1), large uploads (the movie and
    // background slices) are enlarged by it, and 15-bit textures drawn from
    // them are sampled from the enlarged copy instead of video memory.
    void setUpscaler(std::shared_ptr<VideoUpscaler> u);
    // The displayed part of the (scaled) frame buffer for 15-bit display
    // modes: `pixels` has `stride` pixels per row. Returns false in 24-bit
    // modes, where display() must be used.
    bool frame(const u16*& pixels, int& stride, int& x, int& y, int& width, int& height) const;

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
    void triangleHi(Vertex a, Vertex b, Vertex c, bool textured, bool gouraud, bool semi, bool raw);
    void rectHi(int x, int y, int w, int h, int u0, int v0, int r, int g, int b, bool textured, bool semi, bool raw);
    void plotHi(int x, int y, u16 color, bool semi);
    void fillHi(int x, int y, u16 color);  // one native pixel as a scale x scale block
    // Only the parts of video memory used as frame buffers (everything that
    // has been a drawing or display area) are kept in `hires`; texture
    // uploads elsewhere are not mirrored.
    bool inFrameBuffer(int x, int y) const { return x >= fbX1 && x <= fbX2 && y >= fbY1 && y <= fbY2; }
    void growFrameBuffer(int x1, int y1, int x2, int y2);
    void trackDisplay();
    int fbX1 = 1024, fbY1 = 512, fbX2 = -1, fbY2 = -1;
    u16 texel(int u, int v) const;

    // Enlarged pictures: `video` is video memory at the render scale (like
    // `hires`), valid where `videoState` is 2. State 1 marks pixels uploaded
    // and not enlarged yet (done when first drawn); 0 is everything else.
    struct VideoRect {
        int x1, y1, x2, y2;
        unsigned picture;  // slices uploaded back to back belong to one picture
    };
    unsigned videoPicture = 0;
    bool videoBreak = true;  // a draw happened since the last slice: the next one starts a picture
    void videoUploaded(int x, int y, int w, int h);
    void videoInvalidate(int x, int y, int w, int h);
    void videoFlush(int x1, int y1, int x2, int y2);  // enlarges the pictures touching this area
    std::vector<u8> videoRgb, videoOut;  // the picture being enlarged, before and after
    // A 15-bit texel at native position (fixed point 16.16, absolute in video
    // memory) from the enlarged copy; false where there is none.
    bool videoTexel(s64 x, s64 y, u16& out) const;
    std::shared_ptr<VideoUpscaler> upscaler;
    std::vector<u16> video;
    std::vector<u8> videoState;
    std::vector<VideoRect> videoDirty;
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
