## v0.13.0

### Added
- Memory per heap (**Memory Use** on the physical device in Inspect, `renderer/memory_heaps.ts`):
  what the application has allocated from each memory heap, in how many allocations and how large
  the largest is, as a share of that heap, broken down by memory type — and, where the device has
  `VK_EXT_memory_budget`, what the driver says is resident and how much it will let this process
  have. A single memory total cannot say whether it is a problem; a gigabyte in a 16 GB heap and a
  gigabyte in a 256 MB one are different situations. Heaps close to their limit are flagged,
  including ones another process is filling, which only the driver can see.
- Where a captured frame's CPU time went (`src/vulkan/src/cpu_timeline.h`, **Where the CPU went** in
  Frame Stats): the layer times the host calls a frame spends its time in — submitting, presenting,
  waiting on fences, acquiring a swapchain image — with the thread that made each, and the capture
  carries them. It separates three cases the Frame Bound card could only infer from aggregates: the
  CPU waiting on fences (the GPU sets the frame time), the CPU inside submission (submission is the
  cost), and the CPU waiting in present (the display paces the frame and neither processor is the
  limit). Where the device has `VK_KHR_calibrated_timestamps` the capture also carries the relation
  between the GPU and CPU clocks, so pass timings can be placed on the same axis.
- Compiler statistics per pipeline for Vulkan (`src/vulkan/src/shader_statistics.h`): **Compiler
  statistics** in the launch dialog (`VKINSP_SHADER_STATISTICS=1`, `shaderStatistics` for
  `launch_app`) asks the driver what its shader compiler made of each stage — registers used, code
  size, spilled and shared memory, inputs and outputs — and the Inspect tab shows it on the
  pipeline. Through `VK_KHR_pipeline_executable_properties`, the same data Nsight reports per
  shader, and it explains the occupancy the hardware counters measure. The names are the driver's
  own and are passed through as it reports them. Off by default: it costs compile time and driver
  memory. A latency-bound pass in GPU Bottlenecks now says *why* where a capture carries them: the
  registers its heaviest stage holds are usually what keeps occupancy low, and where the shader is
  light the report rules register pressure out instead.
- The CPU and GPU as tracks on one axis (**Timeline** in Frame Stats, `renderer/timeline_tracks.ts`,
  docs/PROFILING.md): one lane per thread with the calls the layer timed, and a GPU lane with the
  passes, laid on the shared axis the clock calibration provides. Every card above it reports a
  total, and a total cannot show an idle GPU: that is the space *between* spans. The verdict names
  the longest gap between passes and what the CPU lanes were doing across it, which separates a
  frame paced by the display (headroom) from one whose GPU is waiting on work the CPU has not handed
  over (a stall), and reports how long the first pass waited after the submission before it, which
  is latency no total holds. Only gaps between passes count as idle; the axis reaches wider to cover
  the CPU calls, and time outside the timed passes is not measured idleness. `get_capture_summary`
  reports the same. Without `VK_KHR_calibrated_timestamps` the CPU lanes still draw and the GPU lane
  is left out rather than placed on a guessed origin.
- Device-removed diagnostics for D3D12 (`src/d3d12/src/device_removed.h`, docs/TROUBLESHOOTING.md):
  the library turns on Device Removed Extended Data before it creates the device, so when the GPU
  stops responding the session log names the operation each command list stopped on rather than only
  reporting `DXGI_ERROR_DEVICE_REMOVED`, and spells out what the removal code means. A page fault
  adds the faulting address and the objects allocated nearest it, where a recently freed one is a
  use-after-free. Unlike the Vulkan breadcrumbs the runtime keeps these itself, so it costs nothing
  per draw and is on by default (`DXINSP_NO_DRED=1` turns it off).
- Device-lost diagnostics for Vulkan (`src/vulkan/src/device_lost.h`, docs/TROUBLESHOOTING.md):
  **Device-lost breadcrumbs** in the launch dialog (`VKINSP_BREADCRUMBS=1`, `breadcrumbs` for
  `launch_app`) has the GPU write a marker before and after every draw and dispatch, so when it
  stops responding the session log names the command it was executing rather than only reporting
  `VK_ERROR_DEVICE_LOST`. It says whether that command also finished, which separates a hang inside
  it from a hang in what came next. Through `VK_AMD_buffer_marker`, which NVIDIA implements too; the
  layer now checks every call that can report a lost device. Off by default: two GPU writes per
  action.
- Hardware counters for Vulkan captures (`vkinsp_replay --counters`, docs/REPLAY.md): the GPU's own
  counters for which unit inside the shader core a pass saturates, per render pass and, with
  `--counter-draws`, per draw. From NVIDIA's Nsight Perf SDK or `VK_KHR_performance_query`, with
  `get_hw_counters` in the MCP server. Needs GPU performance-counter access enabled.
  **Measure hardware counters** in the GPU Bottlenecks report reads them for an open capture and
  shows a column per counter beside each pass; they are saved into the capture file with it. Each
  pass's verdict is then measured rather than inferred — shader, bandwidth, cache or latency bound,
  naming the unit at its limit — which on Vulkan is the only per-pass verdict there has been, since
  the vertex and fragment spans the inferred one needs are Metal's. `get_bottlenecks` reports it too.

## v0.12.0

### Added
- Direct3D 12 on Windows (`d3d12/`, docs/D3D12.md): a capture library injected into the
  application at process start by `dxinsp_launch.exe`, hooking the D3D12 and DXGI entry points
  and the vtables of the objects they create, speaking the Vulkan layer's protocol. The launch
  dialog needs no API field: every local Windows target is started with the Vulkan layer and the
  D3D12 library both, and whichever the application uses connects.
  - Object inspection: every D3D12 and DXGI object with the call that created it, its descriptor
    under the D3D12 names and its `SetName`; descriptor heap contents; the adapter's description
    and the device's feature level and `CheckFeatureSupport` results as device sections; the leak
    report when the device is released.
  - Frame capture: the command lists executed during the frame in submission order, bundles
    inlined at `ExecuteBundle`, passes synthesized from `OMSetRenderTargets` (a synthetic
    `EndRenderTargets` closes one where the application made no call) or taken from
    `BeginRenderPass`, compute passes from runs of dispatches. Render targets are read back at
    the end of each pass, multisampled ones resolved. Descriptor tables and root views are
    snapshotted at each draw and dispatch with their buffers and textures read back, root
    constants shown as push constants, vertex and index buffers decoded with the pipeline's
    input layout, and `ExecuteIndirect` arguments read.
  - Pass timings, pipeline statistics and occlusion counts per pass, so Frame Stats and GPU
    Bottlenecks work on D3D12 captures.
  - The D3D12 debug layer (**Validation layer**) as validation messages, linked to the command
    that fired them where `ID3D12InfoQueue1` exists.
  - Creation and command stack traces, with the runtime's and the driver's frames marked
    internal.
  - Shaders: DXBC and DXIL reflection at pipeline creation (constant buffer members, resources
    by register and space, inputs and outputs), so a draw's constant buffers render as typed
    blocks and the Shader Flame Graph weighs the stages; `dxinsp_shader.exe` gives the Inspect
    panel disassembly and the shader's HLSL: what `dxc -Zi` embedded in the container, or, for a
    `dxc -Zs` build that kept it out, what it wrote to a PDB beside the build — found by the name
    and hash the container carries under the session's symbol directories (the launch
    configuration's, or `set_search_paths`' `symbolDirs`). A stage with no HLSL anywhere opens in
    the editor as HLSL generated from its reflection: the same constant buffers at the same
    offsets, the same resources at the same registers and spaces and the same entry signature, so
    **Compile & Apply** works on a shipped shader with no debug information.
  - Shader editing: an HLSL stage is compiled with `dxc` and the library rebuilds the pipeline
    state with it. Pipelines from streams are rebuilt from their streams, and so is one loaded
    from a pipeline library, which is how Unity's D3D12 player loads every graphics pipeline.
  - A submit frame boundary for a device that never presents (Chrome's Dawn WebGPU device on
    D3D12 renders into textures the compositor presents), the D3D12 form of the Vulkan layer's
    OpenXR fallback: after a run of submissions with no present, frames end at every
    `ExecuteCommandLists`. `DXINSP_FRAME_BOUNDARY=submit` forces it and ignores presents, for
    capturing Dawn when the compositor also presents on a hooked device. The boundary and frame
    count are decided per device, so several D3D12 devices in one process (a game and a background
    device, or Dawn beside the compositor) each keep their own without hijacking a capture.
  - **Waiting for an application to start**, the D3D12 counterpart of the Vulkan implicit layer,
    for an application the inspector does not launch (a game behind its launcher, a Unity player
    started from the editor): the launch dialog's **An application started elsewhere (Direct3D
    12)** target takes the executable's name, and `dxinsp_launch.exe --watch <image> --dll <dll>`
    polls the process list every few milliseconds and injects the capture library into the process
    the moment it appears, freezing it until the library is in so the hooks precede
    `D3D12CreateDevice`. `--env NAME=VALUE` carries the library's variables into a process that
    could not inherit them, `--once` then stands in for the application (exit code and all) and
    `--timeout` gives up with exit code 3. The MCP server has it as `wait_for_app`, and the app
    as `--wait-for-d3d12=<image>`. It races the application's start: the wait has to be running
    before the application is launched, and an application that already has a device cannot be
    caught — the session says so when nothing connects after an injection.
  - `DXINSP_RECORD_ALWAYS` (**Record all command buffers**) for command lists recorded once and
    executed every frame; `DXINSP_LOG`, `DXINSP_LOG_FILE`, `DXINSP_STACKTRACES`,
    `DXINSP_DEBUG_LAYER`, `DXINSP_FRAME_BOUNDARY` and `DXINSP_PORT`.
  - `test/d3d12_triangle`: `dxinsp_triangle.exe`, the D3D12 counterpart of the Vulkan test
    application, with `--msaa`, `--bundle`, `--indirect`, `--render-pass`, `--compute`, `--leak`,
    `--offscreen` (no swap chain and no present, the Dawn shape) and `--debug-layer`.
  - Not there yet for D3D12: the shader debugger and the replay-based analyses (overdraw, pixel
    history, draw overlays, mesh output, per-draw measurements, ablation), stencil read-back,
    32-bit targets, and attaching to a process that is already running (one can be caught at its
    start, above, but a device that already exists cannot be reached).
- The Windows build needs the Windows SDK 10.0.26100 or newer and the `third_party/minhook`
  submodule (BSD-2-Clause) for the D3D12 library, and `dxc` for its test application.

### Changed
- The source directories moved under `src/`: `src/app`, `src/d3d12`, `src/metal`, `src/replay`
  and `src/vulkan`. `test/`, `tools/`, `docs/`, `claude-plugin/` and `third_party/` stay at the
  repository root, and so does the `build/` tree CMake writes. Build it the same way; only the
  paths changed (`cd src/app && npm start` where it used to be `cd app`).
- A version bump no longer has to rebuild the committed MCP server bundle. The bundle used to
  carry the version from `package.json`, so bumping one without rebuilding the other left a
  stale file that failed the release workflow's freshness check; it now reads the version from
  `claude-plugin/.claude-plugin/plugin.json` at startup and is byte-identical whatever the app
  version says.

- Stencil read-back on Vulkan and Direct3D 12. A depth-stencil attachment's stencil is read back
  as a texture of its own beside its depth (Vulkan: through the resolve pass when multisampled;
  D3D12: plane 1 of the target, single-sampled), shown in the render target tab as "Stencil" with
  its values. On Vulkan the frame-start contents take a loaded stencil apart from the depth, and
  the replay uploads and compares it. `test/triangle --stencil` and `test/d3d12_triangle --stencil`
  record one, and `tools/ui_tests.py` has `stencil`, `stencil-msaa` and `d3d12-stencil` cases.

### Fixed
- A Direct3D 12 application could die of a stack overflow a few seconds in, which a Unity player
  did reliably. Something in the process copies an already patched vtable into heap memory; a call
  arriving on the copy found no registry entry for it and fell back to reading the slot, which
  holds our own replacement, so the hook forwarded to itself for ever. Every replacement is now
  remembered, and a copy holding one is matched back to the vtable it came from and adopts its
  saved originals; a copy whose source cannot be found refuses the call instead of recursing.
- Two inspected applications on one machine silently shared port 47531. `SO_REUSEADDR` lets a
  second listener bind the same address on Windows, so both `bind` calls succeeded and the
  inspector reached whichever the stack routed to, reporting one application's frame as the
  other's. The capture library now reads the system's listening sockets before it binds: the
  default port steps to the next free one and says so, and a port named by `DXINSP_PORT` is
  refused with an explanation rather than shared. It also stands down when a Vulkan application
  whose driver creates a D3D12 device has both capture libraries in the one process.
- Quitting GPU Inspector left the application it had launched running. The kill of the process
  tree was left to a callback that never ran once Electron was on its way out; it is synchronous
  on that path now.
- A Vulkan capture of an application that records its draws into secondary command buffers (a
  Unity player) raised validation errors of the layer's own making: 24 in a Unity URP frame
  (`VUID-vkCmdExecuteCommands-commandBuffer-00101` and `-00104`, `VUID-vkCmdEndQuery-None-07007`,
  `VUID-vkCmdEndQuery-commandBuffer-recording`). The pass counters' queries were active across
  `vkCmdExecuteCommands`, and the occlusion query was then ended inside the render pass it had
  begun outside of. The layer now enables `inheritedQueries` beside the counters' other features
  and begins secondary command buffers able to inherit its queries, so those passes run cleanly
  and gain the depth rejection rate they used to go without. On a device without the feature
  they are timed but not counted. A query of the application's own no longer ends the layer's
  early either: the first occlusion or pipeline statistics query the application begins turns
  the layer's counter of that type off.
- Launching a Vulkan application on an NVIDIA GPU froze GPU Inspector's window within seconds, so
  captures, overdraw, pixel history and every other analysis never finished. The driver presents
  through a D3D12 device of its own, which the D3D12 capture library (launched into every Windows
  target) tracked and logged call by call, and the session log redrew itself for each line. The
  library now leaves a D3D12 device alone when GPU Inspector's Vulkan layer already has a device in
  the process, and the log draws new lines in batches.
- A Vulkan frame captured while `replace_shader` (or the shader editor) had a stage replaced
  recorded the application's original pipeline at every bind, so the capture's draw state, its
  shader views and its replay showed the original code while the read-back targets showed the
  edit's. The bind now records the replacement pipeline (or shader objects) that ran, with the
  original beside it as `replaced` (a "Replaces" line in the draw's Pipeline State, and in
  `get_command`), so everything built on the capture uses the edited code and such a frame replays
  identical.
- A dynamic rendering pass suspended at the end of one command buffer and resumed in another
  (`VK_RENDERING_SUSPENDING_BIT` / `RESUMING_BIT`) had the layer's timestamps, queries and
  read-back copies recorded between its parts, where the specification allows no command at all
  (the Khronos validation layer has no check for it, so it never said so). The suspended part now
  gets nothing after it: the part that resumes the pass reads the attachments back and records the
  bound buffers' copies of both, and such a pass is neither timed nor counted. `test/triangle
  --suspend` records one, and `tools/ui_tests.py` has a `suspend` case.
- A Vulkan launch ran the layer of an installed GPU Inspector instead of its own once the installer
  (or **Set for my account**) had registered that layer as an implicit layer: the loader enabled
  the registered layer of the same name. A checkout's build, or a second installed version, now
  loads the layer the launch names.

## v0.11.0

### Added
- Draws with shader objects (`VK_EXT_shader_object`) in Vulkan captures:
  - The command details, `get_command` and the frame statistics show the shader objects a draw
    runs and the dynamic state it set in place of a pipeline's.
  - **Analyze Shaders**, `analyze_shaders`, the **Shader Flame Graph** and
    `get_shader_flame_graph` weigh their stages, grouped by the set of shader objects bound.
  - The shader debugger steps their vertex, fragment and compute shaders. It reads cull mode,
    front face and depth test from the dynamic state.
  - The mesh view's VS Out and `get_mesh_output` capture their vertex shader's outputs.
  - Measuring a shader by ablation still needs a pipeline, and says so.
- Ray tracing in Vulkan captures:
  - A ray tracing pipeline shows every one of its stages, and its shader groups in the Inspect
    panel, a trace command's details and `get_command`.
  - A trace command shows its shader binding table regions.
  - An acceleration structure shows its type, storage and what its last build held: geometries,
    primitive counts, vertex formats.
  - Descriptor sets show the acceleration structures they bind.
  - `vkinsp_replay` leaves ray tracing out and reports it, instead of crashing.
  - The test application's `--ray-tracing` option traces a triangle each frame.
- Shader editing of graphics pipeline libraries and shader objects:
  - A pipeline linked from libraries shows the stages the libraries hold, and editing one rebuilds
    the libraries and relinks.
  - A shader object (`VK_EXT_shader_object`) is edited like a pipeline's stage. A linked set is
    made again unlinked.
  - `replace_shader` takes a VkShaderEXT, and `get_shader` reads one.
  - `vkinsp_replay` replays shader objects.
  - The test application's `--pipeline-library` and `--shader-object` options draw this way.
- Implicit layer:
  - **Set for my account** in the launch dialog sets `VKINSP_ENABLE` and `VKINSP_PORT` for every
    program the user starts, for an application behind a launcher.
  - The Windows installer registers the implicit layer, and uninstalling removes it.
- Descriptors pushed through an update template (`vkCmdPushDescriptorSetWithTemplate` and its
  `2` and KHR forms) are captured like other push descriptors:
  - The command shows what it bound.
  - Its buffers and images are read back.
  - `vkinsp_replay` pushes them again. The test application's `--push-template` option draws
    this way.
- Frame Issues rules:
  - `oversized-attachment`: an attachment larger than every render area drawn into it.
  - `subpass-candidate`: a render pass reading nothing but what the previous pass rendered, at
    the same size, through fragment shaders that read each input once (a blur's pass is not
    reported).
  - `redundant-transition`: a layout transition nothing uses before the next one, or a barrier
    that changes nothing.
- Shader cost by ablation (Vulkan): **Measure shader** in the Shader Flame Graph replays a draw
  with variants of its fragment or compute shader, each with one function, source line or texture
  taken out, and sizes the stage's frames by the time each saved on this GPU. A line is charged only
  what it does beyond the parts feeding it. Values that decide a branch or a loop are left in, and
  every variant is checked with spirv-val first. The measurements are saved with the capture.
  `measure_shader_cost` in the MCP server, `vkinsp_replay --ablate`, and the test application's
  `--heavy` option draws with a deliberately costly fragment shader.
- Shader debugger: step through a draw's vertex or fragment shader, or a dispatch's compute
  shader, line by line on the capture's inputs, with breakpoints and every variable's value.
  Opened from a draw's or dispatch's details, a pixel history or the mesh view. `debug_shader` in
  the MCP server.
- The shader debugger works on Metal captures, stepping the Metal Shading Language the application
  compiled rather than SPIR-V. A fragment needs no replay: the draw's own vertex shader is run in
  the interpreter and its result rasterized, so a pixel can be debugged with nothing else built.
  A library loaded as a precompiled `metallib` has no source, and the debugger says so.
- The shader debugger can step SPIR-V built without debug information by line, through GLSL that
  `spirv-cross` decompiles and `glslangValidator` compiles back with line information (**Decompiled
  GLSL**, or `decompiled` in `debug_shader`). The original SPIR-V runs the same invocation, and
  the debugger says whether the two agree.
- The shader debugger's stepping buttons are icons.
- Metal captures read back the textures a draw or dispatch sampled, not only its render targets, so
  a debugged fragment samples what the GPU sampled. Deduplicated per capture and capped by
  `maxSampledTextureTotal`.
- Metal captures record the function constants a shader was specialized with
  (`newFunctionWithName:constantValues:`), which Metal itself will not report, so the shader
  debugger steps the variant the draw used rather than the one the constants default to. Engines
  ship one library of `[[function_constant]]`-guarded variants, so this is what makes their shaders
  debuggable at all.
- Vulkan captures hold what a frame found in the images it reads before writing them, so frames that
  build on earlier ones replay: a pass that loads an attachment and a copy or blit from an image take
  the image's contents first, once, unless the frame already wrote it whole; the source of every
  buffer copy is read whole, including staging buffers the host fills each frame. Part of **Images**
  and **Buffers**; `list_textures` lists them as kind `initial`.
- `vkinsp_replay` compares multisampled render targets, through the same resolve the capture read
  them through, and starts each mip and layer of an image in the layout the frame expects it in.
- The test application's `--persistent` option renders a frame that depends on the frames before it.

### Changed
- The Vulkan capture layer's sources moved from `layer/` to `vulkan/`, beside `metal/`. An existing
  CMake build directory reconfigures on its own. A packaged app still keeps the layer in
  `resources/layer`.
- Replay-based analyses (pixel history, overdraw, draw overlays, the mesh view, **Measure draws**)
  answer in tens of milliseconds after the first: each capture keeps one `vkinsp_replay --serve`
  process alive, and the capture is sent to it once.

### Fixed
- Pipelines linked from graphics pipeline libraries:
  - Overdraw, draw-call overlays, the mesh view's VS Out, pixel history and **Measure shader** work
    on their draws. Before, `vkinsp_replay` made invalid copies of these pipelines, and VS Out
    found no vertex shader.
  - The command details, `get_command` and the shader debugger show the state the libraries hold:
    topology, rasterization, depth, blending, dynamic states and vertex input.
- Vulkan applications with more than one device (a second device for compute, or a runtime's own)
  are captured whole:
  - **Passes and read-backs:** every device's passes are timed and counted, and its render
    targets and bound buffers are read back. Before, only the device the capture started on had
    query pools and staging memory, and read-backs recorded on another device used buffers that
    were not its own.
  - **Frames:** a capture starts at a present, not at another device's fence wait.
  - **Replay:** `vkinsp_replay` replays every device's objects.
  - **Test options:** `--second-device` and `--second-queue` in the test application.
- A command buffer recorded before the capture began no longer shows a later command buffer's
  rendering in its render targets when both are in one submission. The capture now splits the
  submission after it and reads its targets back in between. The triangle test app's
  `--prerecord` option now submits a second prerecorded buffer that draws over the first.
- `vkinsp_replay` took seconds to read back render targets and analysis results on NVIDIA GPUs: its
  staging buffers were in memory meant for writing, where reading is very slow. A Unity frame's
  replay went from 2.2 s to 0.3 s.

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
- Draw-call overlays for Vulkan captures: **Highlight Draw**, **Depth Test** and **Wireframe** in
  the render target tab, for any draw of the pass (`vkinsp_replay --overlay`).
- Mesh view: **View Mesh** shows a draw's vertices as a wireframe and a table, before its vertex
  shader (VS In) and after it (VS Out, replayed with transform feedback: `vkinsp_replay --mesh`),
  with what keeps the mesh out of view. `get_mesh_output` in the MCP server.
- Per-draw timing and counters for Vulkan captures (`vkinsp_replay --draws`): **Measure draws** in
  the Shader Flame Graph replays the frame with a timestamp pair and a pipeline statistics query
  around every draw and dispatch. A pass's measured time is then split between its draws by what
  each was timed at, and each stage takes its measured invocation count. `get_shader_flame_graph`
  measures on first use, and capture files keep the measurements. The replay also counts the samples
  each draw passed, which gives a depth rejection rate to passes that record their draws into
  secondary command buffers, where the layer's own occlusion query cannot reach.

### Fixed
- Applications that enable multiview (every Unity player does) lost their GPU pass counters
  entirely: the layer skipped pipeline statistics for the whole device rather than for the passes
  that actually render several views. Only those passes go uncounted now, so a Unity capture has
  overdraw, fragments per primitive and the rest of the GPU Bottlenecks report.
- `vkinsp_replay --draws` measured nothing for a frame whose draws are recorded into secondary
  command buffers, which is how a Unity player records every draw: they are measured there too.
- `vkinsp_replay --draws` hung on a multiview frame (an XR application's): each query in a multiview
  pass takes one index per view, and the draws' queries overlapped. They are spaced by the view
  count now, and summed over the views.
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
  (`vulkan/src/pipeline_stats.h`), so the bottleneck report and its rules work for Vulkan captures.
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
