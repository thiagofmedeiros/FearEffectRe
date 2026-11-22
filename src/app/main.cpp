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
#include "ModernControls.h"
#if FE_HAVE_WINDOW
#include "Launcher.h"
#include "VideoAI.h"
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

int usage() {
    std::fprintf(stderr,
                 "usage: FearEffectRe <disc.cue> [--frames N] [--interpreter] [--input script] [--shots dir] [--dump png]\n"
                 "Without --frames the game runs in a window. Keys: arrows/WASD move, E/Enter cross, Q/Esc circle,\n"
                 "F square, Space triangle, Tab L1, R R1, 1 L2, 4 R2, P start, Backspace select, F1 fast-forward,\n"
                 "F2-F5 insert disc 1-4. Saves go to ./saves (--saves dir, or --saves none for no memory card).\n"
                 "--resolution original|720p|1080p|1440p|4k and --fullscreen set the output size,\n"
                 "--display N picks the monitor (1 = primary),\n"
                 "--scale N sets the internal resolution multiplier (default: enough for the output height),\n"
                 "--video original|fast|best: AI upscaling of movies and backgrounds (best keeps an HD cache),\n"
                 "--widescreen: gameplay and movies (a letterboxed 2:1 picture) fill the width, without black bands,\n"
                 "--modern: the stick or D-pad moves the character in that direction on screen (no tank controls),\n"
                 "--build-hd-cache enlarges every movie and background frame of all discs into the HD cache (hours).\n"
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
        viaLauncher = true;
        FreeConsole();  // started by double-click: no need for the console window
    }
#else
    if (argc < 2) return usage();
#endif
    if (!viaLauncher) discPath = argv[1];
    bool buildHdCache = false;
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
    fe::u64 ramFrom = 0;  // first frame --ram writes
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
        else if (a == "--every" && i + 1 < argc) shotEvery = static_cast<unsigned>(std::atoi(argv[++i]));
        else if (a == "--input" && i + 1 < argc) inputScript = argv[++i];
        else if (a == "--dump" && i + 1 < argc) dumpPath = argv[++i];
        else if (a == "--dump-vram" && i + 1 < argc) vramPath = argv[++i];
        else if (a == "--log-cd" && i + 1 < argc) logCd = std::atoi(argv[++i]);
        else if (a == "--build-hd-cache") buildHdCache = true;
        else if (a == "--modern") modern = true;
        else if (a == "--widescreen") widescreen = true;
        else return usage();
    }
#if FE_HAVE_WINDOW
    if (buildHdCache) {
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
        return app::buildHdCache(discs, exeDir / "ai", exeDir / "hdcache");
    }
#endif

    try {
        auto discPtr = std::make_shared<fe::DiscImage>(discPath);
        fe::DiscImage& disc = *discPtr;
        auto boot = disc.bootExecutable();
        if (!boot) throw std::runtime_error("SYSTEM.CNF does not name a boot executable");
        auto exe = disc.readFile(*boot);
        std::printf("disc    %s (%u sectors, %zu files)\n", disc.volumeId().c_str(), disc.sectorCount(),
                    disc.files().size());
        std::printf("boot    %s, %zu bytes\n", boot->path.c_str(), exe.size());

        fe::Machine m;
        m.loadExe(exe);
        m.cd = std::make_shared<fe::Cdrom>(discPtr);
        m.cd->traceLeft = logCd;
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
                presses.push_back(p);
            }
        }
#if FE_HAVE_WINDOW
        std::unique_ptr<app::Window> window;
        if (frames == 0)
            window = std::make_unique<app::Window>(app::kResolutions[resolution].width, app::kResolutions[resolution].height,
                                                   fullscreen, display);
        if (window) window->setWidescreen(widescreen);
#else
        if (frames == 0) frames = 300;
#endif
        auto insertDisc = [&](int number) {
            std::string path = discPathFor(discPath, number);
            try {
                m.cd->insert(m, std::make_shared<fe::DiscImage>(path));
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
        fe::u64 lastActivity = ~0ull;
        app::ModernControls modernControls;
        m.onVblank = [&]() {
            bool analog = false;
            float stickX = 0, stickY = 0;
            fe::u16 held = 0;
            if (m.frame == swapFrame) insertDisc(swapDisc);
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
                if (int disc = window->discRequest()) insertDisc(disc);
                window->audio(m.audio);
            }
#endif
            m.audio.clear();
            for (const auto& p : presses)
                if (m.frame >= p.frame && m.frame < p.frame + p.length) held |= p.mask;
            if (modern) held = modernControls.apply(m, held, analog, stickX, stickY);
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
                m.gpu->display(rgb, w, h);
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
