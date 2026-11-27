// Software GPU: 1024x512 15-bit VRAM, GP0 drawing commands, GP1 display
// control. Rendering is at native resolution and not pixel-exact against
// hardware in edge rules and dithering.
#pragma once

#include <algorithm>
#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "psx/Machine.h"
#include "psx/HiresBackend.h"
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
    // Draw the high-resolution picture with `backend` (on the graphics card)
    // instead of on the CPU: the CPU copy `hires` is not kept then, and
    // frame() only gives the picture at the console's resolution.
    void setBackend(HiresBackend* backend);
    HiresBackend* hw = nullptr;
    // The displayed part of the (scaled) frame buffer for 15-bit display
    // modes: `pixels` has `stride` pixels per row. Returns false in 24-bit
    // modes, where display() must be used.
    bool frame(const u16*& pixels, int& stride, int& x, int& y, int& width, int& height) const;
    // The displayed rectangle in native pixels; false in 24-bit modes.
    bool displayArea(int& x, int& y, int& width, int& height) const;

    void gp0(u32 word);
    // A vertex word whose precise screen position (before rounding) is known:
    // the high-resolution copy places the vertex there.
    void gp0Precise(u32 word, float x, float y, float z = 0);
    u64 preciseVertices = 0, vertices = 0;
    // Set by the machine every frame: the precise positions of this frame and
    // the last are reused for vertices drawn at the same whole-pixel point.
    u32 frameTag = 0;

    // ---- 60 fps: in-between frames ----------------------------------------
    // The game draws a new picture every second display frame. With
    // `recordPasses` on, each picture it draws (a "pass": everything drawn
    // while the drawing area is one buffer) is kept: the buffer as it was
    // just before the first 3D polygon (background and 2D are already in it),
    // copied into the second Gpu's picture at the same place, then every
    // command after that, with the vertices' precise positions. The second
    // Gpu draws the picture again with each 3D polygon moved part of the way
    // back towards the same polygon in the picture before: the frame in between.
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
        u64 serial = 0;                         // which snapshot (it is kept in the second Gpu's picture)
        Regs regs{};
        std::vector<u32> words;                 // commands from the snapshot on
        std::vector<std::array<float, 3>> precise;  // per word: precise position and depth, or NaN
    };
    bool recordPasses = false;
    Gpu* snapshotTo = nullptr;                  // the Gpu that draws the in-between frames
    std::function<void(int)> beforeSnapshot;    // called before a snapshot (of the area at this top line) is written into it
    u64 passSerial = 0;
    // which snapshot the second Gpu's picture holds, per drawing area (by its top line); main thread only
    std::unordered_map<int, u64> snapshotHeld;
    std::deque<std::shared_ptr<const Pass>> passes;  // the last few, oldest first (the newest is being drawn)
    // The newest finished picture with a snapshot, and the one before it.
    bool newestPair(std::shared_ptr<const Pass>& a, std::shared_ptr<const Pass>& b) const;
    // The picture being shown (the newest finished one drawn where the display is), and the one before it.
    bool shownPair(std::shared_ptr<const Pass>& a, std::shared_ptr<const Pass>& b) const;
    // On the second Gpu, from any one thread: draws `b` again from its
    // snapshot (which must still be in this Gpu's picture) with each 3D
    // polygon moved `t` (0..1) of the way from where it is in `a`. `vram` is
    // a copy of video memory (textures). False when the two pictures do not
    // belong together (a cut: too few polygons match); `matched` is the share
    // of 3D polygons that matched, in percent.
    bool drawBetween(const Pass& a, const Pass& b, float t, const std::vector<u16>& vram, int& matched);
    // The second Gpu shows what the main one shows (display position and mode).
    void showLike(const Gpu& source);
    bool ghost = false;  // this Gpu draws in-between frames: high resolution only, no movie tracking, own workers
    // Draw at the positions given with gp0Precise() instead of the whole
    // pixels in the commands (the frames in between: halfway positions).
    // Otherwise those positions only tell which polygons are 3D.
    bool renderPrecise = false;
    const Gpu* videoFrom = nullptr;  // where it reads the enlarged movie pictures (the main Gpu's)
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
        float fx = 0, fy = 0, fz = 0;  // the precise position (native pixels, offset included) and depth (0 unknown)
    };
    void toBackend(const Vertex& a, const Vertex& b, const Vertex& c, bool textured, bool semi, bool raw);
    HwState hwState(bool textured, bool semi, bool raw) const;
    void dirtyRows(int y1, int y2) {
        if (hw) hw->dirty(std::clamp(y1, 0, 511), std::clamp(y2, 0, 511));
    }
    static constexpr int kNoPrecise = -0x40000000;
    int commandLength(u32 word) const;
    Regs saveRegs() const;
    void loadRegs(const Regs& r);
    void recordWord(u32 word, float x, float y, float z);
    Pass& current() { return const_cast<Pass&>(*passes.back()); }
    const std::vector<u8>& videoStates() const { return videoFrom ? videoFrom->videoState : videoState; }
    const std::vector<u16>& videoPixels() const { return videoFrom ? videoFrom->video : video; }
    void startPass();
    // Copies a native rectangle of the high-resolution picture to the same
    // place in another Gpu's, on the worker threads.
    void copyHiresTo(Gpu& to, int x1, int y1, int x2, int y2) const;
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
    std::vector<std::array<float, 3>> cmdPrecise;  // x, y, depth (0 unknown)
    struct CachedVertex {
        float x, y, z;  // native pixels, offset included
        u32 frame;
    };
    std::unordered_map<u32, CachedVertex> preciseCache;  // native (x, y) -> high-resolution position
    bool pendingPrecise = false;  // the word being added comes with a precise position
    float pendingX = 0, pendingY = 0, pendingZ = 0;
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
