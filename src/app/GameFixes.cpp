#include "GameFixes.h"

#include <memory>

namespace app {

namespace {

// DecDCTvlc, the library function that turns a movie frame's bitstream into
// run-length codes for the motion decoder, in slices: called with the
// bitstream it starts a picture, called with 0 it goes on with the one in
// progress, and it returns 0 once the picture is done. Its state between
// calls is a context in RAM.
//
// The game's movie and background driver (800572CCh) starts a picture, sets
// its "picture in progress" flag (80092F4Bh) without looking at the result,
// and calls DecDCTvlc(0) the next time. When the picture was finished in one
// slice, that resumes from the context of a finished picture: it reads the
// stream ring at an old position and writes codes through an old output
// pointer, over memory that has been reused since. (Found and fixed the same
// way in project-helix's gamefix.c.)
//
// Fix: when a call finishes a picture, the context is left as "finished":
// no continuation (+30h = 0), last code 3FFh (end of picture) at +8, and the
// output pointer (+10h) at its end (+20h). A stale "go on" then ends at once
// and writes nothing.
constexpr fe::u32 kDecDctVlc = 0x80057D30;
constexpr fe::u32 kDecDctVlcFirstWord = 0x23BDFFFC;  // addi sp, sp, -4
constexpr fe::u32 kVlcContext = 0x8007A1C8;

}  // namespace

std::vector<std::string> installGameFixes(fe::Machine& m, bool apply, GameFixStats& stats) {
    std::vector<std::string> installed;
    if (m.read(kDecDctVlc, 4) == kDecDctVlcFirstWord) {
        auto finished = std::make_shared<bool>(true);
        m.wrap(kDecDctVlc, [&m, &stats, apply, finished](fe::Cpu& c) {
            ++stats.vlcCalls;
            if (c.r[4] == 0 && *finished) ++stats.vlcStaleResumes;
            m.callOriginal(c, kDecDctVlc);
            *finished = c.r[2] == 0;
            if (apply && *finished) {
                m.write(kVlcContext + 0x30, 4, 0);
                m.write(kVlcContext + 0x08, 4, 0x3FF);
                m.write(kVlcContext + 0x10, 4, m.read(kVlcContext + 0x20, 4));
            }
        });
        installed.push_back(apply ? "movie decoder: a finished picture is not resumed" : "movie decoder: watched, not fixed");
    }
    return installed;
}

}  // namespace app
