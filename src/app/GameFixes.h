// Fixes for bugs in the game itself (not in this program), applied by
// wrapping the game function that has the bug. Each fix first checks that
// the code is the one it expects, so a different executable is left alone.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "psx/Machine.h"

namespace app {

struct GameFixStats {
    std::uint64_t vlcCalls = 0;          // DecDCTvlc calls
    std::uint64_t vlcStaleResumes = 0;   // "go on" calls after the last picture had already finished
};

// Installs the fixes (with `apply` false the bugs are only counted).
// Returns a line per fix installed.
std::vector<std::string> installGameFixes(fe::Machine& m, bool apply, GameFixStats& stats);

}  // namespace app
