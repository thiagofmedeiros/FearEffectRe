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
#include <GL/gl.h>

#ifndef GL_UNSIGNED_SHORT_1_5_5_5_REV
#define GL_UNSIGNED_SHORT_1_5_5_5_REV 0x8366
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_RGB5_A1
#define GL_RGB5_A1 0x8057
#endif
#ifndef GL_TEXTURE_SWIZZLE_R
#define GL_TEXTURE_SWIZZLE_R 0x8E42
#define GL_TEXTURE_SWIZZLE_B 0x8E44
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#include <algorithm>
#include <thread>

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
    HDC surfaceDc = nullptr;
    HBITMAP surface = nullptr;
    HGDIOBJ surfaceOld = nullptr;
    std::uint32_t* surfaceBits = nullptr;
    int surfaceW = 0, surfaceH = 0;
    std::vector<std::uint32_t> lut;
    std::vector<int> columns;
    // OpenGL presentation: the frame is uploaded as a texture and the graphics
    // card scales it. Null when no context could be created (GDI is used then).
    HDC glDc = nullptr;
    HGLRC gl = nullptr;
    GLuint texture = 0;
    int texW = 0, texH = 0;
    GLenum texType = 0;
    GLuint texture16 = 0;     // 15-bit frames, stored as the card's native 5-5-5-1 layout
    bool swizzle = false;     // red and blue are exchanged by the texture, not by converting pixels
    void draw(GLenum format, GLenum type, const void* pixels, int stride, int x, int y, int width, int height);
    GetPadState xinput = loadXInput();
    DWORD padIndex = 0;       // controller slot that answered last
    int padRetry = 0;         // frames until disconnected slots are probed again
    static constexpr int kBuffers = 24;
    WAVEHDR headers[kBuffers]{};
    std::vector<std::int16_t> buffers[kBuffers];
    // Blocks handed to the sound card and not played yet. The frame timer is
    // nudged to keep this between kAudioLow and kAudioHigh, so a slow frame
    // does not empty the queue (a crack) and clock drift does not overfill it.
    static constexpr int kAudioLow = 4, kAudioHigh = 7;
    int queued() const {
        int n = 0;
        for (const WAVEHDR& h : headers)
            if ((h.dwFlags & WHDR_PREPARED) && !(h.dwFlags & WHDR_DONE)) ++n;
        return n;
    }
    bool write(const std::vector<std::int16_t>& stereo) {
        for (int i = 0; i < kBuffers; ++i) {
            WAVEHDR& h = headers[i];
            if ((h.dwFlags & WHDR_PREPARED) && !(h.dwFlags & WHDR_DONE)) continue;  // still playing
            if (h.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wave, &h, sizeof h);
            buffers[i] = stereo;
            h = WAVEHDR{};
            h.lpData = reinterpret_cast<LPSTR>(buffers[i].data());
            h.dwBufferLength = static_cast<DWORD>(stereo.size() * sizeof(std::int16_t));
            waveOutPrepareHeader(wave, &h, sizeof h);
            waveOutWrite(wave, &h, sizeof h);
            return true;
        }
        return false;
    }
};

void Window::Impl::draw(GLenum format, GLenum type, const void* pixels, int stride, int x, int y, int width, int height) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int cw = rc.right, ch = rc.bottom;
    int w = std::min(cw, ch * 4 / 3), h = w * 3 / 4;
    glViewport(0, 0, cw, ch);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport((cw - w) / 2, (ch - h) / 2, w, h);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, x);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, y);
    bool packed = type == GL_UNSIGNED_SHORT_1_5_5_5_REV;
    glBindTexture(GL_TEXTURE_2D, packed ? texture16 : texture);
    if (width != texW || height != texH || type != texType) {
        glTexImage2D(GL_TEXTURE_2D, 0, packed ? GL_RGB5_A1 : GL_RGB, width, height, 0, format, type, pixels);
        texW = width;
        texH = height;
        texType = type;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, pixels);
    }
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(-1, 1);
    glTexCoord2f(1, 0); glVertex2f(1, 1);
    glTexCoord2f(1, 1); glVertex2f(1, -1);
    glTexCoord2f(0, 1); glVertex2f(-1, -1);
    glEnd();
    SwapBuffers(glDc);
}

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
    // OpenGL context; presentation falls back to GDI if any step fails
    impl_->glDc = GetDC(impl_->hwnd);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    int pf = ChoosePixelFormat(impl_->glDc, &pfd);
    if (pf && SetPixelFormat(impl_->glDc, pf, &pfd)) impl_->gl = wglCreateContext(impl_->glDc);
    if (impl_->gl && wglMakeCurrent(impl_->glDc, impl_->gl)) {
        // the game is paced by its own timer: do not also wait for the display
        using SwapInterval = BOOL(WINAPI*)(int);
        if (auto swap = reinterpret_cast<SwapInterval>(reinterpret_cast<void*>(wglGetProcAddress("wglSwapIntervalEXT")))) swap(0);
        for (GLuint* tex : {&impl_->texture, &impl_->texture16}) {
            glGenTextures(1, tex);
            glBindTexture(GL_TEXTURE_2D, *tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        // Video memory has red in the low bits; graphics cards store 5-5-5-1 with
        // blue there. Uploading it as BGRA needs no conversion, and the texture
        // swaps the two channels back when it is sampled (OpenGL 3.3).
        while (glGetError() != GL_NO_ERROR) {}
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
        impl_->swizzle = glGetError() == GL_NO_ERROR;
        glEnable(GL_TEXTURE_2D);
        glDisable(GL_DEPTH_TEST);
        glClearColor(0, 0, 0, 1);
    } else if (impl_->gl) {
        wglDeleteContext(impl_->gl);
        impl_->gl = nullptr;
    }

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
    if (impl_->gl) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(impl_->gl);
    }
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
    // ran dry (start, or a long stall): put a short silence first so there is
    // a cushion again, instead of crackling block by block
    if (impl_->queued() == 0) {
        std::vector<std::int16_t> silence(stereo.size(), 0);
        for (int i = 0; i < Impl::kAudioLow; ++i) impl_->write(silence);
    }
    impl_->write(stereo);  // false when every buffer is queued: this block is dropped
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

bool Window::stick(float& x, float& y) const {
    if (!impl_->xinput || GetForegroundWindow() != impl_->hwnd) return false;
    PadState pad{};
    if (impl_->xinput(impl_->padIndex, &pad) != ERROR_SUCCESS) return false;
    const float fx = pad.lx / 32767.0f, fy = pad.ly / 32767.0f;
    if (fx * fx + fy * fy < 0.37f * 0.37f) return false;  // same dead zone as the stick-as-D-pad
    x = fx;
    y = fy;
    return true;
}

bool Window::pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (impl_->closed) return false;
    return true;
}

void Window::pace() {
    // 59.94 frames per second unless fast-forwarding
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!fastForward()) {
        // 59.94 Hz, 1% faster or slower to keep the sound queue in range
        LONGLONG period = impl_->freq.QuadPart * 1001 / 60000;
        if (impl_->wave) {
            const int q = impl_->queued();
            if (q < Impl::kAudioLow) period -= period / 100;
            else if (q > Impl::kAudioHigh) period += period / 100;
        }
        impl_->next.QuadPart += period;
        // Fell behind (a slow frame): catch up gently, at most half a
        // frame each frame, instead of running frames back to back, which
        // makes movies visibly rush after every slow frame. Far behind
        // (a pause, a stall): start again from now.
        const LONGLONG late = now.QuadPart - impl_->next.QuadPart;
        if (late > impl_->freq.QuadPart / 10) impl_->next = now;
        else if (late > period / 2) impl_->next.QuadPart = now.QuadPart - period / 2;
        while (now.QuadPart < impl_->next.QuadPart) {
            LONGLONG ms = (impl_->next.QuadPart - now.QuadPart) * 1000 / impl_->freq.QuadPart;
            if (ms > 1) Sleep(static_cast<DWORD>(ms - 1));
            else YieldProcessor();
            QueryPerformanceCounter(&now);
        }
    } else {
        impl_->next = now;
    }
}

bool Window::idle() {
    if (!pump()) return false;
    pace();
    return true;
}

bool Window::present(const std::vector<std::uint8_t>& rgb, int width, int height) {
    if (!pump()) return false;
    if (impl_->gl) {
        impl_->draw(GL_RGB, GL_UNSIGNED_BYTE, rgb.data(), width, 0, 0, width, height);
        pace();
        return true;
    }

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

    pace();
    return true;
}

bool Window::present(const std::uint16_t* pixels, int stride, int x, int y, int width, int height) {
    if (!pump()) return false;
    if (impl_->gl) {
        // video memory layout as OpenGL names it: red in the low five bits, top bit unused
        impl_->draw(impl_->swizzle ? GL_BGRA : GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, pixels, stride, x, y, width, height);
        pace();
        return true;
    }
    RECT rc;
    GetClientRect(impl_->hwnd, &rc);
    int cw = rc.right, ch = rc.bottom;
    int w = std::min(cw, ch * 4 / 3), h = w * 3 / 4;
    if (w <= 0 || h <= 0) {
        pace();
        return true;
    }
    Impl& im = *impl_;
    HDC dc = GetDC(im.hwnd);
    // A 32-bit surface the size of the picture on screen. Scaling and the
    // 15-bit to 32-bit conversion are done here, on several threads, so GDI
    // only has to copy it.
    if (!im.surface || im.surfaceW != w || im.surfaceH != h) {
        if (im.surface) {
            SelectObject(im.surfaceDc, im.surfaceOld);
            DeleteObject(im.surface);
        }
        if (!im.surfaceDc) im.surfaceDc = CreateCompatibleDC(dc);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        im.surface = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        im.surfaceBits = static_cast<std::uint32_t*>(bits);
        im.surfaceOld = SelectObject(im.surfaceDc, im.surface);
        im.surfaceW = w;
        im.surfaceH = h;
        RECT all{0, 0, cw, ch};
        FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    }
    if (im.lut.empty()) {
        im.lut.resize(32768);
        for (std::uint32_t p = 0; p < 32768; ++p) {
            std::uint32_t r = (p & 31) << 3, g = ((p >> 5) & 31) << 3, b = ((p >> 10) & 31) << 3;
            im.lut[p] = (r << 16) | (g << 8) | b;
        }
    }
    im.columns.resize(static_cast<size_t>(w));
    for (int i = 0; i < w; ++i) im.columns[static_cast<size_t>(i)] = x + static_cast<int>(static_cast<long long>(i) * width / w);
    auto band = [&](int from, int to) {
        for (int row = from; row < to; ++row) {
            const std::uint16_t* src = pixels + static_cast<size_t>(y + static_cast<long long>(row) * height / h) * stride;
            std::uint32_t* dst = im.surfaceBits + static_cast<size_t>(row) * w;
            const int* col = im.columns.data();
            const std::uint32_t* lut = im.lut.data();
            for (int i = 0; i < w; ++i) dst[i] = lut[src[col[i]] & 0x7FFF];
        }
    };
    int workers = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 8u));
    if (static_cast<long long>(w) * h < 1500000) workers = 1;
    std::vector<std::thread> threads;
    for (int t = 1; t < workers; ++t) threads.emplace_back(band, h * t / workers, h * (t + 1) / workers);
    band(0, h / workers);
    for (auto& t : threads) t.join();
    BitBlt(dc, (cw - w) / 2, (ch - h) / 2, w, h, im.surfaceDc, 0, 0, SRCCOPY);
    ReleaseDC(im.hwnd, dc);
    pace();
    return true;
}

}  // namespace app
