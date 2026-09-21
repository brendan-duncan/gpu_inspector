# OpenGL ES (Windows)

[Docs index](README.md) › OpenGL ES

On Windows the inspector also captures OpenGL ES applications, whichever way they get OpenGL ES:

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

## What a capture shows

OpenGL ES has no command buffers and no render passes, so a capture is the calls the application
made between two `eglSwapBuffers`, per context. The capture library marks a pass wherever the draw
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
took but not when it started, so its passes are placed end to end. A capture reads textures back in
the middle of passes and waits for each, so a pass that samples many textures for the first time in
the frame is timed longer than it runs; the times are for comparing passes, not for a frame budget.

**View Mesh** opens the draw's vertices, and the **Render Graph** shows the passes and what they
read from each other. It knows which attachments a pass cleared before drawing and which it
invalidated, so its suggestions about unread results and tile memory hold for OpenGL ES too, in
OpenGL ES's terms. Capture files save and reopen as for any API, and the
[Claude Code plugin](MCP.md) reads them.

## What is not there yet

- Depth and stencil targets are not read back, since OpenGL ES reads color only.
- No replay: overdraw, draw overlays, pixel history, per-draw measurements and Export to C++.
- No shader debugger, shader analysis or shader editing.
- Windows only. Android is next: the capture library is shaped for a GLES layer.

`src/plugins/gles/README.md` has how the library does what it does.
