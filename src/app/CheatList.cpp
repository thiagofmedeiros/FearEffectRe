#include "CheatList.h"

#include <commctrl.h>
#include <urlmon.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "Cheats.h"

namespace app {

namespace {

constexpr const wchar_t* kCheatUrl = L"https://raw.githubusercontent.com/duckstation/chtdb/master/cheats/SLUS-00920.cht";

std::filesystem::path exeDir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::filesystem::path(exe).parent_path();
}

std::filesystem::path enabledPath() { return exeDir() / L"cheats" / L"enabled.txt"; }

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::vector<std::string> readEnabled() {
    std::vector<std::string> names;
    std::ifstream in(enabledPath());
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) names.push_back(line);
    }
    return names;
}

void writeEnabled(const std::vector<std::string>& names) {
    std::error_code ec;
    std::filesystem::create_directories(enabledPath().parent_path(), ec);
    std::ofstream out(enabledPath(), std::ios::trunc);
    for (const auto& n : names) out << n << "\n";
}

// Downloads the cheat file; true when a usable one is in place.
bool download(HWND owner) {
    const std::filesystem::path target = cheatFilePath();
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    const std::filesystem::path tmp = target.wstring() + L".download";
    HCURSOR old = SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_WAIT)));
    HRESULT hr = URLDownloadToFileW(nullptr, kCheatUrl, tmp.c_str(), 0, nullptr);
    SetCursor(old);
    bool ok = SUCCEEDED(hr);
    if (ok) {
        try {
            ok = !Cheats::read(narrow(tmp.wstring())).empty();  // a cheat file, not an error page
        } catch (const std::exception&) {
            ok = false;
        }
    }
    if (ok) ok = MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    std::filesystem::remove(tmp, ec);
    if (!ok)
        MessageBoxW(owner,
                    L"The cheat list could not be downloaded. Check the internet connection, or put a DuckStation "
                    L"cheat file for Fear Effect in the \"cheats\" folder as SLUS-00920.cht.",
                    L"Cheats", MB_OK | MB_ICONWARNING);
    return ok;
}

enum { kIdList = 100, kIdFilter, kIdNone, kIdOk, kIdCancel, kIdCount };

struct Dialog {
    HWND hwnd = nullptr, list = nullptr, filter = nullptr, count = nullptr;
    std::vector<Cheats::Cheat> cheats;
    std::vector<bool> on;
    bool filling = false, done = false, ok = false;

    void updateCount() {
        const auto n = std::count(on.begin(), on.end(), true);
        std::wstring text = n == 0 ? L"No cheats on" : n == 1 ? L"1 cheat on" : std::to_wstring(n) + L" cheats on";
        SetWindowTextW(count, text.c_str());
    }
    // Shows the cheats whose name contains the filter text.
    void fill() {
        wchar_t buf[128];
        GetWindowTextW(filter, buf, 128);
        std::wstring want = buf;
        std::transform(want.begin(), want.end(), want.begin(), ::towlower);
        filling = true;
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list);
        int row = 0;
        for (size_t i = 0; i < cheats.size(); ++i) {
            std::wstring name = widen(cheats[i].name), group;
            if (size_t slash = name.rfind(L'\\'); slash != std::wstring::npos) {
                group = name.substr(0, slash);
                name = name.substr(slash + 1);
            }
            std::wstring lower = group + L" " + name;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
            if (!want.empty() && lower.find(want) == std::wstring::npos) continue;
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = row;
            item.pszText = name.data();
            item.lParam = static_cast<LPARAM>(i);
            SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
            LVITEMW sub{};
            sub.iSubItem = 1;
            sub.pszText = group.data();
            SendMessageW(list, LVM_SETITEMTEXTW, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&sub));
            ListView_SetCheckState(list, row, on[i] ? TRUE : FALSE);
            ++row;
        }
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        filling = false;
    }
};

LRESULT CALLBACK dialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = reinterpret_cast<Dialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!d) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_CLOSE: d->done = true; return 0;
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMLISTVIEW*>(lp);
            if (nm->hdr.idFrom == kIdList && nm->hdr.code == LVN_ITEMCHANGED && !d->filling && (nm->uChanged & LVIF_STATE) &&
                ((nm->uNewState ^ nm->uOldState) & LVIS_STATEIMAGEMASK)) {
                LVITEMW item{};
                item.mask = LVIF_PARAM;
                item.iItem = nm->iItem;
                SendMessageW(d->list, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item));
                d->on[static_cast<size_t>(item.lParam)] = ListView_GetCheckState(d->list, nm->iItem) != 0;
                d->updateCount();
            }
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == kIdFilter && HIWORD(wp) == EN_CHANGE) d->fill();
            else if (id == kIdNone) {
                std::fill(d->on.begin(), d->on.end(), false);
                d->fill();
                d->updateCount();
            } else if (id == kIdOk) {
                d->ok = d->done = true;
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

std::wstring cheatFilePath() { return (exeDir() / L"cheats" / L"SLUS-00920.cht").wstring(); }

std::vector<std::string> enabledCheats() {
    std::vector<std::string> out;
    std::error_code ec;
    if (!std::filesystem::exists(cheatFilePath(), ec)) return out;
    std::vector<Cheats::Cheat> all;
    try {
        all = Cheats::read(narrow(cheatFilePath()));
    } catch (const std::exception&) {
        return out;
    }
    for (const auto& name : readEnabled())
        for (const auto& c : all)
            if (c.name == name) {
                out.push_back(name);
                break;
            }
    return out;
}

bool runCheatDialog(HWND owner) {
    std::error_code ec;
    if (!std::filesystem::exists(cheatFilePath(), ec)) {
        int answer = MessageBoxW(owner,
                                 L"Download the list of cheats for Fear Effect from DuckStation's cheat database "
                                 L"(github.com/duckstation/chtdb)?\n\nThe cheats are GameShark codes by their authors. "
                                 L"The list is kept in the \"cheats\" folder next to the program.",
                                 L"Cheats", MB_YESNO | MB_ICONQUESTION);
        if (answer != IDYES || !download(owner)) return false;
    }
    Dialog d;
    try {
        d.cheats = Cheats::read(narrow(cheatFilePath()));
    } catch (const std::exception&) {
    }
    if (d.cheats.empty()) {
        MessageBoxW(owner, L"The cheat file has no cheats that can be used.", L"Cheats", MB_OK | MB_ICONWARNING);
        return false;
    }
    d.on.assign(d.cheats.size(), false);
    for (const auto& name : readEnabled())
        for (size_t i = 0; i < d.cheats.size(); ++i)
            if (d.cheats[i].name == name) d.on[i] = true;

    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = dialogProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"FearEffectReCheats";
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    RegisterClassW(&wc);

    HDC screen = GetDC(nullptr);
    const int dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    auto px = [&](int v) { return MulDiv(v, dpi, 96); };
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);

    const int margin = 16, width = 560, listTop = 84, listH = 400;
    const int noteY = listTop + listH + 10, buttonsY = noteY + 52, height = buttonsY + 32 + margin;
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT r{0, 0, px(width), px(height)};
    AdjustWindowRectEx(&r, style, FALSE, WS_EX_DLGMODALFRAME);
    const int w = r.right - r.left, h = r.bottom - r.top;
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY), &mi);
    const int x = mi.rcWork.left + std::max(0L, (mi.rcWork.right - mi.rcWork.left - w) / 2);
    const int y = mi.rcWork.top + std::max(0L, (mi.rcWork.bottom - mi.rcWork.top - h) / 2);
    d.hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"Cheats", style, x, y, w, h, owner, nullptr, inst, nullptr);
    if (!d.hwnd) {
        DeleteObject(font);
        return false;
    }
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD extra, int cx, int cy, int cw, int ch, int id) {
        HWND c = CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | extra, px(cx), px(cy), px(cw), px(ch), d.hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    };
    make(L"STATIC",
         L"Tick the cheats to use. They are applied every frame while you play. Some can stop the game from "
         L"working properly; turn them off again if it misbehaves.",
         0, margin, margin, width - 2 * margin, 36, 0);
    make(L"STATIC", L"Find:", SS_CENTERIMAGE, margin, 54, 40, 24, 0);
    d.filter = make(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, margin + 44, 54, 240, 24, kIdFilter);
    d.count = make(L"STATIC", L"", SS_CENTERIMAGE | SS_RIGHT, width - margin - 200, 54, 200, 24, kIdCount);
    d.list = make(WC_LISTVIEWW, L"", WS_BORDER | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, margin, listTop,
                  width - 2 * margin, listH, kIdList);
    ListView_SetExtendedListViewStyle(d.list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = const_cast<wchar_t*>(L"Cheat");
    col.cx = px(300);
    SendMessageW(d.list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&col));
    col.pszText = const_cast<wchar_t*>(L"Group");
    col.cx = px(width - 2 * margin - 300 - 24);
    SendMessageW(d.list, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&col));
    make(L"STATIC",
         L"From DuckStation's cheat database (github.com/duckstation/chtdb). Cheats that change the game's code "
         L"make that part of it run in the interpreter, which is slower.",
         0, margin, noteY, width - 2 * margin, 36, 0);
    make(L"BUTTON", L"Turn all off", WS_TABSTOP, margin, buttonsY, 110, 30, kIdNone);
    make(L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, width - margin - 2 * 96 - 8, buttonsY, 96, 30, kIdOk);
    make(L"BUTTON", L"Cancel", WS_TABSTOP, width - margin - 96, buttonsY, 96, 30, kIdCancel);

    SetWindowLongPtrW(d.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&d));
    d.fill();
    d.updateCount();
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(d.hwnd, SW_SHOW);
    SetForegroundWindow(d.hwnd);
    SetFocus(d.filter);

    MSG msg;
    while (!d.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            d.done = true;
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && msg.hwnd != d.list) {
            d.ok = d.done = true;
            continue;
        }
        if (IsDialogMessageW(d.hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (owner) EnableWindow(owner, TRUE);
    SetWindowLongPtrW(d.hwnd, GWLP_USERDATA, 0);
    DestroyWindow(d.hwnd);
    DeleteObject(font);
    if (owner) SetForegroundWindow(owner);
    if (d.ok) {
        std::vector<std::string> names;
        for (size_t i = 0; i < d.cheats.size(); ++i)
            if (d.on[i]) names.push_back(d.cheats[i].name);
        writeEnabled(names);
    }
    return d.ok;
}

}  // namespace app
