#include "Launcher.h"

#include "Controls.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <filesystem>

namespace app {

const Resolution kResolutions[5] = {
    {"original", L"Original (640 x 480)", 640, 480},
    {"720p", L"720p (1280 x 720)", 1280, 720},
    {"1080p", L"1080p (1920 x 1080)", 1920, 1080},
    {"1440p", L"1440p (2560 x 1440)", 2560, 1440},
    {"4k", L"4K (3840 x 2160)", 3840, 2160},
};

const Resolution* findResolution(const std::string& id) {
    for (const auto& r : kResolutions)
        if (id == r.id) return &r;
    return nullptr;
}

std::vector<Display> listDisplays() {
    std::vector<Display> out;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR mon, HDC, LPRECT, LPARAM param) -> BOOL {
            MONITORINFO info{};
            info.cbSize = sizeof info;
            if (GetMonitorInfoW(mon, &info)) {
                const RECT& r = info.rcMonitor;
                reinterpret_cast<std::vector<Display>*>(param)->push_back(
                    {r.left, r.top, r.right - r.left, r.bottom - r.top, (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
    std::sort(out.begin(), out.end(), [](const Display& a, const Display& b) {
        if (a.primary != b.primary) return a.primary;
        return a.x != b.x ? a.x < b.x : a.y < b.y;
    });
    if (out.empty()) out.push_back({0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), true});
    return out;
}

namespace {

enum : int { kIdPath = 101, kIdBrowse, kIdResolution, kIdFullscreen, kIdPlay, kIdQuit, kIdDisplay, kIdVideo, kIdBuildCache, kIdModern, kIdWidescreen, kIdHealth, kIdAmmo, kIdControls };

struct State {
    HWND path = nullptr, combo = nullptr, full = nullptr, display = nullptr;
    HWND video = nullptr, modern = nullptr, widescreen = nullptr, health = nullptr, ammo = nullptr;
    bool play = false, done = false;
};

std::filesystem::path exeDir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::filesystem::path(exe).parent_path();
}

// SeedVR2 is installed in ai/seedvr2 by ai/setup_seedvr2.ps1 (portable
// Python, PyTorch, the SeedVR2 code and model), or by hand with a venv.
bool seedvr2Installed() {
    const std::filesystem::path dir = exeDir() / L"ai" / L"seedvr2";
    std::error_code ec;
    const bool python = std::filesystem::exists(dir / L"python" / L"python.exe", ec) ||
                        std::filesystem::exists(dir / L"venv" / L"Scripts" / L"python.exe", ec);
    return python && std::filesystem::exists(dir / L"repo" / L"inference_cli.py", ec) &&
           std::filesystem::exists(dir / L"models" / L"SEEDVR2" / L"seedvr2_ema_3b_fp16.safetensors", ec);
}

// Asks, then runs the installer in its own window. True when it was started.
bool offerSeedvr2Install(HWND hwnd) {
    const std::filesystem::path script = exeDir() / L"ai" / L"setup_seedvr2.ps1";
    std::error_code ec;
    if (!std::filesystem::exists(script, ec)) {
        MessageBoxW(hwnd, L"SeedVR2 is not installed, and its installer (ai\\setup_seedvr2.ps1) is missing.",
                    L"SeedVR2", MB_OK | MB_ICONWARNING);
        return false;
    }
    if (MessageBoxW(hwnd,
                    L"SeedVR2 is not installed yet. Installing it will:\n\n"
                    L"- download about 12 GB: a portable Python 3.12, PyTorch for NVIDIA graphics cards (CUDA), "
                    L"the SeedVR2 code from GitHub and the SeedVR2 3B model from Hugging Face;\n"
                    L"- put everything in the ai\\seedvr2 folder next to the game (about 16 GB on disk). "
                    L"Nothing is installed on the system; deleting that folder removes it.\n\n"
                    L"SeedVR2 needs an NVIDIA graphics card, ideally with 24 GB of video memory. "
                    L"The installation runs in a separate window and can take a while.\n\n"
                    L"Install SeedVR2 now?",
                    L"Install SeedVR2", MB_OKCANCEL | MB_ICONWARNING) != IDOK)
        return false;
    std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script.wstring() + L"\" -Dir \"" +
                       (exeDir() / L"ai" / L"seedvr2").wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi)) {
        MessageBoxW(hwnd, L"The SeedVR2 installer could not be started.", L"SeedVR2", MB_OK | MB_ICONWARNING);
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

std::wstring iniPath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return (std::filesystem::path(exe).parent_path() / L"FearEffectRe.ini").wstring();
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring windowText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(static_cast<size_t>(n));
    return s;
}

LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_COMMAND:
            if (!st) break;
            switch (LOWORD(wp)) {
                case kIdVideo:
                    // choosing SeedVR2 the first time: offer to install it (playing works without it,
                    // from the x4plus pictures, but its cache can only be built once it is installed)
                    if (HIWORD(wp) == CBN_SELCHANGE && SendMessageW(st->video, CB_GETCURSEL, 0, 0) == 3 &&
                        !seedvr2Installed())
                        offerSeedvr2Install(hwnd);
                    return 0;
                case kIdBrowse: {
                    wchar_t file[MAX_PATH] = L"";
                    OPENFILENAMEW ofn{};
                    ofn.lStructSize = sizeof ofn;
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFilter = L"Disc images (*.cue;*.bin;*.iso;*.img)\0*.cue;*.bin;*.iso;*.img\0All files\0*.*\0";
                    ofn.lpstrFile = file;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.lpstrTitle = L"Select Fear Effect (USA) disc 1";
                    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                    if (GetOpenFileNameW(&ofn)) SetWindowTextW(st->path, file);
                    return 0;
                }
                case kIdPlay: {
                    std::error_code ec;
                    if (!std::filesystem::is_regular_file(windowText(st->path), ec)) {
                        MessageBoxW(hwnd, L"Choose the disc 1 image of Fear Effect (USA) first.", L"FearEffectRe",
                                    MB_OK | MB_ICONINFORMATION);
                        return 0;
                    }
                    st->play = true;
                    st->done = true;
                    return 0;
                }
                case kIdBuildCache: {
                    std::wstring disc = windowText(st->path);
                    std::error_code ec;
                    if (!std::filesystem::is_regular_file(disc, ec)) {
                        MessageBoxW(hwnd, L"Choose the disc 1 image of Fear Effect (USA) first.", L"FearEffectRe",
                                    MB_OK | MB_ICONINFORMATION);
                        return 0;
                    }
                    // the cache for the Video choice: SeedVR2 has its own
                    const bool seedvr = SendMessageW(st->video, CB_GETCURSEL, 0, 0) == 3;
                    if (seedvr && !seedvr2Installed()) {
                        offerSeedvr2Install(hwnd);
                        return 0;
                    }
                    const wchar_t* text =
                        seedvr ? L"This decodes every movie and background frame on all four discs found next to "
                                 L"disc 1 and enlarges them with SeedVR2, as video, into the file hdcache/seedvr2.pack.\n\n"
                                 L"It takes about two days on an RTX 3090 class graphics card and about 300 GB of disk "
                                 L"space. It can be stopped by closing its window and resumed later.\n\nStart now?"
                               : L"This decodes every movie and background frame on all four discs found next to "
                                 L"disc 1 and enlarges each with the best AI model into the file hdcache/hdcache.pack.\n\n"
                                 L"It takes several hours on a fast graphics card and needs about 300 GB of disk "
                                 L"space. It can be stopped by closing its window and resumed later.\n\nStart now?";
                    if (MessageBoxW(hwnd, text, seedvr ? L"Build SeedVR2 HD cache" : L"Build HD cache",
                                    MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
                        return 0;
                    wchar_t exe[MAX_PATH];
                    GetModuleFileNameW(nullptr, exe, MAX_PATH);
                    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" \"" + disc + L"\" " +
                                       (seedvr ? L"--build-seedvr2-cache" : L"--build-hd-cache");
                    STARTUPINFOW si{};
                    si.cb = sizeof si;
                    PROCESS_INFORMATION pi{};
                    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr,
                                       nullptr, &si, &pi)) {
                        CloseHandle(pi.hThread);
                        CloseHandle(pi.hProcess);
                    }
                    return 0;
                }
                case kIdQuit: st->done = true; return 0;
                case kIdControls: {
                    Controls controls = Controls::load(iniPath());
                    runControlsDialog(hwnd, controls, iniPath());
                    return 0;
                }
                default: break;
            }
            break;
        case WM_CLOSE:
            if (st) st->done = true;
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

std::wstring settingsPath() { return iniPath(); }

bool runLauncher(LaunchSettings& settings) {
    std::wstring ini = iniPath();
    wchar_t buf[MAX_PATH];
    GetPrivateProfileStringW(L"launcher", L"disc", widen(settings.discPath).c_str(), buf, MAX_PATH, ini.c_str());
    std::wstring disc = buf;
    int res = static_cast<int>(GetPrivateProfileIntW(L"launcher", L"resolution", settings.resolution, ini.c_str()));
    if (res < 0 || res > 4) res = 1;
    bool full = GetPrivateProfileIntW(L"launcher", L"fullscreen", settings.fullscreen ? 1 : 0, ini.c_str()) != 0;
    std::vector<Display> displays = listDisplays();
    int disp = static_cast<int>(GetPrivateProfileIntW(L"launcher", L"display", settings.display, ini.c_str()));
    if (disp < 0 || disp >= static_cast<int>(displays.size())) disp = 0;
    int vid = static_cast<int>(GetPrivateProfileIntW(L"launcher", L"video", settings.video, ini.c_str()));
    if (vid < 0 || vid > 3) vid = 1;
    bool modern = GetPrivateProfileIntW(L"launcher", L"modern", settings.modern ? 1 : 0, ini.c_str()) != 0;
    bool wide = GetPrivateProfileIntW(L"launcher", L"widescreen", settings.widescreen ? 1 : 0, ini.c_str()) != 0;
    bool health = GetPrivateProfileIntW(L"cheats", L"health", settings.infiniteHealth ? 1 : 0, ini.c_str()) != 0;
    bool ammo = GetPrivateProfileIntW(L"cheats", L"ammo", settings.infiniteAmmo ? 1 : 0, ini.c_str()) != 0;
    // the display row is only there when there is a choice to make
    const int extra = displays.size() > 1 ? 32 : 0;

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"FearEffectReLauncher";
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    RegisterClassW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, 520, 300 + extra};
    AdjustWindowRect(&r, style, FALSE);
    int w = r.right - r.left, h = r.bottom - r.top;
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"FearEffectRe", style, (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
                              (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return false;

    State st;
    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD extra, int x, int y, int cw, int ch, int id) {
        HWND c = CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | extra, x, y, cw, ch, hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    };
    make(L"STATIC", L"Disc 1 image (Fear Effect, USA):", 0, 16, 14, 400, 18, 0);
    st.path = make(L"EDIT", disc.c_str(), WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 16, 36, 394, 24, kIdPath);
    make(L"BUTTON", L"Browse...", WS_TABSTOP, 418, 35, 86, 26, kIdBrowse);
    make(L"STATIC", L"Resolution:", 0, 16, 80, 80, 18, 0);
    st.combo = make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 100, 76, 220, 200, kIdResolution);
    for (const auto& item : kResolutions) SendMessageW(st.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.label));
    SendMessageW(st.combo, CB_SETCURSEL, static_cast<WPARAM>(res), 0);
    st.full = make(L"BUTTON", L"Fullscreen", WS_TABSTOP | BS_AUTOCHECKBOX, 340, 78, 120, 20, kIdFullscreen);
    SendMessageW(st.full, BM_SETCHECK, full ? BST_CHECKED : BST_UNCHECKED, 0);
    if (extra) {
        make(L"STATIC", L"Display:", 0, 16, 112, 80, 18, 0);
        st.display = make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 100, 108, 300, 200, kIdDisplay);
        for (size_t i = 0; i < displays.size(); ++i) {
            std::wstring label = L"Display " + std::to_wstring(i + 1) + L" (" + std::to_wstring(displays[i].width) + L" x " +
                                 std::to_wstring(displays[i].height) + (displays[i].primary ? L", primary)" : L")");
            SendMessageW(st.display, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendMessageW(st.display, CB_SETCURSEL, static_cast<WPARAM>(disp), 0);
    }
    make(L"STATIC", L"Video:", 0, 16, 112 + extra, 80, 18, 0);
    st.video = make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 100, 108 + extra, 404, 200, kIdVideo);
    for (const wchar_t* label : {L"Original (movies and backgrounds as they are)",
                                 L"AI upscale, fast (live on the graphics card)",
                                 L"AI upscale, best (cached; slow the first time)",
                                 L"AI upscale, SeedVR2 (steadiest movies; build its HD cache first)"})
        SendMessageW(st.video, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    SendMessageW(st.video, CB_SETCURSEL, static_cast<WPARAM>(vid), 0);
    st.modern = make(L"BUTTON", L"Modern movement (walk where the stick points on screen)",
                     WS_TABSTOP | BS_AUTOCHECKBOX, 16, 142 + extra, 488, 20, kIdModern);
    SendMessageW(st.modern, BM_SETCHECK, modern ? BST_CHECKED : BST_UNCHECKED, 0);
    st.widescreen = make(L"BUTTON", L"Widescreen (gameplay and movies without the black bands)",
                         WS_TABSTOP | BS_AUTOCHECKBOX, 16, 168 + extra, 488, 20, kIdWidescreen);
    SendMessageW(st.widescreen, BM_SETCHECK, wide ? BST_CHECKED : BST_UNCHECKED, 0);
    make(L"STATIC", L"Cheats:", 0, 16, 196 + extra, 80, 18, 0);
    st.health = make(L"BUTTON", L"Infinite health", WS_TABSTOP | BS_AUTOCHECKBOX, 100, 194 + extra, 140, 20, kIdHealth);
    SendMessageW(st.health, BM_SETCHECK, health ? BST_CHECKED : BST_UNCHECKED, 0);
    st.ammo = make(L"BUTTON", L"Infinite ammo", WS_TABSTOP | BS_AUTOCHECKBOX, 250, 194 + extra, 140, 20, kIdAmmo);
    SendMessageW(st.ammo, BM_SETCHECK, ammo ? BST_CHECKED : BST_UNCHECKED, 0);
    make(L"STATIC", L"The picture keeps its shape: 4:3, or 2:1 in widescreen.", 0, 16, 222 + extra, 488, 18, 0);
    make(L"BUTTON", L"Build HD cache...", WS_TABSTOP, 16, 256 + extra, 140, 30, kIdBuildCache);
    make(L"BUTTON", L"Controls...", WS_TABSTOP, 162, 256 + extra, 100, 30, kIdControls);
    make(L"BUTTON", L"Play", WS_TABSTOP | BS_DEFPUSHBUTTON, 314, 256 + extra, 92, 30, kIdPlay);
    make(L"BUTTON", L"Quit", WS_TABSTOP, 412, 256 + extra, 92, 30, kIdQuit);

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);

    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            SendMessageW(hwnd, WM_COMMAND, kIdPlay, 0);
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            st.done = true;
            continue;
        }
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (st.play) {
        std::wstring chosen = windowText(st.path);
        settings.discPath = narrow(chosen);
        settings.resolution = static_cast<int>(SendMessageW(st.combo, CB_GETCURSEL, 0, 0));
        if (settings.resolution < 0 || settings.resolution > 4) settings.resolution = 1;
        settings.fullscreen = SendMessageW(st.full, BM_GETCHECK, 0, 0) == BST_CHECKED;
        WritePrivateProfileStringW(L"launcher", L"disc", chosen.c_str(), ini.c_str());
        WritePrivateProfileStringW(L"launcher", L"resolution", std::to_wstring(settings.resolution).c_str(), ini.c_str());
        settings.display = st.display ? static_cast<int>(SendMessageW(st.display, CB_GETCURSEL, 0, 0)) : 0;
        if (settings.display < 0) settings.display = 0;
        WritePrivateProfileStringW(L"launcher", L"fullscreen", settings.fullscreen ? L"1" : L"0", ini.c_str());
        WritePrivateProfileStringW(L"launcher", L"display", std::to_wstring(settings.display).c_str(), ini.c_str());
        settings.video = static_cast<int>(SendMessageW(st.video, CB_GETCURSEL, 0, 0));
        if (settings.video < 0 || settings.video > 3) settings.video = 1;
        WritePrivateProfileStringW(L"launcher", L"video", std::to_wstring(settings.video).c_str(), ini.c_str());
        settings.modern = SendMessageW(st.modern, BM_GETCHECK, 0, 0) == BST_CHECKED;
        WritePrivateProfileStringW(L"launcher", L"modern", settings.modern ? L"1" : L"0", ini.c_str());
        settings.widescreen = SendMessageW(st.widescreen, BM_GETCHECK, 0, 0) == BST_CHECKED;
        WritePrivateProfileStringW(L"launcher", L"widescreen", settings.widescreen ? L"1" : L"0", ini.c_str());
        settings.infiniteHealth = SendMessageW(st.health, BM_GETCHECK, 0, 0) == BST_CHECKED;
        settings.infiniteAmmo = SendMessageW(st.ammo, BM_GETCHECK, 0, 0) == BST_CHECKED;
        WritePrivateProfileStringW(L"cheats", L"health", settings.infiniteHealth ? L"1" : L"0", ini.c_str());
        WritePrivateProfileStringW(L"cheats", L"ammo", settings.infiniteAmmo ? L"1" : L"0", ini.c_str());
    }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
    return st.play;
}

}  // namespace app
