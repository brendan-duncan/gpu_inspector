// A small OpenGL ES 3.0 application on ANGLE, for the OpenGL ES plugin (src/plugins/gles): the
// counterpart of test/triangle and test/d3d12_triangle. Each frame it
//   - renders a rotating textured cube into a framebuffer object (an RGBA8 texture and a depth
//     renderbuffer, multisampled with --msaa and resolved with a blit): an indexed draw from a vertex
//     array object, with a std140 uniform block; then invalidates the depth it no longer needs;
//   - draws the default framebuffer: the offscreen image on a quad sampled from that texture, and a
//     small triangle from client-side vertex arrays.
// So a capture has two passes, clears, indexed and non-indexed draws, vertex data in buffers and in
// client memory, a sampled render target, and a compressed texture when the driver takes BC1 (ANGLE
// on D3D11 does).
//
// ANGLE is not part of Windows: libEGL.dll and libGLESv2.dll are loaded from beside the executable,
// where the build copies them from ANGLE_DIR (CMakeLists.txt), or from the directory --angle names.
//
// Usage: glesinsp_triangle [--frames N] [--width W] [--height H] [--msaa] [--angle <dir>] [--capture-at N]
//   --capture-at N asks the inspector for a capture at frame N itself (include/gpu_inspector.h).
#include <windows.h>

#include "../../src/plugins/gles/gen/gles_api.gen.h"
#include "../../src/plugins/gles/gen/gles_constants.gen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gpu_inspector.h"   // --capture-at: the application asking for the capture itself

// ------------------------------------------------------------------------------------------------
// EGL, the few entry points this uses.

typedef void* EGLDisplay;
typedef void* EGLConfig;
typedef void* EGLSurface;
typedef void* EGLContext;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean;
typedef void (*EGLProc)(void);

#define EGL_NONE 0x3038
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_DEPTH_SIZE 0x3025
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_OPENGL_ES3_BIT 0x0040
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_SURFACE_TYPE 0x3033
#define EGL_WINDOW_BIT 0x0004

#define EGLAPI_CALL __stdcall
typedef EGLDisplay(EGLAPI_CALL* PFN_eglGetDisplay)(void*);
typedef EGLBoolean(EGLAPI_CALL* PFN_eglInitialize)(EGLDisplay, EGLint*, EGLint*);
typedef EGLBoolean(EGLAPI_CALL* PFN_eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
typedef EGLSurface(EGLAPI_CALL* PFN_eglCreateWindowSurface)(EGLDisplay, EGLConfig, void*, const EGLint*);
typedef EGLContext(EGLAPI_CALL* PFN_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
typedef EGLBoolean(EGLAPI_CALL* PFN_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLBoolean(EGLAPI_CALL* PFN_eglSwapBuffers)(EGLDisplay, EGLSurface);
typedef EGLBoolean(EGLAPI_CALL* PFN_eglSwapInterval)(EGLDisplay, EGLint);
typedef EGLBoolean(EGLAPI_CALL* PFN_eglDestroyContext)(EGLDisplay, EGLContext);
typedef EGLBoolean(EGLAPI_CALL* PFN_eglDestroySurface)(EGLDisplay, EGLSurface);
typedef EGLBoolean(EGLAPI_CALL* PFN_eglTerminate)(EGLDisplay);
typedef EGLint(EGLAPI_CALL* PFN_eglGetError)(void);
typedef EGLProc(EGLAPI_CALL* PFN_eglGetProcAddress)(const char*);

static PFN_eglGetDisplay eglGetDisplay;
static PFN_eglInitialize eglInitialize;
static PFN_eglChooseConfig eglChooseConfig;
static PFN_eglCreateWindowSurface eglCreateWindowSurface;
static PFN_eglCreateContext eglCreateContext;
static PFN_eglMakeCurrent eglMakeCurrent;
static PFN_eglSwapBuffers eglSwapBuffers;
static PFN_eglSwapInterval eglSwapInterval;
static PFN_eglDestroyContext eglDestroyContext;
static PFN_eglDestroySurface eglDestroySurface;
static PFN_eglTerminate eglTerminate;
static PFN_eglGetError eglGetError;
static PFN_eglGetProcAddress eglGetProcAddress;

// ------------------------------------------------------------------------------------------------
// GL, fetched through eglGetProcAddress (and the library's exports) the way applications do.

// clang-format off
#define GL_FUNCTIONS(X) \
    X(glGetString) X(glViewport) X(glClearColor) X(glClear) X(glEnable) X(glDisable) X(glDepthFunc) \
    X(glCreateShader) X(glShaderSource) X(glCompileShader) X(glGetShaderiv) X(glGetShaderInfoLog) \
    X(glCreateProgram) X(glAttachShader) X(glLinkProgram) X(glGetProgramiv) X(glGetProgramInfoLog) \
    X(glDeleteShader) X(glUseProgram) X(glGetUniformLocation) X(glUniform1i) X(glUniform4f) \
    X(glGetUniformBlockIndex) X(glUniformBlockBinding) X(glGenBuffers) X(glBindBuffer) X(glBufferData) \
    X(glBufferSubData) X(glBindBufferBase) X(glGenVertexArrays) X(glBindVertexArray) \
    X(glEnableVertexAttribArray) X(glDisableVertexAttribArray) X(glVertexAttribPointer) \
    X(glGenTextures) X(glBindTexture) X(glTexImage2D) X(glTexParameteri) X(glActiveTexture) \
    X(glCompressedTexImage2D) X(glGenFramebuffers) X(glBindFramebuffer) X(glFramebufferTexture2D) \
    X(glFramebufferRenderbuffer) X(glCheckFramebufferStatus) X(glGenRenderbuffers) X(glBindRenderbuffer) \
    X(glRenderbufferStorage) X(glRenderbufferStorageMultisample) X(glBlitFramebuffer) \
    X(glInvalidateFramebuffer) X(glDrawArrays) X(glDrawElements) X(glGetError) X(glObjectLabel) \
    X(glPushDebugGroup) X(glPopDebugGroup) X(glGetIntegerv) X(glDrawBuffers) X(glReadBuffer)
// clang-format on

#define DECLARE(name) static PFN_##name name;
GL_FUNCTIONS(DECLARE)
#undef DECLARE

static HMODULE g_gles = nullptr;

static void* Proc(const char* name)
{
    void* p = (void*)eglGetProcAddress(name);
    if (!p && g_gles)
        p = (void*)GetProcAddress(g_gles, name);
    return p;
}

// ------------------------------------------------------------------------------------------------

static void Fail(const char* what)
{
    fprintf(stderr, "glesinsp_triangle: %s\n", what);
    exit(1);
}

static GLuint Shader(GLenum type, const char* source)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        fprintf(stderr, "shader: %s\n", log);
        exit(1);
    }
    return s;
}

static GLuint Program(const char* vs, const char* fs, const char* label)
{
    GLuint v = Shader(GL_VERTEX_SHADER, vs);
    GLuint f = Shader(GL_FRAGMENT_SHADER, fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[2048];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        fprintf(stderr, "program: %s\n", log);
        exit(1);
    }
    // Like most applications: the shaders go as soon as the program is linked.
    glDeleteShader(v);
    glDeleteShader(f);
    if (glObjectLabel)
        glObjectLabel(GL_PROGRAM, p, -1, label);
    return p;
}

static const char* kCubeVs = R"(#version 300 es
layout(std140) uniform Transform {
    mat4 mvp;
    vec4 tint;
    float time;
};
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
out vec2 vUv;
out vec4 vTint;
void main() {
    vUv = uv;
    vTint = tint * (0.75 + 0.25 * sin(time));
    gl_Position = mvp * vec4(position, 1.0);
}
)";

static const char* kCubeFs = R"(#version 300 es
precision mediump float;
uniform sampler2D checker;
uniform sampler2D detail;
in vec2 vUv;
in vec4 vTint;
out vec4 color;
void main() {
    color = texture(checker, vUv) * vTint + 0.25 * texture(detail, vUv * 4.0);
}
)";

static const char* kQuadVs = R"(#version 300 es
layout(location = 0) in vec2 position;
out vec2 vUv;
void main() {
    vUv = position * 0.5 + 0.5;
    gl_Position = vec4(position * 0.8, 0.0, 1.0);
}
)";

static const char* kQuadFs = R"(#version 300 es
precision mediump float;
uniform sampler2D image;
uniform vec4 border;
in vec2 vUv;
out vec4 color;
void main() {
    vec2 d = abs(vUv - 0.5);
    color = max(d.x, d.y) > 0.49 ? border : texture(image, vUv);
}
)";

static const char* kFlatVs = R"(#version 300 es
layout(location = 0) in vec2 position;
layout(location = 1) in vec3 rgb;
out vec3 vColor;
void main() {
    vColor = rgb;
    gl_Position = vec4(position, 0.0, 1.0);
}
)";

static const char* kFlatFs = R"(#version 300 es
precision mediump float;
in vec3 vColor;
out vec4 color;
void main() { color = vec4(vColor, 1.0); }
)";

struct Mat4
{
    float m[16];
};

static Mat4 Multiply(const Mat4& a, const Mat4& b)
{
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k)
                r.m[c * 4 + row] += a.m[k * 4 + row] * b.m[c * 4 + k];
    return r;
}

static Mat4 Perspective(float fovy, float aspect, float n, float f)
{
    const float t = 1.0f / tanf(fovy / 2);
    Mat4 r{};
    r.m[0] = t / aspect;
    r.m[5] = t;
    r.m[10] = (f + n) / (n - f);
    r.m[11] = -1;
    r.m[14] = 2 * f * n / (n - f);
    return r;
}

static Mat4 Rotation(float a, float b)
{
    const float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
    Mat4 y{{ca, 0, -sa, 0, 0, 1, 0, 0, sa, 0, ca, 0, 0, 0, 0, 1}};
    Mat4 x{{1, 0, 0, 0, 0, cb, sb, 0, 0, -sb, cb, 0, 0, 0, 0, 1}};
    Mat4 r = Multiply(x, y);
    r.m[14] = -3.0f;
    return r;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(int argc, char** argv)
{
    int frames = 0;
    int captureAt = 0;   // --capture-at: ask the inspector for a capture at this frame (gpu_inspector.h)
    bool captureAsked = false;
    int width = 800, height = 600;
    bool msaa = false;
    std::string angle;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc)
            frames = atoi(argv[++i]);
        else if (a == "--capture-at" && i + 1 < argc)
            captureAt = atoi(argv[++i]);
        else if (a == "--width" && i + 1 < argc)
            width = atoi(argv[++i]);
        else if (a == "--height" && i + 1 < argc)
            height = atoi(argv[++i]);
        else if (a == "--msaa")
            msaa = true;
        else if (a == "--angle" && i + 1 < argc)
            angle = argv[++i];
    }

    // ANGLE: beside the executable, or where --angle says.
    std::string dir = angle;
    if (dir.empty())
    {
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        dir = exe;
        dir = dir.substr(0, dir.find_last_of("\\/"));
    }
    HMODULE egl = LoadLibraryA((dir + "\\libEGL.dll").c_str());
    g_gles = LoadLibraryA((dir + "\\libGLESv2.dll").c_str());
    if (!egl || !g_gles)
        Fail(("ANGLE (libEGL.dll and libGLESv2.dll) not found in " + dir + ": build with ANGLE_DIR set, or pass --angle <dir>").c_str());
#define EGL_LOAD(name) name = (PFN_##name)GetProcAddress(egl, #name);
    // clang-format off
    EGL_LOAD(eglGetDisplay) EGL_LOAD(eglInitialize) EGL_LOAD(eglChooseConfig) EGL_LOAD(eglCreateWindowSurface)
    EGL_LOAD(eglCreateContext) EGL_LOAD(eglMakeCurrent) EGL_LOAD(eglSwapBuffers) EGL_LOAD(eglSwapInterval)
    EGL_LOAD(eglDestroyContext) EGL_LOAD(eglDestroySurface) EGL_LOAD(eglTerminate) EGL_LOAD(eglGetError)
    EGL_LOAD(eglGetProcAddress)
    // clang-format on
#undef EGL_LOAD
                    if (!eglGetDisplay || !eglGetProcAddress) Fail("libEGL.dll lacks the EGL entry points");

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"glesinsp_triangle";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT r{0, 0, width, height};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"glesinsp_triangle (OpenGL ES on ANGLE)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);

    EGLDisplay display = eglGetDisplay(GetDC(hwnd));
    EGLint major = 0, minor = 0;
    if (!eglInitialize(display, &major, &minor))
        Fail("eglInitialize failed");
    const EGLint configAttribs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(display, configAttribs, &config, 1, &count) || count == 0)
        Fail("no ES 3 config");
    EGLSurface surface = eglCreateWindowSurface(display, config, hwnd, nullptr);
    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, nullptr, contextAttribs);
    if (!surface || !context)
        Fail("the surface or the context could not be made");
    eglMakeCurrent(display, surface, surface, context);
    eglSwapInterval(display, 1);

#define LOAD_GL(name) name = (PFN_##name)Proc(#name);
    GL_FUNCTIONS(LOAD_GL)
#undef LOAD_GL
    // KHR_debug's labels and groups are core in ES 3.2 only; an ES 3.0 context takes them by their
    // extension names, when the driver has the extension.
    {
        const std::string ext = (const char*)glGetString(GL_EXTENSIONS);
        const bool khrDebug = ext.find("GL_KHR_debug") != std::string::npos;
        glObjectLabel = khrDebug ? (PFN_glObjectLabel)Proc("glObjectLabelKHR") : nullptr;
        glPushDebugGroup = khrDebug ? (PFN_glPushDebugGroup)Proc("glPushDebugGroupKHR") : nullptr;
        glPopDebugGroup = khrDebug ? (PFN_glPopDebugGroup)Proc("glPopDebugGroupKHR") : nullptr;
    }
    printf("EGL %d.%d, %s, %s\n", major, minor, (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));
    fflush(stdout);

    // ---- Resources.
    GLuint cubeProgram = Program(kCubeVs, kCubeFs, "cube");
    GLuint quadProgram = Program(kQuadVs, kQuadFs, "present");
    GLuint flatProgram = Program(kFlatVs, kFlatFs, "flat");

    const float cube[] = {
        // position, uv: six faces of two triangles each, drawn with an index buffer.
        -1,
        -1,
        1,
        0,
        0,
        1,
        -1,
        1,
        1,
        0,
        1,
        1,
        1,
        1,
        1,
        -1,
        1,
        1,
        0,
        1,
        1,
        -1,
        -1,
        0,
        0,
        -1,
        -1,
        -1,
        1,
        0,
        -1,
        1,
        -1,
        1,
        1,
        1,
        1,
        -1,
        0,
        1,
        -1,
        -1,
        -1,
        0,
        0,
        -1,
        -1,
        1,
        1,
        0,
        -1,
        1,
        1,
        1,
        1,
        -1,
        1,
        -1,
        0,
        1,
        1,
        -1,
        1,
        0,
        0,
        1,
        -1,
        -1,
        1,
        0,
        1,
        1,
        -1,
        1,
        1,
        1,
        1,
        1,
        0,
        1,
        -1,
        1,
        1,
        0,
        0,
        1,
        1,
        1,
        1,
        0,
        1,
        1,
        -1,
        1,
        1,
        -1,
        1,
        -1,
        0,
        1,
        -1,
        -1,
        -1,
        0,
        0,
        1,
        -1,
        -1,
        1,
        0,
        1,
        -1,
        1,
        1,
        1,
        -1,
        -1,
        1,
        0,
        1,
    };
    std::vector<uint16_t> indices;
    for (uint16_t f = 0; f < 6; ++f)
    {
        const uint16_t b = f * 4;
        indices.insert(indices.end(), {b, (uint16_t)(b + 1), (uint16_t)(b + 2), b, (uint16_t)(b + 2), (uint16_t)(b + 3)});
    }
    GLuint cubeVao = 0, cubeVbo = 0, cubeIbo = 0, ubo = 0;
    glGenVertexArrays(1, &cubeVao);
    glBindVertexArray(cubeVao);
    glGenBuffers(1, &cubeVbo);
    glBindBuffer(GL_ARRAY_BUFFER, cubeVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(cube), cube, GL_STATIC_DRAW);
    glGenBuffers(1, &cubeIbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cubeIbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint16_t), indices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, (const void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, (const void*)12);
    glBindVertexArray(0);
    if (glObjectLabel)
    {
        glObjectLabel(GL_BUFFER, cubeVbo, -1, "cube vertices");
        glObjectLabel(GL_BUFFER, cubeIbo, -1, "cube indices");
    }
    glGenBuffers(1, &ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo);
    glBufferData(GL_UNIFORM_BUFFER, 96, nullptr, GL_DYNAMIC_DRAW);
    glUniformBlockBinding(cubeProgram, glGetUniformBlockIndex(cubeProgram, "Transform"), 2);
    if (glObjectLabel)
        glObjectLabel(GL_BUFFER, ubo, -1, "transform");

    const float quad[] = {-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1};
    GLuint quadVao = 0, quadVbo = 0;
    glGenVertexArrays(1, &quadVao);
    glBindVertexArray(quadVao);
    glGenBuffers(1, &quadVbo);
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);

    // A checkerboard, and a BC1 texture when the driver takes one.
    std::vector<uint8_t> checker(64 * 64 * 4);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
        {
            const bool on = ((x / 8) ^ (y / 8)) & 1;
            uint8_t* p = &checker[(y * 64 + x) * 4];
            p[0] = on ? 240 : 40;
            p[1] = on ? 200 : 60;
            p[2] = on ? 80 : 160;
            p[3] = 255;
        }
    GLuint checkerTex = 0, detailTex = 0;
    glGenTextures(1, &checkerTex);
    glBindTexture(GL_TEXTURE_2D, checkerTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, checker.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (glObjectLabel)
        glObjectLabel(GL_TEXTURE, checkerTex, -1, "checker");
    glGenTextures(1, &detailTex);
    glBindTexture(GL_TEXTURE_2D, detailTex);
    const std::string extensions = (const char*)glGetString(GL_EXTENSIONS);
    if (extensions.find("GL_EXT_texture_compression_dxt1") != std::string::npos ||
        extensions.find("GL_EXT_texture_compression_s3tc") != std::string::npos)
    {
        // 16x16 of BC1 blocks: each block red to blue, alternating which end is which.
        std::vector<uint8_t> blocks(4 * 4 * 8);
        for (int b = 0; b < 16; ++b)
        {
            uint8_t* p = &blocks[b * 8];
            const uint16_t red = 0xF800, blue = 0x001F;
            const uint16_t c0 = (b & 1) ? red : blue, c1 = (b & 1) ? blue : red;
            p[0] = c0 & 0xFF;
            p[1] = c0 >> 8;
            p[2] = c1 & 0xFF;
            p[3] = c1 >> 8;
            p[4] = 0x00;
            p[5] = 0x55;
            p[6] = 0xAA;
            p[7] = 0xFF;
        }
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, 0x83F0, 16, 16, 0, (GLsizei)blocks.size(), blocks.data());
        if (glObjectLabel)
            glObjectLabel(GL_TEXTURE, detailTex, -1, "detail (BC1)");
    }
    else
    {
        const uint8_t gray[4] = {128, 128, 128, 255};
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, gray);
        if (glObjectLabel)
            glObjectLabel(GL_TEXTURE, detailTex, -1, "detail");
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    // The offscreen target: a color texture and a depth renderbuffer (multisampled, and resolved into
    // the texture, with --msaa).
    const int ow = 256, oh = 256;
    GLuint colorTex = 0, depthRb = 0, fbo = 0, msaaColor = 0, msaaFbo = 0;
    glGenTextures(1, &colorTex);
    glBindTexture(GL_TEXTURE_2D, colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ow, oh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    if (glObjectLabel)
        glObjectLabel(GL_TEXTURE, colorTex, -1, "offscreen color");
    glGenRenderbuffers(1, &depthRb);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRb);
    if (msaa)
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_DEPTH_COMPONENT24, ow, oh);
    else
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, ow, oh);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
    if (!msaa)
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRb);
    if (glObjectLabel)
        glObjectLabel(GL_FRAMEBUFFER, fbo, -1, "offscreen");
    if (msaa)
    {
        glGenRenderbuffers(1, &msaaColor);
        glBindRenderbuffer(GL_RENDERBUFFER, msaaColor);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_RGBA8, ow, oh);
        glGenFramebuffers(1, &msaaFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, msaaFbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msaaColor);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRb);
        if (glObjectLabel)
            glObjectLabel(GL_FRAMEBUFFER, msaaFbo, -1, "offscreen (4x MSAA)");
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        Fail("the offscreen framebuffer is incomplete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Client-side vertices, which ES still allows outside a vertex array object.
    const float triangle[] = {-0.95f, -0.95f, 1, 0, 0, -0.75f, -0.95f, 0, 1, 0, -0.85f, -0.75f, 0, 0, 1};

    const GLint checkerLoc = glGetUniformLocation(cubeProgram, "checker");
    const GLint detailLoc = glGetUniformLocation(cubeProgram, "detail");
    const GLint imageLoc = glGetUniformLocation(quadProgram, "image");
    const GLint borderLoc = glGetUniformLocation(quadProgram, "border");

    MSG msg{};
    int frame = 0;
    bool running = true;
    while (running)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running)
            break;
        RECT client;
        GetClientRect(hwnd, &client);
        const int w = std::max<int>(1, client.right), h = std::max<int>(1, client.bottom);
        const float t = frame / 60.0f;

        // Pass 1: the cube, offscreen.
        if (glPushDebugGroup)
            glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 1, -1, "offscreen cube");
        glBindFramebuffer(GL_FRAMEBUFFER, msaa ? msaaFbo : fbo);
        glViewport(0, 0, ow, oh);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glClearColor(0.1f, 0.12f, 0.2f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        struct
        {
            Mat4 mvp;
            float tint[4];
            float time;
            float pad[3];
        } transform{Multiply(Perspective(1.0f, 1.0f, 0.1f, 10.0f), Rotation(t, t * 0.7f)), {1, 1, 1, 1}, t, {}};
        glBindBuffer(GL_UNIFORM_BUFFER, ubo);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(transform), &transform);
        glBindBufferBase(GL_UNIFORM_BUFFER, 2, ubo);
        glUseProgram(cubeProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, checkerTex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, detailTex);
        glUniform1i(checkerLoc, 0);
        glUniform1i(detailLoc, 1);
        glBindVertexArray(cubeVao);
        glDrawElements(GL_TRIANGLES, (GLsizei)indices.size(), GL_UNSIGNED_SHORT, nullptr);
        glBindVertexArray(0);
        if (msaa)
        {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, msaaFbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
            glBlitFramebuffer(0, 0, ow, oh, 0, 0, ow, oh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        // The depth is not needed after the pass: telling the driver lets a tiled GPU skip storing it.
        const GLenum discard[] = {GL_DEPTH_ATTACHMENT};
        glBindFramebuffer(GL_FRAMEBUFFER, msaa ? msaaFbo : fbo);
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, discard);
        if (glPopDebugGroup)
            glPopDebugGroup();

        // Pass 2: the window.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);
        glClearColor(0.05f, 0.05f, 0.05f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(quadProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, colorTex);
        glUniform1i(imageLoc, 0);
        glUniform4f(borderLoc, 0.9f, 0.6f, 0.1f, 1);
        glBindVertexArray(quadVao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
        glUseProgram(flatProgram);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 20, triangle);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 20, triangle + 2);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glDisableVertexAttribArray(0);
        glDisableVertexAttribArray(1);

        eglSwapBuffers(display, surface);
        if (const GLenum e = glGetError())
            fprintf(stderr, "frame %d: GL error 0x%X\n", frame, e);
        // Asked again each frame until somebody is there to hear it: the inspector connects a
        // few frames after the context is made.
        if (captureAt > 0 && frame >= captureAt && !captureAsked)
        {
            char label[48];
            snprintf(label, sizeof label, "asked at frame %d", captureAt);   // the tab's name
            captureAsked = gpu_inspector_capture_named(1, label) != 0;
        }
        ++frame;
        if (frames && frame >= frames)
            break;
    }
    eglMakeCurrent(display, nullptr, nullptr, nullptr);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
    return 0;
}
