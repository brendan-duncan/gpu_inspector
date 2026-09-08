# GPU Inspector TODO

Feature parity with [WebGPU Inspector](https://github.com/brendan-duncan/webgpu_inspector), and
what Vulkan needs beyond it. Ordered roughly by value per effort within each section. Items marked
**(RenderDoc)** have reference implementations in RenderDoc (MIT) that can be adapted: its
replay-driven analysis is the Vulkan counterpart of what WebGPU Inspector does with a WGSL
interpreter and re-created pipelines.

## Done

- Live object inspection: object lists with counts, history, search and per-type filters,
  "used in last capture", meters (frame time with submit time, object count), memory totals.
- Texture viewer (mip, layer, channels, exposure, auto range, zoom, hover/pinned values, copy).
- Descriptor set contents on demand. Shader stage labels. A Reflection section on every shader
  payload (entry points, interface, resources by set and binding, push constants).
- Leak report at device / instance destruction: the objects still alive, by type and by name,
  in the Inspect tab, the session bar and the log.
- Device sections: the physical device's properties, limits, memory heaps and types, queue
  families, features and extensions; the device's enabled extensions, queues and features
  (across the pNext chain); the instance's application info, layers and extensions.
- Frame capture: N frames, queued capture at a frame or after a delay (launch dialog and CLI),
  one tab per capture, command filter, debug-group coloring, render target read-back, a
  thumbnail strip of every pass's attachments, and the image viewer (zoom, channels, exposure,
  texel values) on captured render targets.
- Command inspection: pipeline state, per-stage reflection, descriptor sets with parsed uniform
  and storage buffers (Format editor, radix, array paging) and the contents of bound images
  (read back once per view, budgeted), vertex/index/indirect data, push constants, render
  targets, and "Affected by" on every buffer (the frame's earlier writers of it).
- Frame Stats (API activity, passes, pipelines, bindings, memory, geometry).
- Profile passes: GPU timestamps per render pass and per run of dispatches (compute passes),
  pass durations in headers, pass timeline, Frame Bound card and pass timings in Frame Stats.
- Shader editing: edit as GLSL / HLSL / SPIR-V assembly, compile with the SDK, live replacement
  pipelines, syntax highlighting, line numbers, find, compile errors marked on their lines.
- Shader source maps: embedded source (OpSource / NonSemantic DebugInfo) as a Source view, SPIR-V
  disassembly annotated and linked to source lines, editing from the embedded source.
- Static shader analysis on SPIR-V: modeled Shader Cost per entry point and function, a
  Performance Analysis findings list per shader, and "Analyze Shaders" over a capture's frame.
- Validation messages: the layer's debug-utils messenger forwards validation layer output (with
  repeat counts and object links); "Validation layer" in the launch dialog enables the Khronos
  layer; Inspect lists the messages, marks the objects, the session bar counts them.
- Capture files (`.gpucap`): save from the capture bar or tab menu, open from the launch bar or by
  drag and drop into a session of their own with the object graph, shaders, buffers, render
  targets and timings; "Open in New Tab" copies a capture in memory.

## Next

### Captures
- [ ] Open a capture in a new window (the file session exists only in the window that opened it;
      a session window would need the file path handed over and reopened there).
- [ ] Multisampled depth read-back: `vkCmdResolveImage` handles color only; depth needs a render
      pass with `VK_KHR_depth_stencil_resolve` (or a compute pass) into the temporary image.
- [ ] Sampled image read-back: every mip of a view (only the base mip is copied today), and
      sampled images bound through descriptor buffers / shader objects.

### Inspect
- [ ] Validation messages raised at submit or execution time (synchronization validation, GPU
      assisted validation) attached to the commands they name, where the message text carries a
      command index.
- [ ] Object and command stacktraces (stack capture in the layer, symbolized in the app).
- [ ] Refresh rate from `VK_GOOGLE_display_timing` / `VK_EXT_present_timing` where available,
      instead of the interval-based estimate (which cannot tell a 30 fps app on a 60 Hz display
      from a 30 Hz display).

### Shaders
- [ ] Shader flame graph: the frame's GPU time split by pass, pipeline and function using the
      modeled costs, once per-draw timing exists (WebGPU Inspector's Shader Flame Graph).
- [ ] Shader analysis: workgroup memory size rule (needs type sizes of Workgroup variables),
      loop-invariant detection, and per-statement cost through the source map.
- [ ] Source maps from outside the module: a user-configured source root (file name from
      `OpSource` / `OpLine` looked up on disk) for shaders compiled with `-gVS`-style line info
      but without embedded text, and `#include` resolution for the editor from the same root.
- [ ] Source-level view in captures: the draw's shader panel showing the embedded source, and
      debug-info variable names (DebugLocalVariable / DebugGlobalVariable) in buffer layouts.

## Replay-based features **(RenderDoc)**

These need the capture to be re-executed. WebGPU Inspector does it on the DevTools GPU device with
re-created pipelines plus a CPU WGSL interpreter; for Vulkan the equivalent is RenderDoc's replay
of a serialized frame. Two routes: (a) an in-app replay engine that re-creates the captured
resources and re-executes the frame on the inspector's own Vulkan device (what RenderDoc does,
`renderdoc/driver/vulkan/vk_replay.cpp`), or (b) lean on the existing layer and re-run the live
application with injected state. Route (a) is the general one and is the prerequisite for the rest.

- [ ] Capture enough to replay: full resource contents at frame start (all buffers and images,
      not only bound ranges), initial layouts, descriptor contents — RenderDoc's "initial
      contents" (`vk_initial_contents.cpp`).
- [ ] Replay engine: re-create resources on the inspector's device and execute the captured
      command stream (`vk_replay.cpp`, `vk_core.cpp`).
- [ ] Pixel history (`vk_pixelhistory.cpp`): every draw that touched a pixel, with the test that
      rejected it (depth, stencil, scissor, culled, discarded, write mask) and the value written.
- [ ] Overdraw heatmap (`vk_overlay.cpp`: quad overdraw / triangle size overlays).
- [ ] Draw-call overlays: wireframe, highlight drawcall, depth/stencil test overlays
      (`vk_overlay.cpp`).
- [ ] Per-draw GPU timing and counters via replay with timestamp/pipeline-statistics queries
      (`vk_counters.cpp`).
- [ ] Mesh output view: post-vertex-shader positions via transform feedback or a compute
      re-execution of the vertex shader (`vk_postvs.cpp`).
- [ ] Shader debugger: RenderDoc's SPIR-V interpreter (`vk_shader_debug.cpp`, `spirv_debug.cpp`)
      for vertex, pixel and compute invocations, with variables, stepping and breakpoints.
- [ ] Shader flame graph / statement cost via ablation once per-draw replay timing exists.

## Vulkan-specific
- [ ] Attach to a running process (implicit layer registration), see ARCHITECTURE.md.
- [ ] Remote targets over TCP (the transport is already socket-based; Android devices are
      reached through `adb forward` today, see ARCHITECTURE.md).
- [ ] Android: an Android build of the triangle test app (NativeActivity) so the device path can
      be exercised without a Unity player; a GLES layer for Unity's GLES player; lower default
      read-back limits for phones.
- [ ] Multiple devices and queues in one process (timestamps are per device; the query pool is
      created on the capturing device only).
- [ ] Graphics pipeline libraries and shader objects (`VK_EXT_shader_object`) in the shader editor.
- [ ] Push descriptors with templates in descriptor snapshots.
- [ ] Ray tracing pipelines: shader groups in pipeline state, acceleration structure objects.

## Distribution
- [ ] Code-sign the Windows installer and the layer DLL (SmartScreen warns on unsigned installers).
- [ ] AppImage / rpm targets next to the .deb (electron-updater supports both).
- [ ] macOS build (needs an .icns icon and signing/notarization; the layer has no Metal side yet).

## Tooling
- [ ] Claude Code plugin / MCP server over saved `.gpucap` files (the format is in
      `app/src/renderer/capture_file.ts`; the parser has no DOM dependency and can move to a
      shared module).
- [ ] Automated screenshot tests of the panels against the triangle app and the Unity player.
- [ ] Help links to docs from the panels.
