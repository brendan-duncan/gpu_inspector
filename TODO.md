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
- Metal (macOS): the capture library in `metal/`, hooking the driver's classes without wrapping
  objects, with object tracking and lifetime, frame capture with render targets, buffers in
  every storage mode, pass timings and pipeline reflection (see `metal/README.md`).
- Render graph suggestions: rules over the graph (unread stores per subresource and per write,
  results overwritten before anything reads them, transient-attachment candidates, mergeable
  passes, barriers that synchronize nothing), in the Render Graph view and Frame Issues, shared
  by both APIs; they replace the per-command rules that answered the same questions by proxy.
- Render Graph: the capture's passes and the resources connecting them, from attachments,
  descriptor sets and transfers, versioned per write and keyed per subresource; a resource
  lifetime chart with a node-link view of the selected pass' neighbourhood, GPU times, the
  critical path, external inputs and passes whose output nothing reads.
- Claude Code plugin (`claude-plugin/`): an MCP server (`app/src/mcp/`) over saved `.gpucap` files,
  built on the renderer's own analysis modules (split out of the UI for it).
  - Tools: summary, Frame Issues, GPU Bottlenecks, render graph, command list and bound state,
    objects, validation, read-back images as PNG, buffers through GLSL layouts, vertices with
    bounds, shader reflection, source, cross-compiled text and analysis, the Shader Flame Graph
    with the hottest functions and lines, and before/after comparison. Shader sources and stack
    symbols come from this machine's source and build trees (`set_search_paths`). A Metal draw's
    argument buffers list the resources their members hold.
  - A capture analysis skill, and analyze / profile / debug / compare commands.
  - Live sessions without the app: launch or attach, frame statistics, captures saved as
    `.gpucap`, and shader replacement for an edit, capture, compare loop, with a `live` command.
    Between captures: live images, descriptor sets, and objects with their creation stacks.
    Android applications are launched over adb (`launch_android_app`).
    The capture libraries end a capture's stream with `CaptureComplete`.

## Next

### Claude Code
- [ ] CI: run `npm test` in the release workflow, and fail when the committed MCP bundle is older
      than its sources.

### Captures
- [x] Pipeline statistics queries per pass on Vulkan (`layer/src/pipeline_stats.h`), carrying the
      same counters Metal's statistic set does, so the GPU Bottlenecks report and its rules work
      for Vulkan captures too.
- [ ] Depth rejection on Vulkan: an occlusion query around each pass counts the samples that
      passed the depth and stencil tests, which is what `late-depth-rejection` needs. It nests
      badly with an application's own occlusion queries, so the layer would have to track whether
      one is active and skip those passes.
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
- [ ] Stack traces: a symbol path for PDBs that are not next to the modules on Windows, and
      inlined callers for the DbgHelp path (the host symbolizer shows them for Android/Linux).
- [ ] Refresh rate on Linux without a driver timing extension: the monitor mode through
      RandR / Wayland outputs (Windows reads the monitor mode today; Linux and Android without
      `VK_GOOGLE_display_timing` fall back to the frame-interval estimate).

### Shaders
- [ ] Shader flame graph: per-draw GPU timing (replay) and measured fragment counts instead of
      the scissor-area estimate.
- [ ] Shader editor `#include` resolution from the source roots (the Source view already reads
      files the debug information names from them).
- [ ] Debug-info variable names (DebugLocalVariable / DebugGlobalVariable) in buffer layouts,
      for modules without OpName / OpMemberName.

## Replay-based features **(RenderDoc)**

These need the capture to be re-executed. WebGPU Inspector does it on the DevTools GPU device with
re-created pipelines plus a CPU WGSL interpreter; for Vulkan the equivalent is RenderDoc's replay
of a serialized frame. Two routes: (a) an in-app replay engine that re-creates the captured
resources and re-executes the frame on the inspector's own Vulkan device (what RenderDoc does,
`renderdoc/driver/vulkan/vk_replay.cpp`), or (b) lean on the existing layer and re-run the live
application with injected state. Route (a) is the general one and is the prerequisite for the rest.

- [x] Replay engine (`replay/`, docs/REPLAY.md): `vkinsp_replay` re-creates a capture's objects on
      this machine's GPU through decoders generated from vk.xml, re-executes its command buffers,
      and compares every read-back render target with its own copy. The triangle, hazard and
      Unity captures replay pixel-identical.
- [ ] Capture enough to replay any frame, RenderDoc's "initial contents" (`vk_initstate.cpp`):
      - resource contents at frame start: images never read back, buffers never bound in the
        frame, mapped-memory writes between submits
      - initial layouts per subresource
      - swapchain images of swapchains created before the layer tracked them

      Also compare multisampled targets through a resolve.
- [ ] Record live shader replacements in captures. A frame captured during `replace_shader` keeps
      the original pipeline, so its replay draws what the application asked for, not what the
      frame showed.
- [x] Pixel history (`vkinsp_replay --pixel`, docs/REPLAY.md): every pass start, draw and clear
      that touched a pixel, with what each draw's fragments met (outside the scissor, culled,
      discarded, depth, stencil), measured with occlusion queries on pipeline copies, and the
      pixel's value and depth after each event.
- [ ] Pixel history, the rest: writes outside render passes (clears, copies, blits, compute),
      multisampled images, per-fragment values (a primitive-id pass), early fragment tests.
- [ ] Pixel history in the app (a pixel click in the image viewer) and the MCP server.
- [x] Overdraw heatmap (`vkinsp_replay --overdraw`, docs/REPLAY.md): each pass is replayed with a
      counting fragment shader. It gives two counts per pass (every rasterized fragment, and the
      fragments passing depth and stencil in draw order), with a heatmap and a histogram. The
      triangle's count matches its pipeline statistics exactly.
- [x] Vulkan overdraw in the app and the MCP server: "Measure Overdraw" (a pass's details, or the
      Reports menu) replays the capture with `vkinsp_replay --overdraw-data` and shows the result the
      way a Metal capture's is shown; `get_overdraw` replays a Vulkan capture on first use.
- [x] Overdraw tab: a pass's heatmap at any zoom, over its render target, with both counts and the
      target's texel under the pointer (`renderer/overdraw_view.ts`).
- [ ] Overdraw of fragments a shader discards (alpha-tested geometry counts as opaque), and of every
      view of a multiview pass.
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
- [ ] Implicit layer: a "Set for my account" button for the environment variables (setx /
      the shell profile) next to Register, and registration for the packaged app (the
      installer could register the layer, the uninstaller remove it).
- [ ] Remote targets over TCP (the transport is already socket-based; Android devices are
      reached through `adb forward` today, see ARCHITECTURE.md).
- [ ] Android: verify `test/android_triangle` (the phone NativeActivity, built by
      `tools/build_android_triangle.py`) on a phone: on a Quest it runs as a 2D panel that the
      shell keeps in the background, so it never gets a window; a GLES layer for Unity's GLES
      player; lower default read-back limits for phones.
- [ ] Read-back after submission (command buffers recorded before the capture) copies each
      attachment once, after the whole submission: a pass that renders to an image a later pass
      of the same submission overwrites shows the later contents; such buffers also have no
      pass timings (the timestamps go in at record time). Stencil store ops are left
      alone (no stencil read-back yet).
- [ ] Frame Issues rules to add: attachments larger than the render area, render passes that
      could be subpasses (a pass whose only input is the previous pass's output), and barriers
      whose stages a later barrier repeats.
- [ ] OpenXR: the XR frame period (72/90/120 Hz) has no source without a swapchain, so the
      meter relies on the interval estimate; the runtime's display period would need an
      OpenXR layer or the runtime's own properties.
- [ ] Multiple devices and queues in one process (timestamps are per device; the query pool is
      created on the capturing device only).
- [ ] Graphics pipeline libraries and shader objects (`VK_EXT_shader_object`) in the shader editor.
- [ ] Push descriptors with templates in descriptor snapshots.
- [ ] Ray tracing pipelines: shader groups in pipeline state, acceleration structure objects.

## Metal

The Metal capture library (`metal/`) reaches the Inspect and Capture panels through the same
protocol as the Vulkan layer. What it lacks falls into two groups: what the UI already does for
Vulkan and only needs the library to send, and what Xcode's Metal Debugger has that neither
backend does. Ordered by value per effort.

### Parity with the Vulkan side of the UI
- [x] Typed buffer views through reflection: every pipeline creation asks Metal for argument
      and buffer-type reflection, the pipeline's descriptor carries it per stage, and a draw's
      stage buffers and inline bytes render as named fields with the Format editor; pipeline
      objects get a Reflection section per stage.
- [x] `FrameStats` (frame time, min, max, submit time, refresh rate) from the frame boundary,
      so the frame-time meter and the Frame Bound card fill in; the refresh period from the
      display, or the layer's interval estimate, and none with display sync off.
- [x] Capture options: `atFrame`, `maxBufferSize`, `maxBufferTotal`, `maxTextureSize`,
      `captureTextures`, `captureBuffers`, `profilePasses`.
- [x] Validation messages: a command buffer's error with the encoder that faulted (encoder
      execution status is on while a client is connected), Metal's validation layer in its
      logging mode through an NSLog interpose (the launch dialog's "Validation layer" sets
      `MTL_DEBUG_LAYER` and shader validation), and shader logs, as `ValidationMessage` with
      repeat counts.
- [x] Leak report at process exit from the tracker.
- [x] Creation stack traces (`backtrace` + `dladdr`) for objects and captured commands,
      answering `RequestStacktraces` and `RequestSymbols`; the launch dialog's "Stack traces"
      and the capture bar's switch reach the library.
- [x] Argument buffers decoded: the reflection marks pointer, texture and sampler members,
      objects report their GPU address or resource id, and a draw's argument buffer lists each
      member resolved to its buffer (with offset), texture or sampler.

### What Xcode's frame capture has
- [x] "Xcode Trace" in the capture bar: `MTLCaptureManager` writing the next frame as a
      `.gputrace` from inside the process, so Xcode's shader debugger and per-line profiler
      open the same frame.
- [x] Per-pass GPU counters beyond timestamps: the statistic set (vertex, fragment and kernel
      invocations, clipper counts) and the stage-utilization set (cycles per stage), each in its
      own counter sample buffer on the pass descriptor, in the pass header's tooltip.
- [x] Vertex-versus-fragment split timing from all four stage-boundary samples of a render
      pass, in the pass header.
- [x] Insights as Frame Issues rules over Metal captures (`metal/frame_analysis.ts`): undefined
      loads, first-use loads, unread stores, multisample stores, memoryless candidates,
      mergeable back-to-back passes, redundant binds, tiny draws, single-threadgroup dispatches.
- [x] Memory viewer: `allocatedSize`, heap and purgeable state per resource, the device's
      `currentAllocatedSize`, into the memory totals meter.
- [x] Bottleneck analysis over the pass counters: overdraw, fragments per primitive, depth
      rejection and the bound stage per pass, as a report and as Frame Issues rules
      (`metal/pass_metrics.ts`, `metal/bottleneck_report.ts`, `docs/PROFILING.md`).
- [x] Overdraw measured while capturing (`metal/src/overdraw.mm`): every render pass drawn again
      right after the application's encoder ends, with counting copies of its pipelines, with and
      without its depth and stencil tests. Heatmaps in the pass's details, the measured figure in
      the pass header and GPU Bottlenecks, kept in capture files, `get_overdraw` in the MCP server.
- [x] Run Metal overdraw on a Mac: the heatmaps show in the pass details (2026-09-11).
- [ ] Metal overdraw on a Unity player, and against the pass's `fragmentsPassed` counter.
- [ ] Pixel history for Metal, the same way: the pass's calls issued again one draw at a time with
      a one-pixel scissor, visibility results in counting mode, and cull mode and depth-stencil
      state varied on the encoder (no pipeline copies needed for those).
- [ ] Per-draw counter sampling (`MTLCounterSamplingPointAtDrawBoundary`, already probed in
      `capture.mm`) so the microtriangle and overdraw findings can name the draws inside a pass
      rather than the pass, the way Xcode's GPU Commands tab sorts by fragments per primitive.
- [ ] Pass dependency graph from the recorded attachments and bound textures, doubling as the
      "Affected by" section the Vulkan side shows on buffers.

### Robustness
- [ ] Intel and AMD class trees and the encoder-boundary timing path (only Apple Silicon and
      stage-boundary sampling are verified).
- [ ] Metal 4 command buffers and encoders: a separate class tree with different selectors.
- [x] Test app coverage for the read-back paths: a private-storage vertex buffer, an MSAA pass
      with resolve through a parallel render encoder, a sampled texture and inline constants.
- [x] ASTC, ETC2 / EAC, PVRTC and the extended-range and packed 4:2:2 pixel formats, in the
      read-back table and the UI's decoder, with reference vectors.
- [ ] Stencil attachment read-back; the multi-planar YUV formats.

## Distribution
- [ ] Code-sign the Windows installer and the layer DLL (SmartScreen warns on unsigned installers).
- [ ] AppImage / rpm targets next to the .deb (electron-updater supports both).
- [x] macOS build of the UI, signed with the project's Developer ID and notarized.

## Tooling
- [ ] UI tests: cases for the Unity player and Android devices when attached (the saved-capture
      mode covers their captures), image comparison of the screenshots against references.
- [ ] Help links to docs from the panels.
