// Software GPU: 1024x512 15-bit VRAM, GP0 drawing commands, GP1 display
// control. Rendering is at native resolution and not pixel-exact against
// hardware in edge rules and dithering.
#pragma once

#include <array>
#include <deque>
#include <memory>
#include <unordered_map>
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
    // A vertex word whose precise screen position (before rounding) is known:
    // the high-resolution copy places the vertex there.
    void gp0Precise(u32 word, float x, float y);
    u64 preciseVertices = 0, vertices = 0;
    // Set by the machine every frame: the precise positions of this frame and
    // the last are reused for vertices drawn at the same whole-pixel point.
    u32 frameTag = 0;

    // ---- 60 fps: in-between frames ----------------------------------------
    // The game draws a new picture every second display frame. With
    // `recordPasses` on, each picture it draws (a "pass": everything drawn
    // while the drawing area is one buffer) is kept: the buffer as it was
    // just before the first 3D polygon (background and 2D are already in it),
    // then every command after that, with the vertices' precise positions.
    // renderBetween(), called on a second Gpu, draws the newest finished
    // picture again with each 3D polygon moved part of the way back towards
    // the same polygon in the picture before: the frame in between.
    struct Regs {
        u32 texpage;
        int tpx, tpy, blend, depth, clutX, clutY;
        int twMaskX, twMaskY, twOffX, twOffY;
        int areaX1, areaY1, areaX2, areaY2, offX, offY;
        u16 setMask;
        bool checkMask;
    };
    struct Pass {
        int x1 = 0, y1 = 0, x2 = -1, y2 = -1;  // drawing area, native pixels
        bool snapped = false, done = false;
        std::vector<u16> pixels;                // high-resolution drawing area at the snapshot
        Regs regs{};
        std::vector<u32> words;                 // commands from the snapshot on
        std::vector<std::array<float, 2>> precise;  // per word: precise position, or NaN
    };
    bool recordPasses = false;
    std::deque<Pass> passes;  // the last few, oldest first
    // On the second Gpu: draws the frame `t` (0..1) of the way from the
    // picture before the one `source` shows to the one it shows. False when
    // there is nothing to draw it from or the two pictures do not belong
    // together (a cut: too few polygons match); `matched` is the share of
    // 3D polygons that matched, in percent.
    bool renderBetween(Gpu& source, float t, int& matched);
    bool ghost = false;  // this Gpu draws in-between frames: high resolution only, no movie tracking  // polygon vertices drawn, and how many at a precise position
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

    void state(StateIO& io);
private:
    struct Vertex {
        int x, y;
        int r, g, b;
        int u, v;
        int hx = kNoPrecise, hy = 0;  // position in the high-resolution copy, when known precisely
    };
    static constexpr int kNoPrecise = -0x40000000;
    int commandLength(u32 word) const;
    Regs saveRegs() const;
    void loadRegs(const Regs& r);
    void recordWord(u32 word, float x, float y);
    void startPass();
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
    // per word of `cmd`: its precise position (x, y), NaN when there is none
    std::vector<std::array<float, 2>> cmdPrecise;
    struct CachedVertex {
        int x, y;
        u32 frame;
    };
    std::unordered_map<u32, CachedVertex> preciseCache;  // native (x, y) -> high-resolution position
    bool pendingPrecise = false;  // the word being added comes with a precise position
    float pendingX = 0, pendingY = 0;
    static constexpr float kNan = __builtin_nanf("");
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
