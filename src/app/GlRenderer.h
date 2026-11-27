// The high-resolution picture drawn on the graphics card (OpenGL 3.3), after
// the design of project-helix's gpu_hw.c (MIT): video memory at the render
// scale is a framebuffer; the console's own video memory is an integer
// texture, from which the fragment shader reads 4- and 8-bit textures through
// their palettes; 15-bit textures (movies, backgrounds, pictures drawn by the
// game) are read from the high-resolution copy, so they keep their detail.
// The Gpu still draws everything into video memory at the console's
// resolution: that is what the game sees.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "psx/Gpu.h"
#include "psx/HiresBackend.h"

namespace app {

class GlRenderer : public fe::HiresBackend {
public:
    // Needs a current OpenGL context (3.3 or later). Check ok() afterwards.
    GlRenderer(fe::Gpu& gpu, int scale);
    ~GlRenderer() override;
    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }

    // fe::HiresBackend
    void triangle(const fe::HwVertex v[3], const fe::HwState& s) override;
    void fill(int x, int y, int w, int h, fe::u16 colour) override;
    void copy(int sx, int sy, int dx, int dy, int w, int h) override;
    void upload(int x, int y, int w, int h) override;
    void uploadHires(int x, int y, int w, int h, const fe::u16* pixels, size_t stride) override;
    void dirty(int y1, int y2) override;
    void snapshot(int x1, int y1, int x2, int y2) override;
    void beginBetween(int x1, int y1, int x2, int y2) override;
    void endBetween() override;

    // Draws everything still batched.
    void flush();
    // Draws a native rectangle of the picture (or of the frame in between)
    // into the window's viewport (vx, vy, vw, vh, in window pixels, origin
    // bottom left); `key`: black is see-through (the HUD over the picture).
    void show(bool between, int x, int y, int w, int h, int vx, int vy, int vw, int vh, bool key);
    // A native rectangle of the picture at the render scale, as 8-bit RGB rows from the top.
    void read(bool between, int x, int y, int w, int h, std::vector<std::uint8_t>& rgb, int& outW, int& outH);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool ok_ = false;
    std::string error_;
};

}  // namespace app
