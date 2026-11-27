#include "GlRenderer.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace app {

namespace {

// ---- the OpenGL 3.3 functions (Windows only exports 1.1) ---------------------
using GLchar = char;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;

constexpr GLenum kFramebuffer = 0x8D40, kReadFramebuffer = 0x8CA8, kDrawFramebuffer = 0x8CA9;
constexpr GLenum kColorAttachment0 = 0x8CE0, kFramebufferComplete = 0x8CD5;
constexpr GLenum kArrayBuffer = 0x8892, kStreamDraw = 0x88E0;
constexpr GLenum kVertexShader = 0x8B31, kFragmentShader = 0x8B30, kCompileStatus = 0x8B81, kLinkStatus = 0x8B82;
constexpr GLenum kTexture0 = 0x84C0, kR16ui = 0x8234, kRedInteger = 0x8D94, kRgba8 = 0x8058;
constexpr GLenum kUnsignedShort1555Rev = 0x8366, kClampToEdge = 0x812F;
constexpr GLenum kFuncAdd = 0x8006, kFuncReverseSubtract = 0x800B, kConstantAlpha = 0x8003;
constexpr GLenum kUnpackRowLength = 0x0CF2, kPackRowLength = 0x0D02;

#define FE_GL_FUNCTIONS(X)                                                                                    \
    X(void, GenFramebuffers, (GLsizei, GLuint*))                                                             \
    X(void, BindFramebuffer, (GLenum, GLuint))                                                                \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                                    \
    X(GLenum, CheckFramebufferStatus, (GLenum))                                                               \
    X(void, BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))    \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint*))                                                     \
    X(GLuint, CreateShader, (GLenum))                                                                         \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))                              \
    X(void, CompileShader, (GLuint))                                                                          \
    X(void, GetShaderiv, (GLuint, GLenum, GLint*))                                                            \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                           \
    X(GLuint, CreateProgram, ())                                                                              \
    X(void, AttachShader, (GLuint, GLuint))                                                                   \
    X(void, BindAttribLocation, (GLuint, GLuint, const GLchar*))                                              \
    X(void, LinkProgram, (GLuint))                                                                            \
    X(void, GetProgramiv, (GLuint, GLenum, GLint*))                                                           \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                          \
    X(void, UseProgram, (GLuint))                                                                             \
    X(GLint, GetUniformLocation, (GLuint, const GLchar*))                                                     \
    X(void, Uniform1i, (GLint, GLint))                                                                        \
    X(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))                                           \
    X(void, GenBuffers, (GLsizei, GLuint*))                                                                   \
    X(void, BindBuffer, (GLenum, GLuint))                                                                     \
    X(void, BufferData, (GLenum, GLsizeiptr, const void*, GLenum))                                            \
    X(void, GenVertexArrays, (GLsizei, GLuint*))                                                              \
    X(void, BindVertexArray, (GLuint))                                                                        \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))                    \
    X(void, VertexAttribIPointer, (GLuint, GLint, GLenum, GLsizei, const void*))                              \
    X(void, EnableVertexAttribArray, (GLuint))                                                                \
    X(void, BlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))                                              \
    X(void, BlendEquation, (GLenum))                                                                          \
    X(void, BlendColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                                 \
    X(void, ActiveTexture, (GLenum))

struct Gl {
#define FE_GL_MEMBER(ret, name, args) ret(WINAPI* name) args = nullptr;
    FE_GL_FUNCTIONS(FE_GL_MEMBER)
#undef FE_GL_MEMBER
    bool load(std::string& missing) {
#define FE_GL_LOAD(ret, name, args)                                                                            \
    name = reinterpret_cast<ret(WINAPI*) args>(reinterpret_cast<void*>(wglGetProcAddress("gl" #name)));       \
    if (!name) missing += "gl" #name " ";
        FE_GL_FUNCTIONS(FE_GL_LOAD)
#undef FE_GL_LOAD
        return missing.empty();
    }
};

// ---- shaders ---------------------------------------------------------------------
// Positions are native video memory pixels; the viewport does the scaling.
// Video memory row y is texture row y throughout (upside down for OpenGL,
// which only matters when the picture is shown).
const char* const kDrawVs = R"(#version 330
layout(location = 0) in vec3 a_pos;   // x, y, w
layout(location = 1) in vec4 a_col;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in ivec4 a_tex;  // texture page x, y, palette x, y
layout(location = 4) in ivec4 a_win;  // texture window mask x, y, offset x, y
layout(location = 5) in int a_flags;
layout(location = 6) in ivec4 a_box;  // the triangle's texture rectangle (u, v from, u, v to)
out vec4 v_col;
out vec2 v_uv;
flat out ivec4 v_tex;
flat out ivec4 v_win;
flat out int v_flags;
flat out ivec4 v_box;
void main() {
    v_box = a_box;
    // w: perspective (1 = flat, as the console draws)
    gl_Position = vec4((a_pos.x / 512.0 - 1.0) * a_pos.z, (a_pos.y / 256.0 - 1.0) * a_pos.z, 0.0, a_pos.z);
    v_col = a_col;
    v_uv = a_uv;
    v_tex = a_tex;
    v_win = a_win;
    v_flags = a_flags;
}
)";

const char* const kDrawFs = R"(#version 330
uniform usampler2D u_vram;  // video memory at the console's resolution
uniform sampler2D u_hi;     // the picture at the render scale (a copy of it), for 15-bit textures
uniform int u_scale;
uniform int u_pass;         // 0 everything, 1 the opaque texels, 2 the semi-transparent ones
in vec4 v_col;
in vec2 v_uv;
flat in ivec4 v_tex;
flat in ivec4 v_win;
flat in int v_flags;
flat in ivec4 v_box;
out vec4 o;
uint fetch(int x, int y) { return texelFetch(u_vram, ivec2(x & 1023, y & 511), 0).r; }
void main() {
    bool textured = (v_flags & 1) != 0, raw = (v_flags & 2) != 0, semi = (v_flags & 16) != 0, setmask = (v_flags & 32) != 0;
    int depth = (v_flags >> 2) & 3;
    vec3 c;
    float stp = 1.0;
    if (!textured) {
        c = floor(floor(v_col.rgb * 255.0 + 0.5) / 8.0);
    } else {
        vec2 uvc = clamp(v_uv, vec2(0.0), vec2(255.998));
        // placed precisely, the triangle no longer lines up with its texture
        // to the texel: its edges would pick up the texels next to it
        if ((v_flags & 64) != 0) uvc = clamp(uvc, vec2(v_box.xy), max(vec2(v_box.xy), vec2(v_box.zw) - 0.002));
        ivec2 t = ivec2(uvc) & 255;
        t.x = (t.x & ~(v_win.x * 8)) | ((v_win.z & v_win.x) * 8);
        t.y = (t.y & ~(v_win.y * 8)) | ((v_win.w & v_win.y) * 8);
        uint texel;
        if (depth == 0) {
            uint w = fetch(v_tex.x + (t.x >> 2), v_tex.y + t.y);
            texel = fetch(v_tex.z + int((w >> uint((t.x & 3) * 4)) & 15u), v_tex.w);
        } else if (depth == 1) {
            uint w = fetch(v_tex.x + (t.x >> 1), v_tex.y + t.y);
            texel = fetch(v_tex.z + int((w >> uint((t.x & 1) * 8)) & 255u), v_tex.w);
        } else {
            // from the picture at the render scale: pictures drawn or enlarged there keep their detail
            vec2 hp = (vec2(v_tex.x + t.x, v_tex.y + t.y) + fract(uvc)) * float(u_scale);
            vec4 h = texelFetch(u_hi, ivec2(int(hp.x) % (1024 * u_scale), int(hp.y) % (512 * u_scale)), 0);
            uvec3 q = uvec3(floor(h.rgb * 31.0 + 0.5));
            texel = q.r | (q.g << 5) | (q.b << 10) | (h.a > 0.5 ? 0x8000u : 0u);
        }
        if (texel == 0u) discard;
        stp = (texel & 0x8000u) != 0u ? 1.0 : 0.0;
        vec3 t5 = vec3(float(texel & 31u), float((texel >> 5) & 31u), float((texel >> 10) & 31u));
        c = raw ? t5 : min(floor(t5 * floor(v_col.rgb * 255.0 + 0.5) / 128.0), vec3(31.0));
    }
    bool s = semi && stp > 0.5;
    if (u_pass == 1 && s) discard;
    if (u_pass == 2 && !s) discard;
    o = vec4(c / 31.0, setmask ? 1.0 : (textured ? stp : 0.0));
}
)";

// a quad over the whole target; the scissor picks the part drawn
const char* const kQuadVs = R"(#version 330
layout(location = 0) in vec2 a_pos;
out vec2 v_p;
uniform vec4 u_src;  // for showing: the texture rectangle (x, y, w, h, normalised), top row first
void main() {
    gl_Position = vec4(a_pos, 0.0, 1.0);
    v_p = vec2(u_src.x + (a_pos.x * 0.5 + 0.5) * u_src.z, u_src.y + (0.5 - a_pos.y * 0.5) * u_src.w);
}
)";

// pixels uploaded to video memory, drawn into the picture enlarged
const char* const kUploadFs = R"(#version 330
uniform usampler2D u_vram;
uniform int u_scale;
out vec4 o;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy) / u_scale;
    uint w = texelFetch(u_vram, ivec2(p.x & 1023, p.y & 511), 0).r;
    o = vec4(float(w & 31u) / 31.0, float((w >> 5) & 31u) / 31.0, float((w >> 10) & 31u) / 31.0, float(w >> 15));
}
)";

// the picture in the window
const char* const kShowFs = R"(#version 330
uniform sampler2D u_fb;
uniform int u_key;     // black is see-through
uniform vec4 u_inside; // the shown rectangle less half a pixel: the filter does not reach past its edges
in vec2 v_p;
out vec4 o;
void main() {
    vec3 c = texture(u_fb, clamp(v_p, u_inside.xy, u_inside.zw)).rgb;
    if (u_key != 0 && max(c.r, max(c.g, c.b)) < 0.07) discard;
    o = vec4(c, 1.0);
}
)";

struct Vert {
    float x, y, w;
    std::uint8_t r, g, b, a;
    float u, v;
    std::int16_t tex[4];
    std::int16_t win[4];
    std::int32_t flags;
    std::int16_t box[4];
};

struct Cmd {
    GLint first;
    GLsizei count;
    int blend;  // -1 none, else the console's mode
    int pass;
    int sx, sy, sw, sh;  // scissor, native pixels
};

struct Rect {
    int x1 = 1 << 30, y1 = 1 << 30, x2 = -1, y2 = -1;
    bool empty() const { return x2 < x1; }
    void add(int ax1, int ay1, int ax2, int ay2) {
        x1 = std::min(x1, ax1);
        y1 = std::min(y1, ay1);
        x2 = std::max(x2, ax2);
        y2 = std::max(y2, ay2);
    }
    bool meets(int ax1, int ay1, int ax2, int ay2) const { return !empty() && ax1 <= x2 && ax2 >= x1 && ay1 <= y2 && ay2 >= y1; }
};

}  // namespace

struct GlRenderer::Impl {
    fe::Gpu& gpu;
    int S;
    int W, H;
    Gl gl;
    GLuint fbTex = 0, tmpTex = 0, snapTex = 0, mixTex = 0, vramTex = 0;
    GLuint fbFbo = 0, tmpFbo = 0, snapFbo = 0, mixFbo = 0;
    GLuint drawProg = 0, uploadProg = 0, showProg = 0;
    GLint uPass = -1, uDrawScale = -1, uUploadScale = -1, uShowSrc = -1, uShowKey = -1, uUploadSrc = -1, uShowInside = -1;
    GLuint vao = 0, vbo = 0, quadVao = 0, quadVbo = 0;
    std::vector<Vert> verts;
    std::vector<Cmd> cmds;
    Rect drawn, hiRead;    // this batch: what it draws, and where its 15-bit textures are read
    int dirtyY1 = 0, dirtyY2 = 511;  // rows of video memory to send before the next draw
    bool between = false;  // drawing the frame in between (into mixFbo)

    Impl(fe::Gpu& g, int scale) : gpu(g), S(scale), W(1024 * scale), H(512 * scale) {}

    GLuint target() const { return between ? mixFbo : fbFbo; }

    GLuint makeTexture(GLint internal, GLenum format, GLenum type, int w, int h, GLint filter) {
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kClampToEdge);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kClampToEdge);
        glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, nullptr);
        return t;
    }
    GLuint makeFbo(GLuint tex, std::string& err) {
        GLuint f = 0;
        gl.GenFramebuffers(1, &f);
        gl.BindFramebuffer(kFramebuffer, f);
        gl.FramebufferTexture2D(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, tex, 0);
        if (gl.CheckFramebufferStatus(kFramebuffer) != kFramebufferComplete) err = "a framebuffer of " + std::to_string(W) + " x " + std::to_string(H) + " could not be made";
        glViewport(0, 0, W, H);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        gl.BindFramebuffer(kFramebuffer, 0);
        return f;
    }
    GLuint compile(const char* vs, const char* fs, std::string& err) {
        auto shader = [&](GLenum type, const char* src) {
            GLuint s = gl.CreateShader(type);
            gl.ShaderSource(s, 1, &src, nullptr);
            gl.CompileShader(s);
            GLint okv = 0;
            gl.GetShaderiv(s, kCompileStatus, &okv);
            if (!okv) {
                char log[2048] = {};
                gl.GetShaderInfoLog(s, sizeof log, nullptr, log);
                err += std::string("shader: ") + log;
            }
            return s;
        };
        GLuint p = gl.CreateProgram();
        gl.AttachShader(p, shader(kVertexShader, vs));
        gl.AttachShader(p, shader(kFragmentShader, fs));
        gl.LinkProgram(p);
        GLint okv = 0;
        gl.GetProgramiv(p, kLinkStatus, &okv);
        if (!okv) {
            char log[2048] = {};
            gl.GetProgramInfoLog(p, sizeof log, nullptr, log);
            err += std::string("program: ") + log;
        }
        return p;
    }
    void blit(GLuint from, GLuint to, int x, int y, int w, int h, int dx, int dy) {
        gl.BindFramebuffer(kReadFramebuffer, from);
        gl.BindFramebuffer(kDrawFramebuffer, to);
        glDisable(GL_SCISSOR_TEST);
        gl.BlitFramebuffer(x * S, y * S, (x + w) * S, (y + h) * S, dx * S, dy * S, (dx + w) * S, (dy + h) * S, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    void sendVram() {
        if (dirtyY2 < dirtyY1) return;
        glBindTexture(GL_TEXTURE_2D, vramTex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
        glPixelStorei(kUnpackRowLength, 1024);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, dirtyY1, 1024, dirtyY2 - dirtyY1 + 1, kRedInteger, GL_UNSIGNED_SHORT,
                        gpu.vram.data() + static_cast<size_t>(dirtyY1) * 1024);
        glPixelStorei(kUnpackRowLength, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        dirtyY1 = 512;
        dirtyY2 = -1;
    }
    // leaves OpenGL as the window's own drawing expects it
    void restore() {
        gl.UseProgram(0);
        gl.BindVertexArray(0);
        gl.BindFramebuffer(kFramebuffer, 0);
        gl.ActiveTexture(kTexture0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        gl.BlendEquation(kFuncAdd);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    void setBlend(int mode) {
        if (mode < 0) {
            glDisable(GL_BLEND);
            return;
        }
        glEnable(GL_BLEND);
        // the alpha channel keeps the mask bit of the picture
        switch (mode) {
            case 0:  // B/2 + F/2
                gl.BlendEquation(kFuncAdd);
                gl.BlendColor(0, 0, 0, 0.5f);
                gl.BlendFuncSeparate(kConstantAlpha, kConstantAlpha, GL_ONE, GL_ZERO);
                break;
            case 1:  // B + F
                gl.BlendEquation(kFuncAdd);
                gl.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
                break;
            case 2:  // B - F
                gl.BlendEquation(kFuncReverseSubtract);
                gl.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
                break;
            default:  // B + F/4
                gl.BlendEquation(kFuncAdd);
                gl.BlendColor(0, 0, 0, 0.25f);
                gl.BlendFuncSeparate(kConstantAlpha, GL_ONE, GL_ONE, GL_ZERO);
                break;
        }
    }
    void addCmd(GLint first, int blend, int pass, const fe::HwState& s) {
        Cmd c{first, 3, blend, pass, s.areaX1, s.areaY1, s.areaX2 - s.areaX1 + 1, s.areaY2 - s.areaY1 + 1};
        if (!cmds.empty()) {
            Cmd& last = cmds.back();
            if (last.first + last.count == first && last.blend == blend && last.pass == pass && last.sx == c.sx && last.sy == c.sy &&
                last.sw == c.sw && last.sh == c.sh) {
                last.count += 3;
                return;
            }
        }
        cmds.push_back(c);
    }
    void flush() {
        if (cmds.empty()) return;
        sendVram();
        if (!hiRead.empty()) {
            // the 15-bit textures this batch reads, as the picture holds them now
            const int x1 = std::clamp(hiRead.x1, 0, 1023), y1 = std::clamp(hiRead.y1, 0, 511);
            const int x2 = std::clamp(hiRead.x2, 0, 1023), y2 = std::clamp(hiRead.y2, 0, 511);
            blit(fbFbo, tmpFbo, x1, y1, x2 - x1 + 1, y2 - y1 + 1, x1, y1);
        }
        gl.BindFramebuffer(kFramebuffer, target());
        glViewport(0, 0, W, H);
        gl.UseProgram(drawProg);
        gl.ActiveTexture(kTexture0);
        glBindTexture(GL_TEXTURE_2D, vramTex);
        gl.ActiveTexture(kTexture0 + 1);
        glBindTexture(GL_TEXTURE_2D, tmpTex);
        gl.BindVertexArray(vao);
        gl.BindBuffer(kArrayBuffer, vbo);
        gl.BufferData(kArrayBuffer, static_cast<GLsizeiptr>(verts.size() * sizeof(Vert)), verts.data(), kStreamDraw);
        glEnable(GL_SCISSOR_TEST);
        int lastBlend = -2, lastPass = -1;
        for (const Cmd& c : cmds) {
            glScissor(c.sx * S, c.sy * S, std::max(0, c.sw) * S, std::max(0, c.sh) * S);
            if (c.blend != lastBlend) setBlend(lastBlend = c.blend);
            if (c.pass != lastPass) gl.Uniform1i(uPass, lastPass = c.pass);
            glDrawArrays(GL_TRIANGLES, c.first, c.count);
        }
        verts.clear();
        cmds.clear();
        drawn = Rect{};
        hiRead = Rect{};
        restore();
    }
};

GlRenderer::GlRenderer(fe::Gpu& gpu, int scale) : impl_(new Impl(gpu, scale)) {
    Impl& m = *impl_;
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    int major = 0, minor = 0;
    if (!version || std::sscanf(version, "%d.%d", &major, &minor) != 2 || major * 10 + minor < 33) {
        error_ = std::string("OpenGL 3.3 is needed, the graphics driver offers ") + (version ? version : "none");
        return;
    }
    std::string missing;
    if (!m.gl.load(missing)) {
        error_ = "OpenGL functions missing: " + missing;
        return;
    }
    GLint maxSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
    if (m.W > maxSize || m.H > maxSize) {
        error_ = "the graphics card cannot hold a " + std::to_string(m.W) + " x " + std::to_string(m.H) + " picture";
        return;
    }
    std::string err;
    m.fbTex = m.makeTexture(kRgba8, GL_RGBA, GL_UNSIGNED_BYTE, m.W, m.H, GL_LINEAR);
    m.tmpTex = m.makeTexture(kRgba8, GL_RGBA, GL_UNSIGNED_BYTE, m.W, m.H, GL_NEAREST);
    m.vramTex = m.makeTexture(kR16ui, kRedInteger, GL_UNSIGNED_SHORT, 1024, 512, GL_NEAREST);
    m.fbFbo = m.makeFbo(m.fbTex, err);
    m.tmpFbo = m.makeFbo(m.tmpTex, err);
    m.drawProg = m.compile(kDrawVs, kDrawFs, err);
    m.uploadProg = m.compile(kQuadVs, kUploadFs, err);
    m.showProg = m.compile(kQuadVs, kShowFs, err);
    if (!err.empty()) {
        error_ = err;
        m.restore();
        return;
    }
    m.gl.UseProgram(m.drawProg);
    m.gl.Uniform1i(m.gl.GetUniformLocation(m.drawProg, "u_vram"), 0);
    m.gl.Uniform1i(m.gl.GetUniformLocation(m.drawProg, "u_hi"), 1);
    m.uDrawScale = m.gl.GetUniformLocation(m.drawProg, "u_scale");
    m.gl.Uniform1i(m.uDrawScale, scale);
    m.uPass = m.gl.GetUniformLocation(m.drawProg, "u_pass");
    m.gl.UseProgram(m.uploadProg);
    m.gl.Uniform1i(m.gl.GetUniformLocation(m.uploadProg, "u_vram"), 0);
    m.uUploadScale = m.gl.GetUniformLocation(m.uploadProg, "u_scale");
    m.gl.Uniform1i(m.uUploadScale, scale);
    m.uUploadSrc = m.gl.GetUniformLocation(m.uploadProg, "u_src");
    m.gl.UseProgram(m.showProg);
    m.gl.Uniform1i(m.gl.GetUniformLocation(m.showProg, "u_fb"), 0);
    m.uShowSrc = m.gl.GetUniformLocation(m.showProg, "u_src");
    m.uShowKey = m.gl.GetUniformLocation(m.showProg, "u_key");
    m.uShowInside = m.gl.GetUniformLocation(m.showProg, "u_inside");

    // the triangles' vertex layout
    m.gl.GenVertexArrays(1, &m.vao);
    m.gl.GenBuffers(1, &m.vbo);
    m.gl.BindVertexArray(m.vao);
    m.gl.BindBuffer(kArrayBuffer, m.vbo);
    const GLsizei st = sizeof(Vert);
    m.gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(offsetof(Vert, x)));
    m.gl.VertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, reinterpret_cast<void*>(offsetof(Vert, r)));
    m.gl.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(offsetof(Vert, u)));
    m.gl.VertexAttribIPointer(3, 4, GL_SHORT, st, reinterpret_cast<void*>(offsetof(Vert, tex)));
    m.gl.VertexAttribIPointer(4, 4, GL_SHORT, st, reinterpret_cast<void*>(offsetof(Vert, win)));
    m.gl.VertexAttribIPointer(5, 1, GL_INT, st, reinterpret_cast<void*>(offsetof(Vert, flags)));
    m.gl.VertexAttribIPointer(6, 4, GL_SHORT, st, reinterpret_cast<void*>(offsetof(Vert, box)));
    for (GLuint i = 0; i < 7; ++i) m.gl.EnableVertexAttribArray(i);
    // the full-target quad
    const float quad[12] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};
    m.gl.GenVertexArrays(1, &m.quadVao);
    m.gl.GenBuffers(1, &m.quadVbo);
    m.gl.BindVertexArray(m.quadVao);
    m.gl.BindBuffer(kArrayBuffer, m.quadVbo);
    m.gl.BufferData(kArrayBuffer, sizeof quad, quad, 0x88E4 /* GL_STATIC_DRAW */);
    m.gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    m.gl.EnableVertexAttribArray(0);
    m.verts.reserve(1 << 16);
    m.dirtyY1 = 0;
    m.dirtyY2 = 511;
    m.restore();
    ok_ = true;
}

GlRenderer::~GlRenderer() = default;

void GlRenderer::dirty(int y1, int y2) {
    Impl& m = *impl_;
    m.dirtyY1 = std::min(m.dirtyY1, y1);
    m.dirtyY2 = std::max(m.dirtyY2, y2);
}

void GlRenderer::triangle(const fe::HwVertex v[3], const fe::HwState& s) {
    Impl& m = *impl_;
    if (s.areaX2 < s.areaX1 || s.areaY2 < s.areaY1) return;
    float x1 = v[0].x, x2 = v[0].x, y1 = v[0].y, y2 = v[0].y;
    for (int k = 1; k < 3; ++k) {
        x1 = std::min(x1, v[k].x);
        x2 = std::max(x2, v[k].x);
        y1 = std::min(y1, v[k].y);
        y2 = std::max(y2, v[k].y);
    }
    const int bx1 = std::max(static_cast<int>(x1), s.areaX1), by1 = std::max(static_cast<int>(y1), s.areaY1);
    const int bx2 = std::min(static_cast<int>(x2) + 1, s.areaX2), by2 = std::min(static_cast<int>(y2) + 1, s.areaY2);
    if (bx2 < bx1 || by2 < by1) return;
    if (s.textured && s.depth >= 2) {
        // a 15-bit texture: read from the picture, so what this batch has
        // drawn there must be in it first
        const int tx1 = s.tpx, ty1 = s.tpy, tx2 = s.tpx + 255, ty2 = s.tpy + 255;
        if (m.drawn.meets(tx1, ty1, tx2, ty2)) m.flush();
        m.hiRead.add(tx1, ty1, std::min(tx2, 1023), std::min(ty2, 511));
    }
    m.drawn.add(bx1, by1, bx2, by2);
    const int flags = (s.textured ? 1 : 0) | (s.raw ? 2 : 0) | (s.depth << 2) | (s.semi ? 16 : 0) | (s.setMask ? 32 : 0) |
                      (s.precise ? 64 : 0);
    const std::int16_t box[4] = {static_cast<std::int16_t>(std::min({v[0].u, v[1].u, v[2].u})),
                                 static_cast<std::int16_t>(std::min({v[0].v, v[1].v, v[2].v})),
                                 static_cast<std::int16_t>(std::max({v[0].u, v[1].u, v[2].u})),
                                 static_cast<std::int16_t>(std::max({v[0].v, v[1].v, v[2].v}))};
    auto push = [&]() {
        const GLint first = static_cast<GLint>(m.verts.size());
        for (int k = 0; k < 3; ++k) {
            Vert q;
            q.x = v[k].x;
            q.y = v[k].y;
            q.w = v[k].w > 0 ? v[k].w : 1.0f;
            q.r = v[k].r;
            q.g = v[k].g;
            q.b = v[k].b;
            q.a = 255;
            q.u = v[k].u;
            q.v = v[k].v;
            q.tex[0] = static_cast<std::int16_t>(s.tpx);
            q.tex[1] = static_cast<std::int16_t>(s.tpy);
            q.tex[2] = static_cast<std::int16_t>(s.clutX);
            q.tex[3] = static_cast<std::int16_t>(s.clutY);
            q.win[0] = static_cast<std::int16_t>(s.twMaskX);
            q.win[1] = static_cast<std::int16_t>(s.twMaskY);
            q.win[2] = static_cast<std::int16_t>(s.twOffX);
            q.win[3] = static_cast<std::int16_t>(s.twOffY);
            q.flags = flags;
            std::memcpy(q.box, box, sizeof box);
            m.verts.push_back(q);
        }
        return first;
    };
    if (!s.semi) {
        m.addCmd(push(), -1, 0, s);
    } else if (!s.textured) {
        m.addCmd(push(), s.blend, 0, s);
    } else {
        // textured and semi-transparent: the opaque texels, then the others blended
        m.addCmd(push(), -1, 1, s);
        m.addCmd(push(), s.blend, 2, s);
    }
    if (m.verts.size() > (1u << 20)) m.flush();
}

void GlRenderer::fill(int x, int y, int w, int h, fe::u16 colour) {
    Impl& m = *impl_;
    m.flush();
    m.gl.BindFramebuffer(kFramebuffer, m.target());
    glEnable(GL_SCISSOR_TEST);
    glScissor(x * m.S, y * m.S, w * m.S, h * m.S);
    glClearColor((colour & 31) / 31.0f, ((colour >> 5) & 31) / 31.0f, ((colour >> 10) & 31) / 31.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    m.restore();
}

void GlRenderer::copy(int sx, int sy, int dx, int dy, int w, int h) {
    Impl& m = *impl_;
    m.flush();
    m.blit(m.target(), m.tmpFbo, sx, sy, w, h, sx, sy);
    m.blit(m.tmpFbo, m.target(), sx, sy, w, h, dx, dy);
    m.restore();
}

void GlRenderer::upload(int x, int y, int w, int h) {
    Impl& m = *impl_;
    m.flush();
    m.sendVram();
    m.gl.BindFramebuffer(kFramebuffer, m.target());
    glViewport(0, 0, m.W, m.H);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x * m.S, y * m.S, w * m.S, h * m.S);
    m.gl.UseProgram(m.uploadProg);
    m.gl.ActiveTexture(kTexture0);
    glBindTexture(GL_TEXTURE_2D, m.vramTex);
    m.gl.BindVertexArray(m.quadVao);
    m.gl.Uniform4f(m.uUploadSrc, 0, 0, 1, 1);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    m.restore();
}

void GlRenderer::uploadHires(int x, int y, int w, int h, const fe::u16* pixels, size_t stride) {
    Impl& m = *impl_;
    m.flush();
    // the console's colour layout (red in the low bits, bit 15 the mask) is
    // OpenGL's RGBA 1-5-5-5 reversed
    glBindTexture(GL_TEXTURE_2D, m.fbTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glPixelStorei(kUnpackRowLength, static_cast<GLint>(stride));
    glTexSubImage2D(GL_TEXTURE_2D, 0, x * m.S, y * m.S, w * m.S, h * m.S, GL_RGBA, kUnsignedShort1555Rev, pixels);
    glPixelStorei(kUnpackRowLength, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void GlRenderer::snapshot(int x1, int y1, int x2, int y2) {
    Impl& m = *impl_;
    m.flush();
    if (!m.snapTex) {
        std::string err;
        m.snapTex = m.makeTexture(kRgba8, GL_RGBA, GL_UNSIGNED_BYTE, m.W, m.H, GL_NEAREST);
        m.snapFbo = m.makeFbo(m.snapTex, err);
        m.mixTex = m.makeTexture(kRgba8, GL_RGBA, GL_UNSIGNED_BYTE, m.W, m.H, GL_LINEAR);
        m.mixFbo = m.makeFbo(m.mixTex, err);
    }
    m.blit(m.fbFbo, m.snapFbo, x1, y1, x2 - x1 + 1, y2 - y1 + 1, x1, y1);
    m.restore();
}

void GlRenderer::beginBetween(int x1, int y1, int x2, int y2) {
    Impl& m = *impl_;
    m.flush();
    if (!m.mixFbo) return;
    m.blit(m.snapFbo, m.mixFbo, x1, y1, x2 - x1 + 1, y2 - y1 + 1, x1, y1);
    m.restore();
    m.between = true;
}

void GlRenderer::endBetween() {
    Impl& m = *impl_;
    m.flush();
    m.between = false;
}

void GlRenderer::flush() { impl_->flush(); }

void GlRenderer::show(bool between, int x, int y, int w, int h, int vx, int vy, int vw, int vh, bool key) {
    Impl& m = *impl_;
    m.flush();
    m.gl.BindFramebuffer(kFramebuffer, 0);
    glViewport(vx, vy, vw, vh);
    glDisable(GL_SCISSOR_TEST);
    m.gl.UseProgram(m.showProg);
    m.gl.ActiveTexture(kTexture0);
    glBindTexture(GL_TEXTURE_2D, between && m.mixTex ? m.mixTex : m.fbTex);
    m.gl.Uniform4f(m.uShowSrc, x / 1024.0f, y / 512.0f, w / 1024.0f, h / 512.0f);
    const float hx = 0.5f / m.W, hy = 0.5f / m.H;
    m.gl.Uniform4f(m.uShowInside, x / 1024.0f + hx, y / 512.0f + hy, (x + w) / 1024.0f - hx, (y + h) / 512.0f - hy);
    m.gl.Uniform1i(m.uShowKey, key ? 1 : 0);
    m.gl.BindVertexArray(m.quadVao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    m.restore();
}

void GlRenderer::read(bool between, int x, int y, int w, int h, std::vector<std::uint8_t>& rgb, int& outW, int& outH) {
    Impl& m = *impl_;
    m.flush();
    outW = w * m.S;
    outH = h * m.S;
    rgb.resize(static_cast<size_t>(outW) * outH * 3);
    m.gl.BindFramebuffer(kReadFramebuffer, between && m.mixFbo ? m.mixFbo : m.fbFbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x * m.S, y * m.S, outW, outH, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());  // video memory row order: top first
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    m.restore();
}

}  // namespace app
