// AI enlargement of the movies and backgrounds: Real-ESRGAN models run by
// ONNX Runtime on the graphics card (DirectML). onnxruntime.dll, DirectML.dll
// and the models are loaded from the `ai` folder next to the program, so the
// game still runs when they are missing.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "psx/VideoUpscaler.h"

namespace app {

enum class VideoMode {
    Original,  // no enlargement: the 320x160 pictures as they are, in big blocks
    AiFast,    // small video model, run live
    AiBest,    // large model; results kept in a cache on disk so later runs are full speed
    AiSeedVR2, // SeedVR2 video model, from its own cache built beforehand (x4plus where it has no picture)
};

const char* videoModeId(VideoMode m);  // "original", "fast", "best"
bool parseVideoMode(const std::string& id, VideoMode& m);

// Null (with `error` set) when the runtime or the model cannot be loaded.
std::shared_ptr<fe::VideoUpscaler> createAiUpscaler(VideoMode mode, const std::filesystem::path& aiDir,
                                                    const std::filesystem::path& cacheDir, std::string& error);

// Decodes every movie and background frame on the given disc images and
// enlarges each with the best model into the cache, skipping pictures already
// there (so it can be stopped and run again). Returns an exit code.
int buildHdCache(const std::vector<std::string>& discs, const std::filesystem::path& aiDir,
                 const std::filesystem::path& cacheDir);

// The same for SeedVR2: the frames go through seedvr2_chunk.py (in the ai
// folder, using ai/seedvr2) in chunks, in disc order, into hdcache/seedvr2.pack.
int buildSeedVR2Cache(const std::vector<std::string>& discs, const std::filesystem::path& aiDir,
                      const std::filesystem::path& cacheDir);

}  // namespace app
