// Headless boot runner: loads the game from a disc image, runs it for a
// number of frames and reports how far it got and which BIOS functions and
// hardware registers it needed.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <map>
#include <string>

#include "disc/DiscImage.h"
#include "psx/Cdrom.h"
#include "psx/Gpu.h"
#include "psx/Mdec.h"
#include "psx/Spu.h"
#include "psx/Machine.h"
#include "Cheats.h"
#include "InputLog.h"
#include "ModernControls.h"
#if FE_HAVE_WINDOW
#include "Launcher.h"
#include "VideoAI.h"
#include "StrVideo.h"
#include "Window.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if FE_HAVE_RECOMP
namespace fe {
RecompModule exeModule();
const JumpMeta* exeJumpMeta(size_t& count);
u32 exeImageCrc();
}  // namespace fe
#endif

namespace {

fe::u32 crc32(const fe::u8* data, size_t size) {
    fe::u32 crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

// PNG with stored (uncompressed) deflate blocks: no compression library needed.
bool writePng(const std::string& path, const std::vector<fe::u8>& rgb, int w, int h) {
    std::vector<fe::u8> raw;
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + static_cast<size_t>(y) * w * 3, rgb.begin() + static_cast<size_t>(y + 1) * w * 3);
    }
    std::vector<fe::u8> z = {0x78, 0x01};
    fe::u32 a = 1, b = 0;
    for (fe::u8 v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t off = 0; off < raw.size(); off += 65535) {
        size_t n = std::min<size_t>(65535, raw.size() - off);
        z.push_back(off + n >= raw.size());
        z.push_back(static_cast<fe::u8>(n));
        z.push_back(static_cast<fe::u8>(n >> 8));
        z.push_back(static_cast<fe::u8>(~n));
        z.push_back(static_cast<fe::u8>(~n >> 8));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
    }
    fe::u32 adler = (b << 16) | a;
    for (int i = 3; i >= 0; --i) z.push_back(static_cast<fe::u8>(adler >> (8 * i)));

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto be = [](std::vector<fe::u8>& v, fe::u32 x) {
        for (int i = 3; i >= 0; --i) v.push_back(static_cast<fe::u8>(x >> (8 * i)));
    };
    auto chunk = [&](const char* type, const std::vector<fe::u8>& body) {
        std::vector<fe::u8> c(type, type + 4);
        c.insert(c.end(), body.begin(), body.end());
        std::vector<fe::u8> head;
        be(head, static_cast<fe::u32>(body.size()));
        std::fwrite(head.data(), 1, 4, f);
        std::fwrite(c.data(), 1, c.size(), f);
        std::vector<fe::u8> tail;
        be(tail, crc32(c.data(), c.size()));
        std::fwrite(tail.data(), 1, 4, f);
    };
    std::fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    std::vector<fe::u8> ihdr;
    be(ihdr, static_cast<fe::u32>(w));
    be(ihdr, static_cast<fe::u32>(h));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    std::fclose(f);
    return true;
}

// Path of another disc of the set: "... (Disc 1).cue" -> "... (Disc N).cue".
std::string discPathFor(const std::string& path, int number) {
    size_t at = path.rfind("Disc ");
    if (at == std::string::npos || at + 5 >= path.size()) return {};
    std::string other = path;
    other[at + 5] = static_cast<char>('0' + number);
    return other;
}

// The Redump dumps of the USA release (one data track each): size and CRC32
// of the .bin, from the Redump PlayStation DAT.
struct RedumpDisc {
    const char* volumeId;
    std::uint64_t size;
    fe::u32 crc;
};
constexpr RedumpDisc kRedump[] = {
    {"SLUS-00920", 487435536, 0x0044D5F7},
    {"SLUS-01056", 505757616, 0xCB8E774F},
    {"SLUS-01057", 451666320, 0xFD0F561B},
    {"SLUS-01058", 509955936, 0x9853995E},
};

// Checks a disc image: its size and CRC32 against Redump, and every sector
// against the error detection code (EDC) stored in it, which tells a damaged
// image from a different but good dump. Returns the number of problems found.
int discNumber(const std::string& volumeId);

long verifyDisc(const std::string& path) {
    static fe::u32 table[256], crcTable[256];
    if (!table[1])
        for (fe::u32 i = 0; i < 256; ++i) {
            fe::u32 e = i, c = i;
            for (int k = 0; k < 8; ++k) {
                e = (e >> 1) ^ (e & 1 ? 0xD8018001u : 0);
                c = (c >> 1) ^ (c & 1 ? 0xEDB88320u : 0);
            }
            table[i] = e;
            crcTable[i] = c;
        }
    auto edc = [&](const fe::u8* p, size_t n) {
        fe::u32 e = 0;
        for (size_t i = 0; i < n; ++i) e = (e >> 8) ^ table[(e ^ p[i]) & 0xFF];
        return e;
    };
    fe::DiscImage disc(path);
    std::printf("disc    %s: %s, %u sectors\n", path.c_str(), disc.volumeId().c_str(), disc.sectorCount());
    if (!discNumber(disc.volumeId())) std::printf("        not a Fear Effect (USA) disc\n");
    const RedumpDisc* known = nullptr;
    for (const RedumpDisc& r : kRedump)
        if (disc.volumeId() == r.volumeId) known = &r;
    if (!disc.raw()) {
        std::printf("        2048-byte image (ISO): it cannot be compared with Redump or checked sector by sector\n");
        return 0;
    }
    static const fe::u8 sync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    fe::u8 raw[2352];
    long bad = 0;
    fe::u32 crc = 0xFFFFFFFF;
    for (fe::u32 lba = 0; lba < disc.sectorCount(); ++lba) {
        if (!disc.readRawSector(lba, raw)) {
            ++bad;
            continue;
        }
        for (fe::u8 v : raw) crc = (crc >> 8) ^ crcTable[(crc ^ v) & 0xFF];
        bool ok = std::memcmp(raw, sync, 12) == 0;
        if (ok && raw[15] == 2) {
            fe::u32 stored;
            if (raw[18] & 0x20) {  // form 2: the EDC is optional (0 = none)
                std::memcpy(&stored, raw + 2348, 4);
                ok = stored == 0 || stored == edc(raw + 16, 2332);
            } else {
                std::memcpy(&stored, raw + 2072, 4);
                ok = stored == edc(raw + 16, 2056);
            }
        }
        if (!ok && bad++ < 10) std::printf("        bad sector at lba %u\n", lba);
    }
    crc = ~crc;
    if (bad) std::printf("        %ld bad sectors: the image is damaged, rip the disc again\n", bad);
    else std::printf("        every sector is intact\n");
    const std::uint64_t size = static_cast<std::uint64_t>(disc.sectorCount()) * 2352;
    if (known && size == known->size && crc == known->crc) {
        std::printf("        matches Redump (CRC32 %08X)\n", crc);
    } else if (known) {
        std::printf("        does not match Redump: %llu bytes, CRC32 %08X (Redump: %llu bytes, CRC32 %08X)\n",
                    static_cast<unsigned long long>(size), crc, static_cast<unsigned long long>(known->size), known->crc);
        if (!bad) std::printf("        the sectors are intact, so it is probably another kind of dump; it should still play\n");
        else ++bad;
    }
    return bad;
}

// Disc number of a volume id, 0 if it is not one of the set.
int discNumber(const std::string& volumeId) {
    if (volumeId == "SLUS-00920") return 1;
    if (volumeId == "SLUS-01056") return 2;
    if (volumeId == "SLUS-01057") return 3;
    if (volumeId == "SLUS-01058") return 4;
    return 0;
}

// Save state file: a header naming the game executable it belongs to (its
// CRC) and the disc in the drive, then the machine state.
constexpr char kStateMagic[8] = {'F', 'E', 'R', 'E', 'S', 'T', 'A', '1'};

bool writeStateFile(const std::string& path, fe::u32 exeCrc, const std::string& volumeId, const std::vector<fe::u8>& payload) {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::path(path).parent_path();
    if (!dir.empty()) std::filesystem::create_directories(dir, ec);
    std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    fe::u32 idLen = static_cast<fe::u32>(volumeId.size());
    bool ok = std::fwrite(kStateMagic, 1, 8, f) == 8 && std::fwrite(&exeCrc, 4, 1, f) == 1 &&
              std::fwrite(&idLen, 4, 1, f) == 1 && std::fwrite(volumeId.data(), 1, idLen, f) == idLen &&
              std::fwrite(payload.data(), 1, payload.size(), f) == payload.size();
    ok = std::fclose(f) == 0 && ok;
    if (!ok) return false;
    std::filesystem::rename(tmp, path, ec);  // the old state is replaced only once the new one is complete
    return !ec;
}

// Empty string on success, otherwise what is wrong.
std::string readStateFile(const std::string& path, fe::u32 exeCrc, std::string& volumeId, std::vector<fe::u8>& payload) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return "no save state in " + path;
    std::vector<fe::u8> all;
    static fe::u8 chunk[1 << 16];
    for (size_t n; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) all.insert(all.end(), chunk, chunk + n);
    std::fclose(f);
    if (all.size() < 16 || std::memcmp(all.data(), kStateMagic, 8) != 0) return path + " is not a save state";
    fe::u32 crc, idLen;
    std::memcpy(&crc, all.data() + 8, 4);
    std::memcpy(&idLen, all.data() + 12, 4);
    if (crc != exeCrc) return path + " was saved from a different game executable";
    if (16 + static_cast<size_t>(idLen) > all.size()) return path + " is truncated";
    volumeId.assign(reinterpret_cast<const char*>(all.data() + 16), idLen);
    payload.assign(all.begin() + 16 + idLen, all.end());
    return {};
}

int usage() {
    std::fprintf(stderr,
                 "usage: FearEffectRe <disc.cue> [--frames N] [--interpreter] [--input script] [--shots dir] [--dump png]\n"
                 "Without --frames the game runs in a window. Keys: arrows/WASD move, E/Enter cross, Q/Esc circle,\n"
                 "F square, Space triangle, Tab L1, R R1, 1 L2, 4 R2, P start, Backspace select, F1 fast-forward,\n"
                 "F2-F5 insert disc 1-4. Saves go to ./saves (--saves dir, or --saves none for no memory card).\n"
                 "--resolution original|720p|1080p|1440p|4k and --fullscreen set the output size,\n"
                 "--display N picks the monitor (1 = primary),\n"
                 "--scale N sets the internal resolution multiplier (default: enough for the output height),\n"
                 "--video original|fast|best|seedvr2: AI upscaling of movies and backgrounds (best keeps an HD cache),\n"
                 "--widescreen: gameplay and movies (a letterboxed 2:1 picture) fill the width, without black bands,\n"
                 "--infinite-health and --infinite-ammo turn on the game's own cheats,\n"
                 "--analog: a DualShock in analog mode (the game reads the left stick; also in FearEffectRe.ini),\n"
                 "--modern: the stick or D-pad moves the character in that direction on screen (no tank controls),\n"
                 "--build-hd-cache enlarges every movie and background frame of all discs into the HD cache (hours),\n"
                 "--build-seedvr2-cache does the same with SeedVR2 for --video seedvr2 (days).\n"
                 "Save states: F6 saves, F7 loads, F8 / Shift+F8 change the slot (1-9), kept in ./states\n"
                 "(--states dir). --load-state FILE starts from a state, --save-state-at / --load-state-at FRAME:FILE\n"
                 "write or load one while running.\n"
                 "--record FILE writes the pad input of every frame, --replay FILE plays it back instead of the pad.\n"
                 "--cheats FILE loads DuckStation .cht cheats (all of them, or only those named with --cheat NAME).\n"
                 "--cd-speed N loads data N times as fast as the console (1-8; movies and music keep their speed).\n"
                 "--verify-disc compares the disc images with the Redump dumps and checks every sector for damage.\n"
                 "--volume 0-100, --mute, --audio-latency MS (40-340, default 100); also in FearEffectRe.ini [audio].\n"
                 "F11 or Alt+Enter switches between a window and fullscreen. Controls are set in FearEffectRe.ini.\n"
                 "Run without arguments for the launcher.\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    int resolution = 1;  // index into app::kResolutions
    bool fullscreen = false, viaLauncher = false;
    int display = 0;  // index into app::listDisplays()
    int scale = 0;    // internal resolution multiplier; 0 = chosen from the output height
    bool modern = false;  // screen-relative movement instead of tank controls
    bool widescreen = false;  // letterboxed picture without its black bands
    bool infiniteHealth = false, infiniteAmmo = false;
#if FE_HAVE_WINDOW
    app::VideoMode videoMode = app::VideoMode::Original;
#endif
    std::string discPath;
#if FE_HAVE_WINDOW
    SetProcessDPIAware();  // window sizes are real pixels
    if (argc < 2) {
        app::LaunchSettings launch;
        if (!app::runLauncher(launch)) return 0;
        discPath = launch.discPath;
        resolution = launch.resolution;
        fullscreen = launch.fullscreen;
        display = launch.display;
        videoMode = static_cast<app::VideoMode>(launch.video);
        modern = launch.modern;
        widescreen = launch.widescreen;
        infiniteHealth = launch.infiniteHealth;
        infiniteAmmo = launch.infiniteAmmo;
        viaLauncher = true;
        FreeConsole();  // started by double-click: no need for the console window
    }
#else
    if (argc < 2) return usage();
#endif
    if (!viaLauncher) discPath = argv[1];
    bool buildHdCache = false, buildSeedvr2 = false;
    std::string exportVideo, exportDir;  // --export-video FILE DIR: a movie's frames as PPM pictures
    unsigned frames = 0;  // 0 = play in a window until it is closed
    size_t maxLog = 80;
    bool interpreterOnly = false, logBios = false;
    int logCd = 0, logSio = 0;
    std::string dumpPath, vramPath, ramDir, shotDir, inputScript, wavPath, swapScript;
    std::string savesDir = "saves";  // memory card in slot 1; "none" removes the card
    std::vector<fe::s16> wav;
    double audioEnergy = 0;
    fe::u64 audioSamples = 0;
    int audioPeak = 0;
    unsigned shotEvery = 300;
    std::string statesDir = "states", loadStatePath, saveStateAt, loadStateAt, recordPath, replayPath, cheatsPath;
    std::vector<std::string> cheatNames;
    int cdSpeed = 1;
    int volume = -1, audioLatency = -1;  // -1: from FearEffectRe.ini
    bool mute = false;
    bool verifyOnly = false;
    bool analogPad = false;  // DualShock in analog mode instead of a digital pad
    fe::u64 ramFrom = 0;  // first frame --ram writes
    bool hdShots = false;  // --shots at the internal resolution instead of the console's
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) frames = static_cast<unsigned>(std::atoi(argv[++i]));
        else if (a == "--max-log" && i + 1 < argc) maxLog = static_cast<size_t>(std::atoi(argv[++i]));
        else if (a == "--interpreter") interpreterOnly = true;
        else if (a == "--log-bios") logBios = true;
        else if (a == "--fullscreen") fullscreen = true;
        else if (a == "--scale" && i + 1 < argc) scale = std::atoi(argv[++i]);
        else if (a == "--display" && i + 1 < argc) display = std::atoi(argv[++i]) - 1;
#if FE_HAVE_WINDOW
        else if (a == "--resolution" && i + 1 < argc) {
            const app::Resolution* r = app::findResolution(argv[++i]);
            if (!r) return usage();
            resolution = static_cast<int>(r - app::kResolutions);
        }
        else if (a == "--video" && i + 1 < argc) {
            if (!app::parseVideoMode(argv[++i], videoMode)) return usage();
        }
#endif
        else if (a == "--swap" && i + 1 < argc) swapScript = argv[++i];
        else if (a == "--saves" && i + 1 < argc) savesDir = argv[++i];
        else if (a == "--wav" && i + 1 < argc) wavPath = argv[++i];
        else if (a == "--log-sio" && i + 1 < argc) logSio = std::atoi(argv[++i]);
        else if (a == "--shots" && i + 1 < argc) shotDir = argv[++i];
        else if (a == "--ram" && i + 1 < argc) ramDir = argv[++i];
        else if (a == "--ram-from" && i + 1 < argc) ramFrom = static_cast<fe::u64>(std::atoll(argv[++i]));
        else if (a == "--hd-shots") hdShots = true;
        else if (a == "--every" && i + 1 < argc) shotEvery = static_cast<unsigned>(std::atoi(argv[++i]));
        else if (a == "--input" && i + 1 < argc) inputScript = argv[++i];
        else if (a == "--dump" && i + 1 < argc) dumpPath = argv[++i];
        else if (a == "--dump-vram" && i + 1 < argc) vramPath = argv[++i];
        else if (a == "--log-cd" && i + 1 < argc) logCd = std::atoi(argv[++i]);
        else if (a == "--build-hd-cache") buildHdCache = true;
        else if (a == "--build-seedvr2-cache") buildSeedvr2 = true;
        else if (a == "--export-video" && i + 2 < argc) {
            exportVideo = argv[++i];
            exportDir = argv[++i];
        }
        else if (a == "--verify-disc") verifyOnly = true;
        else if (a == "--states" && i + 1 < argc) statesDir = argv[++i];
        else if (a == "--load-state" && i + 1 < argc) loadStatePath = argv[++i];
        else if (a == "--save-state-at" && i + 1 < argc) saveStateAt = argv[++i];
        else if (a == "--load-state-at" && i + 1 < argc) loadStateAt = argv[++i];
        else if (a == "--record" && i + 1 < argc) recordPath = argv[++i];
        else if (a == "--replay" && i + 1 < argc) replayPath = argv[++i];
        else if (a == "--cheats" && i + 1 < argc) cheatsPath = argv[++i];
        else if (a == "--cheat" && i + 1 < argc) cheatNames.push_back(argv[++i]);
        else if (a == "--cd-speed" && i + 1 < argc) cdSpeed = std::clamp(std::atoi(argv[++i]), 1, 8);
        else if (a == "--volume" && i + 1 < argc) volume = std::clamp(std::atoi(argv[++i]), 0, 100);
        else if (a == "--mute") mute = true;
        else if (a == "--audio-latency" && i + 1 < argc) audioLatency = std::clamp(std::atoi(argv[++i]), 40, 340);
        else if (a == "--modern") modern = true;
        else if (a == "--analog") analogPad = true;
        else if (a == "--widescreen") widescreen = true;
        else if (a == "--infinite-health") infiniteHealth = true;
        else if (a == "--infinite-ammo") infiniteAmmo = true;
        else return usage();
    }
#if FE_HAVE_WINDOW
    if (!exportVideo.empty()) {
        fe::DiscImage disc(discPath);
        std::filesystem::create_directories(exportDir);
        int n = 0;
        int failed = app::forEachStrFrame(
            disc,
            [&](const app::StrFrame& f) {
                char name[32];
                std::snprintf(name, sizeof name, "/%05d.ppm", n++);
                FILE* out = std::fopen((exportDir + name).c_str(), "wb");
                if (!out) return;
                std::fprintf(out, "P6\n%d %d\n255\n", f.width, f.height);
                std::fwrite(f.rgb.data(), 1, f.rgb.size(), out);
                std::fclose(out);
            },
            [](std::uint64_t, std::uint64_t) {}, exportVideo);
        std::printf("export  %d frames of %s written to %s (%d not decoded)\n", n, exportVideo.c_str(), exportDir.c_str(), failed);
        return 0;
    }
    if (buildHdCache || buildSeedvr2) {
        // every disc of the set that is found next to the given one
        std::vector<std::string> discs{discPath};
        for (int n = 2; n <= 4; ++n) {
            std::string other = discPathFor(discPath, n);
            std::error_code ec;
            if (!other.empty() && std::filesystem::exists(other, ec)) discs.push_back(other);
        }
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        if (buildSeedvr2) return app::buildSeedVR2Cache(discs, exeDir / "ai", exeDir / "hdcache");
        return app::buildHdCache(discs, exeDir / "ai", exeDir / "hdcache");
    }
#endif

    if (verifyOnly) {
        // every disc of the set found next to the given one
        long bad = 0;
        try {
            bad += std::max(0L, verifyDisc(discPath));
            for (int n = 2; n <= 4; ++n) {
                std::string other = discPathFor(discPath, n);
                std::error_code ec;
                if (!other.empty() && other != discPath && std::filesystem::exists(other, ec)) bad += std::max(0L, verifyDisc(other));
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return 1;
        }
        return bad ? 1 : 0;
    }
    try {
        auto discPtr = std::make_shared<fe::DiscImage>(discPath);
        fe::DiscImage& disc = *discPtr;
#if FE_HAVE_WINDOW
        // SeedVR2 shows only pictures from its own cache, built disc by disc:
        // say so when the disc being played has not been built
        auto seedvr2Check = [&](fe::DiscImage& d) {
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            const auto cache = std::filesystem::path(exePath).parent_path() / "hdcache";
            if (app::seedvr2DiscReady(cache, d.volumeId())) return;
            const std::string& v = d.volumeId();
            const int number = v == "SLUS-01056" ? 2 : v == "SLUS-01057" ? 3 : v == "SLUS-01058" ? 4 : 1;
            std::string text = "The SeedVR2 HD cache has not been built for disc " + std::to_string(number) + " (" + v +
                               ").\n\nOn this disc, movies and backgrounds without a SeedVR2 picture are shown as on "
                               "the console, not upscaled. To build it, choose \"AI upscale, SeedVR2\" in the launcher "
                               "and press \"Build HD cache...\" (it builds every disc found next to disc 1).";
            std::printf("video   SeedVR2 cache not built for disc %d (%s)\n", number, v.c_str());
            if (viaLauncher) MessageBoxA(nullptr, text.c_str(), "FearEffectRe - SeedVR2", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        };
#endif
        auto boot = disc.bootExecutable();
        if (!boot) throw std::runtime_error("SYSTEM.CNF does not name a boot executable");
        auto exe = disc.readFile(*boot);
        std::printf("disc    %s (%u sectors, %zu files)\n", disc.volumeId().c_str(), disc.sectorCount(),
                    disc.files().size());
        std::printf("boot    %s, %zu bytes\n", boot->path.c_str(), exe.size());
        for (const RedumpDisc& r : kRedump)
            if (disc.volumeId() == r.volumeId && disc.raw() && static_cast<std::uint64_t>(disc.sectorCount()) * 2352 != r.size)
                std::printf("note    this image is not the size of the Redump dump; --verify-disc checks it for damage\n");

        fe::Machine m;
        m.loadExe(exe);
        const fe::u32 exeCrc = crc32(exe.data() + 0x800, exe.size() - 0x800);  // save states belong to this executable
        m.cd = std::make_shared<fe::Cdrom>(discPtr);
        m.cd->traceLeft = logCd;
        m.cd->speed = cdSpeed;
        m.logBios = logBios;
#if FE_HAVE_WINDOW
        if (scale == 0 && frames == 0) {
            // enough lines to fill the output: the console draws 240
            int outHeight = app::kResolutions[resolution].height;
            if (fullscreen) {
                auto displays = app::listDisplays();
                outHeight = displays[display >= 0 && display < static_cast<int>(displays.size()) ? display : 0].height;
            }
            scale = (outHeight + 239) / 240;
        }
#endif
        if (scale < 1) scale = 1;
        m.gpu->setScale(scale);
#if FE_HAVE_WINDOW
        if (videoMode != app::VideoMode::Original && scale > 1) {
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
            std::string error;
            auto up = app::createAiUpscaler(videoMode, exeDir / "ai", exeDir / "hdcache", error);
            if (up) {
                m.gpu->setUpscaler(up);
                if (videoMode == app::VideoMode::AiSeedVR2) seedvr2Check(disc);
                std::printf("video   AI upscaling (%s)\n", app::videoModeId(videoMode));
            } else {
                std::printf("video   AI upscaling is off: %s\n", error.c_str());
                if (viaLauncher) {
                    std::string text = "AI upscaling is not available, the game runs without it.\n\n" + error;
                    MessageBoxA(nullptr, text.c_str(), "FearEffectRe", MB_OK | MB_ICONWARNING);
                }
            }
        }
#endif
        if (savesDir != "none") {
            std::error_code ec;
            std::filesystem::create_directories(savesDir, ec);
            if (!ec) m.cardDir = savesDir;
        }
        m.sioTrace = logSio;
        m.padAnalog = analogPad;
        const char* mode = "interpreter";
#if FE_HAVE_RECOMP
        if (!interpreterOnly) {
            fe::u32 crc = crc32(exe.data() + 0x800, exe.size() - 0x800);
            if (crc == fe::exeImageCrc()) {
                m.modules.push_back(fe::exeModule());
                size_t n = 0;
                const fe::JumpMeta* meta = fe::exeJumpMeta(n);
                m.jumpMeta.assign(meta, meta + n);
                mode = "recompiled";
            } else {
                std::printf("note    this executable differs from the one the code was generated from\n");
            }
        }
#endif
        // --input "frame:button[+button][*frames],...": a press is held for 6 frames unless *frames says otherwise
        struct Press {
            fe::u64 frame;
            fe::u16 mask;
            fe::u64 length;  // frames held
            float stickX = 0, stickY = 0;  // left stick: stick-e/-w/-n/-s push it that way, fully or by the percent after it (stick-n40)
        };
        std::vector<Press> presses;
        {
            static const std::pair<const char*, fe::u16> names[] = {
                {"select", 0x0001}, {"start", 0x0008}, {"up", 0x0010}, {"right", 0x0020}, {"down", 0x0040},
                {"left", 0x0080},   {"l2", 0x0100},    {"r2", 0x0200}, {"l1", 0x0400},    {"r1", 0x0800},
                {"triangle", 0x1000}, {"circle", 0x2000}, {"cross", 0x4000}, {"square", 0x8000}};
            size_t pos = 0;
            while (pos < inputScript.size()) {
                size_t end = inputScript.find(',', pos);
                std::string item = inputScript.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                pos = end == std::string::npos ? inputScript.size() : end + 1;
                size_t colon = item.find(':');
                if (colon == std::string::npos) continue;
                Press p{static_cast<fe::u64>(std::atoll(item.substr(0, colon).c_str())), 0, 6};
                std::string buttons = item.substr(colon + 1);
                if (size_t star = buttons.find('*'); star != std::string::npos) p.length = static_cast<fe::u64>(std::atoll(buttons.substr(star + 1).c_str()));
                for (const auto& n : names)
                    if (buttons.find(n.first) != std::string::npos) p.mask |= n.second;
                auto push = [&](const char* name) -> float {
                    size_t at = buttons.find(name);
                    if (at == std::string::npos) return 0.0f;
                    int percent = std::atoi(buttons.c_str() + at + 7);
                    return percent > 0 ? std::min(percent, 100) / 100.0f : 1.0f;
                };
                p.stickX = push("stick-e") - push("stick-w");
                p.stickY = push("stick-n") - push("stick-s");
                presses.push_back(p);
            }
        }
#if FE_HAVE_WINDOW
        std::unique_ptr<app::Window> window;
        if (frames == 0)
            window = std::make_unique<app::Window>(app::kResolutions[resolution].width, app::kResolutions[resolution].height,
                                                   fullscreen, display);
        if (window) {
            window->setWidescreen(widescreen);
            // controls and sound settings live in FearEffectRe.ini; the command line overrides the sound ones
            const std::wstring ini = app::settingsPath();
            window->setControls(app::Controls::load(ini));
            if (volume < 0) volume = static_cast<int>(GetPrivateProfileIntW(L"audio", L"volume", 100, ini.c_str()));
            if (audioLatency < 0) audioLatency = static_cast<int>(GetPrivateProfileIntW(L"audio", L"latency", 100, ini.c_str()));
            mute = mute || GetPrivateProfileIntW(L"audio", L"mute", 0, ini.c_str()) != 0;
            analogPad = analogPad || window->controls().analog;
            app::Controls withPad = window->controls();
            withPad.analog = analogPad;  // --analog too: the stick then stops pressing the D-pad
            window->setControls(withPad);
            m.padAnalog = analogPad;
            wchar_t probe[8];
            if (!GetPrivateProfileStringW(L"audio", L"volume", L"", probe, 8, ini.c_str())) {
                WritePrivateProfileStringW(L"audio", L"volume", L"100", ini.c_str());
                WritePrivateProfileStringW(L"audio", L"mute", L"0", ini.c_str());
                WritePrivateProfileStringW(L"audio", L"latency", L"100", ini.c_str());
            }
            window->setAudio(volume, mute, audioLatency);
        }
#else
        if (frames == 0) frames = 300;
#endif
        std::unique_ptr<app::InputRecorder> recorder;
        std::unique_ptr<app::InputReplay> replay;
        if (!recordPath.empty()) recorder = std::make_unique<app::InputRecorder>(recordPath, analogPad, modern);
        if (!replayPath.empty()) {
            replay = std::make_unique<app::InputReplay>(replayPath);
            // played back with the pad and movement it was recorded with
            analogPad = replay->analog();
            m.padAnalog = analogPad;
            modern = replay->modern();
            std::printf("replay  %s (input until frame %llu)\n", replayPath.c_str(), static_cast<unsigned long long>(replay->lastFrame()));
        }
        std::unique_ptr<app::Cheats> cheats;
        if (!cheatsPath.empty()) {
            cheats = std::make_unique<app::Cheats>(cheatsPath, cheatNames);
            for (const auto& c : cheats->active()) std::printf("cheat   %s (%zu codes)\n", c.name.c_str(), c.codes.size());
            for (const auto& s : cheats->skipped) std::printf("cheat   code type not supported, skipped: %s\n", s.c_str());
            if (cheats->active().empty()) std::printf("cheat   no cheats loaded from %s\n", cheatsPath.c_str());
        }
        auto insertDisc = [&](int number) {
            std::string path = discPathFor(discPath, number);
            try {
                auto next = std::make_shared<fe::DiscImage>(path);
                m.cd->insert(m, next);
                if (recorder) recorder->disc(m.frame, number);
#if FE_HAVE_WINDOW
                if (videoMode == app::VideoMode::AiSeedVR2) seedvr2Check(*next);
#endif
                m.note("disc " + std::to_string(number) + " inserted");
            } catch (const std::exception& e) {
                m.note(std::string("cannot insert disc: ") + e.what());
            }
        };
        // --swap "frame:disc": change disc at that frame (headless testing)
        fe::u64 swapFrame = ~0ull;
        int swapDisc = 0;
        if (size_t colon = swapScript.find(':'); colon != std::string::npos) {
            swapFrame = static_cast<fe::u64>(std::atoll(swapScript.substr(0, colon).c_str()));
            swapDisc = std::atoi(swapScript.substr(colon + 1).c_str());
        }
        // ---- save states ----
        // A state is written at the machine's next safe point (within a
        // fraction of a frame); loading replaces the machine and restarts it.
        int slot = 1;
        std::string pendingSave;
        auto slotPath = [&](int n) { return statesDir + "/slot" + std::to_string(n) + ".state"; };
        auto say = [&](const std::string& text) {
            std::printf("state   %s\n", text.c_str());
#if FE_HAVE_WINDOW
            if (window) window->status(text);
#endif
        };
        m.onSafePoint = [&]() {
            const std::string path = pendingSave;
            pendingSave.clear();
            if (writeStateFile(path, exeCrc, m.cd->disc->volumeId(), m.saveState())) say("saved " + path + " at frame " + std::to_string(m.frame));
            else say("could not write " + path);
        };
        auto requestSave = [&](const std::string& path) {
            pendingSave = path;
            m.safePointWanted = true;
        };
        // Reads a state and puts the disc it was saved with in the drive.
        // Returns the machine state, or empty (after saying why) when it cannot be used.
        auto readState = [&](const std::string& path) -> std::vector<fe::u8> {
            std::string volume;
            std::vector<fe::u8> payload;
            if (std::string err = readStateFile(path, exeCrc, volume, payload); !err.empty()) {
                say(err);
                return {};
            }
            if (volume != m.cd->disc->volumeId()) {
                const int number = discNumber(volume);
                const std::string other = number ? discPathFor(discPath, number) : std::string();
                try {
                    if (other.empty()) throw std::runtime_error("unknown disc " + volume);
                    m.cd->disc = std::make_shared<fe::DiscImage>(other);
                } catch (const std::exception& e) {
                    say(path + " needs disc " + std::to_string(number) + ": " + e.what());
                    return {};
                }
            }
            return payload;
        };
        if (!loadStatePath.empty()) {
            std::vector<fe::u8> payload = readState(loadStatePath);
            if (payload.empty()) throw std::runtime_error("cannot start from " + loadStatePath);
            m.loadState(payload);
            say("started from " + loadStatePath + " (frame " + std::to_string(m.frame) + ")");
        }
        // --save-state-at FRAME:FILE (headless testing)
        fe::u64 saveAtFrame = ~0ull;
        std::string saveAtPath;
        if (size_t colon = saveStateAt.find(':'); colon != std::string::npos) {
            saveAtFrame = static_cast<fe::u64>(std::atoll(saveStateAt.substr(0, colon).c_str()));
            saveAtPath = saveStateAt.substr(colon + 1);
        }
        // --load-state-at FRAME:FILE: loads a state while running, once (headless testing of F7)
        fe::u64 loadAtFrame = ~0ull;
        std::string loadAtPath;
        if (size_t colon = loadStateAt.find(':'); colon != std::string::npos) {
            loadAtFrame = static_cast<fe::u64>(std::atoll(loadStateAt.substr(0, colon).c_str()));
            loadAtPath = loadStateAt.substr(colon + 1);
        }

        fe::u64 lastActivity = ~0ull;
        app::ModernControls modernControls;
        m.onVblank = [&]() {
            bool analog = false;
            float stickX = 0, stickY = 0;
            fe::u16 held = 0;
            if (m.frame == swapFrame) insertDisc(swapDisc);
            if (m.frame == saveAtFrame) requestSave(saveAtPath);
            if (m.frame == loadAtFrame) {
                loadAtFrame = ~0ull;
                std::vector<fe::u8> payload = readState(loadAtPath);
                if (!payload.empty()) {
                    say("loaded " + loadAtPath + " at frame " + std::to_string(m.frame));
                    throw fe::Restart{std::move(payload)};
                }
            }
            if (replay)
                if (int disc = replay->disc(m.frame)) insertDisc(disc);
            for (fe::s16 v : m.audio) {
                audioEnergy += static_cast<double>(v) * v;
                audioPeak = std::max(audioPeak, std::abs(static_cast<int>(v)));
            }
            audioSamples += m.audio.size();
            if (!wavPath.empty()) wav.insert(wav.end(), m.audio.begin(), m.audio.end());
#if FE_HAVE_WINDOW
            if (window) {
                const fe::u16* pixels;
                int stride, fx, fy, w = 0, h = 0;
                bool open;
                // the game draws 30 or fewer pictures a second: nothing to show when the GPU was idle
                fe::u64 activity = m.gp0Words + m.gp1Words;
                bool changed = activity != lastActivity || (m.frame & 63) == 0;
                lastActivity = activity;
                if (!changed) {
                    open = window->idle();
                } else if (m.gpu->displayEnabled && m.gpu->frame(pixels, stride, fx, fy, w, h)) {
                    open = window->present(pixels, stride, fx, fy, w, h);
                } else {
                    std::vector<fe::u8> rgb;
                    m.gpu->display(rgb, w, h);
                    open = window->present(rgb, w, h);
                }
                if (!open) throw fe::Stop{"window closed"};
                held |= window->buttons();
                analog = window->stick(stickX, stickY);
                if (int disc = window->discRequest(); disc && !replay) insertDisc(disc);
                window->audio(m.audio);
                for (app::Hotkey key : window->hotkeys()) {
                    if (key == app::Hotkey::NextSlot || key == app::Hotkey::PrevSlot) {
                        slot = (slot + (key == app::Hotkey::NextSlot ? 1 : 7)) % 9 + 1;
                        std::error_code ec;
                        say("slot " + std::to_string(slot) + (std::filesystem::exists(slotPath(slot), ec) ? "" : " (empty)"));
                    } else if (key == app::Hotkey::Controls) {
                        if (window->editControls(app::settingsPath())) {
                            // a recording keeps the pad it started with
                            if (!recorder && !replay) analogPad = window->controls().analog;
                            app::Controls withPad = window->controls();
                            withPad.analog = analogPad;
                            window->setControls(withPad);
                            m.padAnalog = analogPad;
                            say("controls saved");
                        }
                    } else if (key == app::Hotkey::SaveState) {
                        requestSave(slotPath(slot));
                    } else {
                        std::vector<fe::u8> payload = readState(slotPath(slot));
                        if (!payload.empty()) {
                            m.audio.clear();
                            say("loaded slot " + std::to_string(slot));
                            throw fe::Restart{std::move(payload)};
                        }
                    }
                }
            }
#endif
            m.audio.clear();
            for (const auto& p : presses)
                if (m.frame >= p.frame && m.frame < p.frame + p.length) {
                    held |= p.mask;
                    if (p.stickX || p.stickY) {
                        analog = true;
                        stickX = p.stickX;
                        stickY = p.stickY;
                    }
                }
            // The stick at the pad's resolution (a byte per axis, 80h centre),
            // so a replay, which has only those bytes, steers exactly alike.
            fe::u8 rawX = 0x80, rawY = 0x80;
            if (analog) {
                rawX = static_cast<fe::u8>(std::clamp(std::lround(128.0f + stickX * 127.5f), 0L, 255L));
                rawY = static_cast<fe::u8>(std::clamp(std::lround(128.0f - stickY * 127.5f), 0L, 255L));
                if (rawX == 0x80 && rawY == 0x80) rawY = 0x7F;  // pushed, however little
            }
            // --replay: the recorded controller replaces everything above;
            // --record: what the player did, before modern movement
            if (replay) {
                held = replay->buttons(m.frame);
                const fe::u16 st = replay->stick(m.frame);
                rawX = static_cast<fe::u8>(st >> 8);
                rawY = static_cast<fe::u8>(st);
            }
            if (recorder) recorder->buttons(m.frame, held, static_cast<fe::u16>((rawX << 8) | rawY));
            analog = rawX != 0x80 || rawY != 0x80;
            stickX = analog ? (rawX - 128.0f) / 127.5f : 0.0f;
            stickY = analog ? (128.0f - rawY) / 127.5f : 0.0f;

            if (modern) held = modernControls.apply(m, held, analog, stickX, stickY);
            if (analogPad) {
                // the left stick as the game sees it: 00h left/up, 80h centre, FFh right/down
                auto axis = [](float v) { return static_cast<fe::u8>(std::clamp(128.0f + v * 127.5f, 0.0f, 255.0f)); };
                float ax = analog ? stickX : 0.0f, ay = analog ? stickY : 0.0f;
                if (modern && modernControls.steering()) {
                    // Modern movement turns the character and walks it forward: the
                    // game must see the stick pushed forward too (it reads the stick
                    // the tank way), as far as the player pushes it, so slow walking
                    // still works. Nothing while it is still turning.
                    const float push = analog ? std::min(1.0f, std::hypot(stickX, stickY)) : 1.0f;
                    ax = 0.0f;
                    ay = (held & 0x0010) ? push : 0.0f;
                    // the D-pad would make it walk at full speed whatever the stick says
                    if (analog) held = static_cast<fe::u16>(held & ~0x00F0);
                }
                m.padAxes[2] = axis(ax);
                m.padAxes[3] = axis(-ay);
            }
            if (cheats) cheats->apply(m.ram.get());
            // The game's own cheat switches (the published GameShark codes
            // 80077810 0001 and 0002): bit 0 infinite ammo, bit 1 infinite health.
            if (infiniteHealth || infiniteAmmo) {
                fe::u16 flags;
                std::memcpy(&flags, m.ram.get() + 0x77810, 2);
                flags = static_cast<fe::u16>(flags | (infiniteAmmo ? 1 : 0) | (infiniteHealth ? 2 : 0));
                std::memcpy(m.ram.get() + 0x77810, &flags, 2);
            }
            m.padState = static_cast<fe::u16>(~held);
            // --ram <dir>: main memory at the same frames as the screenshots (finding game variables)
            if (!ramDir.empty() && shotEvery && m.frame >= ramFrom && m.frame % shotEvery == 0) {
                char name[32];
                std::snprintf(name, sizeof name, "/f%06llu.ram", static_cast<unsigned long long>(m.frame));
                if (FILE* f = std::fopen((ramDir + name).c_str(), "wb")) {
                    std::fwrite(m.ram.get(), 1, fe::kRamSize, f);
                    std::fclose(f);
                }
            }
            if (!shotDir.empty() && shotEvery && m.frame % shotEvery == 0) {
                std::vector<fe::u8> rgb;
                int w = 0, h = 0;
                const fe::u16* hp;
                int hs, hx, hy;
                if (hdShots && m.gpu->scale > 1 && m.gpu->displayEnabled && m.gpu->frame(hp, hs, hx, hy, w, h)) {
                    rgb.resize(static_cast<size_t>(w) * h * 3);
                    for (int yy = 0; yy < h; ++yy)
                        for (int xx = 0; xx < w; ++xx) {
                            const fe::u16 p = hp[static_cast<size_t>(hy + yy) * hs + hx + xx];
                            fe::u8* o = &rgb[(static_cast<size_t>(yy) * w + xx) * 3];
                            o[0] = static_cast<fe::u8>((p & 31) << 3);
                            o[1] = static_cast<fe::u8>(((p >> 5) & 31) << 3);
                            o[2] = static_cast<fe::u8>(((p >> 10) & 31) << 3);
                        }
                } else {
                    m.gpu->display(rgb, w, h);
                }
                char name[32];
                std::snprintf(name, sizeof name, "/f%06llu.png", static_cast<unsigned long long>(m.frame));
                writePng(shotDir + name, rgb, w, h);
            }
        };
        m.nativeEnabled = !interpreterOnly;
        m.cycleLimit = frames ? static_cast<fe::u64>(frames) * fe::kCyclesPerFrame : ~0ull;
        std::printf("cpu     %s, entry %08x, sp %08x\n", mode, m.cpu.pc, m.cpu.r[29]);
        std::printf("render  %dx internal resolution\n", m.gpu->scale);

        auto wallStart = std::chrono::steady_clock::now();
        std::string reason = "returned";
        try {
            m.run();
        } catch (const fe::Stop& stop) {
            reason = stop.reason;
        } catch (const fe::ExceptionReturn&) {
            reason = "ReturnFromException outside an interrupt";
        }
        std::printf("stopped %s after %llu frames (%llu cycles), pc %08x ra %08x\n", reason.c_str(),
                    static_cast<unsigned long long>(m.frame), static_cast<unsigned long long>(m.clock), m.cpu.pc,
                    m.cpu.r[31]);
        {
            double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
            std::printf("speed   %.1f frames per second over %.1f s (console: 59.94)\n", wall > 0 ? m.frame / wall : 0.0, wall);
        }
        std::printf("irq     I_STAT %04x I_MASK %04x SR %08x hook %08x\n", m.iStat, m.iMask, m.cpu.cop0[12],
                    m.entryIntHook);
        std::printf("cdrom   %llu commands, %llu sectors read, %llu XA audio sectors\n",
                    static_cast<unsigned long long>(m.cd->commands), static_cast<unsigned long long>(m.cd->sectorsRead),
                    static_cast<unsigned long long>(m.cd->xaSectors));
        std::printf("audio   %llu key-ons, %.1f s generated, peak %d, rms %.0f\n", static_cast<unsigned long long>(m.spu->keyOns),
                    audioSamples / 2 / 44100.0, audioPeak, audioSamples ? std::sqrt(audioEnergy / audioSamples) : 0.0);
        if (!wavPath.empty()) {
            if (FILE* f = std::fopen(wavPath.c_str(), "wb")) {
                fe::u32 bytes = static_cast<fe::u32>(wav.size() * 2);
                fe::u32 head[11] = {0x46464952, 36 + bytes, 0x45564157, 0x20746D66, 16, 0x00020001, 44100, 44100 * 4, 0x00100004, 0x61746164, bytes};
                std::fwrite(head, 4, 11, f);
                std::fwrite(wav.data(), 2, wav.size(), f);
                std::fclose(f);
            }
        }
        std::printf("mdec    %llu macroblocks decoded\n", static_cast<unsigned long long>(m.mdec->macroblocks));
        for (const auto& t : m.mdec->trace) std::printf("  %s\n", t.c_str());
        {
            std::vector<fe::u8> rgb;
            int w = 0, h = 0;
            m.gpu->display(rgb, w, h);
            std::printf("display %dx%d at (%d,%d) %s, %llu primitives drawn\n", w, h, m.gpu->dispX, m.gpu->dispY,
                        m.gpu->displayEnabled ? "on" : "off", static_cast<unsigned long long>(m.gpu->primitives));
            if (!dumpPath.empty()) writePng(dumpPath, rgb, w, h);
            if (!vramPath.empty()) {
                std::vector<fe::u8> all(1024 * 512 * 3);
                for (size_t i = 0; i < 1024 * 512; ++i) {
                    fe::u16 p = m.gpu->vram[i];
                    all[i * 3] = static_cast<fe::u8>((p & 31) << 3);
                    all[i * 3 + 1] = static_cast<fe::u8>(((p >> 5) & 31) << 3);
                    all[i * 3 + 2] = static_cast<fe::u8>(((p >> 10) & 31) << 3);
                }
                writePng(vramPath, all, 1024, 512);
            }
        }
        std::printf("gpu     %llu GP0 words, %llu GP1 words\n", static_cast<unsigned long long>(m.gp0Words),
                    static_cast<unsigned long long>(m.gp1Words));

        std::map<std::string, size_t> counts;
        std::vector<std::string> order;
        for (const auto& line : m.log)
            if (counts[line]++ == 0) order.push_back(line);
        std::printf("log     %zu lines, %zu distinct\n", m.log.size(), order.size());
        for (size_t i = 0; i < order.size() && i < maxLog; ++i)
            std::printf("  %s%s\n", order[i].c_str(),
                        counts[order[i]] > 1 ? ("  (x" + std::to_string(counts[order[i]]) + ")").c_str() : "");

        std::vector<std::pair<fe::u64, fe::u32>> io;
        for (size_t i = 0; i < m.ioAccess.size(); ++i)
            if (m.ioAccess[i]) io.push_back({m.ioAccess[i], 0x1F801000u + static_cast<fe::u32>(i) * 4});
        std::sort(io.rbegin(), io.rend());
        std::printf("io      %zu registers touched; busiest:\n", io.size());
        for (size_t i = 0; i < io.size() && i < 24; ++i)
            std::printf("  %08x  %llu\n", io[i].second, static_cast<unsigned long long>(io[i].first));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
#if FE_HAVE_WINDOW
        if (viaLauncher) MessageBoxA(nullptr, e.what(), "FearEffectRe", MB_OK | MB_ICONERROR);
#endif
        return 1;
    }
    return 0;
}
