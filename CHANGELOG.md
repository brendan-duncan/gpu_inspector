## 0.8.0

### Added
- Render Graph: a capture's passes and the resources that connect them, from the "Render Graph"
  button in the capture bar. Attachments, the descriptor sets bound at each draw and dispatch and
  the transfer commands are rolled up per pass into a dependency graph, keyed on the mip level and
  array layer each pass touched and versioned per write, so a mip chain is a chain and an
  attachment a pass loads and stores again is not a cycle. Shown as a resource lifetime chart —
  passes along the top in execution order, one row per resource, marked where each pass reads or
  writes it — with the selected pass' immediate producers and consumers drawn as a node-link
  diagram beside it, its GPU time, the frame's critical path, resources read from before the
  capture, and passes whose output nothing in the capture reads. Vulkan and Metal.
- Metal capture library (`metal/`, macOS, Apple Silicon verified): injected with
  `DYLD_INSERT_LIBRARIES`, hooks the driver's classes rather than wrapping objects, and speaks
  the Vulkan layer's protocol so the Inspect and Capture panels work unchanged. Tracks the
  device, queues, buffers, textures and views, heaps, libraries and functions, samplers,
  depth-stencil states, pipelines in every creation spelling, fences, events, argument encoders
  and indirect command buffers, with `DeleteObjects` from a `dealloc` hook. Records about two
  hundred selectors across the command buffer and the render, compute and blit encoders; reads
  back colour and depth attachments (multisample through the resolve), bound buffers in shared,
  managed and private storage, and inline constant blocks; times every pass with a counter
  sample buffer. Frames end at the commit of the presenting command buffer on both
  `presentDrawable:` and `[drawable present]` paths, the latter being what Unity's player uses.
- Launch dialog on macOS launches a `.app` with the Metal library, and says how to re-sign a
  hardened-runtime target that dyld would otherwise silently refuse.
- Metal pipeline reflection: every pipeline creation asks Metal for argument and buffer-type
  reflection, so a draw's stage buffers and inline constant bytes show as named, typed fields
  with the Format editor, and pipeline objects get a Reflection section per stage in Inspect.
- Metal frame stats (frame time, submit time, refresh rate) for the session bar and Frame
  Stats, and the capture bar's options (frame to capture at, buffer and texture limits, the
  read-back and profiling switches) honoured by the Metal library.
- Metal validation messages: command buffer errors with the faulting encoder, Metal's
  validation layer through the launch dialog's "Validation layer" switch, and shader logs, in
  the Inspect panel's message list with repeat counts; a leak report at process exit.
- Metal in Inspect: functions and the device listed by their own names, a Functions section on
  libraries linking to their function objects, pipelines linked to the functions they use, and
  Metal Shading Language highlighting (also for the cross-compiled MSL view of Vulkan shaders).
  Captured Metal commands show their key arguments beside the name: labels, draw counts, bound
  slots and objects, a pass's target.
- Metal stack traces: where each object was created and, with the capture bar's switch, where
  each captured command was issued, symbolized on demand through the dynamic linker with the
  library's, Metal's and the driver's frames folded away.
- Metal argument buffers decoded in a draw's details: each member resolved to the buffer (with
  offset), texture or sampler it holds, from the pipeline's reflection and the GPU address or
  resource id every object now reports.
- Metal draw details: only the stage buffer slots the bound pipeline's shaders read are listed,
  the rest folded into one line (an engine leaves dozens bound, most to nothing); bindings the
  compiler dropped count as unread; no Vulkan "no descriptor sets" note in a Metal capture; and
  the pipeline's reflection now reaches the buffer views, which had read it from the wrong
  place.
- Metal functions in Inspect show their library's source, scrolled to the definition.
- Metal memory: every buffer's and texture's allocatedSize, heap and offset, purgeable and
  aliasable state (kept current as the application changes them), heaps with their usage, and
  the device's allocated total against its recommended working set in the Inspect memory meter.
- Metal pass counters: the vertex and fragment stages' own spans beside every render pass's
  duration, and the GPU's statistic and stage-utilization counter sets over each pass
  (invocations, clipper counts, cycles per stage) in the pass header's tooltip.
- Frame Issues for Metal captures: undefined and first-use loads, unread stores, multisample
  stores, memoryless candidates, mergeable back-to-back passes, redundant binds, tiny draws and
  single-threadgroup dispatches, read off the pass descriptors' load and store actions.
- "Xcode Trace" in the capture bar on macOS writes the next frame as a .gputrace document for
  Xcode's Metal debugger. The Metal test application now draws through a private-storage vertex
  buffer, a multisampled pass with a resolve through a parallel encoder, and a sampled textured
  pass with inline constants, so every read-back path has a test.

### Changed
- The capture's whole-frame reports (Frame Stats, Analyze Shaders, Shader Flame Graph, Render
  Graph) moved from four buttons into one "Reports" menu at the right of the filter row, which
  the filter field now fills. Four buttons wrapped the row, and the menu holds however many
  reports there come to be; the entry whose report is showing is marked.

### Fixed
- `--launch` started the application twice: it was acted on by two separate handlers, so every
  run left a stray process behind, and the second launch ignored `--args`, `--validation` and
  the rest, which left the UI tests checking the wrong session.
- `--screenshot` could hang the run instead of writing a shot: `capturePage()` rejects with
  `UnknownVizError` when the GPU process will not produce a frame (which is what happens with
  the process's output redirected, as the UI tests run it), and the unhandled rejection skipped
  the `--quit-after-screenshot` quit. A failed or slow capture is now reported and skipped.

## 0.7.0

### Added
- macOS build of the user interface (`npm run dist:mac`, `GPU-Inspector-<version>-arm64.dmg`
  and `-x64.dmg`, published by the release workflow): inspects Android devices over adb and
  opens `.gpucap` files. There is no Apple build of the capture layer, so the launch dialog
  offers only the Android target there and says why. The download is signed with the project's
  Apple Developer ID and notarized, so it opens without a Gatekeeper warning.
- Source roots (launch dialog, `--source-roots`): a shader with line information but no
  embedded text gets its Source view, line costs and findings from the file its debug
  information names, looked up under the roots. The triangle test app's compute shader ships
  that way (`tools/strip_shader_source.py`).

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
