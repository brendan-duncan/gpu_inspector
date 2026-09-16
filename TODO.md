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
- Metal (macOS): the capture library in `src/metal/`, hooking the driver's classes without wrapping
  objects, with object tracking and lifetime, frame capture with render targets, buffers in
  every storage mode, pass timings and pipeline reflection (see `src/metal/README.md`).
- Direct3D 12 (Windows): the capture library in `src/d3d12/`, injected at process start by
  `dxinsp_launch.exe` and hooking the D3D12 and DXGI entry points and vtables without wrapping
  objects, speaking the Vulkan layer's protocol: object tracking with descriptors, names and
  descriptor heap contents, frame capture with synthesized passes, render targets, bound buffers
  and textures, root constants, `ExecuteIndirect` arguments, pass timings and counters, the
  debug layer's messages, stack traces, DXBC/DXIL reflection and disassembly (`dxinsp_shader.exe`),
  shader editing through `dxc`, and record-always for pre-recorded lists (see `src/d3d12/README.md`).
  `test/d3d12_triangle` is its test application.
- Render graph suggestions: rules over the graph (unread stores per subresource and per write,
  results overwritten before anything reads them, transient-attachment candidates, mergeable
  passes, barriers that synchronize nothing), in the Render Graph view and Frame Issues, shared
  by both APIs; they replace the per-command rules that answered the same questions by proxy.
- Render Graph: the capture's passes and the resources connecting them, from attachments,
  descriptor sets and transfers, versioned per write and keyed per subresource; a resource
  lifetime chart with a node-link view of the selected pass' neighbourhood, GPU times, the
  critical path, external inputs and passes whose output nothing reads.
- Claude Code plugin (`claude-plugin/`): an MCP server (`src/app/src/mcp/`) over saved `.gpucap` files,
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
- [x] CI: the release workflow runs `npm test` on every platform, and fails when the committed MCP
      bundle differs from a fresh build of its sources.

### Captures
- [x] Pipeline statistics queries per pass on Vulkan (`src/vulkan/src/pipeline_stats.h`), carrying the
      same counters Metal's statistic set does, so the GPU Bottlenecks report and its rules work
      for Vulkan captures too.
- [x] Depth rejection on Vulkan: the layer runs a precise occlusion query around each render pass
      (`src/vulkan/src/capture.cpp`), counting the samples that passed its depth and stencil tests
      (`fragmentsPassed`, Metal's name for the same figure), so `late-depth-rejection` and the
      report's depth rejection column work for Vulkan captures. A pass whose command buffer has an
      application query open is skipped, and the query ends early (dropping that pass's count) when
      one begins or a secondary command buffer runs inside the pass.
- [x] Multisampled stencil read-back: the resolve carries both aspects of a depth-stencil image,
      with the same resolve mode so a device without `independentResolve` can still do it. Untested:
      no test application has a multisampled stencil attachment.
- [x] Dynamic rendering passes suspended and resumed across command buffers: the layer records
      nothing between the parts (the resumed part reads back for both; neither is timed). The
      replay's own per-pass instrumentation (overdraw, pixel history: `PrepareOverdraw` /
      `PrepareHistory` before each `vkCmdBeginRendering`) still treats each part as a pass, so those
      analyses of a split-pass capture inject commands between the parts; a plain replay is fine.
- [x] Read back the stencil aspect of a depth-stencil image: a render target's stencil is a texture
      of its own beside its depth (through the resolve when multisampled), the frame-start contents
      take a loaded stencil apart from the depth, and the replay uploads and compares it.
      `test/triangle --stencil` (with or without `--msaa`) exercises it. D3D12 reads its stencil
      plane back too (below); Metal still reads depth only (its own item).
- [ ] Multisampled read-back on Vulkan 1.0 devices: the depth/stencil resolve needs dynamic
      rendering (core 1.3, `VK_KHR_dynamic_rendering` on 1.2), so a 1.0 device would need a
      shader-based resolve of sample zero instead.
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
- [x] Shader flame graph: fragment stages are weighted by the fragment invocations a pass's GPU
      counters measured, split between its draws by scissor area, instead of the scissor-area
      upper bound. A pass without counters still uses the estimate.
- [x] Shader flame graph: per-draw GPU timing and per-draw fragment counts from the replay
      (**Measure draws**, `get_shader_flame_graph`). A pass's measured time is split between its
      draws by what the replay timed each at, and fragment stages take their measured counts.
- [x] Shader editor `#include` resolution from the source roots: `-I` per root for glslang and dxc,
      with the Google include-directive preamble where a GLSL source needs it (glslang counts the
      preamble separately, so error lines stay the source's).
- [x] Debug-info variable names (DebugGlobalVariable, DebugTypeComposite, DebugTypeMember) in
      buffer layouts, for modules without OpName / OpMemberName.

## Replay-based features **(RenderDoc)**

- [x] Replay-based analyses of a live Vulkan capture seemed to hang in the app (the `overdraw`,
      `mesh`, `overlay`, `debug-pixel` and `debug-vertex` UI cases). Nothing was wrong with the
      replay: the renderer was frozen within seconds of launching. NVIDIA's Vulkan driver makes a
      D3D12 device to present through DXGI, the D3D12 library launched alongside the layer tracked
      it and logged every call, and the session log redrew itself for each of ~1,400 lines a second.
      The library now leaves a D3D12 device made while the layer has a Vulkan device alone, and the
      log draws its lines in batches. Two things hid it: the launch's `VK_LOADER_LAYERS_ENABLE` ran
      the *installed* GPU Inspector's implicit layer instead of the build's (the launch now adds its
      layer to `VK_ADD_IMPLICIT_LAYER_PATH`), and an occluded test window stopped `ResizeObserver`,
      so the overdraw tab's image stayed at 10% and the scripted click missed it.

These need the capture to be re-executed. WebGPU Inspector does it on the DevTools GPU device with
re-created pipelines plus a CPU WGSL interpreter; for Vulkan the equivalent is RenderDoc's replay
of a serialized frame. Two routes: (a) an in-app replay engine that re-creates the captured
resources and re-executes the frame on the inspector's own Vulkan device (what RenderDoc does,
`renderdoc/driver/vulkan/vk_replay.cpp`), or (b) lean on the existing layer and re-run the live
application with injected state. Route (a) is the general one and is the prerequisite for the rest.

- [x] Replay engine (`src/replay/`, docs/REPLAY.md): `vkinsp_replay` re-creates a capture's objects on
      this machine's GPU through decoders generated from vk.xml, re-executes its command buffers,
      and compares every read-back render target with its own copy. The triangle, hazard and
      Unity captures replay pixel-identical.
- [x] Swapchain images of a recreated swapchain: a driver hands the new swapchain its
      predecessor's image handles, and the tracker kept them under the old swapchain, so destroying
      it took them and the views over them away. A recycled handle now moves to its new owner
      (`src/vulkan/src/tracker.cpp`). A Unity frame used to lose its final image in the capture and drop
      two passes in the replay; it now replays with 0 problems and every target identical.
- [x] Frame-start contents, RenderDoc's "initial contents" (`vk_initstate.cpp`), the way a layer
      that sees every command can take them: the first read of an image subresource the capture has
      not written whole (a loaded attachment, a copy, blit or copy-to-buffer source) copies it before
      the command runs (texture kind `initial`, the command's `imageData`), and buffer copy sources
      are read whole into `bufferData`, which covers staging buffers the host writes between
      submits. The replay uploads them, starts every subresource in its own layout, and compares
      multisampled targets through the capture's resolve. `test/triangle --persistent` replays
      identical (its 3 persistent targets differed before), and a Unity frame takes no copies.
- [ ] Frame-start contents, the rest (docs/REPLAY.md, "What is left"): stencil and multisampled
      contents, memory no command names (buffer device addresses, descriptor buffers), storage
      resources a shader reads before writing inside a pass, command buffers recorded in another
      order than they run.
- [x] Per-draw timing and counters inside secondary command buffers: the replay instruments them
      there too, so a Unity frame (every draw in a secondary) measures all of them. Its per-draw
      fragment counts add up to the layer's own per-pass counters exactly.
- [x] Depth rejection for a pass that executes secondary command buffers: an occlusion query cannot
      stay active across `vkCmdExecuteCommands`, so the layer leaves those passes unmeasured. The
      replay now runs a precise occlusion query around each draw, inside the secondary, and the
      pass metrics sum them where the capture's own counter is missing. A Unity frame's 11 draws
      are all measured.
- [x] Record live shader replacements in captures: a bind recorded during `replace_shader` names
      the replacement pipeline (or shader objects), the original beside it as `replaced`, so the
      draw's state, `get_shader`, the shader debugger and the replay all use the edited code.
- [x] Pixel history (`vkinsp_replay --pixel`, docs/REPLAY.md): every pass start, draw and clear
      that touched a pixel, with what each draw's fragments met (outside the scissor, culled,
      discarded, depth, stencil), measured with occlusion queries on pipeline copies, and the
      pixel's value and depth after each event.
- [ ] Pixel history, the rest: writes outside render passes (clears, copies, blits, compute),
      multisampled images, per-fragment values (a primitive-id pass), early fragment tests.
- [x] Pixel history in the app and the MCP server: the pixel clicked in a capture's render target
      tab, beside the image, and `get_pixel_history`. Both replay the capture with
      `vkinsp_replay --pixel-data`.
- [x] Replays kept alive (`vkinsp_replay --serve`, `ReplayServerPool`): one process per capture,
      its device and objects made once, each analysis replaying only the frame (tens of
      milliseconds on a Unity frame), for the app and the MCP server.
- [ ] A frame restored between served analyses: buffers the frame writes outside their captured
      ranges keep what the previous frame left.
- [x] Overdraw heatmap (`vkinsp_replay --overdraw`, docs/REPLAY.md): each pass is replayed with a
      counting fragment shader. It gives two counts per pass (every rasterized fragment, and the
      fragments passing depth and stencil in draw order), with a heatmap and a histogram. The
      triangle's count matches its pipeline statistics exactly.
- [x] Vulkan overdraw in the app and the MCP server: "Measure Overdraw" (a pass's details, or the
      Reports menu) replays the capture with `vkinsp_replay --overdraw-data` and shows the result the
      way a Metal capture's is shown; `get_overdraw` replays a Vulkan capture on first use.
- [x] A capture's render target in a tab of its own (`renderer/capture_texture_view.ts`), after
      WebGPU Inspector's capture texture viewer: the image with the pass's overdraw over it when
      **Overdraw** is ticked (legend, counts under the pointer, how much colour covers it), and the
      history of the pixel clicked beside the image.
- [ ] Overdraw of fragments a shader discards (alpha-tested geometry counts as opaque), and of every
      view of a multiview pass.
- [x] Draw-call overlays (`vkinsp_replay --overlay`, `src/replay/src/overlay.cpp`): highlight draw,
      depth test and wireframe in the render target tab, for any draw of the pass.
- [ ] Draw overlays, the rest: stencil apart from depth, backface cull, viewport/scissor,
      NaN/INF, clipping, triangle size and quad overdraw (RenderDoc's other overlays); discarded
      fragments; `get_draw_overlay` in the MCP server.
- [x] Per-draw GPU timing and counters via replay with timestamp and pipeline-statistics queries
      (`src/replay/src/draw_stats.cpp`, `vkinsp_replay --draws`, docs/REPLAY.md): every draw and
      dispatch timed and counted, kept in capture files, read by the Shader Flame Graph.
- [x] Mesh output view (`renderer/mesh_view.ts`, `vkinsp_replay --mesh`, `src/replay/src/mesh.cpp`):
      VS In from the captured buffers and VS Out through transform feedback, as a wireframe and a
      table, with `get_mesh_output` in the MCP server.
- [ ] Mesh output, the rest: tessellation and geometry stage outputs, every view of a multiview
      pass, GPUs without transform feedback (RenderDoc's compute-shader conversion), and a solid
      shaded preview. A Metal draw's VS Out could come from the interpreter the shader debugger
      already runs it in (`renderer/metal/shader_debug.ts`, `interpretedMeshOutput`).
- [x] Shader debugger (`renderer/spirv/`, `renderer/shader_debugger_view.ts`, `debug_shader`): a
      SPIR-V interpreter for vertex, pixel and compute invocations, with variables, stepping and
      breakpoints.
- [x] The debugger behind an API-neutral seam (`renderer/debug/program.ts`): the stepping, the tab
      and `debug_shader` are written against DebugProgram and DebugInvocation, and each language
      supplies one (`renderer/spirv/program.ts`, `renderer/msl/program.ts`).
- [x] Metal shaders (`renderer/msl/`): the Metal Shading Language a capture holds, lexed, parsed,
      lowered to a linear form and interpreted, for all three stages. A fragment's varyings come
      from running the draw's own vertex shader rather than from a replay, since Metal has none.
- [ ] Shader debugger, the rest: tessellation and geometry stages (Metal: object, mesh and tile),
      per-sample shading, watch expressions, and editing a value and running on.
- [x] Shader cost by ablation (`renderer/vulkan/spirv_ablate.ts`, `src/replay/src/ablation.cpp`,
      **Measure shader**, `measure_shader_cost`): a draw replayed with SPIR-V variants that leave out
      a function, a line or a texture, sizing the flame graph's measured stages.
- [ ] Ablation, the rest:
  - Vertex stages, which decide what is rasterized: time them with the fragment stage off.
  - Parts inside control flow: measure a branch's arms by forcing the condition, rather than
    skipping everything a branch depends on.
  - More than one draw of a pipeline: several targets per request exist, but nothing sends them.
  - Metal, which has no replay to time variants in.
  - Engine shaders with no line information: steps through GLSL decompiled by spirv-cross, the
    way the shader debugger does.

## Vulkan-specific
- [x] Implicit layer: **Set for my account** for the environment variables (the account's
      environment on Windows, `~/.config/environment.d` on Linux), and the Windows installer
      registers the layer and the uninstaller removes it (`src/app/installer/installer.nsh`).
- [ ] Implicit layer, the rest: the .deb could register the layer in
      `/usr/share/vulkan/implicit_layer.d` (a postinst script); the installer script is built but
      has not been run on a machine.
- [ ] Remote targets over TCP (the transport is already socket-based; Android devices are
      reached through `adb forward` today, see ARCHITECTURE.md).
- [ ] Android: verify `test/android_triangle` (the phone NativeActivity, built by
      `tools/build_android_triangle.py`) on a phone: on a Quest it runs as a 2D panel that the
      shell keeps in the background, so it never gets a window; a GLES layer for Unity's GLES
      player; lower default read-back limits for phones.
- [x] Read-back after submission (command buffers recorded before the capture) splits the
      submission after each such buffer, so a later buffer of the same submission cannot
      overwrite what it rendered before it is read (`PreHook_vkQueueSubmit`, triangle `--prerecord`).
- [ ] Read-back after submission, the rest: two passes of one prerecorded buffer that write the
      same image still read back after both. Such buffers have no pass timings: the timestamps
      go in at record time, though a split could time each buffer as a whole. Submissions
      extended with structures other than timeline semaphore values are not split. Stencil store
      ops are left alone (no stencil read-back yet).
- [x] Frame Issues rules: `oversized-attachment` (larger than every render area drawn into it),
      `subpass-candidate` (a pass reading only the previous pass's output, its shaders checked for
      filtering), `redundant-transition` (a transition nothing uses before the next, or a barrier
      that changes nothing).
- [ ] OpenXR: the XR frame period (72/90/120 Hz) has no source without a swapchain, so the
      meter relies on the interval estimate; the runtime's display period would need an
      OpenXR layer or the runtime's own properties.
- [x] Multiple devices and queues in one process: query pools, staging and resolve images per
      device (`CaptureManager::DeviceCapture`), captures started by presents rather than another
      device's fence waits, and the replay running every device's objects on its one device
      (triangle `--second-device`, `--second-queue`).
- [ ] Several devices, the rest: devices on different GPUs replay on one (their formats and
      features may not all be there), and a second device's own frame counter can disagree with the
      presenting device's, so its submissions are numbered from the frame it joined at.
- [x] Graphics pipeline libraries and shader objects (`VK_EXT_shader_object`) in the shader editor:
      linked pipelines show and edit their libraries' stages (the libraries made again from their
      records), shader objects are replaced at `vkCmdBindShadersEXT` (a linked set made again
      unlinked), and the replay makes both (triangle `--pipeline-library`, `--shader-object`).
- [x] Pipelines linked from libraries in the replay's copies and the app's state views: a copy is
      made whole from the libraries' records (`Replayer::MergeLibraries`), and a linked pipeline's
      descriptor fills in what its libraries hold (`withLibraries`).
- [x] Shader objects in captures: a draw bound with `vkCmdBindShadersEXT` carries its shader
      objects and the dynamic state standing in for a pipeline's (`DrawState.shaders`,
      `DrawState.dynamic`), and reports that group by pipeline group these by a program key
      (`ShaderProgram`): the command details, Analyze Shaders, the Shader Flame Graph, the shader
      debugger, frame statistics and `get_command` all see them. The replay's VS Out issues such a
      draw in dynamic rendering with a feedback copy of its vertex shader object (triangle
      `--shader-object`).
- [ ] Shader objects, the rest: the replay's overdraw, draw-call overlays, pixel history and
      per-stage ablation (**Measure shader**, `measure_shader_cost`) copy pipelines, so draws with
      shader objects are left out of them.
- [x] Push descriptors with templates in descriptor snapshots (`DescriptorTracker::FromTemplate`),
      replayed as plain pushes from the snapshot (`Replayer::IssueCommand`, triangle `--push-template`).
- [x] Ray tracing pipelines: every stage's code (payloads named with their index in pStages), shader
      groups in the Inspect panel, the command details and `get_command`, a trace command's shader
      binding table regions, acceleration structures with what their last build held, and
      acceleration structures in descriptor snapshots (triangle `--ray-tracing`).
- [ ] Ray tracing, the rest:
  - The replay makes no ray tracing pipelines or acceleration structures, and leaves their
    commands out. Building needs the geometry buffers the builds read by device address, and
    tracing needs the shader binding table copied with its handles made again.
  - Which group each binding table record holds, which needs the table's contents read back.
  - Editing a ray tracing stage.
  - Ray queries in the shader debugger.

## What Nsight Graphics has **(Nsight)**

Measured against Nsight Graphics, with the Nsight Systems and Aftermath pieces used beside it.
Ordered by value per effort. Everything here is reachable through public APIs; what needs the
vendor's driver is listed at the end so nobody spends time on it.

- [x] Hardware unit counters per pass and per draw (Nsight's GPU Trace and Range Profiler: SM
      throughput, L2 hit rate, VRAM bandwidth, texture unit load): `vkinsp_replay --counters`
      (`src/replay/src/hw_counters.cpp`, docs/REPLAY.md "Hardware counters") reads the GPU's own
      counters around each render pass, and around each draw with `--counter-draws`, replaying the
      frame once per collection pass (a `pct_of_peak` metric needs dozens). Two backends:
      NVIDIA's Nsight Perf SDK (`src/replay/src/nvperf.cpp`, headers vendored in `third_party/nvperf`,
      per pass and per draw) and `VK_KHR_performance_query` (per draw). `get_hw_counters` in the MCP
      server, `--list-counters` for what a GPU offers. docs/PROFILING.md's "the limiters" are now
      reachable on Vulkan. Verified on an RTX 4080 against the triangle and a 34-pass Unity frame;
      collection needs GPU counter access enabled (`ERR_NVGPUCTRPERM`; NVIDIA Control Panel >
      Developer > Manage GPU Performance Counters).
- [x] Hardware counters in the app: **Measure hardware counters** in the GPU Bottlenecks report
      (`renderer/bottleneck_report.ts`) replays the capture and shows a column per counter beside
      each pass, kept with the capture and saved into its file.
- [x] The counters decide each pass's verdict (`PassMetrics.limiter`, `passLimiter` in
      `renderer/hw_counters.ts`): shader, bandwidth, cache or latency bound, naming the unit at its
      limit, shown in GPU Bottlenecks' Verdict column and its advice and reported by
      `get_bottlenecks`. On Vulkan it is the only per-pass verdict there has been, the inferred one
      needing Metal's stage spans. Checked against three real frames on an RTX 4080: a Unity frame
      at 800x600 (20 passes latency bound, 5 shader, 4 cache, 5 saturating nothing),
      `vkinsp_triangle --heavy --width 2560 --height 1440` (shader bound, SM at 90% of peak) and
      `vkinsp_triangle --msaa --width 3840 --height 2160` (cache bound, L2 at 48% against SM's 13%,
      a pass the stage verdict would have called fragment bound). The bandwidth verdict has only
      synthetic coverage: nothing here drives DRAM above L2.
- [ ] Hardware counters, the rest: a portable default counter set for the
      `VK_KHR_performance_query` path (it takes the first command-scoped counters now), which has
      only ever run as far as its precondition check, never against a driver that offers the
      extension; per-draw counters in the Shader Flame Graph beside the ablation costs; the
      thresholds the verdict uses (60% saturated, 30% busy, 30% occupancy) are starting points,
      judged against the three frames above.
- [x] Device-lost diagnostics on Vulkan (Nsight Aftermath's answer to "what was the GPU running when
      it hung"): `src/vulkan/src/device_lost.h`, breadcrumbs through `VK_AMD_buffer_marker` written
      before and after every draw and dispatch, read back when any call reports
      `VK_ERROR_DEVICE_LOST`. The session log names the command and says whether it also finished,
      which separates a hang inside it from a hang in what came next. **Device-lost breadcrumbs** in
      the launch dialog, `breadcrumbs` for `launch_app`, `VKINSP_BREADCRUMBS=1`; off by default (two
      GPU writes per action). Verified on an RTX 4080 with `VKINSP_SIMULATE_DEVICE_LOST=<n>[:hung]`,
      which reports a loss without hanging the GPU.
- [x] Device-removed diagnostics on D3D12 (`src/d3d12/src/device_removed.h`): Device Removed Extended
      Data turned on before the device is created, so a removal reports which operation each command
      list stopped on, what the removal code means, and for a page fault the faulting address with
      the objects allocated nearest it. On by default — the runtime keeps the breadcrumbs itself, so
      unlike the Vulkan side there is no per-draw cost; `DXINSP_NO_DRED=1` turns it off. Verified on
      an RTX 4080 with `DXINSP_SIMULATE_DEVICE_REMOVED=<n>`, with DRED on and off.
- [ ] Device-lost diagnostics, the rest: `VK_NV_device_diagnostic_checkpoints`, which reports every
      checkpoint still in flight rather than the last two markers; the faulting address through
      `VK_EXT_device_fault`; a dialog rather than only a log line; and a real hang, which has not
      been tried because it trips a TDR reset on the machine running it (`test/triangle --hang`
      would be the way). The D3D12 breadcrumbs in particular have only been reached by simulation,
      which by design cannot carry data: the runtime hands DRED over only after a real removal, so
      the code that names the stopped operation has never run against one.
- [x] Where a frame's CPU time went (`src/vulkan/src/cpu_timeline.h`, `renderer/cpu_timeline.ts`,
      **Where the CPU went** in Frame Stats): the layer times submit, present, fence waits, acquire
      and wait-idle during a capture, with the thread of each, and the capture carries them with a
      `VK_KHR_calibrated_timestamps` relation between the GPU and CPU clocks. The verdict separates
      waiting for the GPU from being paced by the display from paying for submission, which
      docs/PROFILING.md's first step could previously only infer. Checked on an RTX 4080: a vsynced
      triangle reads as display-paced (65% in present and acquire), an offscreen one as spending its
      time outside timed calls. The fence-wait and submission verdicts have tests but no real
      capture that reaches them — nothing here is GPU-bound or submission-bound enough.
- [x] The timeline as a drawing (**Timeline** in Frame Stats, `renderer/timeline_tracks.ts`): one
      lane per thread with each timed call as its own span, and a GPU lane with the passes, on the
      shared axis the calibration provides. The layer now sends `originTicks`, the device tick the
      pass starts are measured from, without which the passes cannot be placed on the host clock.
      The verdict names the longest gap between passes, attributes it to what the CPU lanes were
      doing across it (display-paced, waiting on submission, or the application's own untimed work),
      and reports the wait from a submission to the pass it queued. `get_capture_summary` carries
      it. Checked on an RTX 4080: a 1-frame triangle capture places its passes 2.69 ms after the
      submission (the swapchain image, not a stall) and reports its passes back to back; a 4-frame
      one finds the 1.99 ms interframe gap and correctly calls it display pacing rather than a
      stall. Two things the real capture caught: the tick values are past 2^53 and so arrive as
      JSON strings, and counting the axis either side of the GPU lane as idle reported a 96% idle
      GPU on a frame whose GPU was simply not the limit.
- [ ] The timeline as a drawing, the rest: the lanes zoomable and scrollable rather than fitted to
      the card (a 4,000-draw frame's spans are sub-pixel at frame scale, and `MAX_SPANS_PER_TRACK`
      drops the rest); clicking a span to select the pass or call it names; the CPU timeline live in
      the session bar rather than only in a capture; and per-queue GPU lanes rather than one, which
      needs the layer to report the queue each pass ran on.

- [x] Compiler statistics per pipeline (Nsight: register count, occupancy, spills per shader):
      `src/vulkan/src/shader_statistics.h`, through `VK_KHR_pipeline_executable_properties`. The
      layer adds the capture flag to every pipeline and asks the driver what it made of each stage;
      the Inspect tab shows it on the pipeline. **Compiler statistics** in the launch dialog,
      `shaderStatistics` for `launch_app`, `VKINSP_SHADER_STATISTICS=1`; off by default, since the
      driver has to keep the information. On an RTX 4080 the driver reports register count, binary
      size, stack, local and shared memory, and input/output counts per stage. The names are the
      driver's own, so they are passed through rather than mapped.
- [x] The register count explains the occupancy verdict (`limiterAdvice` in
      `renderer/hw_counters.ts`, `PassLimiter.registers`): a latency-bound pass names the stage whose
      registers are holding occupancy down, or rules register pressure out when the shader is light.
      `pass_metrics.ts` tracks the pipelines each pass's draws bind and reads the driver's register
      count off them. Checked on an RTX 4080: the heavy shader at 96x96 is 8.6% occupancy with 40
      registers (blamed), the plain one 9.5% with 16 (ruled out).
- [ ] Compiler statistics, the rest: the internal representations
      (`VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR`, the driver's own disassembly)
      beside the SPIR-V in the shader viewer; statistics for shader objects
      (`VK_EXT_shader_object`), which have no pipeline to query; and the register count in the flame
      graph's modelled cost.

- [ ] Acceleration structure viewer (the TLAS and BLAS drawn with overlap heatmaps and
      per-instance flags). The ray tracing item above already needs the build inputs captured
      by device address; once they are, the boxes and geometry belong in the mesh view.
- [x] Memory per heap (`renderer/memory_heaps.ts`, **Memory Use** on the physical device): what the
      application allocated from each heap and type, its share of the heap, and the driver's own
      residency and budget through `VK_EXT_memory_budget` (added at device creation, sampled with
      the frame report, attached to the physical device as `memoryBudget`). Heaps near their limit
      are flagged, including ones another process is filling. Checked on an RTX 4080: the triangle
      holds 15 MB of a 16 GB device-local heap while the driver reports 156 MB resident, the
      difference being its own overhead and other processes.
- [ ] Memory, the rest: over *time* rather than at one instant (Nsight's resource view plots
      allocation against the frame, which needs the layer to report the totals per frame report
      rather than only the budget); fragmentation, which Vulkan does not expose and which would have
      to be approximated from the allocation size distribution; what each allocation is bound to
      (images and buffers already name their memory, so the reverse mapping is derivable); and the
      D3D12 equivalent through `QueryVideoMemoryInfo`.

- [ ] Export to C++: a frame serialized into a standalone compilable project, mainly for driver
      bug reports. The replay engine already recreates every object, so emitting source from the
      same walk is feasible; lower priority.
- [ ] In-app HUD and live pause (frame time drawn over the target, the paused frame scrubbed in
      the app itself). The served replay is close to the second half; the overlay is small work
      through the swapchain hook on Vulkan and Present on D3D12.

Out of reach without the vendor's driver, so ablation stays the honest substitute and the docs
should say so:
- Instruction-level shader profiling (Nsight's Shader Profiler samples the program counter to
  rank SASS lines). No public API on any vendor exposes PC sampling.
- Warp-state and occupancy stall reasons, for the same reason.

Nsight Graphics has no shader debugger and no LLM-facing surface, both of which this project has.

## Metal

The Metal capture library (`src/metal/`) reaches the Inspect and Capture panels through the same
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
      `captureTextures`, `captureBuffers`, `profilePasses`, `captureSampledTextures`,
      `maxSampledTextureTotal`.
- [x] Sampled texture read-back (`QueueTextureCapture` in `src/metal/src/capture.mm`): every texture a
      draw or dispatch bound, whole (all mips, all slices), read once per capture and blitted at
      the end of the pass the bind was in, carried on the binding command as `textureData`. It is
      what lets a debugged fragment sample what the GPU sampled.
- [x] The shader debugger on a Metal capture: the Metal Shading Language interpreter in
      `src/app/src/renderer/msl/`, with the sessions in `src/app/src/renderer/metal/shader_debug.ts`.
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
- [x] Overdraw measured while capturing (`src/metal/src/overdraw.mm`): every render pass drawn again
      right after the application's encoder ends, with counting copies of its pipelines, with and
      without its depth and stencil tests. Heatmaps in the pass's details, the measured figure in
      the pass header and GPU Bottlenecks, kept in capture files, `get_overdraw` in the MCP server.
- [x] Run Metal overdraw on a Mac: the heatmaps show in the pass details (2026-09-11).
- [ ] Metal overdraw on a Unity player, and against the pass's `fragmentsPassed` counter.
- [x] Pixel history for Metal, the same way (`src/metal/src/pixel_history.mm`): a pixel picked in a
      capture captures the next frame with every pass that renders to the texture drawn again one
      draw at a time at the pixel, into copies of its attachments, with a one-pixel scissor,
      visibility results in counting mode, and cull mode and depth-stencil state varied on the
      encoder. The same JSON as `vkinsp_replay --pixel-data`, the same tab, `get_pixel_history`.
- [x] Run Metal pixel history on a Mac (2026-09-12).
- [ ] Metal pixel history on a Unity player, and a pass that loads rather than clears.
- [ ] Metal pixel history, the rest: multisampled and layered passes, indirect command buffers'
      draws, and writes outside render passes (blits, compute).
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
- [x] Function constants in the capture (`src/metal/src/function_constants.mm`): MTLFunctionConstantValues
      has no getters, so its setters are hooked and the values ride along on the tracked MTLFunction.
      The shader debugger specializes the invocation with them, so a `[[function_constant]]`-guarded
      variant steps the branches the draw used. `is_function_constant_defined` answers from them.
- [ ] Function constants on an entry point's *arguments* (`[[function_constant(isEnabled)]]` on a
      parameter), which decide whether the argument exists at all: the debugger binds it regardless.
- [ ] The shader debugger on a Unity player's Metal shaders, which are generated MSL rather than
      hand-written: the parser's coverage is what to watch (`src/app/test/vectors/msl/`).

## Direct3D 12

The D3D12 capture library (`src/d3d12/`) reaches the Inspect and Capture panels through the same
protocol as the Vulkan layer. What it lacks is what the replay does for Vulkan, and what the
library does not read back yet.

- [ ] Shader debugger: the interpreters are SPIR-V's (`renderer/spirv/`) and MSL's
      (`renderer/msl/`); DXIL has none. Either a DXIL interpreter behind the `DebugProgram` seam,
      or the HLSL compiled to SPIR-V with `dxc -spirv` and stepped in the SPIR-V one, with the
      D3D12 bindings mapped to sets and bindings.
- [x] Overdraw and pixel history, by the Metal route: measured while capturing, inside the
      application (`src/d3d12/src/overdraw.*`, `pixel_history.cpp`, `pass_record.h`). The pass is
      issued again on the application's own command list with a counting pixel shader, and one
      pixel is followed draw by draw under occlusion queries.
- [ ] The rest of the replay-based analyses — draw overlays, mesh output, per-draw timings and
      counters (**Measure draws**), shader cost by ablation (**Measure shader**) — which
      `vkinsp_replay` does for Vulkan captures only. The same in-application route fits them.
- [x] Stencil read-back: plane 1 of a depth-stencil target, beside its depth (`--stencil` in
      `test/d3d12_triangle`, the `d3d12-stencil` UI case). A multisampled stencil is not resolved.
- [ ] The contents of sampler feedback, video, work graph and raytracing objects; enhanced
      barriers (`Barrier`) beyond the layouts that map to legacy states.
- [ ] A descriptor table set in a bundle before the bundle set its own root signature is recorded
      without contents (bundles inherit the caller's root signature).
- [ ] 32-bit targets: only x64 processes are injected.
- [x] Catching an application started elsewhere: `dxinsp_launch.exe --watch <image>` polls for the
      process and injects it while it is held suspended, which is what D3D12 has in place of an
      implicit layer (the "wait for an application" launch target, and `wait_for_app`).
- [ ] Attaching to a process that already has a device: injection still has to happen at process
      start, because the hooks go on the entry points and D3D12 cannot enumerate an existing device.
- [ ] DXIL reflection and disassembly without `dxcompiler.dll` on the machine: ship it beside the
      library, or parse the DXIL container's reflection part in the library.
- [x] Automated test: `tools/ui_tests.py` runs five D3D12 cases over `dxinsp_triangle` (a plain
      capture, a render pass, a bundle, an offscreen frame with no swap chain, and opening a saved
      capture), the way the Vulkan cases run.

## iOS devices

Inspecting a Unity iOS player on a device with the full inspector UI, without changing the Unity
project or the Xcode project it generates. The macOS design carries over unchanged in principle:
dyld honours `DYLD_INSERT_LIBRARIES` on iOS for a process signed with `get-task-allow` (every
development-profile build), which is how Xcode itself inserts `libMTLCapture.dylib` for GPU Frame
Capture; `__DATA,__interpose` and the class hooks work the same; and the transport already binds
`127.0.0.1` and listens (`src/metal/src/transport.mm`), which is exactly what a USB port forward
(usbmux, the iOS `adb forward`) connects to, with no Local Network permission prompt.

What is unknown until measured on a device is where the library may live and whether iOS accepts
its signature. iOS refuses a library signed by a different team ("mapping process and mapped file
(non-platform) have different Team IDs"), so the library must be signed with the **same team** as
the application. Everything below is done on the Mac; this Windows machine cannot build any of it.

Limits known up front: App Store, TestFlight, Ad Hoc and Enterprise builds lack `get-task-allow`
and would need re-signing with a development profile first; the device needs Developer Mode on
(Settings > Privacy & Security); a Mac is the host (codesign, devicectl, the signing identity).

### Setup (once)
- [ ] Tools: Xcode 16 or later (`xcrun devicectl`), CMake, and a USB port forwarder, either
      `brew install libimobiledevice` (`iproxy`) or `pipx install pymobiledevice3`. pymobiledevice3
      also answers "where is this app installed" (`apps query`), which step 0 needs.
- [ ] The device plugged in over USB, trusted, Developer Mode on, Auto-Lock off (a locked device
      suspends the app and its socket).
- [ ] The device's identifier: `xcrun devicectl list devices` (the Identifier column), and its
      UDID for iproxy / pymobiledevice3: `xcrun xctrace list devices` or Finder.
- [ ] The signing identity and team:
      ```sh
      security find-identity -v -p codesigning     # "Apple Development: Name (XXXXXXXXXX)"
      ```
      The team the library must match is the one the app is signed with (step below), which is not
      necessarily the ID in parentheses above.
- [ ] A Unity iOS player built the normal way: Unity > Build for iOS, open `Unity-iPhone.xcodeproj`,
      Signing & Capabilities with *Automatically manage signing* and your team, build and run it
      once from Xcode so it is installed and known to work. Then take the built bundle from
      DerivedData (Xcode > Product > Show Build Folder in Finder, `Products/<config>-iphoneos/`),
      or build from the command line:
      ```sh
      xcodebuild -project Unity-iPhone.xcodeproj -scheme Unity-iPhone -configuration Release \
        -destination 'generic/platform=iOS' -derivedDataPath build/ios -allowProvisioningUpdates \
        DEVELOPMENT_TEAM=<team>
      # -> build/ios/Build/Products/Release-iphoneos/<product>.app
      ```
- [ ] Record what the app is signed with:
      ```sh
      APP=path/to/<product>.app
      codesign -dv --verbose=4 "$APP" 2>&1 | grep -E 'TeamIdentifier|Authority'
      codesign -d --entitlements - --xml "$APP" > app-entitlements.plist
      plutil -p app-entitlements.plist             # must contain "get-task-allow" => true
      /usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$APP/Info.plist"
      ```
      No `get-task-allow`: the profile is not a development one, and nothing below can work.

### Step 0: will iOS load an inserted library at all, and from where
A probe library that only logs, so the answer does not depend on porting `src/metal/`. The same
experiment `src/metal/README.md` records for macOS signing; put the resulting table beside it.
- [ ] Build and sign the probe:
      ```c
      // probe.c
      #include <os/log.h>
      #include <stdio.h>
      #include <unistd.h>
      __attribute__((constructor)) static void probe(void) {
          os_log(OS_LOG_DEFAULT, "[mtlinsp-probe] loaded into pid %d", getpid());
          fprintf(stderr, "[mtlinsp-probe] loaded into pid %d\n", getpid());
      }
      ```
      ```sh
      xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=15.0 -dynamiclib \
        -install_name @rpath/libprobe.dylib probe.c -o libprobe.dylib
      codesign --force --timestamp=none --sign "Apple Development: <you>" libprobe.dylib
      codesign -dv libprobe.dylib 2>&1 | grep TeamIdentifier    # must equal the app's
      ```
- [ ] How to see the result: the constructor's line in Console.app (pick the device in the sidebar,
      filter `mtlinsp-probe`, start streaming before launching), or on stdout with `--console` on the
      launch below. **A library dyld cannot load is fatal**: the app dies at launch, and the reason
      (`could not load inserted library`, `code signature invalid`, `different Team IDs`) is in the
      `--console` output, in Console.app under the `kernel` / `amfid` processes, or in the crash
      log (Xcode > Window > Devices and Simulators > Open Recent Logs). A launch that runs normally
      with no probe line means dyld dropped the variable.
- [ ] The launch command, used for every case (check `xcrun devicectl device process launch
      --help` if a flag has been renamed):
      ```sh
      xcrun devicectl device process launch --device <identifier> --terminate-existing --console \
        --environment-variables '{"DYLD_INSERT_LIBRARIES":"<path>"}' <bundle-id>
      ```
      Add `"DYLD_PRINT_LIBRARIES":"1"` to see on the console whether dyld honours `DYLD_*`
      variables for the process at all.
- [ ] Case A, in the bundle (re-signs a copy of the app; the Unity and Xcode projects stay as they
      are):
      ```sh
      cp -R "$APP" Probe.app
      cp libprobe.dylib Probe.app/Frameworks/
      codesign --force --timestamp=none --sign "Apple Development: <you>" \
        --entitlements app-entitlements.plist Probe.app
      codesign --verify --deep --strict Probe.app
      xcrun devicectl device install app --device <identifier> Probe.app
      ```
      - [ ] A1: `DYLD_INSERT_LIBRARIES=@executable_path/Frameworks/libprobe.dylib`
      - [ ] A2: the absolute path: the install location from `pymobiledevice3 apps query
            <bundle-id>` (its `Path`), then `<Path>/Frameworks/libprobe.dylib`. The UUID in it
            changes with every install.
- [ ] Case B, in the app's data container (no re-signing, the app exactly as Xcode built it;
      reinstall the original `$APP` first):
      ```sh
      xcrun devicectl device copy to --device <identifier> --domain-type appDataContainer \
        --domain-identifier <bundle-id> --source libprobe.dylib --destination Documents/libprobe.dylib
      ```
      - [ ] B1: `DYLD_INSERT_LIBRARIES=<Container>/Documents/libprobe.dylib`, `Container` from
            `pymobiledevice3 apps query <bundle-id>`. If this loads, the inspector never needs to
            touch the application bundle, which is the better product.
- [ ] Controls, so a failure is understood rather than guessed at:
      - [ ] the probe ad-hoc signed (`codesign --force --sign - libprobe.dylib`) in the winning
            placement: expected to be refused, and shows what a signature refusal looks like;
      - [ ] if devicectl turns out not to pass the variables, the same case launched from Xcode
            instead: in the Unity Xcode project, Product > Scheme > Edit Scheme > Run > Arguments >
            Environment Variables, with Diagnostics > *Metal API Validation* and Options > *GPU Frame
            Capture* set to Disabled so Xcode inserts nothing of its own. The scheme only affects
            the launch, not the build, so this still leaves the build uninstrumented.
- [ ] Write the results down (placement × path form × launcher → loaded / refused and the message)
      in `src/metal/README.md` next to the macOS table. **No case loads: stop here**; Route A is not
      possible and the remaining option is Xcode's `.gputrace` via LLDB.

### Step 1: build the capture library for iOS
- [ ] Configure and build with the iOS SDK:
      ```sh
      cmake -S metal -B build/ios -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
        -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo
      cmake --build build/ios
      # -> build/ios/bin/libmtlinsp_capture.dylib
      ```
- [ ] Fix what the iOS SDK marks unavailable, each behind `#if TARGET_OS_OSX` (or `@available` /
      `respondsToSelector:` where iOS has the API from some version on). Found by reading, so the
      compiler may find more:
      - `interpose.mm`: `MTLCopyAllDevices`, `MTLCopyAllDevicesWithObserver`,
        `CGDirectDisplayCopyCurrentMetalDevice` and the `CGDirectDisplayMetal.h` import are
        macOS-only; iOS keeps `MTLCreateSystemDefaultDevice` and the `nextDrawable` fallback.
      - `hooks_descriptors.mm:521`: `lowPower`, `headless`, `removable` are macOS-only;
        `registryID` depends on the iOS version.
      - `capture.mm:895` and `capture.mm:1329`, `formats.mm:318`: `MTLStorageModeManaged` and the blit
        encoder's `synchronizeResource:` do not exist on iOS (no managed storage; shared buffers
        read directly). Hooking the `synchronizeResource:` selector in `hooks_encoders.mm` is
        harmless, since the class simply has no such method.
      - `frame_stats.mm` `QueryDisplayRefreshMs`: no CoreGraphics display modes on iOS; ask
        `UIScreen.mainScreen.maximumFramesPerSecond` through the runtime, the way `NSScreen` is
        asked now (60 or 120 with ProMotion).
      - `gpu_trace.mm` `DefaultPath`: no Desktop; write under `NSTemporaryDirectory()` and fetch with
        `xcrun devicectl device copy from --domain-type appDataContainer ... --source tmp/<file>`.
      - `CMakeLists.txt`: guard the CoreGraphics link if nothing else needs it.
- [ ] Sign it with the same identity as the probe, and check its TeamIdentifier.

### Step 2: inject it into the Unity player and connect
- [ ] Put `libmtlinsp_capture.dylib` where step 0 said a library loads from (Frameworks + re-sign,
      or the data container), and launch:
      ```sh
      xcrun devicectl device process launch --device <identifier> --terminate-existing --console \
        --environment-variables '{"DYLD_INSERT_LIBRARIES":"<path>","MTLINSP_PORT":"47531","MTLINSP_LOG":"1","METAL_CAPTURE_ENABLED":"1"}' \
        <bundle-id>
      ```
      Expect `[mtlinsp] loaded into pid …` then `[mtlinsp] listening on 127.0.0.1:47531` on the
      console.
- [ ] Forward the port over USB, in a second terminal, and leave it running:
      ```sh
      iproxy 47531:47531 -u <udid>            # libimobiledevice 1.3+; older: iproxy 47531 47531 <udid>
      # or: pymobiledevice3 usbmux forward 47531 47531
      ```
- [ ] Connect the UI: **Connect** in the launch bar with port 47531, or from `src/app/`:
      `npm start -- --connect=47531`. The MCP server's attach should work the same way.
- [ ] What to check, in order, noting anything that differs from a Mac:
      - [ ] Inspect: the snapshot arrives; device, queues, buffers, textures, pipelines listed.
            Record the concrete class names (the `AGX…Device` family for the device's GPU, and the
            `MTLDebug*` ones with `MTL_DEBUG_LAYER=1`) in the class-tree table in `src/metal/README.md`.
      - [ ] Frame capture of one frame: commands per command buffer and pass, render targets read
            back, vertex and index buffers, pass timings.
      - [ ] Pixel formats a phone uses that a Mac player does not: ASTC textures, `BGRA8_sRGB`
            drawables, memoryless depth (cannot be read back, must not crash).
      - [ ] Frame Stats: frame time and the refresh rate (60 / 120).
      - [ ] Validation layer: add `"MTL_DEBUG_LAYER":"1","MTL_DEBUG_LAYER_ERROR_MODE":"nslog",
            "MTL_DEBUG_LAYER_WARNING_MODE":"nslog"` to the environment (what `captureEnvironment`
            in `src/app/src/main/metal.ts` sets on a Mac).
      - [ ] Stack traces (`MTLINSP_STACKTRACES=1`): addresses symbolize against the dSYMs Xcode
            wrote for `UnityFramework`, which the host needs via `set_search_paths`.
      - [ ] Overdraw, pixel history and Xcode Trace (then `devicectl device copy from` the
            `.gputrace` and open it in Xcode).
      - [ ] Memory: a full capture's read-back on a phone near jetsam's limit; watch for the app
            being killed and lower `maxBufferTotal` / `maxTextureSize` as the Android note suggests.
      - [ ] Backgrounding the app and returning: the listener survives, and the UI can reconnect.

### Step 3: make it a launch target (only once step 2 works)
- [ ] `src/app/src/main/ios.ts`, the counterpart of `metal.ts` and the Android launcher: list devices
      (`xcrun devicectl list devices --json-output`), check the app's `get-task-allow` and team,
      sign and place the library the way step 0 found works, install, launch with the environment,
      forward the port, connect. The usbmux forward can be spoken directly (a plist protocol over
      `/var/run/usbmuxd`) instead of depending on iproxy.
- [ ] The launch dialog's *Run On* lists iOS devices beside Android ones; a `launch_ios_app` MCP
      tool beside `launch_android_app`.
- [ ] The iOS library built by CI and staged into the macOS app's resources
      (`src/app/tools/stage_layer.mjs`), signed at launch time with the user's identity rather than
      the project's (a Developer ID signature is a different team from the app's).
- [ ] `docs/IOS.md`: requirements (development-signed build, Developer Mode, same team), and a
      Troubleshooting section built from step 0's failure messages.

## Distribution
- [ ] Code-sign the Windows installer and the layer DLL (SmartScreen warns on unsigned installers).
- [ ] AppImage / rpm targets next to the .deb (electron-updater supports both).
- [x] macOS build of the UI, signed with the project's Developer ID and notarized.

## Tooling
- [ ] UI tests: cases for the Unity player and Android devices when attached (the saved-capture
      mode covers their captures), image comparison of the screenshots against references.
- [ ] Help links to docs from the panels.
