// The part of EGL the library uses and hooks: types, constants and the dispatch of the real entry
// points. Written out rather than taken from the Khronos headers, like the GL types (gen/gles_api.gen.h),
// so the library builds with no EGL SDK around.
#pragma once

#include <cstdint>

#if defined(_WIN32)
#define EGLAPIENTRY __stdcall
#else
#define EGLAPIENTRY
#endif

typedef void* EGLDisplay;
typedef void* EGLConfig;
typedef void* EGLSurface;
typedef void* EGLContext;
typedef void* EGLClientBuffer;
typedef unsigned int EGLBoolean;
typedef int32_t EGLint;
typedef unsigned int EGLenum;
typedef intptr_t EGLAttrib;
typedef void* EGLNativeWindowType;
typedef void* EGLNativePixmapType;
typedef void (*EGLFuncPtr)(void);

#define EGL_FALSE 0
#define EGL_TRUE 1
#define EGL_NONE 0x3038
#define EGL_ALPHA_SIZE 0x3021
#define EGL_BLUE_SIZE 0x3022
#define EGL_GREEN_SIZE 0x3023
#define EGL_RED_SIZE 0x3024
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_SAMPLES 0x3031
#define EGL_HEIGHT 0x3056
#define EGL_WIDTH 0x3057
#define EGL_DRAW 0x3059
#define EGL_READ 0x305A
#define EGL_GL_COLORSPACE 0x309D
#define EGL_GL_COLORSPACE_SRGB 0x3089
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#define EGL_CONTEXT_CLIENT_TYPE 0x3097
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_CONFIG_ID 0x3028
#define EGL_RENDER_BUFFER 0x3086
#define EGL_BACK_BUFFER 0x3084
#define EGL_SINGLE_BUFFER 0x3085

typedef EGLFuncPtr (EGLAPIENTRY* PFN_eglGetProcAddress)(const char* name);
typedef EGLContext (EGLAPIENTRY* PFN_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
typedef EGLBoolean (EGLAPIENTRY* PFN_eglDestroyContext)(EGLDisplay, EGLContext);
typedef EGLBoolean (EGLAPIENTRY* PFN_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLBoolean (EGLAPIENTRY* PFN_eglSwapBuffers)(EGLDisplay, EGLSurface);
typedef EGLBoolean (EGLAPIENTRY* PFN_eglSwapBuffersWithDamageKHR)(EGLDisplay, EGLSurface, const EGLint*, EGLint);
typedef EGLSurface (EGLAPIENTRY* PFN_eglCreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint*);
typedef EGLSurface (EGLAPIENTRY* PFN_eglCreatePlatformWindowSurface)(EGLDisplay, EGLConfig, void*, const EGLAttrib*);
typedef EGLSurface (EGLAPIENTRY* PFN_eglCreatePlatformWindowSurfaceEXT)(EGLDisplay, EGLConfig, void*, const EGLint*);
typedef EGLSurface (EGLAPIENTRY* PFN_eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint*);
typedef EGLBoolean (EGLAPIENTRY* PFN_eglDestroySurface)(EGLDisplay, EGLSurface);
typedef EGLBoolean (EGLAPIENTRY* PFN_eglQuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint*);
typedef EGLBoolean (EGLAPIENTRY* PFN_eglQueryContext)(EGLDisplay, EGLContext, EGLint, EGLint*);
typedef EGLBoolean (EGLAPIENTRY* PFN_eglGetConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint*);
typedef EGLContext (EGLAPIENTRY* PFN_eglGetCurrentContext)(void);
typedef EGLSurface (EGLAPIENTRY* PFN_eglGetCurrentSurface)(EGLint);
typedef EGLDisplay (EGLAPIENTRY* PFN_eglGetCurrentDisplay)(void);
typedef EGLenum (EGLAPIENTRY* PFN_eglQueryAPI)(void);

namespace glesinsp {

struct EglDispatch {
    PFN_eglGetProcAddress eglGetProcAddress;
    PFN_eglCreateContext eglCreateContext;
    PFN_eglDestroyContext eglDestroyContext;
    PFN_eglMakeCurrent eglMakeCurrent;
    PFN_eglSwapBuffers eglSwapBuffers;
    PFN_eglSwapBuffersWithDamageKHR eglSwapBuffersWithDamageKHR;
    PFN_eglSwapBuffersWithDamageKHR eglSwapBuffersWithDamageEXT;
    PFN_eglCreateWindowSurface eglCreateWindowSurface;
    PFN_eglCreatePlatformWindowSurface eglCreatePlatformWindowSurface;
    PFN_eglCreatePlatformWindowSurfaceEXT eglCreatePlatformWindowSurfaceEXT;
    PFN_eglCreatePbufferSurface eglCreatePbufferSurface;
    PFN_eglDestroySurface eglDestroySurface;
    PFN_eglQuerySurface eglQuerySurface;
    PFN_eglQueryContext eglQueryContext;
    PFN_eglGetConfigAttrib eglGetConfigAttrib;
    PFN_eglGetCurrentContext eglGetCurrentContext;
    PFN_eglGetCurrentSurface eglGetCurrentSurface;
    PFN_eglGetCurrentDisplay eglGetCurrentDisplay;
    PFN_eglQueryAPI eglQueryAPI;
};

extern EglDispatch g_egl;

/** An EGL entry point we hook: its name, our replacement, and where its real entry point goes. */
struct EglHookEntry {
    const char* name;
    void* hook;
    void** real;
};
extern const EglHookEntry kEglHooks[];
extern const size_t kEglHookCount;

/** The EGL functions the library only calls (eglQuerySurface and the rest), by name, and where they go. */
struct EglImport {
    const char* name;
    void** slot;
};
extern const EglImport kEglImports[];
extern const size_t kEglImportCount;

/** Our hook for a GL or EGL entry point, by name: what eglGetProcAddress hands out in its place. */
void* HookFor(const char* name);

}  // namespace glesinsp
