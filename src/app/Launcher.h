// Start-up dialog shown when the program is run without arguments: pick the
// disc image, the output resolution and fullscreen, then play. The choices
// are remembered in FearEffectRe.ini next to the program.
#pragma once

#include <string>
#include <vector>

namespace app {

struct Resolution {
    const char* id;       // command-line name
    const wchar_t* label; // shown in the launcher
    int width, height;
};

// Original, 720p, 1080p, 1440p, 4K.
extern const Resolution kResolutions[5];
const Resolution* findResolution(const std::string& id);

struct Display {
    int x, y, width, height;  // position and size on the desktop, in pixels
    bool primary;
};

// Connected displays: the primary one first, the others left to right.
std::vector<Display> listDisplays();

struct LaunchSettings {
    std::string discPath;
    int resolution = 1;  // index into kResolutions
    bool fullscreen = false;
    int display = 0;     // index into listDisplays()
    int video = 1;       // movies and backgrounds: 0 original, 1 AI fast, 2 AI best (cached), 3 SeedVR2 (cached)
    bool modern = false; // screen-relative movement instead of tank controls
    bool widescreen = false;  // gameplay and movies without the black bands, filling the width
    bool infiniteHealth = false, infiniteAmmo = false;  // the game's own cheats
};

// FearEffectRe.ini next to the program.
std::wstring settingsPath();

// Returns false when the dialog was closed without pressing Play.
bool runLauncher(LaunchSettings& settings);

}  // namespace app
