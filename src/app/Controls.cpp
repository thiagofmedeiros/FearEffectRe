#include "Controls.h"

#include <mmsystem.h>

#include <algorithm>
#include <cstring>
#include <cwctype>

namespace app {

const char* const Controls::kButtonNames[16] = {"select", "l3",  "r3",       "start",  "up",    "right", "down", "left",
                                                "l2",     "r2",  "l1",       "r1",     "triangle", "circle", "cross", "square"};

namespace {

// How the buttons are shown in the controls window, in that window's order.
struct Row {
    int bit;
    const wchar_t* label;
};
const Row kRows[16] = {{4, L"Up"},       {6, L"Down"},     {7, L"Left"},   {5, L"Right"},  {14, L"Cross"},
                       {13, L"Circle"},  {15, L"Square"},  {12, L"Triangle"}, {10, L"L1"},  {11, L"R1"},
                       {8, L"L2"},       {9, L"R2"},       {3, L"Start"},  {0, L"Select"}, {1, L"L3"},
                       {2, L"R3"}};

struct Named {
    const wchar_t* name;
    unsigned code;
};

const Named kKeyNames[] = {
    {L"Up", VK_UP}, {L"Down", VK_DOWN}, {L"Left", VK_LEFT}, {L"Right", VK_RIGHT}, {L"Enter", VK_RETURN},
    {L"Esc", VK_ESCAPE}, {L"Space", VK_SPACE}, {L"Tab", VK_TAB}, {L"Backspace", VK_BACK}, {L"LShift", VK_LSHIFT},
    {L"RShift", VK_RSHIFT}, {L"LCtrl", VK_LCONTROL}, {L"RCtrl", VK_RCONTROL}, {L"Alt", VK_MENU}, {L"Home", VK_HOME},
    {L"End", VK_END}, {L"PageUp", VK_PRIOR}, {L"PageDown", VK_NEXT}, {L"Insert", VK_INSERT}, {L"Delete", VK_DELETE},
    {L"F10", VK_F10}, {L"F12", VK_F12}, {L"Numpad0", VK_NUMPAD0}, {L"Numpad1", VK_NUMPAD1},
    {L"Numpad2", VK_NUMPAD2}, {L"Numpad3", VK_NUMPAD3}, {L"Numpad4", VK_NUMPAD4}, {L"Numpad5", VK_NUMPAD5},
    {L"Numpad6", VK_NUMPAD6}, {L"Numpad7", VK_NUMPAD7}, {L"Numpad8", VK_NUMPAD8}, {L"Numpad9", VK_NUMPAD9},
    {L"Comma", VK_OEM_COMMA}, {L"Period", VK_OEM_PERIOD}, {L"Semicolon", VK_OEM_1}, {L"Slash", VK_OEM_2},
    {L"Minus", VK_OEM_MINUS}, {L"Equals", VK_OEM_PLUS}, {L"LBracket", VK_OEM_4}, {L"RBracket", VK_OEM_6},
    {L"Quote", VK_OEM_7}, {L"Backslash", VK_OEM_5},
};

const Named kXNames[] = {
    {L"A", 0x1000}, {L"B", 0x2000}, {L"X", 0x4000}, {L"Y", 0x8000}, {L"LB", 0x0100}, {L"RB", 0x0200},
    {L"LT", kPadLT}, {L"RT", kPadRT}, {L"Back", 0x0020}, {L"Start", 0x0010}, {L"LS", 0x0040}, {L"RS", 0x0080},
    {L"DPadUp", 0x0001}, {L"DPadDown", 0x0002}, {L"DPadLeft", 0x0004}, {L"DPadRight", 0x0008},
};
const wchar_t* const kPovNames[4] = {L"PovUp", L"PovRight", L"PovDown", L"PovLeft"};

bool sameName(const std::wstring& a, const wchar_t* b) { return _wcsicmp(a.c_str(), b) == 0; }

int keyCode(const std::wstring& name) {
    if (name.size() == 1) {
        wchar_t ch = static_cast<wchar_t>(std::towupper(name[0]));
        if ((ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9')) return ch;
    }
    for (const Named& n : kKeyNames)
        if (sameName(name, n.name)) return static_cast<int>(n.code);
    return 0;
}

unsigned padCode(const std::wstring& name) {
    for (const Named& n : kXNames)
        if (sameName(name, n.name)) return n.code;
    for (unsigned d = 0; d < 4; ++d)
        if (sameName(name, kPovNames[d])) return kJoyPov + d + 1;
    if (name.size() > 6 && _wcsnicmp(name.c_str(), L"Button", 6) == 0) {
        int n = _wtoi(name.c_str() + 6);
        if (n >= 1 && n <= 32) return kJoyButton + static_cast<unsigned>(n);
    }
    return 0;
}

std::vector<std::wstring> splitList(const std::wstring& text) {
    std::vector<std::wstring> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(L',', pos);
        if (end == std::wstring::npos) end = text.size();
        std::wstring item = text.substr(pos, end - pos);
        size_t a = item.find_first_not_of(L" \t"), b = item.find_last_not_of(L" \t");
        if (a != std::wstring::npos) out.push_back(item.substr(a, b - a + 1));
        pos = end + 1;
    }
    return out;
}

std::wstring widenAscii(const char* s) { return std::wstring(s, s + std::strlen(s)); }

std::wstring keyList(const std::vector<int>& keys) {
    std::wstring list;
    for (int vk : keys) list += (list.empty() ? L"" : L", ") + keyName(vk);
    return list;
}

std::wstring padList(const std::vector<unsigned>& codes) {
    std::wstring list;
    for (unsigned code : codes) list += (list.empty() ? L"" : L", ") + padName(code);
    return list;
}

}  // namespace

std::wstring keyName(int vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::wstring(1, static_cast<wchar_t>(vk));
    for (const Named& n : kKeyNames)
        if (static_cast<int>(n.code) == vk) return n.name;
    return L"";
}

std::wstring padName(unsigned code) {
    for (const Named& n : kXNames)
        if (n.code == code) return n.name;
    if (code > kJoyPov && code <= kJoyPov + 4) return kPovNames[code - kJoyPov - 1];
    if (code > kJoyButton && code <= kJoyButton + 32) return L"Button" + std::to_wstring(code - kJoyButton);
    return L"";
}

// Xbox controllers through XInput; generic controllers numbered as Windows
// shows them for a DualShock 4 or DualSense (Square 1, Cross 2, Circle 3,
// Triangle 4, L1 5, R1 6, L2 7, R2 8, Share 9, Options 10, L3 11, R3 12).
Controls Controls::defaults() {
    Controls c;
    auto k = [&](int bit, std::initializer_list<int> keys) { c.keys[bit] = keys; };
    k(0, {VK_BACK});
    k(3, {'P'});
    k(4, {'W', VK_UP});
    k(5, {'D', VK_RIGHT});
    k(6, {'S', VK_DOWN});
    k(7, {'A', VK_LEFT});
    k(8, {'1'});
    k(9, {'4'});
    k(10, {VK_TAB});
    k(11, {'R'});
    k(12, {VK_SPACE});
    k(13, {'Q', VK_ESCAPE});
    k(14, {'E', VK_RETURN});
    k(15, {'F'});
    auto p = [&](int bit, std::initializer_list<unsigned> codes) { c.pad[bit] = codes; };
    p(0, {0x0020, kJoyButton + 9});
    p(1, {0x0040, kJoyButton + 11});
    p(2, {0x0080, kJoyButton + 12});
    p(3, {0x0010, kJoyButton + 10});
    p(4, {0x0001, kJoyPov + 1});
    p(5, {0x0008, kJoyPov + 2});
    p(6, {0x0002, kJoyPov + 3});
    p(7, {0x0004, kJoyPov + 4});
    p(8, {kPadLT, kJoyButton + 7});
    p(9, {kPadRT, kJoyButton + 8});
    p(10, {0x0100, kJoyButton + 5});
    p(11, {0x0200, kJoyButton + 6});
    p(12, {0x8000, kJoyButton + 4});
    p(13, {0x2000, kJoyButton + 3});
    p(14, {0x1000, kJoyButton + 2});
    p(15, {0x4000, kJoyButton + 1});
    return c;
}

Controls Controls::load(const std::wstring& ini) {
    Controls c = defaults();
    wchar_t buf[512];
    // a missing section is written out with the defaults, so it can be edited
    const bool haveKeys = GetPrivateProfileSectionW(L"keyboard", buf, 512, ini.c_str()) > 0;
    const bool havePad = GetPrivateProfileSectionW(L"controller", buf, 512, ini.c_str()) > 0;
    for (int bit = 0; bit < 16; ++bit) {
        const std::wstring button = widenAscii(kButtonNames[bit]);
        if (haveKeys) {
            GetPrivateProfileStringW(L"keyboard", button.c_str(), L"", buf, 512, ini.c_str());
            c.keys[bit].clear();
            for (const auto& name : splitList(buf))
                if (int vk = keyCode(name)) c.keys[bit].push_back(vk);
        }
        if (havePad) {
            GetPrivateProfileStringW(L"controller", button.c_str(), L"", buf, 512, ini.c_str());
            c.pad[bit].clear();
            for (const auto& name : splitList(buf))
                if (unsigned code = padCode(name)) c.pad[bit].push_back(code);
        }
    }
    if (havePad) {
        c.deadzone = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"controller", L"deadzone", c.deadzone, ini.c_str())), 0, 90);
        c.stickAsDpad = GetPrivateProfileIntW(L"controller", L"stick_as_dpad", 1, ini.c_str()) != 0;
        c.analog = GetPrivateProfileIntW(L"controller", L"analog", 0, ini.c_str()) != 0;
    }
    if (!haveKeys || !havePad) c.save(ini);
    return c;
}

void Controls::save(const std::wstring& ini) const {
    for (int bit = 0; bit < 16; ++bit) {
        const std::wstring button = widenAscii(kButtonNames[bit]);
        WritePrivateProfileStringW(L"keyboard", button.c_str(), keyList(keys[bit]).c_str(), ini.c_str());
        WritePrivateProfileStringW(L"controller", button.c_str(), padList(pad[bit]).c_str(), ini.c_str());
    }
    WritePrivateProfileStringW(L"controller", L"deadzone", std::to_wstring(deadzone).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"controller", L"stick_as_dpad", stickAsDpad ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"controller", L"analog", analog ? L"1" : L"0", ini.c_str());
}

// ---- controllers -------------------------------------------------------------

namespace {
struct XState {
    DWORD packet;
    WORD buttons;
    BYTE leftTrigger, rightTrigger;
    SHORT lx, ly, rx, ry;
};
}  // namespace

bool PadSnapshot::held(unsigned code) const {
    if (code >= kJoyPov) return code - kJoyPov == static_cast<unsigned>(pov) || code - kJoyPov == static_cast<unsigned>(pov2);
    if (code >= kJoyButton) return (joyButtons >> (code - kJoyButton - 1)) & 1;
    return (xbuttons & code) != 0;
}

std::vector<unsigned> PadSnapshot::heldCodes() const {
    std::vector<unsigned> out;
    for (const Named& n : kXNames)
        if (xbuttons & n.code) out.push_back(n.code);
    for (unsigned i = 0; i < 32; ++i)
        if ((joyButtons >> i) & 1) out.push_back(kJoyButton + i + 1);
    if (pov) out.push_back(kJoyPov + static_cast<unsigned>(pov));
    if (pov2) out.push_back(kJoyPov + static_cast<unsigned>(pov2));
    return out;
}

Pads::Pads() {
    // XInput is loaded at run time so the program still starts where it is missing
    for (const wchar_t* name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"})
        if (HMODULE lib = LoadLibraryW(name))
            if ((xinput_ = reinterpret_cast<GetState>(reinterpret_cast<void*>(GetProcAddress(lib, "XInputGetState")))))
                break;
}

PadSnapshot Pads::poll() {
    PadSnapshot s;
    if (xinput_) {
        XState x{};
        bool found = xinput_(xIndex_, &x) == ERROR_SUCCESS;
        if (!found && --xRetry_ <= 0) {
            xRetry_ = 60;
            for (DWORD i = 0; i < 4 && !found; ++i)
                if (xinput_(i, &x) == ERROR_SUCCESS) {
                    xIndex_ = i;
                    found = true;
                }
        }
        if (found) {
            s.connected = s.xinput = true;
            s.xbuttons = x.buttons;
            if (x.leftTrigger > 64) s.xbuttons |= kPadLT;
            if (x.rightTrigger > 64) s.xbuttons |= kPadRT;
            s.lx = x.lx / 32767.0f;
            s.ly = x.ly / 32767.0f;
            return s;
        }
    }
    // Any other game controller (DualShock 4, DualSense, Switch Pro, USB pads)
    // through the Windows joystick API. It is only used while no Xbox
    // controller answers, since an Xbox controller shows up there too.
    JOYINFOEX j{};
    j.dwSize = sizeof j;
    j.dwFlags = JOY_RETURNALL;
    bool found = joyId_ >= 0 && joyGetPosEx(static_cast<UINT>(joyId_), &j) == JOYERR_NOERROR;
    if (!found && --joyRetry_ <= 0) {
        joyRetry_ = 60;
        joyId_ = -1;
        const UINT count = std::min<UINT>(joyGetNumDevs(), 16);
        for (UINT i = 0; i < count && !found; ++i) {
            j = JOYINFOEX{};
            j.dwSize = sizeof j;
            j.dwFlags = JOY_RETURNALL;
            if (joyGetPosEx(i, &j) == JOYERR_NOERROR && joyGetDevCapsW(i, &caps_, sizeof caps_) == JOYERR_NOERROR) {
                joyId_ = static_cast<int>(i);
                found = true;
            }
        }
    }
    if (!found) return s;
    s.connected = true;
    s.joyButtons = static_cast<unsigned>(j.dwButtons);
    if (j.dwPOV != JOY_POVCENTERED && j.dwPOV <= 36000) {
        const int deg = static_cast<int>(j.dwPOV) / 100;  // 0 up, 90 right, ...
        const int octant = ((deg + 22) / 45) % 8;         // 0 up, 1 up-right, ...
        static const int first[8] = {1, 1, 2, 2, 3, 3, 4, 4}, second[8] = {0, 2, 0, 3, 0, 4, 0, 1};
        s.pov = first[octant];
        s.pov2 = second[octant];
    }
    auto axis = [](DWORD v, UINT lo, UINT hi) {
        if (hi <= lo) return 0.0f;
        return std::clamp((static_cast<float>(v) - lo) * 2.0f / static_cast<float>(hi - lo) - 1.0f, -1.0f, 1.0f);
    };
    s.lx = axis(j.dwXpos, caps_.wXmin, caps_.wXmax);
    s.ly = -axis(j.dwYpos, caps_.wYmin, caps_.wYmax);  // the joystick API counts down
    return s;
}

// ---- the controls window -----------------------------------------------------

namespace {

enum { kIdKey = 100, kIdPad = 200, kIdAnalog = 300, kIdStick, kIdDead, kIdDefaults, kIdSave, kIdCancel };

const wchar_t* const kHelp =
    L"Click a box, then press a key or a controller button to add it. Pressing one that is already "
    L"there takes it off. Right-click a box to empty it.";

struct Dialog {
    HWND hwnd = nullptr;
    HWND help = nullptr;
    HWND keyBox[16]{}, padBox[16]{};
    HWND analog = nullptr, stick = nullptr, dead = nullptr;
    Controls c;
    Pads pads;
    int captureRow = -1;
    bool capturePad = false;
    std::vector<unsigned> wasHeld;  // controller buttons already down when capturing started
    bool done = false, saved = false;

    void refresh() {
        SetWindowTextW(help, kHelp);
        for (int r = 0; r < 16; ++r) {
            const int bit = kRows[r].bit;
            std::wstring keys = keyList(c.keys[bit]), pad = padList(c.pad[bit]);
            if (captureRow == r && !capturePad) keys = L"Press a key...";
            if (captureRow == r && capturePad) pad = L"Press a controller button...";
            keys = L"  " + (keys.empty() ? std::wstring(L"(none)") : keys);  // a little room from the edge
            pad = L"  " + (pad.empty() ? std::wstring(L"(none)") : pad);
            SetWindowTextW(keyBox[r], keys.c_str());
            SetWindowTextW(padBox[r], pad.c_str());
        }
        SendMessageW(analog, BM_SETCHECK, c.analog ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(stick, BM_SETCHECK, c.stickAsDpad ? BST_CHECKED : BST_UNCHECKED, 0);
        EnableWindow(stick, !c.analog);  // an analog pad does not press the D-pad with its stick
        SetWindowTextW(dead, std::to_wstring(c.deadzone).c_str());
    }
    void startCapture(int row, bool pad) {
        if (captureRow == row && capturePad == pad) {
            captureRow = -1;  // clicking the same box again cancels
        } else {
            captureRow = row;
            capturePad = pad;
            wasHeld = pads.poll().heldCodes();
        }
        refresh();
    }
    // a key or controller button toggles in the list being captured
    void toggleKey(int vk) {
        if (keyName(vk).empty()) {
            SetWindowTextW(help, L"That key cannot be used. Choose another one, or click the box again to stop.");
            return;
        }
        auto& list = c.keys[kRows[captureRow].bit];
        if (auto it = std::find(list.begin(), list.end(), vk); it != list.end()) list.erase(it);
        else list.push_back(vk);
        captureRow = -1;
        refresh();
    }
    void togglePad(unsigned code) {
        auto& list = c.pad[kRows[captureRow].bit];
        if (auto it = std::find(list.begin(), list.end(), code); it != list.end()) list.erase(it);
        else list.push_back(code);
        captureRow = -1;
        refresh();
    }
};

LRESULT CALLBACK dialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = reinterpret_cast<Dialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!d) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_CLOSE: d->done = true; return 0;
        case WM_TIMER:
            if (d->captureRow >= 0 && d->capturePad) {
                std::vector<unsigned> now = d->pads.poll().heldCodes();
                for (unsigned code : now)
                    if (std::find(d->wasHeld.begin(), d->wasHeld.end(), code) == d->wasHeld.end()) {
                        d->togglePad(code);
                        break;
                    }
                if (d->captureRow >= 0) d->wasHeld = now;
            }
            return 0;
        case WM_CONTEXTMENU: {
            // right-click on a box empties it
            HWND target = reinterpret_cast<HWND>(wp);
            for (int r = 0; r < 16; ++r) {
                if (target == d->keyBox[r]) d->c.keys[kRows[r].bit].clear();
                if (target == d->padBox[r]) d->c.pad[kRows[r].bit].clear();
            }
            d->captureRow = -1;
            d->refresh();
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id >= kIdKey && id < kIdKey + 16) d->startCapture(id - kIdKey, false);
            else if (id >= kIdPad && id < kIdPad + 16) d->startCapture(id - kIdPad, true);
            else if (id == kIdAnalog) {
                d->c.analog = SendMessageW(d->analog, BM_GETCHECK, 0, 0) == BST_CHECKED;
                EnableWindow(d->stick, !d->c.analog);
            }
            else if (id == kIdStick) d->c.stickAsDpad = SendMessageW(d->stick, BM_GETCHECK, 0, 0) == BST_CHECKED;
            else if (id == kIdDefaults) {
                d->c = Controls::defaults();
                d->captureRow = -1;
                d->refresh();
            } else if (id == kIdSave) {
                wchar_t text[16];
                GetWindowTextW(d->dead, text, 16);
                d->c.deadzone = std::clamp(_wtoi(text), 0, 90);
                d->saved = d->done = true;
            } else if (id == kIdCancel) {
                d->done = true;
            }
            return 0;
        }
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool runControlsDialog(HWND owner, Controls& controls, const std::wstring& ini) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = dialogProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"FearEffectReControls";
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    RegisterClassW(&wc);

    Dialog d;
    d.c = controls;

    // Sizes are in 96-dpi units, scaled to the screen; the font is the one
    // Windows uses for its own dialogs.
    HDC screen = GetDC(nullptr);
    const int dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    auto px = [&](int v) { return MulDiv(v, dpi, 96); };
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW boldLf = ncm.lfMessageFont;
    boldLf.lfWeight = FW_SEMIBOLD;
    HFONT bold = CreateFontIndirectW(&boldLf);

    const int margin = 16, labelW = 84, colW = 220, gap = 10, rowH = 30, boxH = 26;
    const int colKeys = margin + labelW, colPad = colKeys + colW + gap;
    const int width = colPad + colW + margin;
    const int helpH = 36, headY = margin + helpH + 8, top = headY + 24;
    const int optionsY = top + 16 * rowH + 10;
    const int buttonsY = optionsY + 70;
    const int height = buttonsY + 32 + margin;

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT r{0, 0, px(width), px(height)};
    AdjustWindowRectEx(&r, style, FALSE, WS_EX_TOPMOST | WS_EX_DLGMODALFRAME);
    const int w = r.right - r.left, h = r.bottom - r.top;
    // centred on the owner's monitor, above a fullscreen game window
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY), &mi);
    const int x = mi.rcWork.left + std::max(0L, (mi.rcWork.right - mi.rcWork.left - w) / 2);
    const int y = mi.rcWork.top + std::max(0L, (mi.rcWork.bottom - mi.rcWork.top - h) / 2);
    d.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, wc.lpszClassName, L"Controls", style, x, y, w, h, owner,
                             nullptr, inst, nullptr);
    if (!d.hwnd) {
        DeleteObject(font);
        DeleteObject(bold);
        return false;
    }

    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD extra, int cx, int cy, int cw, int ch, int id, HFONT f) {
        HWND c = CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | extra, px(cx), px(cy), px(cw), px(ch), d.hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
        return c;
    };
    d.help = make(L"STATIC", kHelp, 0, margin, margin, width - 2 * margin, helpH, 0, font);
    make(L"STATIC", L"Button", 0, margin, headY, labelW, 20, 0, bold);
    make(L"STATIC", L"Keyboard", 0, colKeys, headY, colW, 20, 0, bold);
    make(L"STATIC", L"Controller", 0, colPad, headY, colW, 20, 0, bold);
    for (int i = 0; i < 16; ++i) {
        const int ry = top + i * rowH;
        make(L"STATIC", kRows[i].label, SS_CENTERIMAGE, margin, ry, labelW - 8, boxH, 0, font);
        d.keyBox[i] = make(L"BUTTON", L"", WS_TABSTOP | BS_LEFT | BS_VCENTER, colKeys, ry, colW, boxH, kIdKey + i, font);
        d.padBox[i] = make(L"BUTTON", L"", WS_TABSTOP | BS_LEFT | BS_VCENTER, colPad, ry, colW, boxH, kIdPad + i, font);
    }
    make(L"STATIC", L"", SS_ETCHEDHORZ, margin, optionsY - 4, width - 2 * margin, 2, 0, font);
    d.analog = make(L"BUTTON", L"Analog pad (DualShock): the game reads the left stick itself", WS_TABSTOP | BS_AUTOCHECKBOX,
                    margin, optionsY + 4, width - 2 * margin, 22, kIdAnalog, font);
    d.stick = make(L"BUTTON", L"Left stick also presses the D-pad", WS_TABSTOP | BS_AUTOCHECKBOX,
                   margin, optionsY + 30, colPad - margin - gap, 22, kIdStick, font);
    make(L"STATIC", L"Stick dead zone (%)", SS_CENTERIMAGE | SS_RIGHT, colPad, optionsY + 30, colW - 60, 22, 0, font);
    d.dead = make(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_CENTER, colPad + colW - 52, optionsY + 30, 52, 22,
                  kIdDead, font);
    make(L"BUTTON", L"Defaults", WS_TABSTOP, margin, buttonsY, 96, 30, kIdDefaults, font);
    make(L"BUTTON", L"Save", WS_TABSTOP | BS_DEFPUSHBUTTON, width - margin - 2 * 96 - 8, buttonsY, 96, 30, kIdSave, font);
    make(L"BUTTON", L"Cancel", WS_TABSTOP, width - margin - 96, buttonsY, 96, 30, kIdCancel, font);
    d.refresh();

    SetWindowLongPtrW(d.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&d));
    SetTimer(d.hwnd, 1, 30, nullptr);
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(d.hwnd, SW_SHOW);
    SetForegroundWindow(d.hwnd);

    MSG msg;
    while (!d.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        const bool key = msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN;
        if (key && d.captureRow >= 0 && !d.capturePad) {
            int vk = static_cast<int>(msg.wParam);
            // tell left and right Shift / Ctrl apart
            if (vk == VK_SHIFT) vk = static_cast<int>(MapVirtualKeyW((msg.lParam >> 16) & 0xFF, MAPVK_VSC_TO_VK_EX));
            if (vk == VK_CONTROL) vk = (msg.lParam & (1 << 24)) ? VK_RCONTROL : VK_LCONTROL;
            d.toggleKey(vk);
            continue;
        }
        if (key && msg.wParam == VK_ESCAPE && d.captureRow < 0) {
            d.done = true;
            continue;
        }
        if (IsDialogMessageW(d.hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    KillTimer(d.hwnd, 1);
    if (owner) EnableWindow(owner, TRUE);
    SetWindowLongPtrW(d.hwnd, GWLP_USERDATA, 0);
    DestroyWindow(d.hwnd);
    DeleteObject(font);
    DeleteObject(bold);
    if (owner) SetForegroundWindow(owner);
    if (d.saved) {
        controls = d.c;
        controls.save(ini);
    }
    return d.saved;
}

}  // namespace app
