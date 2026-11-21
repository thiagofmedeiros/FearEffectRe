#include "Launcher.h"

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

enum : int { kIdPath = 101, kIdBrowse, kIdResolution, kIdFullscreen, kIdPlay, kIdQuit, kIdDisplay };

struct State {
    HWND path = nullptr, combo = nullptr, full = nullptr, display = nullptr;
    bool play = false, done = false;
};

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
                case kIdQuit: st->done = true; return 0;
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
    RECT r{0, 0, 520, 190 + extra};
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
        st.display = make(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 100, 108, 220, 200, kIdDisplay);
        for (size_t i = 0; i < displays.size(); ++i) {
            std::wstring label = L"Display " + std::to_wstring(i + 1) + L" (" + std::to_wstring(displays[i].width) + L" x " +
                                 std::to_wstring(displays[i].height) + (displays[i].primary ? L", primary)" : L")");
            SendMessageW(st.display, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendMessageW(st.display, CB_SETCURSEL, static_cast<WPARAM>(disp), 0);
    }
    make(L"STATIC", L"The picture keeps its 4:3 shape inside the chosen size.", 0, 16, 112 + extra, 480, 18, 0);
    make(L"BUTTON", L"Play", WS_TABSTOP | BS_DEFPUSHBUTTON, 314, 146 + extra, 92, 30, kIdPlay);
    make(L"BUTTON", L"Quit", WS_TABSTOP, 412, 146 + extra, 92, 30, kIdQuit);

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
    }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
    return st.play;
}

}  // namespace app
