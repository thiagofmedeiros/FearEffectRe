#include "ModernControls.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace app {

namespace {
// Found in the USA release (disc 1, first area) by comparing memory while
// turning, walking and pausing; see the --ram option.
constexpr fe::u32 kPlayClock = 0x0007780C;   // 30 ticks a second; stops when paused
constexpr fe::u32 kAngle = 0x001D8082;       // character's facing, 0..4095 (4096 = full turn)
constexpr fe::u32 kAngleCopy = 0x001D855A;   // second copy the game keeps in step

constexpr fe::u16 kUp = 0x0010, kRight = 0x0020, kDown = 0x0040, kLeft = 0x0080;
constexpr fe::u16 kDpad = kUp | kRight | kDown | kLeft;

constexpr int kTurnPerFrame = 256;      // 22.5 degrees: a half turn in 8 frames
constexpr int kWalkWithin = 768;        // walk once facing within 67.5 degrees of the target
constexpr int kMatchTolerance = 64;     // per matrix element, of 4096 (animation wobbles the body a little)
constexpr fe::u64 kCameraStale = 120;   // frames without seeing the character before giving up

fe::s16 read16(const fe::Machine& m, fe::u32 a) {
    fe::s16 v;
    std::memcpy(&v, m.ram.get() + a, 2);
    return v;
}
fe::u32 read32(const fe::Machine& m, fe::u32 a) {
    fe::u32 v;
    std::memcpy(&v, m.ram.get() + a, 4);
    return v;
}
void write16(fe::Machine& m, fe::u32 a, fe::s16 v) { std::memcpy(m.ram.get() + a, &v, 2); }

constexpr double kPi = 3.14159265358979323846;

// Signed difference b - a on the 4096-unit circle, in -2048..2047.
int angleDiff(int a, int b) { return ((b - a + 2048) & 4095) - 2048; }
}  // namespace

// The character is drawn with camera * rotateY(angle). Among this frame's
// GTE matrices, the camera is the one that, times the character's rotation,
// gives another of them. The game draws at 30 frames a second with the angle
// of its last update, so the angles of the last few frames are all tried.
bool ModernControls::findCamera(fe::Machine& m) {
    const auto& list = m.rotations;
    auto pureTurn = [](const std::array<fe::s16, 9>& q) {
        return std::abs(q[3]) < 16 && std::abs(q[5]) < 16 && q[4] > 4096 - 16;
    };
    int best = kMatchTolerance + 1;
    for (int angle : recentAngles_) {
        const double t = angle * (kPi / 2048.0);
        const double c = std::cos(t), s = std::sin(t);
        // near angle 0 the character's matrix equals the camera's
        if (std::fabs(s) < 0.2 && c > 0) continue;
        for (const auto& b : list) {
            // the character's own world rotation is loaded too; it would make
            // the identity look like the camera
            if (pureTurn(b)) continue;
            // a = b * rotateY(angle)^T, rotateY = [[c 0 s] [0 1 0] [-s 0 c]]
            int a[9];
            for (int r = 0; r < 3; ++r) {
                const double b0 = b[r * 3], b2 = b[r * 3 + 2];
                a[r * 3] = static_cast<int>(std::lround(b0 * c + b2 * s));
                a[r * 3 + 1] = b[r * 3 + 1];
                a[r * 3 + 2] = static_cast<int>(std::lround(-b0 * s + b2 * c));
            }
            for (const auto& cand : list) {
                if (pureTurn(cand)) continue;
                int worst = 0;
                for (int i = 0; i < 9 && worst < best; ++i) worst = std::max(worst, std::abs(cand[i] - a[i]));
                if (worst >= best) continue;
                best = worst;
                for (int i = 0; i < 9; ++i) camera_[i] = cand[i];
            }
        }
    }
    return best <= kMatchTolerance;
}

fe::u16 ModernControls::apply(fe::Machine& m, fe::u16 held, bool analog, float x, float y) {
    m.recordRotations = true;
    const fe::u32 clock = read32(m, kPlayClock);
    if (clock != clock_) {
        clock_ = clock;
        clockFrame_ = m.frame;
    }
    const int angle = read16(m, kAngle) & 4095;
    const bool sane = (read16(m, kAngleCopy) & 4095) == angle;
    std::rotate(recentAngles_.rbegin(), recentAngles_.rbegin() + 1, recentAngles_.rend());
    recentAngles_[0] = angle;
    if (sane && findCamera(m)) {
        haveCamera_ = true;
        cameraFrame_ = m.frame;
    }
    m.rotations.clear();

    // the game ticks at 30 a second: a clock still for 4 frames is stopped
    const bool playing = m.frame - clockFrame_ <= 4;
    if (!playing || !sane || !haveCamera_ || m.frame - cameraFrame_ > kCameraStale) return held;

    if (!analog) {
        x = static_cast<float>(((held & kRight) ? 1 : 0) - ((held & kLeft) ? 1 : 0));
        y = static_cast<float>(((held & kUp) ? 1 : 0) - ((held & kDown) ? 1 : 0));
    }
    if (x == 0 && y == 0) return held & ~kDpad;

    // World direction (dx, dz) on the ground that the camera shows as screen
    // direction (x, y): camera-space x is right, camera-space z is away from
    // the viewer, which is up on the screen for a camera looking down.
    const double c00 = camera_[0], c02 = camera_[2], c20 = camera_[6], c22 = camera_[8];
    const double det = c00 * c22 - c02 * c20;
    if (std::fabs(det) < 1e5) return held;  // looking straight down the ground plane
    const double dx = (x * c22 - y * c02) / det, dz = (y * c00 - x * c20) / det;
    // walking forward at angle a moves along (-sin a, -cos a)
    const int target = static_cast<int>(std::lround(std::atan2(-dx, -dz) * (2048.0 / kPi))) & 4095;

    int diff = angleDiff(angle, target);
    const int step = diff > kTurnPerFrame ? kTurnPerFrame : diff < -kTurnPerFrame ? -kTurnPerFrame : diff;
    if (step) {
        const fe::s16 now = static_cast<fe::s16>((angle + step) & 4095);
        write16(m, kAngle, now);
        write16(m, kAngleCopy, now);
        diff -= step;
    }
    fe::u16 out = held & ~kDpad;
    if (std::abs(diff) <= kWalkWithin) out |= kUp;
    return out;
}

}  // namespace app
