#include "Window.h"

#include "Launcher.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>

namespace app {

namespace {
// XInput is loaded at run time so the program still starts where it is missing.
struct PadState {
    DWORD packet;
    WORD buttons;
    BYTE leftTrigger, rightTrigger;
    SHORT lx, ly, rx, ry;
};
using GetPadState = DWORD(WINAPI*)(DWORD, PadState*);

GetPadState loadXInput() {
    for (const wchar_t* name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"})
        if (HMODULE lib = LoadLibraryW(name))
            if (auto fn = reinterpret_cast<GetPadState>(reinterpret_cast<void*>(GetProcAddress(lib, "XInputGetState"))))
                return fn;
    return nullptr;
}
}  // namespace

struct Window::Impl {
    HWND hwnd = nullptr;
    bool closed = false;
    LARGE_INTEGER freq{}, next{};
    std::vector<std::uint8_t> bgr;
    HWAVEOUT wave = nullptr;
    bool discKey[4]{};
    GetPadState xinput = loadXInput();
    DWORD padIndex = 0;       // controller slot that answered last
    int padRetry = 0;         // frames until disconnected slots are probed again
    static constexpr int kBuffers = 24;
    WAVEHDR headers[kBuffers]{};
    std::vector<std::int16_t> buffers[kBuffers];
};

namespace {

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE || msg == WM_DESTROY) {
        if (auto* impl = reinterpret_cast<Window::Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) impl->closed = true;
        if (msg == WM_CLOSE) DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

}  // namespace

Window::Window(int width, int height, bool fullscreen, int display) : impl_(new Impl) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FearEffectRe";
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassW(&wc);
    std::vector<Display> displays = listDisplays();
    const Display& target = displays[display >= 0 && display < static_cast<int>(displays.size()) ? display : 0];
    int screenW = target.width, screenH = target.height;
    // a window as large as the screen cannot show its frame: go borderless instead
    if (width >= screenW && height >= screenH) fullscreen = true;
    if (fullscreen) {
        impl_->hwnd = CreateWindowW(wc.lpszClassName, L"FearEffectRe", WS_POPUP | WS_VISIBLE, target.x, target.y, screenW, screenH,
                                    nullptr, nullptr, wc.hInstance, nullptr);
    } else {
        RECT r{0, 0, width, height};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        int w = r.right - r.left, h = r.bottom - r.top;
        // centred; a window larger than the screen starts at its top-left corner
        int x = target.x + std::max(0, (screenW - w) / 2), y = target.y + std::max(0, (screenH - h) / 2);
        impl_->hwnd = CreateWindowW(wc.lpszClassName, L"FearEffectRe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, x, y, w, h,
                                    nullptr, nullptr, wc.hInstance, nullptr);
    }
    SetWindowLongPtrW(impl_->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl_));
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = 44100;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = 44100 * 4;
    if (waveOutOpen(&impl_->wave, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) impl_->wave = nullptr;
    QueryPerformanceFrequency(&impl_->freq);
    QueryPerformanceCounter(&impl_->next);
}

Window::~Window() {
    if (impl_->wave) {
        waveOutReset(impl_->wave);
        for (auto& h : impl_->headers)
            if (h.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(impl_->wave, &h, sizeof h);
        waveOutClose(impl_->wave);
    }
    if (impl_->hwnd && !impl_->closed) DestroyWindow(impl_->hwnd);
    delete impl_;
}

void Window::audio(const std::vector<std::int16_t>& stereo) {
    if (!impl_->wave || stereo.empty() || fastForward()) return;
    for (int i = 0; i < Impl::kBuffers; ++i) {
        WAVEHDR& h = impl_->headers[i];
        if ((h.dwFlags & WHDR_PREPARED) && !(h.dwFlags & WHDR_DONE)) continue;  // still playing
        if (h.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(impl_->wave, &h, sizeof h);
        impl_->buffers[i] = stereo;
        h = WAVEHDR{};
        h.lpData = reinterpret_cast<LPSTR>(impl_->buffers[i].data());
        h.dwBufferLength = static_cast<DWORD>(stereo.size() * sizeof(std::int16_t));
        waveOutPrepareHeader(impl_->wave, &h, sizeof h);
        waveOutWrite(impl_->wave, &h, sizeof h);
        return;
    }
    // every buffer is queued: the game is running ahead of the sound card, drop this block
}

int Window::discRequest() {
    int request = 0;
    bool focused = GetForegroundWindow() == impl_->hwnd;
    for (int i = 0; i < 4; ++i) {
        bool now = focused && down(VK_F2 + i);
        if (now && !impl_->discKey[i]) request = i + 1;
        impl_->discKey[i] = now;
    }
    return request;
}

bool Window::fastForward() const { return GetForegroundWindow() == impl_->hwnd && down(VK_F1); }

std::uint16_t Window::buttons() const {
    if (GetForegroundWindow() != impl_->hwnd) return 0;
    std::uint16_t b = 0;
    if (down(VK_BACK)) b |= 0x0001;                    // select
    if (down('P')) b |= 0x0008;                        // start
    if (down('W') || down(VK_UP)) b |= 0x0010;
    if (down('D') || down(VK_RIGHT)) b |= 0x0020;
    if (down('S') || down(VK_DOWN)) b |= 0x0040;
    if (down('A') || down(VK_LEFT)) b |= 0x0080;
    if (down('1')) b |= 0x0100;                        // L2
    if (down('4')) b |= 0x0200;                        // R2
    if (down(VK_TAB)) b |= 0x0400;                     // L1
    if (down('R')) b |= 0x0800;                        // R1
    if (down(VK_SPACE)) b |= 0x1000;                   // triangle
    if (down('Q') || down(VK_ESCAPE)) b |= 0x2000;     // circle
    if (down('E') || down(VK_RETURN)) b |= 0x4000;     // cross
    if (down('F')) b |= 0x8000;                        // square

    // Xbox controller (XInput): the first connected one. Probing empty
    // slots is slow, so that is only done about once a second.
    if (impl_->xinput) {
        PadState pad{};
        bool found = impl_->xinput(impl_->padIndex, &pad) == ERROR_SUCCESS;
        if (!found && --impl_->padRetry <= 0) {
            impl_->padRetry = 60;
            for (DWORD i = 0; i < 4 && !found; ++i)
                if (impl_->xinput(i, &pad) == ERROR_SUCCESS) {
                    impl_->padIndex = i;
                    found = true;
                }
        }
        if (found) {
            const int dead = 12000;
            WORD x = pad.buttons;
            if (x & 0x0020) b |= 0x0001;                       // Back   -> select
            if (x & 0x0010) b |= 0x0008;                       // Start  -> start
            if ((x & 0x0001) || pad.ly > dead) b |= 0x0010;    // up
            if ((x & 0x0008) || pad.lx > dead) b |= 0x0020;    // right
            if ((x & 0x0002) || pad.ly < -dead) b |= 0x0040;   // down
            if ((x & 0x0004) || pad.lx < -dead) b |= 0x0080;   // left
            if (pad.leftTrigger > 64) b |= 0x0100;             // LT -> L2
            if (pad.rightTrigger > 64) b |= 0x0200;            // RT -> R2
            if (x & 0x0100) b |= 0x0400;                       // LB -> L1
            if (x & 0x0200) b |= 0x0800;                       // RB -> R1
            if (x & 0x8000) b |= 0x1000;                       // Y  -> triangle
            if (x & 0x2000) b |= 0x2000;                       // B  -> circle
            if (x & 0x1000) b |= 0x4000;                       // A  -> cross
            if (x & 0x4000) b |= 0x8000;                       // X  -> square
        }
    }
    return b;
}

bool Window::present(const std::vector<std::uint8_t>& rgb, int width, int height) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (impl_->closed) return false;

    // bottom-up BGR rows padded to 4 bytes
    int stride = (width * 3 + 3) & ~3;
    impl_->bgr.assign(static_cast<size_t>(stride) * height, 0);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* src = rgb.data() + static_cast<size_t>(y) * width * 3;
        std::uint8_t* dst = impl_->bgr.data() + static_cast<size_t>(height - 1 - y) * stride;
        for (int x = 0; x < width; ++x) {
            dst[x * 3] = src[x * 3 + 2];
            dst[x * 3 + 1] = src[x * 3 + 1];
            dst[x * 3 + 2] = src[x * 3];
        }
    }
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    RECT rc;
    GetClientRect(impl_->hwnd, &rc);
    int cw = rc.right, ch = rc.bottom;
    int w = std::min(cw, ch * 4 / 3), h = w * 3 / 4;
    HDC dc = GetDC(impl_->hwnd);
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, (cw - w) / 2, (ch - h) / 2, w, h, 0, 0, width, height, impl_->bgr.data(), &bmi, DIB_RGB_COLORS,
                  SRCCOPY);
    ReleaseDC(impl_->hwnd, dc);

    // 59.94 frames per second unless fast-forwarding
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!fastForward()) {
        impl_->next.QuadPart += impl_->freq.QuadPart * 1001 / 60000;
        if (impl_->next.QuadPart < now.QuadPart - impl_->freq.QuadPart / 10) impl_->next = now;  // fell behind
        while (now.QuadPart < impl_->next.QuadPart) {
            LONGLONG ms = (impl_->next.QuadPart - now.QuadPart) * 1000 / impl_->freq.QuadPart;
            if (ms > 1) Sleep(static_cast<DWORD>(ms - 1));
            else YieldProcessor();
            QueryPerformanceCounter(&now);
        }
    } else {
        impl_->next = now;
    }
    return true;
}

}  // namespace app
