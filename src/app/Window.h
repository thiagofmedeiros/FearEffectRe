// Minimal Win32 window: shows the GPU's picture at 4:3, reads the keyboard
// as a digital pad and paces the game to the console's frame rate.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Controls.h"

namespace app {

enum class Hotkey { SaveState, LoadState, NextSlot, PrevSlot, Controls };

class Window {
public:
    struct Impl;
    // Client area of width x height pixels, or a borderless window covering
    // the screen. The picture is scaled to the largest 4:3 area that fits.
    // `display` is an index into app::listDisplays().
    Window(int width, int height, bool fullscreen, int display = 0, bool visible = true);
    ~Window();

    // Presents one frame and waits for the next frame time. Returns false
    // once the window has been closed.
    bool present(const std::vector<std::uint8_t>& rgb, int width, int height);
    // Same, straight from a 15-bit frame buffer (red in the low bits): the
    // rectangle x,y,width,height of an image `stride` pixels wide.
    bool present(const std::uint16_t* pixels, int stride, int x, int y, int width, int height);
    // Same, for a picture drawn on the graphics card: `show` draws a native
    // rectangle of it into a viewport (window pixels, origin bottom left),
    // black see-through when `key`; `native` is the same rectangle at the
    // console's resolution (to tell gameplay from menus for widescreen).
    using ShowFn = void (*)(void* ctx, int x, int y, int w, int h, int vx, int vy, int vw, int vh, bool key);
    bool presentGl(ShowFn show, void* ctx, const std::uint16_t* native, int stride, int x, int y, int width, int height);
    // Pad buttons held right now, one bit per button (1 = pressed), in the
    // PlayStation's bit order.
    std::uint16_t buttons() const;
    // Left stick of the controller, -1..1 with up positive, when it is
    // pushed past the dead zone (window focused); false otherwise. Read by buttons().
    bool stick(float& x, float& y) const;
    // A frame in which the picture did not change: handles window messages
    // and waits for the next frame time without drawing.
    bool idle();
    // True when frames are timed by the display (vsync, a 60 or 120 Hz
    // monitor): every frame should then be drawn, changed or not (idle()
    // cannot wait for the display without drawing).
    bool syncedToDisplay() const;
    bool fastForward() const;
    // Show the letterboxed 2:1 gameplay and movie picture without its black
    // bands, as wide as the window allows (menus stay 4:3).
    void setWidescreen(bool on);
    // 1..4 when F2..F5 was pressed since the last call (insert that disc), else 0.
    int discRequest();
    // Queues stereo 44100 Hz samples for playback.
    void audio(const std::vector<std::int16_t>& stereo);
    // volume 0..100; latency: how much sound is queued ahead, in milliseconds (40..340)
    void setAudio(int volume, bool mute, int latencyMs);
    void setControls(const Controls& controls);
    const Controls& controls() const;
    // Opens the controls window over the game (F9); true when they were changed and saved.
    bool editControls(const std::wstring& iniPath);
    // Hotkeys pressed since the last call (F6 save, F7 load, F8 / Shift+F8 slot, F9 controls).
    // F11 and Alt+Enter (window / fullscreen) are handled inside.
    std::vector<Hotkey> hotkeys();
    // A line of text shown over the picture for a few seconds.
    void status(const std::string& text);
    void toggleFullscreen();
    // True while frames have been finishing late (within the last second or
    // so): the program cannot keep up, and optional work (60 fps in-between
    // frames) should wait, or the sound runs dry.
    bool behind() const;
    std::uint64_t lateFrames() const;    // frames that were done after their time
    std::uint64_t behindFrames() const;  // frames during which behind() was true

private:
    bool pump();
    void pace();
    Impl* impl_;
};

}  // namespace app
