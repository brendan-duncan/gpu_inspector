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
- Frame Issues: frame-level rules over a capture (attachment loads and stores, clears outside
  passes, transient depth, MSAA stores, stereo without multiview, redundant binds, tiny draws)
  in Frame Stats, linked to their commands; the slow XR test package exercises them.
- Validation messages: the layer's debug-utils messenger forwards validation layer output (with
  repeat counts and object links); "Validation layer" in the launch dialog enables the Khronos
  layer; Inspect lists the messages, marks the objects, the session bar counts them.
- Capture files (`.gpucap`): save from the capture bar or tab menu, open from the launch bar or by
  drag and drop into a session of their own with the object graph, shaders, buffers, render
  targets and timings; "Open in New Tab" copies a capture in memory.

## Next

### Captures
- [ ] Multisampled stencil read-back (the depth resolve covers the depth aspect; stencil would
      need a stencil resolve attachment), and multisampled read-back on Vulkan 1.0 devices.
- [ ] Sampled images bound through descriptor buffers / shader objects.

### Inspect
- [ ] GPU-assisted validation messages attached to the commands they name (submit-time
      synchronization validation messages are linked through the command buffer handle and the
      command name in their text; a name that occurs several times in the buffer links to its
      first occurrence). The capture's own read-back barriers can resolve a hazard in the
      captured frame (a barrier before the layer's buffer copy orders the draw after an earlier
      unsynchronized write), so a hazard seen every other frame may be missing from the
      captured one: turning the Buffers and Render targets options off avoids that.
- [ ] Stack traces: source lines on Linux/Android (addr2line / DWARF; dladdr gives exported
      names only), and a symbol path setting for PDBs that are not next to the modules.
- [ ] Refresh rate on Linux without a driver timing extension: the monitor mode through
      RandR / Wayland outputs (Windows reads the monitor mode today; Linux and Android without
      `VK_GOOGLE_display_timing` fall back to the frame-interval estimate).

### Shaders
- [ ] Shader flame graph: per-draw GPU timing (replay) and measured fragment counts instead of
      the scissor-area estimate.
- [ ] Source maps from outside the module: a user-configured source root (file name from
      `OpSource` / `OpLine` looked up on disk) for shaders compiled with `-gVS`-style line info
      but without embedded text, and `#include` resolution for the editor from the same root.
- [ ] Debug-info variable names (DebugLocalVariable / DebugGlobalVariable) in buffer layouts,
      for modules without OpName / OpMemberName.

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
- [ ] Android: a phone build of the triangle test app (NativeActivity with a swapchain; the
      OpenXR one in `test/xr_triangle` only runs on headsets); a GLES layer for Unity's GLES
      player; lower default read-back limits for phones.
- [ ] Store-everything render pass copies cover `vkCmdBeginRenderPass*` and dynamic rendering
      recorded during the capture; command buffers pre-recorded before it (Dawn-style, "Record
      all command buffers") still run the application's DONT_CARE ops. Stencil store ops are
      left alone (no stencil read-back yet).
- [ ] Frame Issues rules to add: attachments larger than the render area, render passes that
      could be subpasses (a pass whose only input is the previous pass's output), MSAA without a
      resolve (the storeOp STORE of a sampled multisampled image), full-pipeline barriers
      (ALL_COMMANDS to ALL_COMMANDS) and barriers whose stages a later barrier repeats; a
      per-rule on/off filter like the shader findings' severity filter.
- [ ] OpenXR: the XR frame period (72/90/120 Hz) has no source without a swapchain, so the
      meter relies on the interval estimate; the runtime's display period would need an
      OpenXR layer or the runtime's own properties.
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
