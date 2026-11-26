// GameShark cheats from a DuckStation .cht file, applied to main memory once
// per frame. Both file layouts DuckStation reads are accepted:
//   [Name]                      cheats = 2
//   Type = Gameshark            cheat0_desc = "Name"
//   80077810 0002               cheat0_code = "80077810 0002"
// (and the older "#Name" headers). Code types: 80/30/90 write 16/8/32 bits,
// 10/11/20/21 increment and decrement, D0-D3/E0-E3 conditions on the next
// code. A cheat with a code of another type is left out (`skipped`).
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
    struct Code {
        std::uint32_t op;     // type and address, as written: 80077810
        std::uint32_t value;  // 16 bits, or 32 for type 90
    };
    struct Cheat {
        std::string name;     // "Group\\Name" for cheats in a group
        std::vector<Code> codes;
        bool unsupported = false;
    };

    // Every cheat in the file that can be applied.
    static std::vector<Cheat> read(const std::string& path, std::vector<std::string>* skipped = nullptr) {
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
                all.push_back({trim(line.substr(1)), {}});
            } else if (size_t eq = line.find('='); eq != std::string::npos) {
                // libretro layout: cheatN_desc / cheatN_code; DuckStation's
                // "Type = ..." and similar lines are skipped here
                std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
                if (value.size() >= 2 && value.front() == '"') value = value.substr(1, value.size() - 2);
                unsigned n;
                char what[16] = {};
                if (std::sscanf(key.c_str(), "cheat%u_%15s", &n, what) == 2 && n < 10000) {
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
        std::vector<Cheat> usable;
        for (auto& c : all) {
            if (c.codes.empty() && !c.unsupported) continue;
            if (c.unsupported) {
                if (skipped) skipped->push_back(c.name);
                continue;
            }
            usable.push_back(std::move(c));
        }
        return usable;
    }

    // The cheats of the file named in `only` (compared without case), or all
    // of them when `only` is empty.
    Cheats(const std::string& path, const std::vector<std::string>& only) {
        for (auto& c : read(path, &skipped)) {
            bool wanted = only.empty();
            for (const auto& o : only) wanted = wanted || same(o, c.name);
            if (wanted) cheats_.push_back(std::move(c));
        }
    }

    const std::vector<Cheat>& active() const { return cheats_; }
    std::vector<std::string> skipped;  // cheats left out: a code type that is not supported

    // RAM addresses (offsets) the active cheats write to.
    std::vector<std::uint32_t> writes() const {
        std::vector<std::uint32_t> out;
        for (const Cheat& c : cheats_)
            for (const Code& k : c.codes)
                if (isWrite(k.op >> 24)) out.push_back(k.op & 0x1FFFFF);
        return out;
    }

    void apply(std::uint8_t* ram) const {
        for (const Cheat& c : cheats_) {
            for (size_t i = 0; i < c.codes.size(); ++i) {
                const std::uint32_t a = c.codes[i].op & 0x1FFFFF;
                const std::uint32_t v = c.codes[i].value;
                std::uint16_t h;
                std::memcpy(&h, ram + (a & ~1u), 2);
                const std::uint8_t b = ram[a];
                auto put16 = [&](std::uint16_t x) { std::memcpy(ram + (a & ~1u), &x, 2); };
                bool skipNext = false;
                switch (c.codes[i].op >> 24) {
                    case 0x80: put16(static_cast<std::uint16_t>(v)); break;
                    case 0x30: ram[a] = static_cast<std::uint8_t>(v); break;
                    case 0x90: std::memcpy(ram + (a & ~3u), &v, 4); break;
                    case 0x10: put16(static_cast<std::uint16_t>(h + v)); break;
                    case 0x11: put16(static_cast<std::uint16_t>(h - v)); break;
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
    static bool isWrite(std::uint32_t type) {
        return type == 0x80 || type == 0x30 || type == 0x90 || type == 0x10 || type == 0x11 || type == 0x20 || type == 0x21;
    }
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
    // A code line: "TTAAAAAA VVVV" (or VVVVVVVV for type 90). Other lines
    // (descriptions, options) are not codes and are ignored.
    static void parseCode(const std::string& text, Cheat& into) {
        std::string t = trim(text);
        size_t space = t.find(' ');
        if (space != 8) return;
        std::string addr = t.substr(0, 8), value = trim(t.substr(9));
        auto hex = [](const std::string& s) {
            for (char ch : s)
                if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
            return !s.empty();
        };
        if (!hex(addr) || !hex(value) || (value.size() != 4 && value.size() != 8)) return;
        const std::uint32_t op = static_cast<std::uint32_t>(std::stoul(addr, nullptr, 16));
        const std::uint32_t v = static_cast<std::uint32_t>(std::stoul(value, nullptr, 16));
        const std::uint32_t type = op >> 24;
        const bool known = isWrite(type) || (type >= 0xD0 && type <= 0xD3) || (type >= 0xE0 && type <= 0xE3);
        if (!known || (type == 0x90) != (value.size() == 8)) {
            into.unsupported = true;
            return;
        }
        into.codes.push_back({op, v});
    }

    std::vector<Cheat> cheats_;
};

}  // namespace app
