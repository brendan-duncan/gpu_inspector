# OpenGL ES plugin

Captures OpenGL ES 2.0 to 3.2 applications. On Windows OpenGL ES comes two ways, and both are
captured: from the GPU's own driver, as an OpenGL ES profile context made through WGL (Unity's
`-force-gles32`), and from ANGLE, the `libEGL.dll` and `libGLESv2.dll` an Electron or Chromium
application ships. On Android (10 and later) it is an OpenGL ES layer, which the system's EGL loads
into a debuggable application. It is the example the plugin SDK (docs/PLUGINS.md) was written
against, and it is built and shipped with the app like any other plugin would be.

```
plugin.json            the manifest: the backend module, the library the launcher injects on Windows, the Android layer
CMakeLists.txt         glesinsp_capture, into build/plugins/gles/bin (tools/build_android.py copies
                       the Android build to build/plugins/gles/android/lib/<abi>)
gen/                   the entry points and enum tables, generated from gl.xml (tools/gen_gles.py)
src/
  platform_win32.cpp   GpuInspectorInitialize; the libraries OpenGL ES comes from hooked as they load
  hooks_wgl.cpp        WGL: OpenGL ES profile contexts, wglMakeCurrent, SwapBuffers, wglGetProcAddress
  platform_android.cpp Android: the OpenGL ES layer's entry points, AndroidGLESLayer_*
  platform_linux.cpp   Linux: preloaded; dlopen of the system's libEGL and libGLESv2 answered with its own handle
  hooks_glx.cpp        GLX: OpenGL ES profile contexts, glXMakeCurrent, glXSwapBuffers, glXGetProcAddress
  hooks_egl.cpp        EGL: contexts, surfaces, eglMakeCurrent, eglSwapBuffers, eglGetProcAddress
  hooks_gl.cpp         objects made, described and deleted; what begins, ends and reads a pass
  capture.cpp          the capture: recording, passes, read-backs, the state at each draw
  state.cpp            contexts, share groups and objects; what the inspector is told about them
  formats.cpp          GL internal formats as VK_FORMAT names, and how each is read back
  server.cpp           the connection (the SDK's server) and the requests it answers
ui/backend.ts          the backend module: how the inspector reads the library's captures
```

## What the library does

**Hooks.** `tools/gen_gles.py` reads Khronos's `gl.xml` and writes a hook for each of the 446
OpenGL ES commands and extension commands that are not queries (`glGet*`, `glIs*`). Each hook calls
the real entry point and, while a capture records, serializes the call: enums by name, through the
table of the parameter's group; objects as references to the ids the library gave them, through the
parameter's class; arrays and strings by the registry's lengths. About 140 of the hooks also
call code written by hand, which tracks objects and bindings and marks passes. On Windows the hooks
patch exports in place (MinHook) the moment a library loads, so every caller reaches them: ANGLE's
`libGLESv2.dll`, and `opengl32.dll`'s OpenGL 1.1 exports once an OpenGL ES profile context exists
(a desktop OpenGL context is never taken on). `eglGetProcAddress` and `wglGetProcAddress` hand out
the hooks for everything else. On Android nothing is patched: the EGL loader asks the layer's
`AndroidGLESLayer_GetProcAddress` what to call for each entry point, with the next one in the chain,
and the library answers with its hook and keeps `next` as the real entry point. On Linux the
library is preloaded and exports every hook under the entry point's own name (the generator writes
those exports), so an application linked against libEGL or libGLESv2 calls it; `dlopen` is hooked, so
one that loads the system's libraries itself gets this library's handle, whose `dlsym` finds the
hooks here and every other name in the libraries it links, which are those same ones.

**Objects.** Every buffer, texture, renderbuffer, shader, program, sampler, framebuffer, vertex array,
query, sync, context and surface is announced with its description as it changes: a texture's target,
format and size, a buffer's size, a shader's source and its compile log, and a program's linked
stages with their sources, attributes, uniforms and uniform blocks with their member offsets.
Names are resolved per share group, or per context for the container objects.

**Passes.** OpenGL ES has none. The library begins one at the first draw, clear or blit into a
framebuffer and ends it when the draw framebuffer changes, its attachments change, a debug group it
began in closes, or the surface is swapped. `BeginRenderPass` and `EndRenderPass` are recorded
around it (`synthetic: true`). The pass's color attachments are read back when it ends, or at a
`glInvalidateFramebuffer`, which would throw them away first. What the pass cleared before its first
draw and what it invalidated are written into its `BeginRenderPass`, so the render graph knows
which results never reach memory.

**State.** Every draw and dispatch carries `state`, asked of the driver just before the call ran:
the program, each attribute with its buffer, format and read-back, the index buffer, each sampler
uniform's texture with a read-back, each uniform block's buffer range with a read-back, the default
block's uniform values, and the rasterizer, depth, stencil and blend state. In ES 3 a buffer range
is copied (`glCopyBufferSubData`) into a buffer of the library's own and mapped once the frame is
over; in ES 2 it comes from the library's copy of what the application uploaded, and client-side
arrays from client memory. Each read-back is made once per contents. Since a buffer's indices are
read after the frame, an indexed draw's attributes are read from their offset to the end of their
buffers, and the inspector finds the vertices the draw reads in the indices.

**Read-backs.** Color targets and textures are read with `glReadPixels` through a framebuffer of the
library's own: 8-bit formats as RGBA8, float formats as RGBA32F, integer formats as 32-bit integers.
Multisampled renderbuffers are resolved first, and cube maps are read face by face. A texture a draw
samples is copied (`glCopyImageSubData`, ES 3.2 or `EXT`/`OES_copy_image`) into a 2D array texture of
the library's own and read once the frame is over, so the frame is not stalled mid-pass; without
copy-image, or when the copy fails, it is read where it is met. A compressed
texture is sent as the application uploaded its level 0, which the inspector decodes (BC, ETC2, EAC
and ASTC). Rows are sent top first, so a render target reads the way it appeared on screen.

**Timings.** With `profilePasses` each pass is timed with `EXT_disjoint_timer_query`: a timestamp
at each end where the counter has bits, else an elapsed-time query around it, which is all ANGLE on
Direct3D 11 offers; the passes are then placed end to end. The entry points are looked up by
whichever name the driver has them (NVIDIA's ES context has only the core and desktop names). A pass
is left untimed while the
application has an elapsed-time query of its own running, and none are sent when the GPU reports it
was disjoint.

The library leaves the application's state as it found it: every binding a read-back moves is put
back, and an error the application had not read yet is returned by its next `glGetError` rather
than lost.

## What it does not do yet

- **Depth and stencil** targets are not read back: OpenGL ES's `glReadPixels` reads color only.
  A depth read-back needs a draw of its own, sampling the depth into a color target.
- **Linux** is written but has not been run on a Linux machine yet (TODO.md has what to check).
- **Live image read-back** in the Inspect tab (`RequestImage`) and creation stack traces.
- **Shader storage buffers, images and atomic counters** bound at a dispatch are not in its state.

## Testing it

`test/gles_triangle` is an OpenGL ES 3.0 application on ANGLE with two passes, indexed and
client-side draws, a uniform block, a BC1 texture, an invalidated depth buffer, and 4x MSAA with
`--msaa`. It loads ANGLE from beside itself, where the build copies it from `ANGLE_DIR` (Firefox's
install directory, or an Electron application's `node_modules/electron/dist`). The app launches it
like any Windows target, and the OpenGL ES library goes in beside the others:

```
node tools/run_electron.mjs . --launch=<build>\bin\Release\glesinsp_triangle.exe --debug-capture --debug-command=19
```

`src/app/test/plugins.test.js` checks the backend against a capture in the library's format.
