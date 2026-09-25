# OpenGL ES (Windows, Linux and Android)

[Docs index](README.md) › OpenGL ES

The inspector captures OpenGL ES applications on Windows, on Linux and on Android devices (Android
10 and newer). On Windows it catches them whichever way they get OpenGL ES:

- **From the GPU's own driver**, as an OpenGL ES context made through WGL. This is what a Unity
  player started with `-force-gles32` or `-force-gles31` does.
- **From ANGLE**: the `libEGL.dll` and `libGLESv2.dll` that Electron and Chromium applications ship,
  and that ports from mobile use on the desktop.

OpenGL ES support is a [plugin](PLUGINS.md), the one the plugin SDK was written against. It comes
with the app, and it does less than the built-in APIs do: [what is not there
yet](#what-is-not-there-yet) has the list.

## Launching an application

Press **Launch...** and choose the executable, as for a [Vulkan](VULKAN.md) or
[Direct3D 12](D3D12.md) application. Every Windows target is started with the OpenGL ES capture
library injected beside the others, and it connects when the application creates its first OpenGL
ES context. A desktop OpenGL context is left alone. The session's **Log** tab names each library that went in. **Wait for application** works
the same way for an application started by something else.

An application whose ANGLE runs on Vulkan rather than Direct3D 11 is caught by the Vulkan layer
first, since ANGLE makes its Vulkan device before the application makes a context: what you see
is then ANGLE's Vulkan calls. Start it with ANGLE's Direct3D 11 backend to see its OpenGL ES.

## Linux

Launch the application as for a Vulkan one. The capture library is preloaded into it
(`LD_PRELOAD`), and connects when the application makes its first OpenGL ES context: through EGL
(`libEGL.so.1` and `libGLESv2.so.2`, libglvnd's or Mesa's), or as an OpenGL ES profile context of
GLX's. It catches an application linked against those libraries, and one that loads them itself
with `dlopen`, as SDL and GLFW do. A desktop OpenGL context is left alone, through EGL or GLX.

The library is built with the rest on Linux when `libEGL.so.1` and `libGLESv2.so.2` are installed,
and `test/gles_linux` (which also needs `libegl-dev` and `libgles-dev`) draws the test scene in one
of the ways an application can reach EGL. An EGL an application ships in its own directory, such as
Electron's ANGLE, is not the system's and is not captured.

| | |
|---|---|
| `gles_linux` | a window, through SDL, which loads the driver itself the way an engine does — the default, and offscreen instead if there is no display |
| `gles_linux --window` | the same, but fails rather than falling back |
| `gles_linux --window-egl` | a window, forcing SDL onto EGL where it would take GLX |
| `gles_linux --pbuffer` | offscreen, EGL linked, as an application built against libEGL is |
| `gles_linux --dlopen` | offscreen, EGL `dlopen`ed and `dlsym`ed by hand |
| `gles_linux --frames=N` | stop after N frames |
| `gles_linux --capture-at=N` | ask the inspector for a capture at frame N itself (`include/gpu_inspector.h`) |

The windowed modes need `libsdl2-dev` at build time; without it only the offscreen modes are built
and `--window` says so. The offscreen modes put **nothing on screen** — expected, not a failed
launch. Their frames still end at `eglSwapBuffers`, so they are captured as any other
application's are, and the images are in the Inspect panel.

Which of the two SDL takes matters, because they are different code in the plugin. Left alone SDL
uses GLX on X11 and EGL on Wayland, so an X11 desktop exercises `hooks_glx.cpp` and an OpenGL ES
profile context; `--window-egl` asks for EGL either way and covers `eglCreateWindowSurface`
instead. NVIDIA's X11 EGL rejects SDL's window surface (`Could not create GLES window surface`),
which is the driver, not the plugin — a raw `eglCreateWindowSurface` on the same display works —
so on NVIDIA/X11 use a Wayland session to reach the EGL path.

Linux support is new. On an NVIDIA driver under X11, the offscreen modes (EGL linked and through
`dlopen`) have been inspected and captured, and the windowed mode runs; the GLX capture path,
Wayland, `--window-egl`, and other drivers have not been checked yet.

## Android

Launch the application as described in [Android](ANDROID.md#launching), with **Graphics API** set
to **OpenGL ES**. The capture library is an OpenGL ES layer, which Android loads into a
**debuggable** application (a Unity Development Build) from Android 10 on. The inspector copies it
into the application's data directory, names it in Android's GPU debug layer settings
(`gpu_debug_layers_gles`), starts the application and connects through an `adb forward` port. The
**Log** tab shows the library's logcat output (tag `glesinsp`), and closing the session turns the
settings off again.

A launch captures one API. Every application's own interface is drawn by Android with Vulkan on
Android 12 and newer, so the Vulkan layer would load into an OpenGL ES application too and answer
first. On the command line, `--launch-android=<package> --api=gles`. The Claude Code plugin's
`launch_android_app` takes `api: "gles"`.

The library is built with the Vulkan layer by `python tools/build_android.py`, which writes it to
`build/plugins/gles/android/lib/<abi>/`. `python tools/build_android_gles_triangle.py` builds a
debuggable OpenGL ES 3.2 test application, `build/android/android_gles_triangle.apk`.

## What a capture shows

OpenGL ES has no command buffers and no render passes, so a capture is the calls the application
made between two `eglSwapBuffers`, per context. An application that never swaps — a browser, whose
WebGL draws into surfaces its compositor presents — has frames end at `glFlush` and `glFinish`
instead, once 60 of them pass without a swap, and only at a flush with something drawn since the
last; the capture bar says so. `GLESINSP_FRAME_BOUNDARY=flush` starts that way, `=swap` never does. The capture library marks a pass wherever the draw
framebuffer changes (`BeginRenderPass` and `EndRenderPass`, which are not GL calls), and each pass's
color targets are read back as it ends. Debug groups (`glPushDebugGroup`) nest the command tree as
they do for the other APIs.

Selecting a draw shows the state that was in effect when it ran, since in OpenGL ES that is the
context's state rather than anything bound on a command:

- the **program**, and the source of each shader it was linked from;
- the **vertex input**: each attribute with its buffer (or client memory), format, offset and stride,
  and the bytes the draw read, with the index buffer for an indexed draw;
- the **textures** each sampler uniform reads, with their contents;
- the **uniform blocks**, their buffers decoded by the offsets the driver laid them out at, and the
  default block's **uniform values**;
- the **rasterizer, depth, stencil and blend** state.

With **Profile passes** each pass is timed on the GPU (`EXT_disjoint_timer_query`), which gives
Frame Stats its Frame Bound verdict and pass list. ANGLE on Direct3D 11 measures how long each pass
took but not when it started, so its passes are placed end to end. On a tile-based GPU (Mali,
Adreno) the pass that draws to the window includes the wait for the window's next buffer, and the
GPU copies below run inside the passes they are made in, so the timings compare passes rather
than add up to a frame.

The textures and buffers a draw uses are copied on the GPU where the draw is met and read once the
frame is over, so a capture does not stall the frame it records. That needs OpenGL ES 3.2 or
`EXT_copy_image` for textures, and OpenGL ES 3.0 for buffers; without them each is read where it is
met, and a pass that reads many is timed longer than it runs.

**View Mesh** opens the draw's vertices, and the **Render Graph** shows the passes and what they
read from each other. It knows which attachments a pass cleared before drawing and which it
invalidated, so its suggestions about unread results and tile memory hold for OpenGL ES too, in
OpenGL ES's terms. Capture files save and reopen as for any API, and the
[Claude Code plugin](MCP.md) reads them.

## What is not there yet

- Depth and stencil targets are not read back, since OpenGL ES reads color only.
- No replay: overdraw, draw overlays, pixel history, per-draw measurements and Export to C++.
- No shader debugger, shader analysis or shader editing.
- On Android, a device from Android 10 on and a debuggable application: no layer package as the
  Vulkan layer has, so a rooted device is not enough on its own.

`src/plugins/gles/README.md` has how the library does what it does.
