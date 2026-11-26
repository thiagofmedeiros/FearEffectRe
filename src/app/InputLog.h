// Input recording: the pad state the game saw on every frame, and the disc
// changes, so a session can be played back exactly (--record / --replay).
// The file is text: a header line, then "frame buttons" (hex, 1 = pressed)
// whenever the buttons change and "frame disc N" for a disc change.
#pragma once

#include <cstdint>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>

namespace app {

class InputRecorder {
public:
    explicit InputRecorder(const std::string& path) : f_(std::fopen(path.c_str(), "w")) {
        if (!f_) throw std::runtime_error("cannot write " + path);
        std::fprintf(f_, "FearEffectRe input 1\n");
    }
    ~InputRecorder() { std::fclose(f_); }
    InputRecorder(const InputRecorder&) = delete;
    InputRecorder& operator=(const InputRecorder&) = delete;

    void buttons(std::uint64_t frame, std::uint16_t held) {
        if (held == last_) return;
        last_ = held;
        std::fprintf(f_, "%llu %04x\n", static_cast<unsigned long long>(frame), held);
    }
    void disc(std::uint64_t frame, int number) {
        std::fprintf(f_, "%llu disc %d\n", static_cast<unsigned long long>(frame), number);
        std::fflush(f_);
    }

private:
    FILE* f_;
    std::uint16_t last_ = 0;
};

class InputReplay {
public:
    explicit InputReplay(const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) throw std::runtime_error("cannot read " + path);
        char line[128];
        if (!std::fgets(line, sizeof line, f) || std::string(line).rfind("FearEffectRe input 1", 0) != 0) {
            std::fclose(f);
            throw std::runtime_error(path + " is not an input recording");
        }
        while (std::fgets(line, sizeof line, f)) {
            unsigned long long frame;
            unsigned value;
            int disc;
            if (std::sscanf(line, "%llu disc %d", &frame, &disc) == 2) discs_[frame] = disc;
            else if (std::sscanf(line, "%llu %x", &frame, &value) == 2) changes_[frame] = static_cast<std::uint16_t>(value);
        }
        std::fclose(f);
    }
    // Buttons held on `frame`.
    std::uint16_t buttons(std::uint64_t frame) const {
        auto it = changes_.upper_bound(frame);
        return it == changes_.begin() ? 0 : std::prev(it)->second;
    }
    // Disc inserted on `frame`, 0 for none.
    int disc(std::uint64_t frame) const {
        auto it = discs_.find(frame);
        return it == discs_.end() ? 0 : it->second;
    }
    std::uint64_t lastFrame() const { return changes_.empty() ? 0 : changes_.rbegin()->first; }

private:
    std::map<std::uint64_t, std::uint16_t> changes_;
    std::map<std::uint64_t, int> discs_;
};

}  // namespace app
