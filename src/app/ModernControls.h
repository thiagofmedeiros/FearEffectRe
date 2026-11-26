// Optional "modern" movement: the direction pushed on the stick or D-pad is a
// direction on the screen, and the character turns to face it and walks,
// instead of the original tank controls (up walks forward, left and right
// turn on the spot).
//
// It works from outside the game. Each frame it reads the character's
// facing angle from main memory, finds the camera among the rotation
// matrices the game gave the GTE, turns the character by writing the angle,
// and hands the game a plain "forward" press. When anything it relies on is
// missing (a menu, the pause screen, a cutscene with no character drawn),
// the buttons go through unchanged and the game behaves as it always did.
#pragma once

#include <array>

#include "psx/Machine.h"

namespace app {

class ModernControls {
public:
    // `held`: pad buttons pressed this frame (1 = pressed, PlayStation bit
    // order). With `analog`, (x, y) is the stick direction (y up); otherwise
    // the D-pad bits in `held` give the direction. Returns the buttons to hand
    // to the game, and may turn the character.
    fe::u16 apply(fe::Machine& m, fe::u16 held, bool analog, float x, float y);

    // The last apply() steered the character (the direction buttons it
    // returned are "forward" or nothing, not what was pressed).
    bool steering() const { return steering_; }

private:
    bool steering_ = false;
    bool findCamera(fe::Machine& m);

    std::array<int, 9> camera_{};  // row-major, 4096 = 1.0
    std::array<int, 4> recentAngles_{};  // facing angle of this and the last three frames
    bool haveCamera_ = false;
    fe::u64 cameraFrame_ = 0;      // last frame the camera was seen together with the character
    fe::u32 clock_ = 0;            // the game's play clock, which stops in menus and while paused
    fe::u64 clockFrame_ = 0;       // last frame it advanced
};

}  // namespace app
