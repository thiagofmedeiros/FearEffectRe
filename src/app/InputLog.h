// Input recording: what the player pressed on every frame (buttons and the
// left stick, before modern movement turns them into game input), and the disc
// changes, so a session can be played back exactly (--record / --replay). The
// header says whether the pad was analog and modern movement was on; a
// replay uses the same. The file is text: a header line, then "frame buttons
// [x y]" (hex; buttons 1 = pressed, stick bytes 80h = centre) whenever they
// change and "frame disc N" for a disc change.
#pragma once

#include <cstdint>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>

namespace app {

class InputRecorder {
public:
    InputRecorder(const std::string& path, bool analog, bool modern) : f_(std::fopen(path.c_str(), "w")) {
        if (!f_) throw std::runtime_error("cannot write " + path);
        std::fprintf(f_, "FearEffectRe input 1%s%s\n", analog ? " analog" : "", modern ? " modern" : "");
    }
    ~InputRecorder() { std::fclose(f_); }
    InputRecorder(const InputRecorder&) = delete;
    InputRecorder& operator=(const InputRecorder&) = delete;

    // `stick`: the left stick bytes (x << 8 | y) of an analog pad, 8080h = centre.
    void buttons(std::uint64_t frame, std::uint16_t held, std::uint16_t stick = 0x8080) {
        if (held == last_ && stick == lastStick_) return;
        last_ = held;
        lastStick_ = stick;
        if (stick == 0x8080) std::fprintf(f_, "%llu %04x\n", static_cast<unsigned long long>(frame), held);
        else std::fprintf(f_, "%llu %04x %02x %02x\n", static_cast<unsigned long long>(frame), held, stick >> 8, stick & 0xFF);
    }
    void disc(std::uint64_t frame, int number) {
        std::fprintf(f_, "%llu disc %d\n", static_cast<unsigned long long>(frame), number);
        std::fflush(f_);
    }

private:
    FILE* f_;
    std::uint16_t last_ = 0, lastStick_ = 0x8080;
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
        analog_ = std::string(line).find(" analog") != std::string::npos;
        modern_ = std::string(line).find(" modern") != std::string::npos;
        while (std::fgets(line, sizeof line, f)) {
            unsigned long long frame;
            unsigned value, x = 0x80, y = 0x80;
            int disc;
            if (std::sscanf(line, "%llu disc %d", &frame, &disc) == 2) {
                discs_[frame] = disc;
            } else if (std::sscanf(line, "%llu %x %x %x", &frame, &value, &x, &y) >= 2) {
                changes_[frame] = static_cast<std::uint16_t>(value);
                sticks_[frame] = static_cast<std::uint16_t>((x << 8) | y);
            }
        }
        std::fclose(f);
    }
    // Buttons held on `frame`.
    std::uint16_t buttons(std::uint64_t frame) const {
        auto it = changes_.upper_bound(frame);
        return it == changes_.begin() ? 0 : std::prev(it)->second;
    }
    // Left stick bytes (x << 8 | y) on `frame`, 8080h when centred.
    std::uint16_t stick(std::uint64_t frame) const {
        auto it = sticks_.upper_bound(frame);
        return it == sticks_.begin() ? 0x8080 : std::prev(it)->second;
    }
    // Disc inserted on `frame`, 0 for none.
    int disc(std::uint64_t frame) const {
        auto it = discs_.find(frame);
        return it == discs_.end() ? 0 : it->second;
    }
    bool analog() const { return analog_; }  // recorded with an analog pad: replay with one
    bool modern() const { return modern_; }  // recorded with modern movement on
    std::uint64_t lastFrame() const { return changes_.empty() ? 0 : changes_.rbegin()->first; }

private:
    std::map<std::uint64_t, std::uint16_t> changes_, sticks_;
    std::map<std::uint64_t, int> discs_;
    bool analog_ = false, modern_ = false;
};

}  // namespace app
