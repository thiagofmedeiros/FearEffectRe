// GameShark cheats from a DuckStation .cht file, applied to main memory once
// per frame. Both file layouts DuckStation reads are accepted:
//   [Name]                      cheats = 2
//   Type = Gameshark            cheat0_desc = "Name"
//   80077810 0002               cheat0_code = "80077810 0002"
// (and the older "#Name" headers). Code types: 80/30 write, 10/11/20/21
// increment and decrement, D0-D3/E0-E3 conditions on the next code.
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace app {

class Cheats {
public:
    struct Cheat {
        std::string name;
        std::vector<std::pair<std::uint32_t, std::uint16_t>> codes;
    };

    // Loads every cheat in the file, or only those in `only` when it is not
    // empty (names compared without case). Unknown code types are reported
    // in `skipped`.
    Cheats(const std::string& path, const std::vector<std::string>& only) {
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) throw std::runtime_error("cannot read " + path);
        std::vector<Cheat> all;
        char raw[512];
        while (std::fgets(raw, sizeof raw, f)) {
            std::string line = trim(raw);
            if (line.empty() || line[0] == ';') continue;
            if (line[0] == '[' && line.back() == ']') {
                all.push_back({line.substr(1, line.size() - 2), {}});
            } else if (line[0] == '#') {
                std::string name = line.substr(1);
                if (size_t slash = name.rfind('\\'); slash != std::string::npos) name = name.substr(slash + 1);
                all.push_back({trim(name), {}});
            } else if (size_t eq = line.find('='); eq != std::string::npos) {
                // libretro layout: cheatN_desc / cheatN_code
                std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
                if (value.size() >= 2 && value.front() == '"') value = value.substr(1, value.size() - 2);
                unsigned n;
                char what[16] = {};
                if (std::sscanf(key.c_str(), "cheat%u_%15s", &n, what) == 2) {
                    if (all.size() <= n) all.resize(n + 1);
                    if (!std::strcmp(what, "desc")) all[n].name = value;
                    if (!std::strcmp(what, "code"))
                        for (size_t pos = 0; pos < value.size();) {
                            size_t end = value.find_first_of("+;", pos);
                            parseCode(value.substr(pos, end == std::string::npos ? std::string::npos : end - pos), all[n]);
                            pos = end == std::string::npos ? value.size() : end + 1;
                        }
                }
            } else if (!all.empty()) {
                parseCode(line, all.back());
            }
        }
        std::fclose(f);
        for (auto& c : all) {
            if (c.codes.empty()) continue;
            bool wanted = only.empty();
            for (const auto& o : only) wanted = wanted || same(o, c.name);
            if (wanted) cheats_.push_back(std::move(c));
        }
    }

    const std::vector<Cheat>& active() const { return cheats_; }
    std::vector<std::string> skipped;

    void apply(std::uint8_t* ram) const {
        for (const Cheat& c : cheats_) {
            for (size_t i = 0; i < c.codes.size(); ++i) {
                const std::uint32_t a = c.codes[i].first & 0x1FFFFF;
                const std::uint16_t v = c.codes[i].second;
                std::uint16_t h;
                std::memcpy(&h, ram + (a & ~1u), 2);
                const std::uint8_t b = ram[a];
                bool skipNext = false;
                switch (c.codes[i].first >> 24) {
                    case 0x80: std::memcpy(ram + (a & ~1u), &v, 2); break;
                    case 0x30: ram[a] = static_cast<std::uint8_t>(v); break;
                    case 0x10: h = static_cast<std::uint16_t>(h + v); std::memcpy(ram + (a & ~1u), &h, 2); break;
                    case 0x11: h = static_cast<std::uint16_t>(h - v); std::memcpy(ram + (a & ~1u), &h, 2); break;
                    case 0x20: ram[a] = static_cast<std::uint8_t>(b + v); break;
                    case 0x21: ram[a] = static_cast<std::uint8_t>(b - v); break;
                    case 0xD0: skipNext = !(h == v); break;
                    case 0xD1: skipNext = !(h != v); break;
                    case 0xD2: skipNext = !(h < v); break;
                    case 0xD3: skipNext = !(h > v); break;
                    case 0xE0: skipNext = !(b == (v & 0xFF)); break;
                    case 0xE1: skipNext = !(b != (v & 0xFF)); break;
                    case 0xE2: skipNext = !(b < (v & 0xFF)); break;
                    case 0xE3: skipNext = !(b > (v & 0xFF)); break;
                    default: break;
                }
                if (skipNext) ++i;
            }
        }
    }

private:
    static std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    }
    static bool same(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
        return true;
    }
    void parseCode(const std::string& text, Cheat& into) {
        unsigned addr, value;
        char rest;
        if (std::sscanf(text.c_str(), " %8x %4x %c", &addr, &value, &rest) != 2) return;  // "Type = ..." lines etc.
        switch (addr >> 24) {
            case 0x80: case 0x30: case 0x10: case 0x11: case 0x20: case 0x21:
            case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0xE0: case 0xE1: case 0xE2: case 0xE3:
                into.codes.push_back({addr, static_cast<std::uint16_t>(value)});
                break;
            default:
                skipped.push_back(into.name + ": " + trim(text));
                break;
        }
    }

    std::vector<Cheat> cheats_;
};

}  // namespace app
