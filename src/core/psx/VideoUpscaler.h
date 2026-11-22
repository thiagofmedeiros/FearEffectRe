// Interface for enlarging the movie and background pictures the game decodes
// into video memory. The GPU hands over a picture as 8-bit RGB and gets back
// one four times larger in each direction; the implementation (an AI model,
// a cache on disk) lives in the application.
#pragma once

#include <vector>

#include "psx/Machine.h"

namespace fe {

struct VideoUpscaler {
    static constexpr int kFactor = 4;
    virtual ~VideoUpscaler() = default;
    // rgb: width*height*3 bytes. On success `out` holds (width*4)*(height*4)*3 bytes.
    virtual bool upscale(const u8* rgb, int width, int height, std::vector<u8>& out) = 0;
};

}  // namespace fe
