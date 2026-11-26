// Minimal Win32 window: shows the GPU's picture at 4:3, reads the keyboard
// as a digital pad and paces the game to the console's frame rate.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace app {

// Which keys and controller buttons press each PlayStation button, read from
// the [keyboard] and [controller] sections of FearEffectRe.ini (written with
// the defaults when they are missing). Index = bit in the pad word.
struct Controls {
    static const char* const kButtonNames[16];  // "select", "l3", ... "square"
    std::vector<int> keys[16];     // Windows virtual-key codes
    std::vector<unsigned> pad[16]; // XInput button bits; 0x10000 LT, 0x20000 RT
    int deadzone = 37;             // left stick, percent of its travel
    bool stickAsDpad = true;       // the left stick also presses the D-pad
    static Controls defaults();
    static Controls load(const std::wstring& iniPath);
};

enum class Hotkey { SaveState, LoadState, NextSlot, PrevSlot };

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
    // Same, straight from a 15-bit frame buffer (red in the low bits): the
    // rectangle x,y,width,height of an image `stride` pixels wide.
    bool present(const std::uint16_t* pixels, int stride, int x, int y, int width, int height);
    // Pad buttons held right now, one bit per button (1 = pressed), in the
    // PlayStation's bit order.
    std::uint16_t buttons() const;
    // Left stick of the Xbox controller, -1..1 with up positive, when it is
    // pushed past the dead zone (window focused); false otherwise.
    bool stick(float& x, float& y) const;
    // A frame in which the picture did not change: handles window messages
    // and waits for the next frame time without drawing.
    bool idle();
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
    // Hotkeys pressed since the last call (F6 save, F7 load, F8 / Shift+F8 slot).
    // F11 and Alt+Enter (window / fullscreen) are handled inside.
    std::vector<Hotkey> hotkeys();
    // A line of text shown over the picture for a few seconds.
    void status(const std::string& text);
    void toggleFullscreen();

private:
    bool pump();
    void pace();
    Impl* impl_;
};

}  // namespace app
