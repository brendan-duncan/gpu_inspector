## v0.17.0

### Added
- A Windows launch can follow the target's own child processes, so a Chromium browser's GPU process is captured: **Follow child processes** in the launch dialog (`--type=gpu-process`), `follow` from `launch_app`, `--follow` in `dxinsp_launch`.
- Each of a capture's reports opens in a tab beside the capture's, with **Open in New Window** on the tab (a copy of the capture there, opened on that report) and on its context menu.
- Every report, and the render target tab, has an **Export to HTML** control: the report as it is on screen, with the application's styles in it, as a standalone file.
- The Shader Flame Graph zooms continuously with **Ctrl+Wheel** about the pointer and pans by dragging, besides the click zoom it already had.

### Fixed
- A capture of an application launched with a long command line is saved again: the file name is cut to the application's own part of it rather than the whole line.
- A capture whose manifest passes 512 MB can be saved and opened: it is written and read a batch of commands at a time rather than as one JSON string, which V8 could not hold (`src/app/src/renderer/utils/json_stream.ts`).
- Opening and saving a capture no longer copy the whole command list, which cost 34 MB and 40 MB on a 400k-command frame.

## v0.16.0

### Added
- The **Timeline** card zooms and pans, so a frame whose spans are sub-pixel at frame scale can be read, and clicking a pass's span selects it in the command list.
- A stretch of a timing capture dragged out on its frame-time graph: every figure and hitch below it is then of that stretch alone.
- GPU-assisted validation messages are attached to the draw or dispatch they happened in, and the same mistake from every shader invocation is folded into one message with a count.
- Stack traces find PDBs that are not beside their modules, through the launch's symbol directories, and name the functions a frame was inlined into on Windows.
- Pixel history follows the writes that happen outside a render pass: a clear, a copy, a blit, a resolve, and a dispatch or trace with the image bound.
- Pixel history names the primitive whose fragment won the pixel, follows a multisampled target through the resolve of its samples, and measures a shader that asks for early fragment tests with those tests on.
- **HUD** draws the application's frame time over its own window on all three backends, so it reads without looking away and is in any screenshot (`VKINSP_HUD=1`, `src/vulkan/src/hud_text.h`).
- **Live pause** holds the application at its frame boundary on the frame it just drew, and steps one frame at a time from there (`src/vulkan/src/frame_pause.h`).
- D3D12 lists its textures and its buffers as their own groups in the Inspect tab rather than one Resources group.
- A D3D12 capture records from the moment it is asked for, and lets a frame pass before it starts, so an engine that records its command lists ahead is captured rather than reported as unrecorded.
- A D3D12 capture says when a submitted command list holds no commands, in the log and as a frame issue, with what to turn on.
- A D3D12 frame says which render passes the application suspended across command lists, since those are the passes a capture can measure nothing of.
- `mtlinsp_triangle --compile-hitch` compiles a library and a pipeline inside every frame, and the sample reserves a heap and suballocates from it, so the Metal CPU timeline and memory breakdown have something to report.
- The Metal CPU timeline and Memory Use are checked end to end on a Mac (`tools/ui_tests.py`, the `metal-cpu-timeline`, `metal-compile-hitch`, `metal-self-compile` and `metal-memory` cases), including that the capture library's own pipeline compiles stay out of the application's timeline.
- A capture library says how long it took to send a frame, so a capture that seems to hang says whether the wait is in the application or in the app reading it.
- Metal pixel history follows a multisampled pass, which it used to decline: the copies it draws into are made with the pass's sample count and resolved before the pixel is read, so the sample's own multisampled triangle pass reports the draw that wrote the pixel and how many of its samples passed.
- Metal overdraw and pixel history are checked against a real Unity player, opt-in through `tools/ui_tests.py --unity <player.app>`.
- `mtlinsp_triangle --occluded` draws the triangles twice, the second set behind the first in a pass with a depth attachment, so an overdraw measurement has fragments the depth test must reject.
- `--debug-capture-delay=<ms>` takes the debug capture that long after the application connects, for a real application that is still on its loading screen 1.5 seconds in.

### Changed
- The outermost debug label group in the command list is blue rather than red, which read as an error on every frame that has one.
- Smaller downloads on every platform — the Windows installer 98MB rather than 113MB, the installed app 304MB rather than 377MB — from shipping one Chromium locale instead of 55 and dropping Dawn's DirectX shader compiler, which the app never loads.

### Fixed
- A frame of a million commands left the Capture tab on "Capturing..." for minutes: a command buffer and each render pass in it are now listed collapsed and their rows built when they are opened, which took one Unity frame from 172 seconds to 2.
- Capturing a D3D12 frame left a pass's pipeline-statistics and occlusion queries open on a command list the application was still recording when the capture ended, and a list closed with a query open fails with E_FAIL, which the application takes for a lost device.
- A D3D12 capture freed the staging buffers it had copied into while the application still had command lists open that named them, which fails those lists' Close the same way; they are held until the next capture.
- The Timeline drew at most 4,000 spans a lane and dropped the rest silently, which also hid those passes from its idle-gap analysis.
- Capturing a D3D12 frame added its queries and read-back copies to a render pass the application suspends across command lists, which Direct3D forbids: the list closed with E_FAIL and the application took it for a lost device and exited.
- Recording a D3D12 application's command lists asked a resource named by a stale descriptor for its description, which crashed the application: a descriptor keeps no reference to what it names, so an engine that recycles resources leaves slots naming released ones. Nothing the resource tracker has let go is touched now.
- The session bar's application command line is truncated with the whole of it in its tooltip, rather than pushing the bar's buttons onto a second row.
- The D3D12 capture library is built with debug information in every configuration, so its frames in an application's own crash report carry function names and lines.
- A D3D12 capture read every render target of every pass back with no total budget, which on a frame with thousands of passes is more work than the frame itself.
- The memory accounting asked the runtime to size a resource whose description uses tight alignment, which it refuses.
- Metal's Memory Use reported every heap as entirely empty: it looked for the heap's live usage under the key the library groups those fields by, but an `ObjectUpdate` carries them flat beside the id, so what it read was the creation-time zero.
- The Frame Bound card told the reader of a Metal or D3D12 capture that the CPU bar is time inside `vkQueueSubmit`, naming a function their application never calls.
- The Overdraw report opened on the first measured pass, which in a real frame is a prepass that drew nothing into the target being viewed and whose heatmap is an empty image; it opens on the pass with the most overdraw now.

## v0.15.0

### Added
- The shader debugger on a D3D12 capture, by recompiling the container's HLSL to SPIR-V with `dxc -spirv` and stepping that (`src/app/src/shared/hlsl_debug.ts`).
- Descriptor buffers are read (`VK_EXT_descriptor_buffer`, `src/vulkan/src/descriptor_buffer.h`), so a draw bound through one shows its bindings like any other; `test/triangle --descriptor-buffer`.
- Pipeline and shader creation is timed on the CPU timeline on all three backends, as its own *Creating pipelines* category; `test/triangle --compile-hitch`.
- The image viewer marks NaN and the infinities (**Highlight**, on by default), and draws a per-channel histogram.
- `--debug-view=target[:color|depth|<id>]` opens a render target in its own tab, for screenshots and UI tests.
- **GPU validation** in the launch dialog: GPU-assisted validation on Vulkan and the debug layer's GPU-based validation on D3D12, which catch out-of-bounds descriptor and buffer access the CPU cannot see; `test/triangle --oob`.
- **Timing Capture** records every frame's time and where its CPU went for as long as it runs, and reports the hitches with what caused each (Vulkan; `renderer/frame_timing.ts`).
- D3D12 reports dropped frames measured from the swap chain's refresh counters instead of a hard-coded zero, and the Vulkan layer's estimate now says so (`droppedMeasured`); `test/d3d12_triangle --stall <ms>`.
- Metal's Memory Use breaks down by what is holding the memory — buffers, textures and heaps — since Metal has no heap table to enumerate (`renderer/metal/metal_memory.ts`).

### Fixed
- A D3D12 draw whose pipeline came from its command list's `Reset` had no pipeline in its reconstructed state.
- An image's **Min** and **Max** counted infinities, so one `+Inf` made **Auto Range** show the whole image as black.
- The replay gave every acceleration structure build the same scratch, overlapping them and freeing memory recorded builds still pointed at (`Replayer::ReserveScratch`).
- `test/triangle --ray-tracing` never ordered its trace after the top level's build, which synchronization validation does not report and the replay's image comparison found.

## v0.14.0

### Added
- Metal gets the CPU timeline and the memory series (`src/metal/src/cpu_timeline.h`), so **Where the CPU went** and the **Timeline** card work on a Metal capture.
- The replay builds acceleration structures, makes ray tracing pipelines and replays `vkCmdTraceRaysKHR` with a shader binding table of its own (`vkinsp_replay`, docs/REPLAY.md).
- The replay compares the storage images a frame computed into, not only its render targets (`Replayer::InjectStorageReadbacks`); `test/triangle --ray-tracing` rebuilds its bottom level every frame.
- **Shader Binding Table** on a trace command names the shader group each record runs, and calls out a record whose handle matches no group (`renderer/binding_table.ts`).
- The acceleration structure viewer (**Instances** on a top level in Inspect): each instance's bottom level, transform, mask and flags, and the scene drawn in the mesh preview.
- Ray tracing builds capture what they were built from: a bottom level's vertices, indices and transforms, and a top level's instances; captures keep every acceleration structure.
- Memory over time (**Over time** in the Memory Use section, `renderer/memory_timeline.ts`), with a verdict of leaking, cycling, steady or releasing.
- D3D12 gets the CPU timeline and memory per heap (`src/d3d12/src/cpu_timeline.h`, docs/D3D12.md), and times waits on fence events so a frame can be called GPU-bound.
- **Timeline** in Frame Stats: the CPU threads and the GPU passes as tracks on one axis, with a verdict on the longest GPU gap and what the CPU did across it (`renderer/timeline_tracks.ts`, docs/PROFILING.md).

### Fixed
- A bound acceleration structure read as **(not written)** on a trace, and was missing from a descriptor set's contents.
- Every verdict in Frame Stats was clipped at the details pane's edge.
- Frame Bound called a frame GPU bound when its captured passes outlasted the uncaptured frame interval; that is now reported as the capture's own cost.
- Most passes of a multi-frame capture had no GPU time, because pass indices were not restarted at each recording of a command buffer (`renderer/pass_metrics.ts`).

## v0.13.0

### Added
- Memory per heap (**Memory Use** on the physical device in Inspect, `renderer/memory_heaps.ts`): allocations by heap and memory type, the driver's budget through `VK_EXT_memory_budget`, and heaps near their limit flagged.
- **Where the CPU went** in Frame Stats: the layer times submits, presents, fence waits and swapchain acquires per thread (`src/vulkan/src/cpu_timeline.h`), and relates the GPU and CPU clocks through `VK_KHR_calibrated_timestamps`.
- Compiler statistics per Vulkan pipeline (**Compiler statistics** in the launch dialog, `VKINSP_SHADER_STATISTICS=1`, `shaderStatistics` for `launch_app`), through `VK_KHR_pipeline_executable_properties`; off by default.
- A latency-bound pass in GPU Bottlenecks says whether register pressure explains it, where a capture carries compiler statistics.
- Device-removed diagnostics for D3D12 (`src/d3d12/src/device_removed.h`, docs/TROUBLESHOOTING.md): DRED names the operation each command list stopped on and a page fault's nearest objects; `DXINSP_NO_DRED=1` turns it off.
- Device-lost diagnostics for Vulkan (**Device-lost breadcrumbs** in the launch dialog, `VKINSP_BREADCRUMBS=1`, `breadcrumbs` for `launch_app`): the session log names the command the GPU stopped on, through `VK_AMD_buffer_marker`; off by default.
- Hardware counters for Vulkan captures (`vkinsp_replay --counters`, `--counter-draws`, docs/REPLAY.md), from NVIDIA's Nsight Perf SDK or `VK_KHR_performance_query`; `get_hw_counters` in the MCP server.
- **Measure hardware counters** in GPU Bottlenecks shows a column per counter and a measured verdict per pass, saved with the capture; `get_bottlenecks` reports it too.

## v0.12.0

### Added
- Direct3D 12 on Windows (`d3d12/`, docs/D3D12.md): a capture library injected at process start by `dxinsp_launch.exe`, speaking the Vulkan layer's protocol; every local Windows target is launched with both.
  - Object inspection: every D3D12 and DXGI object with its creating call, descriptor and `SetName`, descriptor heap contents, adapter and feature sections, and a leak report.
  - Frame capture: command lists in submission order with bundles inlined, passes from `OMSetRenderTargets` or `BeginRenderPass`, and compute passes from runs of dispatches.
  - Read-back: render targets at the end of each pass (multisampled ones resolved), descriptor tables and root views at each draw, vertex and index buffers, and `ExecuteIndirect` arguments.
  - Pass timings, pipeline statistics and occlusion counts per pass, so Frame Stats and GPU Bottlenecks work.
  - The D3D12 debug layer (**Validation layer**) as validation messages, linked to their commands where `ID3D12InfoQueue1` exists.
  - Creation and command stack traces, with the runtime's and the driver's frames marked internal.
  - Shaders: DXBC and DXIL reflection, and disassembly and HLSL through `dxinsp_shader.exe`, from the container, a PDB under the symbol directories, or generated from reflection.
  - Shader editing: an HLSL stage is compiled with `dxc` and the pipeline state rebuilt, including ones from streams and pipeline libraries.
  - A submit frame boundary per device for one that never presents (Chrome's Dawn); `DXINSP_FRAME_BOUNDARY=submit` forces it.
  - **An application started elsewhere (Direct3D 12)**: `dxinsp_launch.exe --watch <image>` injects into a process the moment it appears; `wait_for_app` in the MCP server, `--wait-for-d3d12=<image>` in the app.
  - `DXINSP_RECORD_ALWAYS` (**Record all command buffers**), `DXINSP_LOG`, `DXINSP_LOG_FILE`, `DXINSP_STACKTRACES`, `DXINSP_DEBUG_LAYER`, `DXINSP_FRAME_BOUNDARY` and `DXINSP_PORT`.
  - `test/d3d12_triangle`, with `--msaa`, `--bundle`, `--indirect`, `--render-pass`, `--compute`, `--leak`, `--offscreen` and `--debug-layer`.
  - Not there yet: the shader debugger, the replay-based analyses, 32-bit targets, and attaching to a process that is already running.
- Stencil read-back on Vulkan and D3D12, shown as "Stencil" in the render target tab; `--stencil` in both test applications, and `stencil`, `stencil-msaa` and `d3d12-stencil` cases in `tools/ui_tests.py`.
- The Windows build needs the Windows SDK 10.0.26100 or newer, the `third_party/minhook` submodule, and `dxc` for the D3D12 test application.

### Changed
- The source directories moved under `src/`: `src/app`, `src/d3d12`, `src/metal`, `src/replay` and `src/vulkan`.
- The MCP server bundle reads its version from `claude-plugin/.claude-plugin/plugin.json` at startup, so a version bump no longer makes it stale.

### Fixed
- A D3D12 application (a Unity player, reliably) died of a stack overflow when a hook was called through a copy of a patched vtable.
- Two inspected applications on one machine silently shared port 47531; the default port now steps to the next free one, and a taken `DXINSP_PORT` is refused.
- Quitting GPU Inspector left the application it had launched running.
- A Vulkan capture of draws recorded into secondary command buffers (a Unity player) raised validation errors from the layer's own queries; the layer now enables `inheritedQueries`.
- Launching a Vulkan application on an NVIDIA GPU froze the window, because the D3D12 library logged the driver's own D3D12 device call by call.
- A Vulkan frame captured with a replaced shader recorded the original pipeline at every bind; the bind now records the replacement, with the original as `replaced`.
- The layer recorded commands between the parts of a suspended and resumed dynamic rendering pass; `test/triangle --suspend`, and a `suspend` case in `tools/ui_tests.py`.
- A Vulkan launch ran an installed GPU Inspector's registered implicit layer instead of its own.

## v0.11.0

### Added
- Draws with shader objects (`VK_EXT_shader_object`) in Vulkan captures:
  - The command details, `get_command` and the frame statistics show a draw's shader objects and dynamic state.
  - **Analyze Shaders**, the **Shader Flame Graph** and their MCP tools weigh their stages.
  - The shader debugger steps their vertex, fragment and compute shaders.
  - The mesh view's VS Out and `get_mesh_output` capture their vertex shader's outputs.
  - Measuring a shader by ablation still needs a pipeline, and says so.
- Ray tracing in Vulkan captures:
  - A ray tracing pipeline shows its stages and shader groups, and a trace command its shader binding table regions.
  - An acceleration structure shows its type, storage and what its last build held.
  - Descriptor sets show the acceleration structures they bind.
  - `vkinsp_replay` leaves ray tracing out and reports it, instead of crashing.
  - `test/triangle --ray-tracing` traces a triangle each frame.
- Shader editing of graphics pipeline libraries and shader objects:
  - Editing a stage of a pipeline linked from libraries rebuilds the libraries and relinks.
  - A shader object is edited like a pipeline's stage; a linked set is made again unlinked.
  - `replace_shader` takes a VkShaderEXT, and `get_shader` reads one.
  - `vkinsp_replay` replays shader objects.
  - `test/triangle --pipeline-library` and `--shader-object`.
- Implicit layer:
  - **Set for my account** in the launch dialog sets `VKINSP_ENABLE` and `VKINSP_PORT` for every program the user starts.
  - The Windows installer registers the implicit layer, and uninstalling removes it.
- Descriptors pushed through an update template (`vkCmdPushDescriptorSetWithTemplate`) are captured, read back and replayed like other push descriptors; `test/triangle --push-template`.
- Frame Issues rules: `oversized-attachment`, `subpass-candidate` and `redundant-transition`.
- Shader cost by ablation (Vulkan): **Measure shader** in the Shader Flame Graph sizes a stage's frames by the GPU time each function, line or texture costs; `measure_shader_cost`, `vkinsp_replay --ablate`, `test/triangle --heavy`.
- Shader debugger: step a draw's vertex or fragment shader, or a dispatch's compute shader, line by line with breakpoints and variables; `debug_shader` in the MCP server.
- The shader debugger works on Metal captures, stepping the application's Metal Shading Language.
- The shader debugger steps SPIR-V without debug information through decompiled GLSL (**Decompiled GLSL**, or `decompiled` in `debug_shader`).
- The shader debugger's stepping buttons are icons.
- Metal captures read back the textures a draw or dispatch sampled, capped by `maxSampledTextureTotal`.
- Metal captures record the function constants a shader was specialized with, so the debugger steps the variant the draw used.
- Vulkan captures hold the contents of images and buffers a frame reads before writing, so frames that build on earlier ones replay; `list_textures` lists them as kind `initial`.
- `vkinsp_replay` compares multisampled render targets, and starts each mip and layer in the layout the frame expects.
- `test/triangle --persistent` renders a frame that depends on the frames before it.

### Changed
- The Vulkan capture layer's sources moved from `layer/` to `vulkan/`.
- Replay-based analyses answer in tens of milliseconds after the first: each capture keeps one `vkinsp_replay --serve` process alive.

### Fixed
- Overdraw, draw overlays, VS Out, pixel history and **Measure shader** failed on pipelines linked from graphics pipeline libraries, and their draws showed none of the libraries' state.
- Vulkan applications with more than one device were only captured on the device the capture started on; `test/triangle --second-device` and `--second-queue`.
- A command buffer recorded before the capture showed a later buffer's rendering in its targets when both were in one submission.
- `vkinsp_replay` took seconds to read back results on NVIDIA GPUs, from staging buffers in write-combined memory (a Unity frame: 2.2 s to 0.3 s).

## v0.10.0

### Added
- Pixel history: every clear and draw that touched a pixel, and the pixel's value after each; `get_pixel_history` in the MCP server.
- A capture's render target in a tab of its own, with the pass's overdraw and the clicked pixel's history.
- Shader editor: `#include` is resolved against the session's source roots.
- Buffer layouts name their fields from a module's Vulkan debug information where it has no `OpName` / `OpMemberName`.
- Shader Flame Graph: fragment stages are weighted by measured fragment invocations where the capture has them.
- Depth rejection for Vulkan captures, from an occlusion query around each render pass.
- Multisampled stencil read-back.
- Draw-call overlays for Vulkan captures: **Highlight Draw**, **Depth Test** and **Wireframe** in the render target tab (`vkinsp_replay --overlay`).
- Mesh view: **View Mesh** shows a draw's vertices before (VS In) and after (VS Out) its vertex shader (`vkinsp_replay --mesh`); `get_mesh_output` in the MCP server.
- Per-draw timing and counters for Vulkan captures: **Measure draws** in the Shader Flame Graph (`vkinsp_replay --draws`), saved with the capture.

### Fixed
- Applications that enable multiview (every Unity player) lost their GPU pass counters entirely; only passes that render several views go uncounted now.
- `vkinsp_replay --draws` measured nothing for draws recorded into secondary command buffers.
- `vkinsp_replay --draws` hung on a multiview frame, where the draws' queries overlapped.
- Captures of applications that recreate their swapchain (every Unity player) lost the frame's final image.

## v0.9.0

### Added
- `vkinsp_replay` (`replay/`, [docs/REPLAY.md](docs/REPLAY.md)) re-executes a Vulkan capture and compares every render target byte for byte, with `--overdraw`, `--pixel` and `--dump`.
- Overdraw: fragments per pixel of a pass, with and without its depth and stencil tests, as heatmaps and in GPU Bottlenecks; `get_overdraw`.
- Claude Code plugin (`claude-plugin/`): an MCP server over saved `.gpucap` files and live sessions, with a capture analysis skill and five commands.
- `CaptureComplete` ends a capture's stream, so a client knows when a capture has fully arrived.

### Fixed
- Shader editing failed on applications that destroy their shader modules once their pipelines exist.

## 0.8.0

### Added
- Render Graph (capture bar, Vulkan and Metal): the frame's passes and the resources connecting them, with resource lifetimes, GPU times and the critical path.
- Render graph suggestions, also in Frame Issues: unread stores, replaced results, mergeable passes and barriers that synchronize nothing.
- Metal capture library (`metal/`, macOS, [metal/README.md](metal/README.md)): injected with `DYLD_INSERT_LIBRARIES` and speaking the Vulkan layer's protocol, so Inspect and Capture work unchanged.
- The launch dialog on macOS launches a `.app` with the Metal library, and says how to re-sign a hardened-runtime target.
- Metal pipeline reflection: stage buffers and inline constants as named, typed fields, and a Reflection section per stage.
- Metal frame stats (frame time, submit time, refresh rate), and the capture bar's options honoured by the library.
- Metal validation: command buffer errors, Metal's validation layer, shader logs, and a leak report at exit.
- Metal in Inspect: functions, libraries with their source, Metal Shading Language highlighting, and each command's key arguments.
- Metal stack traces of object creations and captured commands.
- Metal argument buffers decoded in a draw's details.
- Metal memory: each resource's allocated size, heap and purgeable state, and the device's total against its working set.
- Metal pass counters: vertex and fragment stage spans, and the GPU's statistic and utilization counters.
- Frame Issues for Metal captures, off the pass descriptors' load and store actions.
- **GPU Bottlenecks** in the Reports menu (Vulkan and Metal, [docs/PROFILING.md](docs/PROFILING.md)): each pass's overdraw, fragments per triangle, waiting stage and depth rejection, slowest first.
- Vulkan pass counters from a pipeline statistics query (`vulkan/src/pipeline_stats.h`); `VKINSP_NO_PIPELINE_STATISTICS=1` turns it off.
- Frame Issues rules: `high-overdraw`, `microtriangles`, `late-depth-rejection` (Metal only) and `unmipped-texture`.
- The image viewer decodes BC6H, BC7, ETC2, EAC, ASTC and PVRTC, and Metal captures read those formats back.
- **Xcode Trace** in the capture bar on macOS writes the next frame as a .gputrace document.

### Changed
- Frame Stats, Analyze Shaders, Shader Flame Graph and Render Graph moved into one **Reports** menu.

### Fixed
- Metal compute passes showed no GPU time.
- `--launch` started the application twice, ignoring `--args`, `--validation` and the rest on the second launch.
- `--screenshot` could hang the run instead of writing a shot.

## 0.7.0

### Added
- macOS build of the user interface (`npm run dist:mac`), signed and notarized: inspects Android devices over adb and opens `.gpucap` files.
- Source roots (launch dialog, `--source-roots`): a shader with line information but no embedded text gets its source from the file its debug information names.

## 0.6.0

### Added
- Frames without a swapchain (OpenXR): frame boundaries from fence waits or submissions; `VKINSP_FRAME_BOUNDARY=wait|submit` forces one; `test/triangle --offscreen`.
- The refresh estimate picks the best-fitting common rate, and the layer logs refresh rate changes.
- Multiview: a pass's view mask layers are read back.
- Frame Issues in Frame Stats: sixteen rules with severity and per-rule filters, markers in the command list and a Performance section in a command's details.
- OpenXR test app for headsets (`test/xr_triangle`, `tools/build_xr_triangle.py`): multiview, and a deliberately slow twin.
- Android: the layer listens on an abstract socket, so no INTERNET permission is needed.
- Shader analysis rules: loop-invariant computation and workgroup memory size.
- Shader cost per source line, clickable into the Source view; line frames in the flame graph.
- Sampled image read-back of every mip level.
- Launch dialog: the Package field filters the device's packages.
- Applications started elsewhere: **Register** the layer as an implicit layer and **Wait** for a connection; `--wait-for-app` and `--implicit-layer=on|off`.
- `tools/ui_tests.py`: end-to-end UI checks against the triangle application and saved captures, through `--debug-dump`.
- Phone test application (`test/android_triangle`, `tools/build_android_triangle.py`).
- Stack traces on Android, symbolized with the NDK's llvm-symbolizer against the launch dialog's **Symbol directories**.
- Android launch diagnostics in the Log: a sleeping device, and a headset's "controllers required" dialog.
- **Sync validation** launch option: hazards reported at `vkQueueSubmit` are linked to their commands; `test/triangle --hazard`.
- Command buffers recorded before the capture began get their render targets read back after submission; `test/triangle --prerecord`.

### Changed
- The desktop triangle test app no longer stores its depth buffer.

### Fixed
- Render targets with storeOp DONT_CARE read back wrong; a captured render pass now runs as a store-everything copy.
- Android launch on a Quest reported the successful `am start` output as an error.
- Android launches could stay at "connecting", or connect to an earlier application left running on the same port.

## 0.4.0

### Added
- Device sections in the Inspect tab: properties, limits, memory, queue families, features, extensions; physical device names in the object list.
- "Affected by" on captured buffers: the frame's earlier writes to them.
- Leak report on device or instance destruction; triangle app `--leak`.
- Static shader analysis on SPIR-V: Shader Cost and Performance Analysis sections, "Analyze Shaders" over a capture.
- Shader editor: line numbers, find bar, compile errors marked on their lines.
- "At frame" capture field; recent capture files in the Recent menu.
- Multisampled color and depth read-back (resolve before the copy); triangle app `--msaa`.
- Captures in windows of their own ("Open in New Window").
- Display refresh rate from `VK_EXT_present_timing`, `VK_GOOGLE_display_timing` or the Windows monitor mode; dropped frames against the real period.
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
- The live image viewer crashed the application under the validation layer.
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
