// The OpenGL ES test application for Linux: an OpenGL ES 3.2 context of EGL's drawing the scene of
// test/gles_scene/scene.h into a pbuffer, so it runs with no window (and over ssh) as well as on a
// desktop. What the OpenGL ES plugin has to catch on Linux is how the application reaches EGL:
//
//     gles_linux                 EGL linked, as an application built against libEGL is
//     gles_linux --dlopen        EGL loaded with dlopen("libEGL.so.1") and its entry points taken
//                                with dlsym, as SDL, GLFW and most engines do
//     gles_linux --frames=N      stop after N frames (default: run until killed)
//
// Frames end at eglSwapBuffers, about 60 a second.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <thread>
#include <unistd.h>

#define GLES_SCENE_LOG(...) (printf(__VA_ARGS__), printf("\n"), fflush(stdout))
#include "../gles_scene/scene.h"

namespace {

/** The EGL entry points the application uses, from the linked library or from dlsym. */
struct Egl {
    decltype(&eglGetDisplay) GetDisplay;
    decltype(&eglInitialize) Initialize;
    decltype(&eglBindAPI) BindAPI;
    decltype(&eglChooseConfig) ChooseConfig;
    decltype(&eglCreateContext) CreateContext;
    decltype(&eglCreatePbufferSurface) CreatePbufferSurface;
    decltype(&eglMakeCurrent) MakeCurrent;
    decltype(&eglSwapBuffers) SwapBuffers;
    decltype(&eglGetProcAddress) GetProcAddress;
    decltype(&eglGetError) GetError;
};

bool LoadEgl(bool viaDlopen, Egl& e) {
    if (!viaDlopen) {
        e = {eglGetDisplay, eglInitialize, eglBindAPI, eglChooseConfig, eglCreateContext, eglCreatePbufferSurface,
             eglMakeCurrent, eglSwapBuffers, eglGetProcAddress, eglGetError};
        return true;
    }
    void* lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        printf("dlopen(libEGL.so.1): %s\n", dlerror());
        return false;
    }
    e.GetDisplay = (decltype(&eglGetDisplay))dlsym(lib, "eglGetDisplay");
    e.Initialize = (decltype(&eglInitialize))dlsym(lib, "eglInitialize");
    e.BindAPI = (decltype(&eglBindAPI))dlsym(lib, "eglBindAPI");
    e.ChooseConfig = (decltype(&eglChooseConfig))dlsym(lib, "eglChooseConfig");
    e.CreateContext = (decltype(&eglCreateContext))dlsym(lib, "eglCreateContext");
    e.CreatePbufferSurface = (decltype(&eglCreatePbufferSurface))dlsym(lib, "eglCreatePbufferSurface");
    e.MakeCurrent = (decltype(&eglMakeCurrent))dlsym(lib, "eglMakeCurrent");
    e.SwapBuffers = (decltype(&eglSwapBuffers))dlsym(lib, "eglSwapBuffers");
    e.GetProcAddress = (decltype(&eglGetProcAddress))dlsym(lib, "eglGetProcAddress");
    e.GetError = (decltype(&eglGetError))dlsym(lib, "eglGetError");
    return e.GetDisplay && e.Initialize && e.BindAPI && e.ChooseConfig && e.CreateContext && e.CreatePbufferSurface
        && e.MakeCurrent && e.SwapBuffers && e.GetProcAddress && e.GetError;
}

/** The desktop's display, or with none (a console, ssh) Mesa's surfaceless platform. */
EGLDisplay OpenDisplay(const Egl& e) {
    EGLDisplay display = e.GetDisplay(EGL_DEFAULT_DISPLAY);
    if (display != EGL_NO_DISPLAY && e.Initialize(display, nullptr, nullptr)) return display;
    auto platform = (PFNEGLGETPLATFORMDISPLAYEXTPROC)e.GetProcAddress("eglGetPlatformDisplayEXT");
    constexpr EGLenum kSurfaceless = 0x31DD;   // EGL_PLATFORM_SURFACELESS_MESA
    display = platform ? platform(kSurfaceless, EGL_DEFAULT_DISPLAY, nullptr) : EGL_NO_DISPLAY;
    if (display != EGL_NO_DISPLAY && e.Initialize(display, nullptr, nullptr)) return display;
    return EGL_NO_DISPLAY;
}

}  // namespace

int main(int argc, char** argv) {
    bool viaDlopen = false;
    long frames = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--dlopen") == 0) viaDlopen = true;
        else if (strncmp(argv[i], "--frames=", 9) == 0) frames = strtol(argv[i] + 9, nullptr, 10);
    }
    Egl egl{};
    if (!LoadEgl(viaDlopen, egl)) return 1;
    EGLDisplay display = OpenDisplay(egl);
    if (display == EGL_NO_DISPLAY) {
        printf("no EGL display (error 0x%x)\n", egl.GetError());
        return 1;
    }
    egl.BindAPI(EGL_OPENGL_ES_API);
    const EGLint attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                              EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint n = 0;
    if (!egl.ChooseConfig(display, attribs, &config, 1, &n) || n < 1) {
        printf("no OpenGL ES 3 pbuffer config\n");
        return 1;
    }
    EGLContext context = EGL_NO_CONTEXT;
    for (int minor : {2, 0}) {
        const EGLint ctx[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, minor, EGL_NONE};
        context = egl.CreateContext(display, config, EGL_NO_CONTEXT, ctx);
        if (context != EGL_NO_CONTEXT) break;
    }
    if (context == EGL_NO_CONTEXT) {
        printf("no OpenGL ES 3 context (error 0x%x)\n", egl.GetError());
        return 1;
    }
    const int width = 640, height = 360;
    const EGLint surfaceAttribs[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
    EGLSurface surface = egl.CreatePbufferSurface(display, config, surfaceAttribs);
    if (surface == EGL_NO_SURFACE || !egl.MakeCurrent(display, surface, surface, context)) {
        printf("no pbuffer surface (error 0x%x)\n", egl.GetError());
        return 1;
    }
    printf("EGL %s; pid %d\n", viaDlopen ? "through dlopen" : "linked", (int)getpid());

    gles_scene::Scene scene;
    scene.Create();
    const auto start = std::chrono::steady_clock::now();
    for (long frame = 0; !frames || frame < frames; ++frame) {
        const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
        scene.Render(t, width, height);
        egl.SwapBuffers(display, surface);
        if (frame % 600 == 0) printf("frame %ld\n", frame), fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    return 0;
}
