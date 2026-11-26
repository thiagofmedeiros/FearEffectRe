// Controls: which keys and controller buttons press each PlayStation button,
// the controllers themselves (Xbox controllers through XInput, any other game
// controller through the Windows joystick API), and the window that changes
// the bindings. The bindings live in the [keyboard] and [controller] sections
// of FearEffectRe.ini, written with the defaults when they are missing.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

namespace app {

// Controller button codes. XInput buttons keep their own bits; the triggers,
// generic controller buttons and the hat switch get codes of their own.
enum : unsigned {
    kPadLT = 0x10000,
    kPadRT = 0x20000,
    kJoyButton = 0x100000,  // + n for "Button n" (1..32)
    kJoyPov = 0x200000,     // + 1 up, 2 right, 3 down, 4 left
};

struct Controls {
    static const char* const kButtonNames[16];  // "select", "l3", ... "square"; index = bit in the pad word
    std::vector<int> keys[16];       // Windows virtual-key codes
    std::vector<unsigned> pad[16];   // controller button codes (above)
    int deadzone = 37;               // left stick, percent of its travel
    bool stickAsDpad = true;         // the left stick also presses the D-pad
    bool analog = false;             // DualShock in analog mode instead of a digital pad

    static Controls defaults();
    static Controls load(const std::wstring& iniPath);
    void save(const std::wstring& iniPath) const;
};

std::wstring keyName(int vk);       // "E", "Enter", ... ("" if it has no name)
std::wstring padName(unsigned code);  // "A", "LT", "Button3", "PovUp", ...

// One reading of the game controller.
struct PadSnapshot {
    bool connected = false;
    bool xinput = false;
    unsigned xbuttons = 0;     // XInput buttons, plus kPadLT / kPadRT
    unsigned joyButtons = 0;   // bit n-1 = "Button n" of a generic controller
    int pov = 0;               // hat: 0 centred, 1 up, 2 right, 3 down, 4 left (diagonals set two)
    int pov2 = 0;
    float lx = 0, ly = 0;      // left stick, -1..1, up positive
    bool held(unsigned code) const;
    // Every code held, for the controls window to pick up.
    std::vector<unsigned> heldCodes() const;
};

// The first connected controller: an Xbox controller if there is one,
// otherwise the first other game controller. Empty slots are probed about
// once a second, which is slow.
class Pads {
public:
    Pads();
    PadSnapshot poll();

private:
    using GetState = DWORD(WINAPI*)(DWORD, void*);
    GetState xinput_ = nullptr;
    DWORD xIndex_ = 0;
    int xRetry_ = 0;
    int joyId_ = -1;
    int joyRetry_ = 0;
    JOYCAPSW caps_{};
};

// Modal window to change the bindings, over `owner` (may be null). Saves
// them to the ini file and returns true when the player presses Save.
bool runControlsDialog(HWND owner, Controls& controls, const std::wstring& iniPath);

}  // namespace app
