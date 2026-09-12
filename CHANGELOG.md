## v0.10.0

### Added
- Pixel history: every clear and draw that touched a pixel, what each draw's fragments met, and the
  pixel's value after each. A Vulkan capture is replayed for it, a Metal application follows the
  pixel while capturing. `get_pixel_history` in the MCP server.
- A capture's render target in a tab of its own: the image, the pass's overdraw over it with the
  counts under the pointer, and the clicked pixel's history beside it.
- Shader editor: `#include` is resolved against the session's source roots.
- Buffer layouts name their fields from a module's Vulkan debug information where it has no
  `OpName` / `OpMemberName`, instead of `member0`, `member1`.
- Shader Flame Graph: fragment stages are weighted by the fragment invocations a pass's GPU
  counters measured, not by the scissor area, where the capture has them.
- Depth rejection for Vulkan captures: the layer runs an occlusion query around each render pass,
  counting the samples that passed its depth and stencil tests, so the GPU Bottlenecks report's
  depth rejection column and the `late-depth-rejection` rule work for Vulkan as well as Metal.
- Multisampled stencil read-back: the depth resolve now carries both aspects of a depth-stencil
  image.
- Per-draw timing and counters for Vulkan captures (`vkinsp_replay --draws`): **Measure draws** in
  the Shader Flame Graph replays the frame with a timestamp pair and a pipeline statistics query
  around every draw and dispatch. A pass's measured time is then split between its draws by what
  each was timed at, and each stage takes its measured invocation count. `get_shader_flame_graph`
  measures on first use, and capture files keep the measurements.

### Fixed
- Applications that enable multiview (every Unity player does) lost their GPU pass counters
  entirely: the layer skipped pipeline statistics for the whole device rather than for the passes
  that actually render several views. Only those passes go uncounted now, so a Unity capture has
  overdraw, fragments per primitive and the rest of the GPU Bottlenecks report.
- `vkinsp_replay --draws` measured nothing for a frame whose draws are recorded into secondary
  command buffers, which is how a Unity player records every draw: they are measured there too.
- Captures of applications that recreate their swapchain (every Unity player does, on its first
  resize) lost the frame's final image. The driver hands the new swapchain its predecessor's image
  handles, and destroying the old swapchain took those images, and the views over them, out of the
  object graph; the passes drawing into the backbuffer were then captured without their colour
  attachment, and `vkinsp_replay` left them out. A Unity frame now replays with no problems and
  every render target identical.

## v0.9.0

### Added
- `vkinsp_replay` (`replay/`, [docs/REPLAY.md](docs/REPLAY.md)) re-executes a Vulkan capture on this
  machine's GPU and compares every read-back render target byte for byte (the test triangle and a
  Unity frame replay identical). `--overdraw` measures a pass's fragments per pixel, `--pixel` gives
  one pixel's history, `--dump` writes the captured, replayed and difference images.
- Overdraw: how many fragments landed on each pixel of a pass, with and without its depth and
  stencil tests. A Metal application measures it while capturing (**Overdraw** in the capture bar,
  [metal/README.md](metal/README.md)); a Vulkan capture is replayed for it. Heatmaps in the pass's
  details, the figure in pass headers and GPU Bottlenecks, kept in capture files, `get_overdraw`.
- Claude Code plugin (`claude-plugin/`): an MCP server over saved `.gpucap` files and running
  applications — the capture summary, Frame Issues, GPU Bottlenecks, the render graph, commands and
  the state bound at a draw, objects, validation, images, buffers, vertices, shaders (reflection,
  source, cross-compiled text, disassembly, analysis, the Shader Flame Graph), capture comparison,
  and live sessions (launch or attach, Android over adb, frame statistics, captures, shader
  replacement). Shader sources and stack symbols come from this machine (`set_search_paths`). A
  capture analysis skill and five commands; one dependency-free file on Node.js 18+.
- `CaptureComplete` ends a capture's stream, so a client knows when a capture has fully arrived.

### Fixed
- Shader editing failed on applications that destroy their shader modules once their pipelines
  exist: the stages that were not edited are now rebuilt from the SPIR-V the layer keeps.

## 0.8.0

### Added
- Render Graph (capture bar, Vulkan and Metal): the frame's passes and the resources connecting
  them, from attachments, descriptor sets and transfers, keyed per subresource and versioned per
  write. A resource lifetime chart, the selected pass' producers and consumers as a node-link
  diagram, GPU times, the critical path, resources read from before the capture, and passes whose
  output nothing reads.
- Render graph suggestions, in that view and in Frame Issues, shared by both APIs: attachments
  stored but never read, results replaced before anything reads them, targets only the next pass
  reads, back-to-back passes that are one pass, and barriers that synchronize nothing.
- Metal capture library (`metal/`, macOS, Apple Silicon verified,
  [metal/README.md](metal/README.md)): injected with `DYLD_INSERT_LIBRARIES`, hooks the driver's
  classes rather than wrapping objects, and speaks the Vulkan layer's protocol, so Inspect and
  Capture work unchanged. Object tracking and lifetime, about two hundred recorded selectors,
  colour and depth read-back (multisample resolved), buffers in every storage mode, inline
  constants, and per-pass timings. Frames end at the presenting command buffer's commit, including
  the `[drawable present]` path Unity's player uses.
- Launch dialog on macOS launches a `.app` with the Metal library, and says how to re-sign a
  hardened-runtime target that dyld would otherwise silently refuse.
- Metal pipeline reflection: a draw's stage buffers and inline constants show as named, typed
  fields with the Format editor, and pipelines get a Reflection section per stage. Only the slots
  the bound shaders read are listed, the rest folded into one line.
- Metal frame stats (frame time, submit time, refresh rate), and the capture bar's options honoured
  by the library.
- Metal validation: command buffer errors with the faulting encoder, Metal's validation layer
  through the launch dialog, shader logs, and a leak report at process exit.
- Metal in Inspect: functions and the device under their own names, a Functions section on
  libraries, pipelines linked to the functions they use, functions showing their library's source,
  Metal Shading Language highlighting, and each captured command's key arguments beside its name.
- Metal stack traces of object creations and, with the capture bar's switch, of captured commands,
  symbolized through the dynamic linker with the library's, Metal's and the driver's frames folded
  away.
- Metal argument buffers decoded in a draw's details: each member resolved to the buffer (with
  offset), texture or sampler it holds.
- Metal memory: every buffer's and texture's allocated size, heap and offset, purgeable and
  aliasable state, heaps with their usage, and the device's total against its working set.
- Metal pass counters: the vertex and fragment stages' spans beside each pass's duration, and the
  GPU's statistic and stage-utilization counters in the pass header's tooltip.
- Frame Issues for Metal captures, off the pass descriptors' load and store actions: undefined and
  first-use loads, unread stores, multisample stores, memoryless candidates, mergeable passes,
  redundant binds, tiny draws and single-threadgroup dispatches.
- "GPU Bottlenecks" in the Reports menu (Vulkan and Metal,
  [docs/PROFILING.md](docs/PROFILING.md)): every pass measured in the terms a bottleneck is
  described in — pixels shaded per pixel, fragments per triangle, the stage it waits on, and depth
  rejection (the last two Metal only) — each with what causes it and what to try. Slowest first,
  linked to their commands; a missing measurement says so rather than showing zeroes.
- Vulkan pass counters: a pipeline statistics query alongside the layer's timestamps
  (`layer/src/pipeline_stats.h`), so the bottleneck report and its rules work for Vulkan captures.
  `VKINSP_NO_PIPELINE_STATISTICS=1` turns it off.
- Four Frame Issues rules from those measurements: `high-overdraw`, `microtriangles`,
  `late-depth-rejection` (Metal only) and `unmipped-texture`. Pass headers gained the same figures
  in their tooltips.
- Compressed textures decode in the image viewer: BC6H and BC7, ETC2 and EAC, every ASTC footprint,
  and PVRTC, beside the BC1-BC5 already there; Metal captures read those formats back too. The
  decoders are checked against an independent decoder (`npm test` in `app`).
- "Xcode Trace" in the capture bar on macOS writes the next frame as a .gputrace document for
  Xcode's Metal debugger.

### Changed
- The capture's whole-frame reports (Frame Stats, Analyze Shaders, Shader Flame Graph, Render Graph)
  moved from four buttons into one "Reports" menu, which marks the report being shown.

### Fixed
- Metal compute passes showed no GPU time: the pass block was filed under the render pass' key.
- `--launch` started the application twice, leaving a stray process behind and ignoring `--args`,
  `--validation` and the rest on the second launch.
- `--screenshot` could hang the run instead of writing a shot: a rejected `capturePage()` skipped
  the `--quit-after-screenshot` quit. A failed or slow capture is now reported and skipped.

## 0.7.0

### Added
- macOS build of the user interface (`npm run dist:mac`, published by the release workflow):
  inspects Android devices over adb and opens `.gpucap` files, signed and notarized. There is no
  Apple build of the capture layer, so the launch dialog offers only the Android target there.
- Source roots (launch dialog, `--source-roots`): a shader with line information but no embedded
  text gets its Source view, line costs and findings from the file its debug information names.

## 0.6.0

### Added
- Frames without a swapchain (OpenXR): frame boundaries from the application's fence waits or
  submissions; `VKINSP_FRAME_BOUNDARY=wait|submit` forces one. Triangle app: `--offscreen`.
- Refresh estimate picks the best-fitting common rate (tolerates fence-wait jitter); the layer
  logs refresh rate changes; capture files keep the frame boundary and refresh source.
- Multiview: a pass's view mask layers are read back.
- Frame Issues in Frame Stats: `clear-outside-pass`, `color-load`, `depth-store`,
  `depth-transient`, `msaa-store`, `stereo-without-multiview`, `barrier-in-render-pass`,
  `barrier-adjacent`, `redundant-pipeline-bind`, `redundant-descriptor-bind`,
  `redundant-buffer-bind`, `push-constants-unchanged`, `single-workgroup-dispatch`,
  `full-pipeline-barrier`, `msaa-sampled`, `tiny-draws`, with severity and per-rule filters.
  Flagged commands carry a marker in the command list and a Performance section in their
  details.
- OpenXR test app for headsets (`test/xr_triangle`, `tools/build_xr_triangle.py`) as two
  packages: multiview, and a deliberately slow twin for Frame Issues and Analyze Shaders.
  Verified on a Quest 3.
- Android: layer on an abstract socket (no INTERNET permission needed), layer package
  force-queryable. Verified on a Pixel 8 Pro.
- Shader analysis rules: loop-invariant computation and workgroup memory size.
- Shader cost per source line, clickable into the Source view; line frames in the flame graph.
- Sampled image read-back of every mip level.
- Launch dialog: the Package field filters the device's packages.
- Applications started elsewhere: the launch dialog registers the layer as an implicit layer
  for the user ("Register", per user, undone with "Unregister") and "Wait" opens a session
  that connects when an application started with `VKINSP_ENABLE=1` and `VKINSP_PORT` loads
  it. `--wait-for-app` and `--implicit-layer=on|off` from the command line.
- `tools/ui_tests.py`: end-to-end checks of the UI against the triangle application (capture,
  MSAA, present-less frames, validation and hazard links, stack traces) and saved captures
  with expected findings, through the new `--debug-dump` testing aid.
- Phone test application (`test/android_triangle`, `tools/build_android_triangle.py`): a
  debuggable NativeActivity rendering a ring of triangles into a swapchain, for the Android
  path without a third-party app.
- Stack traces on Android: the launcher turns the layer's stack collection on, and frames the
  layer can only name by module and offset are resolved on this machine with the NDK's
  llvm-symbolizer against the unstripped libraries under the launch dialog's new "Symbol
  directories" (functions, files, lines and the callers a function was inlined into, kept in
  capture files).
- Android launch diagnostics in the Log: a device that is asleep (an OpenXR session stays
  idle), and a headset shell's "controllers required" dialog blocking launches.
- "Sync validation" launch option (with the validation layer): hazards reported at
  vkQueueSubmit are linked to the command the message names, like record-time messages; the
  per-submission counters in such messages no longer make every frame a new message. The
  triangle test app's `--hazard` option writes its vertex buffer unsynchronized.

- Render targets with storeOp DONT_CARE read back correctly: while capturing (or under
  "Record all command buffers"), a render pass runs as a store-everything copy (dynamic
  rendering gets its attachments rewritten). Object records and captured commands keep the
  application's own arguments, without the layer's usage additions.
- Command buffers recorded before the capture began (engines that record once and resubmit,
  with "Record all command buffers") get their render targets read back after their
  submission. The triangle test app's `--prerecord` option works that way.
- The desktop triangle test app no longer stores its depth buffer.

### Fixed
- Android launch on a Quest reported the successful `am start` output as an error.
- Android launches could stay at "connecting" until Relaunch, or connect to an application
  launched earlier and left running on the same port: the layer's socket now carries the
  package name, the launcher waits for the previous instance to exit, the layer retries its
  bind for 30 s, and a port forward lost to a device reconnect is re-created.

## 0.4.0

### Added
- Device sections in the Inspect tab: properties, limits, memory, queue families, features,
  extensions; physical device names in the object list.
- "Affected by" on captured buffers: the frame's earlier writes to them.
- Leak report on device or instance destruction; triangle app `--leak`.
- Static shader analysis on SPIR-V: Shader Cost and Performance Analysis sections, "Analyze
  Shaders" over a capture.
- Shader editor: line numbers, find bar, compile errors marked on their lines.
- "At frame" capture field; recent capture files in the Recent menu.
- Multisampled color and depth read-back (resolve before the copy); triangle app `--msaa`.
- Captures in windows of their own ("Open in New Window").
- Display refresh rate from `VK_EXT_present_timing`, `VK_GOOGLE_display_timing` or the Windows
  monitor mode, the interval estimate as last resort; dropped frames against the real period.
- Source view, Shader Cost and findings on captured draws and dispatches.
- Shader Flame Graph of the frame's GPU work by pass, pipeline, stage and function.
- Stack traces of object creations and captured commands, symbolized (DbgHelp / dladdr).
- Validation messages attached to the captured commands that raised them.
- Refresh rate and dropped frames in the meter, session bar, timeline and Frame Bound card.

### Changed
- Shader Reflection sections collapsed by default; the pass timeline follows the theme.

### Fixed
- Compile & Apply from the Source view failed on glslang's embedded prefix.
- Compile & Apply closed the editor by re-rendering the details panel.
- The live image viewer crashed the application under the validation layer (missing dispatch
  pointer on the layer's command buffer).
- Unaligned shader payloads in capture files left the shader section at "Loading...".

## 0.3.0

### Added
- Render pass thumbnail strip; captured render targets in the full image viewer.
- Sampled and storage images read back at capture time ("Images" option), kept in capture files.
- Reflection section on every shader payload.
- The mouse test aid accepts a sequence of points.

## 0.2.0

### Added
- Capture files (`.gpucap`): save, reopen, drag and drop, "Open in New Tab".
- Validation messages from the layer's debug-utils messenger; "Validation layer" launch option.
- Compute pass timings ("Compute N" blocks).
- Android targets through adb.
- Triangle app: compute stage and `--bad-scissor`.

### Fixed
- The launcher's `VK_LAYER_PATH` hid the SDK's validation layer.

## 0.1.0

### Added
- Vulkan capture layer generated from `vk.xml` with a TCP transport to the Electron UI.
- Live object inspection, meters, memory totals, descriptor sets, image viewer.
- Frame capture: command stream, pipeline state, reflection, buffers, render targets, Frame Stats.
- Profile passes: GPU timestamps, pass timeline, Frame Bound card.
- Shader editing (GLSL, HLSL, SPIR-V assembly) with live replacement pipelines; source maps.
- Dark and light themes; Windows and Linux builds; installers with self-update.
