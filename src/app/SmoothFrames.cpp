#include "SmoothFrames.h"

#include <algorithm>
#include <chrono>

namespace app {

SmoothFrames::SmoothFrames(fe::Gpu& game, int scale) : game_(game) {
    between_.ghost = true;
    between_.renderPrecise = true;  // the halfway positions
    between_.videoFrom = &game;
    between_.setScale(scale);  // covers all of video memory: nothing is added to it later
    game_.recordPasses = true;
    game_.snapshotTo = &between_;
    if (game_.hw) {
        // On the graphics card: the frame is drawn when it is needed, on this
        // thread (it costs little there).
        between_.setBackend(game_.hw);
        return;
    }
    // A snapshot is written into our picture: not where we are drawing.
    game_.beforeSnapshot = [this](int y1) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto t0 = std::chrono::steady_clock::now();
        idle_.wait(lock, [&] { return !busy_ || !b_ || (b_->y1 & 511) != (y1 & 511); });
        waitedForSnapshot += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    };
    thread_ = std::thread([this] { work(); });
}

SmoothFrames::~SmoothFrames() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    game_.beforeSnapshot = nullptr;
    game_.snapshotTo = nullptr;
    game_.recordPasses = false;
}

void SmoothFrames::waitIdle(std::unique_lock<std::mutex>& lock) {
    idle_.wait(lock, [&] { return !busy_; });
}

void SmoothFrames::work() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        wake_.wait(lock, [&] { return quit_ || busy_; });
        if (quit_) return;
        auto a = a_, b = b_;
        lock.unlock();
        int matched = 0;
        const bool ok = between_.drawBetween(*a, *b, 0.5f, textures_, matched);
        lock.lock();
        results_[b->serial] = ok;
        while (results_.size() > 8) results_.erase(results_.begin());
        busy_ = false;
        idle_.notify_all();
    }
}

void SmoothFrames::poll() {
    if (game_.hw) return;  // drawn in frameFor()
    std::shared_ptr<const fe::Gpu::Pass> a, b;
    if (!game_.newestPair(a, b) || b->serial == started_) return;
    std::unique_lock<std::mutex> lock(mutex_);
    if (busy_) return;  // still drawing the last one: this picture is tried again next frame
    started_ = b->serial;
    // our picture must still hold this picture's snapshot
    auto held = game_.snapshotHeld.find(b->y1 & 511);
    if (held == game_.snapshotHeld.end() || held->second != b->serial) return;
    game_.snapshotHeld.erase(held);  // drawn over from now on
    a_ = std::move(a);
    b_ = std::move(b);
    textures_ = game_.vram;
    busy_ = true;
    lock.unlock();
    wake_.notify_all();
}

fe::Gpu* SmoothFrames::frameFor() {
    std::shared_ptr<const fe::Gpu::Pass> a, b;
    if (!game_.shownPair(a, b)) return nullptr;
    if (game_.hw) {
        auto held = game_.snapshotHeld.find(b->y1 & 511);
        if (held == game_.snapshotHeld.end() || held->second != b->serial) {
            ++missed;
            return nullptr;
        }
        game_.snapshotHeld.erase(held);
        const int x1 = std::max(b->x1, 0), y1 = std::max(b->y1, 0), x2 = std::min(b->x2, 1023), y2 = std::min(b->y2, 511);
        game_.hw->beginBetween(x1, y1, x2, y2);
        int matched = 0;
        const bool ok = between_.drawBetween(*a, *b, 0.5f, game_.vram, matched);
        game_.hw->endBetween();
        if (!ok) {
            ++cuts;
            return nullptr;
        }
        ++drawn;
        between_.showLike(game_);
        return &between_;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (busy_ && b_ && b_->serial == b->serial) {
        const auto t0 = std::chrono::steady_clock::now();
        waitIdle(lock);
        waitedForFrame += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    auto it = results_.find(b->serial);
    if (it == results_.end()) {
        ++missed;
        return nullptr;
    }
    if (!it->second) {
        ++cuts;
        return nullptr;
    }
    ++drawn;
    between_.showLike(game_);
    return &between_;
}

void SmoothFrames::reset() {
    std::unique_lock<std::mutex> lock(mutex_);
    waitIdle(lock);
    results_.clear();
    started_ = 0;
}

}  // namespace app
