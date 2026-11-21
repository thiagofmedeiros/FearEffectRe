// Minimal Win32 window: shows the GPU's picture at 4:3, reads the keyboard
// as a digital pad and paces the game to the console's frame rate.
#pragma once

#include <cstdint>
#include <vector>

namespace app {

class Window {
public:
    struct Impl;
    // Client area of width x height pixels, or a borderless window covering
    // the screen. The picture is scaled to the largest 4:3 area that fits.
    // `display` is an index into app::listDisplays().
    Window(int width, int height, bool fullscreen, int display = 0);
    ~Window();

    // Presents one frame and waits for the next frame time. Returns false
    // once the window has been closed.
    bool present(const std::vector<std::uint8_t>& rgb, int width, int height);
    // Pad buttons held right now, one bit per button (1 = pressed), in the
    // PlayStation's bit order.
    std::uint16_t buttons() const;
    bool fastForward() const;
    // 1..4 when F2..F5 was pressed since the last call (insert that disc), else 0.
    int discRequest();
    // Queues stereo 44100 Hz samples for playback.
    void audio(const std::vector<std::int16_t>& stereo);

private:
    Impl* impl_;
};

}  // namespace app
