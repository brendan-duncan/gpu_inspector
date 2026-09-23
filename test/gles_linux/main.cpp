// The OpenGL ES test application for Linux: an OpenGL ES 3.2 context drawing the scene of
// test/gles_scene/scene.h, either in a window of SDL's or into a pbuffer with no window (so it runs
// over ssh and on a console as well as on a desktop). What the OpenGL ES plugin has to catch on
// Linux is how the application reaches EGL, and there is a mode for each way:
//
//     gles_linux                 a window, through SDL, which loads the driver itself as an engine
//                                does; offscreen instead if SDL is not built in or there is no
//                                display to open
//     gles_linux --window        the same, but fail rather than fall back if there is no window
//     gles_linux --window-egl    a window, forcing SDL onto EGL where it would take GLX (X11).
//                                Not every driver's X11 EGL manages SDL's window surface
//     gles_linux --pbuffer       offscreen, EGL linked, as an application built against libEGL is
//     gles_linux --dlopen        offscreen, EGL loaded with dlopen("libEGL.so.1") and its entry
//                                points taken with dlsym, the way SDL and GLFW do by hand
//     gles_linux --frames=N      stop after N frames (default: run until killed)
//     gles_linux --capture-at=N  ask the inspector for a capture at frame N (include/gpu_inspector.h)
//
// Frames end at a swap, about 60 a second. The windowed modes are the only ones that make a window
// surface (and on X11 the only ones that reach OpenGL ES through GLX); the pbuffer modes are the
// ones that need no desktop.
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

#include "gpu_inspector.h"   // --capture-at: the application asking for the capture itself

#if GLES_LINUX_HAVE_SDL
#include <SDL.h>
#endif

#define GLES_SCENE_LOG(...) (printf(__VA_ARGS__), printf("\n"), fflush(stdout))
#include "../gles_scene/scene.h"

namespace
{

/** The EGL entry points the application uses, from the linked library or from dlsym. */
struct Egl
{
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

bool LoadEgl(bool viaDlopen, Egl& e)
{
    if (!viaDlopen)
    {
        e = {eglGetDisplay, eglInitialize, eglBindAPI, eglChooseConfig, eglCreateContext, eglCreatePbufferSurface,
            eglMakeCurrent, eglSwapBuffers, eglGetProcAddress, eglGetError};
        return true;
    }
    void* lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
    {
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
    return e.GetDisplay && e.Initialize && e.BindAPI && e.ChooseConfig && e.CreateContext && e.CreatePbufferSurface && e.MakeCurrent && e.SwapBuffers && e.GetProcAddress && e.GetError;
}

/** The desktop's display, or with none (a console, ssh) Mesa's surfaceless platform. */
EGLDisplay OpenDisplay(const Egl& e)
{
    EGLDisplay display = e.GetDisplay(EGL_DEFAULT_DISPLAY);
    if (display != EGL_NO_DISPLAY && e.Initialize(display, nullptr, nullptr))
        return display;
    auto platform = (PFNEGLGETPLATFORMDISPLAYEXTPROC)e.GetProcAddress("eglGetPlatformDisplayEXT");
    constexpr EGLenum kSurfaceless = 0x31DD;   // EGL_PLATFORM_SURFACELESS_MESA
    display = platform ? platform(kSurfaceless, EGL_DEFAULT_DISPLAY, nullptr) : EGL_NO_DISPLAY;
    if (display != EGL_NO_DISPLAY && e.Initialize(display, nullptr, nullptr))
        return display;
    return EGL_NO_DISPLAY;
}

/** --capture-at=N: the frame at which the application asks the inspector for a capture itself. */
long g_captureAt = 0;

/** The scene, drawn until `frames` have gone by (or forever if it is 0). `swap` ends each frame. */
template <typename Swap>
void RunScene(long frames, int width, int height, Swap&& swap)
{
    gles_scene::Scene scene;
    scene.Create();
    const auto start = std::chrono::steady_clock::now();
    for (long frame = 0; !frames || frame < frames; ++frame)
    {
        const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
        scene.Render(t, width, height);
        if (!swap())
            return;
        // Asked again each frame until somebody is there to hear it: the inspector connects a
        // few frames after the context is made.
        static bool captureAsked = false;
        if (g_captureAt > 0 && frame >= g_captureAt && !captureAsked)
        {
            char label[48];
            snprintf(label, sizeof label, "asked at frame %ld", g_captureAt);   // the tab's name
            captureAsked = gpu_inspector_capture_named(1, label) != 0;
        }
        if (frame % 600 == 0)
            printf("frame %ld\n", frame), fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

#if GLES_LINUX_HAVE_SDL
/**
 * The scene in a window of SDL's. SDL loads EGL itself, with dlopen, and makes the context and the
 * window surface, so this is the path an engine takes: the plugin has to catch the redirected
 * dlopen and eglCreateWindowSurface. Returns false if there is no display to open, so the caller
 * can fall back to a pbuffer.
 */
bool RunWindow(bool forceEgl, long frames, int width, int height)
{
    // Left to itself SDL reaches an OpenGL ES context through whatever its backend prefers: GLX on
    // X11, EGL on Wayland. These ask for EGL either way, which not every driver's X11 EGL manages
    // (NVIDIA's rejects SDL's window surface), so it is a mode of its own rather than the default.
    if (forceEgl)
    {
#ifdef SDL_HINT_VIDEO_X11_FORCE_EGL
        SDL_SetHint(SDL_HINT_VIDEO_X11_FORCE_EGL, "1");
#else
        SDL_SetHint("SDL_VIDEO_X11_FORCE_EGL", "1");   // the macro is SDL 2.0.22 and newer
#endif
        SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
    }
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        printf("SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_Window* window = SDL_CreateWindow("gles_linux", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    // An OpenGL ES 3.2 context is more than some drivers give; 3.0 runs the scene as well.
    SDL_GLContext context = window ? SDL_GL_CreateContext(window) : nullptr;
    if (window && !context)
    {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        context = SDL_GL_CreateContext(window);
    }
    if (!window || !context)
    {
        printf("no OpenGL ES window: %s\n", SDL_GetError());
        if (window)
            SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }
    SDL_GL_SetSwapInterval(0);   // The frame loop paces itself; do not also wait for the display.
    printf("SDL window (%s, %s); pid %d\n", SDL_GetCurrentVideoDriver(),
        forceEgl ? "EGL forced" : "SDL's own choice of GLX or EGL", (int)getpid());

    bool quit = false;
    RunScene(frames, width, height, [&] {
        for (SDL_Event e; SDL_PollEvent(&e);)
        {
            if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE))
                quit = true;
        }
        if (quit)
            return false;
        SDL_GL_SwapWindow(window);
        return true;
    });
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return true;
}
#endif

/** The scene in a pbuffer, with EGL linked or dlopened by hand. Needs no display. */
bool RunPbuffer(bool viaDlopen, long frames, int width, int height)
{
    Egl egl{};
    if (!LoadEgl(viaDlopen, egl))
        return false;
    EGLDisplay display = OpenDisplay(egl);
    if (display == EGL_NO_DISPLAY)
    {
        printf("no EGL display (error 0x%x)\n", egl.GetError());
        return false;
    }
    egl.BindAPI(EGL_OPENGL_ES_API);
    const EGLint attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint n = 0;
    if (!egl.ChooseConfig(display, attribs, &config, 1, &n) || n < 1)
    {
        printf("no OpenGL ES 3 pbuffer config\n");
        return false;
    }
    EGLContext context = EGL_NO_CONTEXT;
    for (int minor : {2, 0})
    {
        const EGLint ctx[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, minor, EGL_NONE};
        context = egl.CreateContext(display, config, EGL_NO_CONTEXT, ctx);
        if (context != EGL_NO_CONTEXT)
            break;
    }
    if (context == EGL_NO_CONTEXT)
    {
        printf("no OpenGL ES 3 context (error 0x%x)\n", egl.GetError());
        return false;
    }
    const EGLint surfaceAttribs[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
    EGLSurface surface = egl.CreatePbufferSurface(display, config, surfaceAttribs);
    if (surface == EGL_NO_SURFACE || !egl.MakeCurrent(display, surface, surface, context))
    {
        printf("no pbuffer surface (error 0x%x)\n", egl.GetError());
        return false;
    }
    printf("pbuffer (no window), EGL %s; pid %d\n", viaDlopen ? "through dlopen" : "linked", (int)getpid());

    RunScene(frames, width, height, [&] { return egl.SwapBuffers(display, surface) != EGL_FALSE; });
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    enum class Mode
    {
        Default,
        Window,
        WindowEgl,
        Pbuffer,
        Dlopen
    };
    Mode mode = Mode::Default;
    long frames = 0;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--window") == 0)
            mode = Mode::Window;
        else if (strcmp(argv[i], "--window-egl") == 0)
            mode = Mode::WindowEgl;
        else if (strcmp(argv[i], "--pbuffer") == 0)
            mode = Mode::Pbuffer;
        else if (strcmp(argv[i], "--dlopen") == 0)
            mode = Mode::Dlopen;
        else if (strncmp(argv[i], "--frames=", 9) == 0)
            frames = strtol(argv[i] + 9, nullptr, 10);
        else if (strncmp(argv[i], "--capture-at=", 13) == 0)
            g_captureAt = strtol(argv[i] + 13, nullptr, 10);
        else
        {
            printf("unknown option: %s (--window, --window-egl, --pbuffer, --dlopen, --frames=N, --capture-at=N)\n", argv[i]);
            return 2;
        }
    }
    const int width = 640, height = 360;

#if GLES_LINUX_HAVE_SDL
    if (mode == Mode::Default || mode == Mode::Window || mode == Mode::WindowEgl)
    {
        if (RunWindow(mode == Mode::WindowEgl, frames, width, height))
            return 0;
        // A window was asked for by name; only the default mode settles for a pbuffer.
        if (mode != Mode::Default)
            return 1;
        printf("no window to open, drawing into a pbuffer instead\n");
    }
#else
    if (mode == Mode::Window || mode == Mode::WindowEgl)
    {
        printf("built without SDL (libsdl2-dev), so there is no windowed mode\n");
        return 1;
    }
#endif
    return RunPbuffer(mode == Mode::Dlopen, frames, width, height) ? 0 : 1;
}
