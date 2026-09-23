// The OpenGL ES test application for Android (tools/build_android_gles_triangle.py): a NativeActivity
// with an OpenGL ES 3.2 context of EGL's, drawing the scene of test/gles_scene/scene.h onto the screen.
#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <android/log.h>
#include <android_native_app_glue.h>

#include <cstdint>
#include <cstring>
#include <ctime>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "glestriangle", __VA_ARGS__)
#define GLES_SCENE_LOG LOGI
#include "../gles_scene/scene.h"

namespace
{

struct App
{
    android_app* android = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLConfig config = nullptr;
    bool resumed = false;
    bool resources = false;
    uint64_t frameCount = 0;
    gles_scene::Scene scene;

    void CreateContext()
    {
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        eglInitialize(display, nullptr, nullptr);
        const EGLint attribs[] = {EGL_RENDERABLE_TYPE, 0x40 /* EGL_OPENGL_ES3_BIT */, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        EGLint n = 0;
        eglChooseConfig(display, attribs, &config, 1, &n);
        for (int minor : {2, 0})
        {
            const EGLint ctx[] = {EGL_CONTEXT_CLIENT_VERSION, 3, 0x30FB /* EGL_CONTEXT_MINOR_VERSION */, minor, EGL_NONE};
            context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx);
            if (context != EGL_NO_CONTEXT)
                break;
        }
        LOGI("context %p", context);
    }

    void CreateSurface()
    {
        surface = eglCreateWindowSurface(display, config, android->window, nullptr);
        eglMakeCurrent(display, surface, surface, context);
        if (!resources)
        {
            resources = true;
            scene.Create();
        }
    }

    void DestroySurface()
    {
        if (surface == EGL_NO_SURFACE)
            return;
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(display, surface);
        surface = EGL_NO_SURFACE;
    }

    void RenderFrame(float t)
    {
        EGLint w = 0, h = 0;
        eglQuerySurface(display, surface, EGL_WIDTH, &w);
        eglQuerySurface(display, surface, EGL_HEIGHT, &h);
        scene.Render(t, w, h);
        eglSwapBuffers(display, surface);
        ++frameCount;
    }
};

void OnAppCommand(android_app* app, int32_t cmd)
{
    App* self = (App*)app->userData;
    switch (cmd)
    {
        case APP_CMD_INIT_WINDOW:
            if (self->context == EGL_NO_CONTEXT)
                self->CreateContext();
            self->DestroySurface();
            self->CreateSurface();
            break;
        case APP_CMD_TERM_WINDOW: self->DestroySurface(); break;
        case APP_CMD_RESUME: self->resumed = true; break;
        case APP_CMD_PAUSE: self->resumed = false; break;
        default: break;
    }
}

}  // namespace

void android_main(android_app* app)
{
    App self;
    self.android = app;
    app->userData = &self;
    app->onAppCmd = OnAppCommand;
    timespec start{};
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!app->destroyRequested)
    {
        for (;;)
        {
            int events = 0;
            android_poll_source* source = nullptr;
            const bool rendering = self.resumed && self.surface != EGL_NO_SURFACE;
            if (ALooper_pollOnce(rendering ? 0 : -1, nullptr, &events, (void**)&source) < 0)
                break;
            if (source)
                source->process(app, source);
            if (app->destroyRequested)
                break;
        }
        if (app->destroyRequested)
            break;
        if (self.resumed && self.surface != EGL_NO_SURFACE)
        {
            timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now);
            self.RenderFrame((float)(now.tv_sec - start.tv_sec) + (float)(now.tv_nsec - start.tv_nsec) * 1e-9f);
        }
    }
    self.DestroySurface();
    LOGI("exiting after %llu frames", (unsigned long long)self.frameCount);
}
