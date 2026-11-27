// 60 fps: draws a frame in between each two of the game's on a thread of its
// own, while the game goes on with the next picture (see Gpu::recordPasses).
#pragma once

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "psx/Gpu.h"

namespace app {

class SmoothFrames {
public:
    SmoothFrames(fe::Gpu& game, int scale);
    ~SmoothFrames();
    SmoothFrames(const SmoothFrames&) = delete;
    SmoothFrames& operator=(const SmoothFrames&) = delete;

    // Every display frame: when the game has finished a picture, starts
    // drawing the frame between it and the one before.
    void poll();
    // When the game has just flipped to a picture: the frame in between for
    // it (waiting if it is still being drawn), or null when there is none
    // (a cut, or it could not be started in time).
    fe::Gpu* frameFor();
    // After a save state was loaded: what was recorded before does not count.
    void reset();

    std::uint64_t drawn = 0, cuts = 0, missed = 0;
    double waitedForFrame = 0, waitedForSnapshot = 0;  // milliseconds the game's thread waited

private:
    void work();
    void waitIdle(std::unique_lock<std::mutex>& lock);

    fe::Gpu& game_;
    fe::Gpu between_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_, idle_;
    bool busy_ = false, quit_ = false;
    std::shared_ptr<const fe::Gpu::Pass> a_, b_;
    std::vector<fe::u16> textures_;
    std::uint64_t started_ = 0;              // serial of the last picture started
    std::map<std::uint64_t, bool> results_;  // serial -> its frame in between is drawn (false: a cut)
};

}  // namespace app
