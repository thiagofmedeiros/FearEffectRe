#include "StrVideo.h"

#include <cstring>
#include <map>
#include <string>

#include "psx/Mdec.h"

namespace app {

namespace {

// The tables Fear Effect uploads to the MDEC (the standard ones).
const fe::u16 kQuant[32] = {0x1002, 0x1310, 0x1310, 0x1616, 0x1616, 0x1616, 0x181a, 0x1b1a, 0x1b1b, 0x1a1a, 0x1a1a,
                            0x1b1b, 0x1d1b, 0x1d1d, 0x2222, 0x1d22, 0x1d1d, 0x1b1b, 0x1d1d, 0x2020, 0x2222, 0x2625,
                            0x2325, 0x2223, 0x2623, 0x2826, 0x2828, 0x3030, 0x2e2e, 0x3838, 0x453a, 0x5345};
const fe::u16 kScale[64] = {
    0x5a82, 0x5a82, 0x5a82, 0x5a82, 0x5a82, 0x5a82, 0x5a82, 0x5a82, 0x7d8a, 0x6a6d, 0x471c, 0x18f8, 0xe707,
    0xb8e3, 0x9592, 0x8275, 0x7641, 0x30fb, 0xcf04, 0x89be, 0x89be, 0xcf04, 0x30fb, 0x7641, 0x6a6d, 0xe707,
    0x8275, 0xb8e3, 0x471c, 0x7d8a, 0x18f8, 0x9592, 0x5a82, 0xa57d, 0xa57d, 0x5a82, 0x5a82, 0xa57d, 0xa57d,
    0x5a82, 0x471c, 0x8275, 0x18f8, 0x6a6d, 0x9592, 0xe707, 0x7d8a, 0xb8e3, 0x30fb, 0x89be, 0x7641, 0xcf04,
    0xcf04, 0x7641, 0x89be, 0x30fb, 0x18f8, 0xb8e3, 0x6a6d, 0x8275, 0x7d8a, 0x9592, 0x471c, 0xe707};
// The game's decode command: 15-bit output with bit 15 set; the word count is added.
constexpr fe::u32 kDecode = 0x3a000000;

// MPEG-1 DCT coefficient codes (table B.14 without the first-coefficient
// form), each followed by a sign bit. Escape and end of block are separate.
struct Vlc {
    const char* bits;
    int run, level;
};
const Vlc kVlc[] = {
    {"11", 0, 1}, {"011", 1, 1}, {"0100", 0, 2}, {"0101", 2, 1}, {"00101", 0, 3}, {"00111", 3, 1},
    {"00110", 4, 1}, {"000110", 1, 2}, {"000111", 5, 1}, {"000101", 6, 1}, {"000100", 7, 1},
    {"0000110", 0, 4}, {"0000100", 2, 2}, {"0000111", 8, 1}, {"0000101", 9, 1}, {"00100110", 0, 5},
    {"00100001", 0, 6}, {"00100101", 1, 3}, {"00100100", 3, 2}, {"00100111", 10, 1}, {"00100011", 11, 1},
    {"00100010", 12, 1}, {"00100000", 13, 1}, {"0000001010", 0, 7}, {"0000001100", 1, 4},
    {"0000001011", 2, 3}, {"0000001111", 4, 2}, {"0000001001", 5, 2}, {"0000001110", 14, 1},
    {"0000001101", 15, 1}, {"0000001000", 16, 1}, {"000000011101", 0, 8}, {"000000011000", 0, 9},
    {"000000010011", 0, 10}, {"000000010000", 0, 11}, {"000000011011", 1, 5}, {"000000010100", 2, 4},
    {"000000011100", 3, 3}, {"000000010010", 4, 3}, {"000000011110", 6, 2}, {"000000010101", 7, 2},
    {"000000010001", 8, 2}, {"000000011111", 17, 1}, {"000000011010", 18, 1}, {"000000011001", 19, 1},
    {"000000010111", 20, 1}, {"000000010110", 21, 1}, {"0000000011010", 0, 12}, {"0000000011001", 0, 13},
    {"0000000011000", 0, 14}, {"0000000010111", 0, 15}, {"0000000010110", 1, 6}, {"0000000010101", 1, 7},
    {"0000000010100", 2, 5}, {"0000000010011", 3, 4}, {"0000000010010", 5, 3}, {"0000000010001", 9, 2},
    {"0000000010000", 10, 2}, {"0000000011111", 22, 1}, {"0000000011110", 23, 1}, {"0000000011101", 24, 1},
    {"0000000011100", 25, 1}, {"0000000011011", 26, 1}, {"00000000011111", 0, 16}, {"00000000011110", 0, 17},
    {"00000000011101", 0, 18}, {"00000000011100", 0, 19}, {"00000000011011", 0, 20}, {"00000000011010", 0, 21},
    {"00000000011001", 0, 22}, {"00000000011000", 0, 23}, {"00000000010111", 0, 24}, {"00000000010110", 0, 25},
    {"00000000010101", 0, 26}, {"00000000010100", 0, 27}, {"00000000010011", 0, 28}, {"00000000010010", 0, 29},
    {"00000000010001", 0, 30}, {"00000000010000", 0, 31}, {"000000000011000", 0, 32},
    {"000000000010111", 0, 33}, {"000000000010110", 0, 34}, {"000000000010101", 0, 35},
    {"000000000010100", 0, 36}, {"000000000010011", 0, 37}, {"000000000010010", 0, 38},
    {"000000000010001", 0, 39}, {"000000000010000", 0, 40}, {"000000000011111", 1, 8},
    {"000000000011110", 1, 9}, {"000000000011101", 1, 10}, {"000000000011100", 1, 11},
    {"000000000011011", 1, 12}, {"000000000011010", 1, 13}, {"000000000011001", 1, 14},
    {"0000000000010011", 1, 15}, {"0000000000010010", 1, 16}, {"0000000000010001", 1, 17},
    {"0000000000010000", 1, 18}, {"0000000000010100", 6, 3}, {"0000000000011010", 11, 2},
    {"0000000000011001", 12, 2}, {"0000000000011000", 13, 2}, {"0000000000010111", 14, 2},
    {"0000000000010110", 15, 2}, {"0000000000010101", 16, 2}, {"0000000000011111", 27, 1},
    {"0000000000011110", 28, 1}, {"0000000000011101", 29, 1}, {"0000000000011100", 30, 1},
    {"0000000000011011", 31, 1},
};

// Lookup by the next 17 bits (16 code bits + sign): length 0 = no code.
struct Entry {
    fe::u8 length, run;
    fe::s16 level;
};
const std::vector<Entry>& table() {
    static std::vector<Entry> t = [] {
        std::vector<Entry> v(1u << 17);
        for (const Vlc& c : kVlc) {
            const int n = static_cast<int>(std::strlen(c.bits));
            fe::u32 code = 0;
            for (int i = 0; i < n; ++i) code = (code << 1) | static_cast<fe::u32>(c.bits[i] - '0');
            for (int sign = 0; sign < 2; ++sign) {
                const fe::u32 full = (code << 1) | static_cast<fe::u32>(sign);
                const int len = n + 1, free = 17 - len;
                for (fe::u32 rest = 0; rest < (1u << free); ++rest)
                    v[(full << free) | rest] = {static_cast<fe::u8>(len), static_cast<fe::u8>(c.run),
                                                static_cast<fe::s16>(sign ? -c.level : c.level)};
            }
        }
        return v;
    }();
    return t;
}

// Bits as the PlayStation library reads them: 16-bit little-endian words, high bit first.
class Bits {
public:
    Bits(const fe::u8* data, size_t size) : data_(data), size_(size) {}
    fe::u32 peek(int n) {
        fe::u32 v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | bit(pos_ + static_cast<size_t>(i));
        return v;
    }
    void skip(int n) { pos_ += static_cast<size_t>(n); }
    fe::u32 read(int n) {
        fe::u32 v = peek(n);
        skip(n);
        return v;
    }
    bool over() const { return pos_ > size_ * 8; }

private:
    fe::u32 bit(size_t i) const {
        const size_t word = i / 16, b = 15 - i % 16;
        if (word * 2 + 1 >= size_) return 0;
        const fe::u16 w = static_cast<fe::u16>(data_[word * 2] | (data_[word * 2 + 1] << 8));
        return (w >> b) & 1;
    }
    const fe::u8* data_;
    size_t size_, pos_ = 0;
};

// One STR v2 frame to MDEC codes. False on a bad bit stream.
bool toCodes(const std::vector<fe::u8>& frame, int blocks, std::vector<fe::u16>& codes) {
    if (frame.size() < 8) return false;
    const fe::u16 magic = static_cast<fe::u16>(frame[2] | (frame[3] << 8));
    const int q = frame[4] | (frame[5] << 8);
    const int version = frame[6] | (frame[7] << 8);
    if (magic != 0x3800 || version != 2) return false;
    const auto& t = table();
    Bits bits(frame.data() + 8, frame.size() - 8);
    codes.clear();
    for (int b = 0; b < blocks; ++b) {
        const fe::u32 dc = bits.read(10);
        codes.push_back(static_cast<fe::u16>((q << 10) | dc));
        for (int k = 0;;) {
            if (bits.peek(2) == 2) {  // end of block
                bits.skip(2);
                codes.push_back(0xFE00);
                break;
            }
            int run, level;
            if (bits.peek(6) == 1) {  // escape: 6-bit run, 10-bit level
                bits.skip(6);
                run = static_cast<int>(bits.read(6));
                level = static_cast<int>(bits.read(10));
            } else {
                const Entry& e = t[bits.peek(17)];
                if (!e.length) return false;
                bits.skip(e.length);
                run = e.run;
                level = e.level;
            }
            k += run + 1;
            if (k > 63) return false;
            codes.push_back(static_cast<fe::u16>((run << 10) | (level & 0x3FF)));
        }
        if (bits.over()) return false;
    }
    return true;
}

bool decodeFrame(const std::vector<fe::u8>& data, int width, int height, StrFrame& out) {
    const int mbw = (width + 15) / 16, mbh = (height + 15) / 16;
    std::vector<fe::u16> codes;
    if (!toCodes(data, mbw * mbh * 6, codes)) return false;
    if (codes.size() & 1) codes.push_back(0xFE00);
    fe::Mdec m;
    auto pair = [](fe::u16 lo, fe::u16 hi) { return lo | (static_cast<fe::u32>(hi) << 16); };
    m.writeCommand(0x40000001);
    for (int t = 0; t < 2; ++t)  // luminance, then colour
        for (int i = 0; i < 16; ++i) m.writeCommand(pair(kQuant[2 * i], kQuant[2 * i + 1]));
    m.writeCommand(0x60000000);
    for (int i = 0; i < 32; ++i) m.writeCommand(pair(kScale[2 * i], kScale[2 * i + 1]));
    m.writeCommand(kDecode | static_cast<fe::u32>(codes.size() / 2));
    for (size_t i = 0; i < codes.size(); i += 2) m.writeCommand(pair(codes[i], codes[i + 1]));
    // macroblocks come top to bottom, then left to right, each 16x16 15-bit pixels
    out.width = width;
    out.height = height;
    out.rgb.assign(static_cast<size_t>(width) * height * 3, 0);
    for (int mx = 0; mx < mbw; ++mx)
        for (int my = 0; my < mbh; ++my)
            for (int i = 0; i < 256; i += 2) {
                const fe::u32 w = m.readData();
                for (int k = 0; k < 2; ++k) {
                    const fe::u16 p = static_cast<fe::u16>(w >> (16 * k));
                    const int x = mx * 16 + (i + k) % 16, y = my * 16 + (i + k) / 16;
                    if (x >= width || y >= height) continue;
                    fe::u8* o = &out.rgb[(static_cast<size_t>(y) * width + x) * 3];
                    const int c0 = p & 31, c1 = (p >> 5) & 31, c2 = (p >> 10) & 31;
                    o[0] = static_cast<fe::u8>((c0 << 3) | (c0 >> 2));
                    o[1] = static_cast<fe::u8>((c1 << 3) | (c1 >> 2));
                    o[2] = static_cast<fe::u8>((c2 << 3) | (c2 >> 2));
                }
            }
    return true;
}

}  // namespace

int forEachStrFrame(fe::DiscImage& disc, const std::function<void(const StrFrame&)>& frame,
                    const std::function<void(std::uint64_t, std::uint64_t)>& progress, const std::string& only) {
    std::uint64_t total = 0, done = 0;
    for (const auto& f : disc.files())
        if (f.form2 && !f.directory && (only.empty() || f.path == only)) total += (f.size + 2047) / 2048;
    int failed = 0;
    std::vector<fe::u8> sector(2352);
    struct Pending {
        fe::u32 number = ~0u;
        int chunks = 0, have = 0, width = 0, height = 0;
        fe::u32 used = 0;
        std::vector<fe::u8> data;
        std::vector<bool> got;
    };
    StrFrame decoded;
    for (const auto& f : disc.files()) {
        if (!f.form2 || f.directory || (!only.empty() && f.path != only)) continue;
        std::map<int, Pending> streams;  // by channel
        const fe::u32 count = (f.size + 2047) / 2048;
        for (fe::u32 i = 0; i < count; ++i, ++done) {
            if ((done & 1023) == 0) progress(done, total);
            if (!disc.readRawSector(f.lba + i, sector.data())) break;
            const fe::u8 submode = sector[18];
            if (!(submode & 0x08) || (submode & 0x20)) continue;  // video is form 1 data
            const fe::u8* h = &sector[24];
            auto u16at = [&](int o) { return static_cast<int>(h[o] | (h[o + 1] << 8)); };
            auto u32at = [&](int o) { return static_cast<fe::u32>(u16at(o) | (u16at(o + 2) << 16)); };
            if (u16at(0) != 0x0160 || u16at(2) != 0x8001) continue;
            const int chunk = u16at(4), chunks = u16at(6);
            if (chunks <= 0 || chunk >= chunks || chunks > 64) continue;
            Pending& p = streams[sector[17]];
            const fe::u32 number = u32at(8);
            if (p.number != number) {
                p = Pending{};
                p.number = number;
                p.chunks = chunks;
                p.used = u32at(12);
                p.width = u16at(16);
                p.height = u16at(18);
                p.data.assign(static_cast<size_t>(chunks) * 2016, 0);
                p.got.assign(static_cast<size_t>(chunks), false);
            }
            if (chunks != p.chunks || p.got[static_cast<size_t>(chunk)]) continue;
            std::memcpy(&p.data[static_cast<size_t>(chunk) * 2016], h + 32, 2016);
            p.got[static_cast<size_t>(chunk)] = true;
            if (++p.have != p.chunks) continue;
            if (p.used && p.used < p.data.size()) p.data.resize(p.used);
            if (p.width > 0 && p.height > 0 && p.width <= 640 && p.height <= 480 &&
                decodeFrame(p.data, p.width, p.height, decoded))
                frame(decoded);
            else
                ++failed;
            p.number = ~0u;
        }
    }
    progress(total, total);
    return failed;
}

}  // namespace app
