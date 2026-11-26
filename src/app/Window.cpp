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
#include <cmath>
#include <cstring>
#include <cwctype>
#include <thread>

namespace app {


namespace {
bool down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
}  // namespace

// ---- window --------------------------------------------------------------------

struct Window::Impl {
    HWND hwnd = nullptr;
    Controls controls = Controls::defaults();
    int volume = 100;  // percent, 0 when muted
    bool keyWas[256]{};
    bool edge(int vk, bool focused) {
        const bool now = focused && down(vk);
        const bool pressed = now && !keyWas[vk];
        keyWas[vk] = now;
        return pressed;
    }
    // status line drawn over the picture
    std::string statusText;
    LARGE_INTEGER statusUntil{};
    GLuint font = 0;  // display lists for characters 32..127
    bool statusVisible() const {
        if (statusText.empty()) return false;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return now.QuadPart < statusUntil.QuadPart;
    }
    void drawStatusGl(int cw, int ch);
    void drawStatusGdi(HDC dc);
    // window / fullscreen switching
    bool fullscreen = false;
    RECT windowed{};  // window rectangle to go back to
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
    // Widescreen: gameplay and movies are a 2:1 picture on lines 40-199 of
    // the 240-line frame, black above and below. While those bands are
    // black, only the picture is shown, filling the width; menus that use
    // the whole frame are shown at 4:3. The choice follows the picture after
    // a few frames, so a fade to black does not make it jump back and forth.
    bool widescreen = false;
    bool cropped = false;
    int cropVotes = 0;
    void decide(bool bandsBlack) {
        if (!widescreen) {
            cropped = false;
            return;
        }
        if (bandsBlack == cropped) {
            cropVotes = 0;
        } else if (++cropVotes >= 8) {
            cropped = bandsBlack;
            cropVotes = 0;
        }
    }
    // Are the bands above and below the 2:1 picture black, apart from the
    // HUD (the health bars sit in the top band)? Sampled, not every pixel;
    // up to 8% lit still counts as black. A full-frame menu lights far more.
    static bool dark16(std::uint16_t p) { return (p & 31) <= 2 && ((p >> 5) & 31) <= 2 && ((p >> 10) & 31) <= 2; }
    static bool darkRgb(const std::uint8_t* p) { return p[0] <= 20 && p[1] <= 20 && p[2] <= 20; }
    static bool bandsBlack16(const std::uint16_t* pixels, int stride, int x, int y, int width, int height) {
        const int band = height / 6;
        int lit = 0, all = 0;
        for (int r = 0; r < band; r += 3)
            for (int row : {y + r, y + height - 1 - r})
                for (int i = 0; i < width; i += 7, ++all)
                    if (!dark16(pixels[static_cast<size_t>(row) * stride + x + i])) ++lit;
        return lit * 100 <= all * 8;
    }
    static bool bandsBlackRgb(const std::uint8_t* rgb, int width, int height) {
        const int band = height / 6;
        int lit = 0, all = 0;
        for (int r = 0; r < band; r += 3)
            for (int row : {r, height - 1 - r})
                for (int i = 0; i < width; i += 7, ++all)
                    if (!darkRgb(rgb + (static_cast<size_t>(row) * width + i) * 3)) ++lit;
        return lit * 100 <= all * 8;
    }
    // While cropped, whatever is drawn in the bands (the HUD) is laid over
    // the top and bottom of the picture, with black left transparent. The
    // bands are copied at up to 720 lines' detail for the whole frame.
    GLuint overlayTex[2]{};
    std::vector<std::uint32_t> overlay[2];
    int overlayW = 0, overlayH = 0;
    bool overlayReady = false;
    template <typename Pixel>
    void makeOverlays(int width, int height, Pixel pixel) {
        const int band = height / 6, step = std::max(1, height / 720);
        overlayW = width / step;
        overlayH = band / step;
        overlayReady = overlayW > 0 && overlayH > 0;
        for (int k = 0; k < 2 && overlayReady; ++k) {
            overlay[k].resize(static_cast<size_t>(overlayW) * overlayH);
            const int first = k == 0 ? 0 : height - band;
            for (int r = 0; r < overlayH; ++r)
                for (int i = 0; i < overlayW; ++i) overlay[k][static_cast<size_t>(r) * overlayW + i] = pixel(i * step, first + r * step);
        }
    }
    bool shownCropped = false;  // what the GDI path last drew, to clear the window when it changes
    // the part of the frame that is shown
    void crop(int& y, int& height) const {
        if (!cropped) return;
        y += height / 6;
        height = height * 2 / 3;
    }
    // size of the picture on screen inside a cw x ch client area
    void box(int cw, int ch, int& w, int& h) const {
        if (cropped) {
            w = std::min(cw, ch * 2);
            h = w / 2;
        } else {
            w = std::min(cw, ch * 4 / 3);
            h = w * 3 / 4;
        }
    }
    Pads pads;
    PadSnapshot pad;          // read by buttons(), used by stick() too
    static constexpr int kBuffers = 24;
    WAVEHDR headers[kBuffers]{};
    std::vector<std::int16_t> buffers[kBuffers];
    // Blocks handed to the sound card and not played yet. The frame timer is
    // nudged to keep this between kAudioLow and kAudioHigh, so a slow frame
    // does not empty the queue (a crack) and clock drift does not overfill it.
    int audioLow = 4, audioHigh = 7;
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
    int w, h;
    box(cw, ch, w, h);
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
    if (overlayReady && cropped) {
        // each band is a quarter of the picture's height (40 of its 160 lines)
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        for (int k = 0; k < 2; ++k) {
            if (!overlayTex[k]) {
                glGenTextures(1, &overlayTex[k]);
                glBindTexture(GL_TEXTURE_2D, overlayTex[k]);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            }
            glBindTexture(GL_TEXTURE_2D, overlayTex[k]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, overlayW, overlayH, 0, GL_RGBA, GL_UNSIGNED_BYTE, overlay[k].data());
            const float top = k == 0 ? 1.0f : -0.5f, bottom = top - 0.5f;
            glBegin(GL_QUADS);
            glTexCoord2f(0, 0); glVertex2f(-1, top);
            glTexCoord2f(1, 0); glVertex2f(1, top);
            glTexCoord2f(1, 1); glVertex2f(1, bottom);
            glTexCoord2f(0, 1); glVertex2f(-1, bottom);
            glEnd();
        }
        glDisable(GL_BLEND);
    }
    overlayReady = false;
    drawStatusGl(cw, ch);
    SwapBuffers(glDc);
}

void Window::Impl::drawStatusGl(int cw, int ch) {
    if (!statusVisible()) return;
    const int size = std::max(16, ch / 30);
    if (!font) {
        HFONT f = CreateFontW(-size, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
        HGDIOBJ old = SelectObject(glDc, f);
        font = glGenLists(96);
        wglUseFontBitmapsW(glDc, 32, 96, font);
        SelectObject(glDc, old);
        DeleteObject(f);
    }
    glViewport(0, 0, cw, ch);
    glDisable(GL_TEXTURE_2D);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, cw, 0, ch, -1, 1);
    const int x = std::max(12, cw / 60), y = ch - size - std::max(12, ch / 60);
    glListBase(font - 32);
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 0) glColor3f(0, 0, 0);
        else glColor3f(1, 1, 1);
        glRasterPos2i(x + (pass == 0 ? 2 : 0), y - (pass == 0 ? 2 : 0));
        glCallLists(static_cast<GLsizei>(statusText.size()), GL_UNSIGNED_BYTE, statusText.data());
    }
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glColor3f(1, 1, 1);
    glEnable(GL_TEXTURE_2D);
}

void Window::Impl::drawStatusGdi(HDC dc) {
    if (!statusVisible()) return;
    SetBkMode(dc, TRANSPARENT);
    for (int pass = 0; pass < 2; ++pass) {
        SetTextColor(dc, pass == 0 ? RGB(0, 0, 0) : RGB(255, 255, 255));
        TextOutA(dc, 16 - pass * 2, 16 - pass * 2, statusText.c_str(), static_cast<int>(statusText.size()));
    }
}

namespace {

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE || msg == WM_DESTROY) {
        if (auto* impl = reinterpret_cast<Window::Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) impl->closed = true;
        if (msg == WM_CLOSE) DestroyWindow(hwnd);
        return 0;
    }
    auto* window = reinterpret_cast<Window*>(GetPropW(hwnd, L"FearEffectRe.window"));
    const bool firstPress = (lp & (1 << 30)) == 0;  // not a key repeat
    if (window && firstPress && ((msg == WM_SYSKEYDOWN && wp == VK_RETURN) || (msg == WM_KEYDOWN && wp == VK_F11))) {
        window->toggleFullscreen();
        return 0;
    }
    if (msg == WM_SYSCHAR || (msg == WM_SYSKEYDOWN && wp == VK_F10)) return 0;  // no beep, no menu
    return DefWindowProcW(hwnd, msg, wp, lp);
}

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
    impl_->fullscreen = fullscreen;
    {
        // the window to go back to from fullscreen: the asked size (at most 90% of the screen), centred
        RECT r{0, 0, std::min(width, screenW * 9 / 10), std::min(height, screenH * 9 / 10)};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        const int w = r.right - r.left, h = r.bottom - r.top;
        const int x = target.x + std::max(0, (screenW - w) / 2), y = target.y + std::max(0, (screenH - h) / 2);
        impl_->windowed = RECT{x, y, x + w, y + h};
    }
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
    SetPropW(impl_->hwnd, L"FearEffectRe.window", this);
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
        for (int i = 0; i < impl_->audioLow; ++i) impl_->write(silence);
    }
    if (impl_->volume >= 100) {
        impl_->write(stereo);  // false when every buffer is queued: this block is dropped
        return;
    }
    // the gain is cubic, so loudness follows the setting roughly evenly
    const double g = std::pow(impl_->volume / 100.0, 3.0);
    std::vector<std::int16_t> scaled(stereo.size());
    for (size_t i = 0; i < stereo.size(); ++i) scaled[i] = static_cast<std::int16_t>(std::lround(stereo[i] * g));
    impl_->write(scaled);
}

void Window::setAudio(int volume, bool mute, int latencyMs) {
    impl_->volume = mute ? 0 : std::clamp(volume, 0, 100);
    // a block is one frame of sound, about 16.7 ms; the queue is kept between low and low + 3 blocks
    const int blocks = std::clamp((latencyMs + 8) / 17, 3, Impl::kBuffers - 4);
    impl_->audioLow = std::max(1, blocks - 2);
    impl_->audioHigh = impl_->audioLow + 3;
}

void Window::setControls(const Controls& controls) { impl_->controls = controls; }

std::vector<Hotkey> Window::hotkeys() {
    std::vector<Hotkey> out;
    const bool focused = GetForegroundWindow() == impl_->hwnd;
    const bool shift = focused && down(VK_SHIFT);
    if (impl_->edge(VK_F6, focused)) out.push_back(Hotkey::SaveState);
    if (impl_->edge(VK_F7, focused)) out.push_back(Hotkey::LoadState);
    if (impl_->edge(VK_F8, focused)) out.push_back(shift ? Hotkey::PrevSlot : Hotkey::NextSlot);
    if (impl_->edge(VK_F9, focused)) out.push_back(Hotkey::Controls);
    return out;
}

void Window::status(const std::string& text) {
    impl_->statusText = text;
    QueryPerformanceCounter(&impl_->statusUntil);
    impl_->statusUntil.QuadPart += impl_->freq.QuadPart * 3;
    std::wstring title = L"FearEffectRe - " + std::wstring(text.begin(), text.end());
    SetWindowTextW(impl_->hwnd, title.c_str());
}

void Window::toggleFullscreen() {
    Impl& im = *impl_;
    if (im.fullscreen) {
        SetWindowLongPtrW(im.hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(im.hwnd, HWND_NOTOPMOST, im.windowed.left, im.windowed.top, im.windowed.right - im.windowed.left,
                     im.windowed.bottom - im.windowed.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        GetWindowRect(im.hwnd, &im.windowed);
        MONITORINFO mi{};
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(MonitorFromWindow(im.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT& r = mi.rcMonitor;
        SetWindowLongPtrW(im.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(im.hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    im.fullscreen = !im.fullscreen;
    im.shownCropped = !im.cropped;  // the GDI path clears the window on its next frame
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

void Window::setWidescreen(bool on) {
    impl_->widescreen = on;
    impl_->decide(false);
}

bool Window::fastForward() const { return GetForegroundWindow() == impl_->hwnd && down(VK_F1); }

std::uint16_t Window::buttons() const {
    if (GetForegroundWindow() != impl_->hwnd) return 0;
    const Controls& ctl = impl_->controls;
    std::uint16_t b = 0;
    for (int bit = 0; bit < 16; ++bit)
        for (int vk : ctl.keys[bit])
            if (down(vk)) b |= static_cast<std::uint16_t>(1u << bit);
    const PadSnapshot& pad = impl_->pad = impl_->pads.poll();
    if (pad.connected) {
        for (int bit = 0; bit < 16; ++bit)
            for (unsigned code : ctl.pad[bit])
                if (pad.held(code)) b |= static_cast<std::uint16_t>(1u << bit);
        if (ctl.stickAsDpad && !ctl.analog) {  // an analog pad does not press the D-pad with its stick
            const float dead = ctl.deadzone / 100.0f;
            if (pad.ly > dead) b |= 0x0010;
            if (pad.lx > dead) b |= 0x0020;
            if (pad.ly < -dead) b |= 0x0040;
            if (pad.lx < -dead) b |= 0x0080;
        }
    }
    return b;
}

bool Window::stick(float& x, float& y) const {
    const PadSnapshot& pad = impl_->pad;
    if (!pad.connected || GetForegroundWindow() != impl_->hwnd) return false;
    const float dead = impl_->controls.deadzone / 100.0f;  // same dead zone as the stick-as-D-pad
    if (pad.lx * pad.lx + pad.ly * pad.ly < dead * dead) return false;
    x = pad.lx;
    y = pad.ly;
    return true;
}

bool Window::editControls(const std::wstring& iniPath) {
    // the game stops while the window is open; the keys that opened it are forgotten
    const bool changed = runControlsDialog(impl_->hwnd, impl_->controls, iniPath);
    for (bool& k : impl_->keyWas) k = true;
    QueryPerformanceCounter(&impl_->next);
    return changed;
}

const Controls& Window::controls() const { return impl_->controls; }

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
            if (q < impl_->audioLow) period -= period / 100;
            else if (q > impl_->audioHigh) period += period / 100;
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
    if (impl_->widescreen) impl_->decide(Impl::bandsBlackRgb(rgb.data(), width, height));
    if (impl_->cropped && impl_->gl)
        impl_->makeOverlays(width, height, [&](int px, int py) -> std::uint32_t {
            const std::uint8_t* p = rgb.data() + (static_cast<size_t>(py) * width + px) * 3;
            return Impl::darkRgb(p) ? 0u : (p[0] | (p[1] << 8) | (p[2] << 16) | 0xFF000000u);
        });
    int top = 0, rows = height;
    impl_->crop(top, rows);
    if (impl_->gl) {
        impl_->draw(GL_RGB, GL_UNSIGNED_BYTE, rgb.data(), width, 0, top, width, rows);
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
    int w, h;
    impl_->box(cw, ch, w, h);
    HDC dc = GetDC(impl_->hwnd);
    if (impl_->shownCropped != impl_->cropped) {
        RECT all{0, 0, cw, ch};
        FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        impl_->shownCropped = impl_->cropped;
    }
    SetStretchBltMode(dc, COLORONCOLOR);
    // the bitmap is stored bottom-up: source rows count from the bottom
    StretchDIBits(dc, (cw - w) / 2, (ch - h) / 2, w, h, 0, height - top - rows, width, rows, impl_->bgr.data(), &bmi,
                  DIB_RGB_COLORS, SRCCOPY);
    impl_->drawStatusGdi(dc);
    ReleaseDC(impl_->hwnd, dc);

    pace();
    return true;
}

bool Window::present(const std::uint16_t* pixels, int stride, int x, int y, int width, int height) {
    if (!pump()) return false;
    if (impl_->widescreen) impl_->decide(Impl::bandsBlack16(pixels, stride, x, y, width, height));
    if (impl_->cropped && impl_->gl)
        impl_->makeOverlays(width, height, [&](int px, int py) -> std::uint32_t {
            const std::uint16_t p = pixels[static_cast<size_t>(y + py) * stride + x + px];
            if (Impl::dark16(p)) return 0u;
            const std::uint32_t r = (p & 31) << 3, g = ((p >> 5) & 31) << 3, b = ((p >> 10) & 31) << 3;
            return r | (g << 8) | (b << 16) | 0xFF000000u;
        });
    impl_->crop(y, height);
    if (impl_->gl) {
        // video memory layout as OpenGL names it: red in the low five bits, top bit unused
        impl_->draw(impl_->swizzle ? GL_BGRA : GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, pixels, stride, x, y, width, height);
        pace();
        return true;
    }
    RECT rc;
    GetClientRect(impl_->hwnd, &rc);
    int cw = rc.right, ch = rc.bottom;
    int w, h;
    impl_->box(cw, ch, w, h);
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
    im.drawStatusGdi(dc);
    ReleaseDC(im.hwnd, dc);
    pace();
    return true;
}

}  // namespace app
