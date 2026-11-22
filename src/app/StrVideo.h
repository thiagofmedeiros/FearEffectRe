// Reads the game's movies and backgrounds straight from a disc image: STR
// version 2 frames (320x160) in the XA files, turned into the MDEC codes the
// game would send and decoded by the same MDEC as the game, so the pictures
// match what the GPU receives pixel for pixel.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "disc/DiscImage.h"

namespace app {

struct StrFrame {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgb;  // width*height*3, from the 15-bit MDEC output
};

// Calls `frame` for every complete video frame in the disc's XA files, in disc
// order. `progress(done, total)` is called now and then with sector counts.
// Returns the number of frames that could not be decoded.
int forEachStrFrame(fe::DiscImage& disc, const std::function<void(const StrFrame&)>& frame,
                    const std::function<void(std::uint64_t, std::uint64_t)>& progress);

}  // namespace app
