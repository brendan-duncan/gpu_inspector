## v0.24.0

### Added
- A **Tile-Based GPUs** report, and `analyze_tiling` over MCP: what a frame would cost a mobile GPU in attachment traffic, what is avoidable, and what leaves the tile.
- A Direct3D 12 command list reset before the capture was asked for, as an engine that pools its lists leaves them, has its passes timed and their render targets read back.
- The Timeline draws a GPU lane per queue when a capture's passes ran on more than one, and says whether the queues ran at once or took turns.
- `test/d3d12_triangle --async-compute` runs its compute dispatch on a compute queue of its own.
- Export to C++ writes a Vulkan frame's acceleration structure builds and ray traces, so the exported program reproduces a ray traced image too.
- Export to C++ writes a Direct3D 12 frame's state objects, acceleration structure builds and `DispatchRays` too.
- Direct3D 12 ray tracing replay and export translate the descriptor tables and root views in binding table records.
- Vulkan ray tracing replay and export translate the device addresses in shader record data.
- Direct3D 12 replay and export write the descriptors shaders index straight out of the heap (shader model 6.6).
- `test/d3d12_triangle --bindless` samples a texture through `ResourceDescriptorHeap`.
- A Direct3D 12 draw's details and `get_command` list the heap its shaders index directly, as of its submission.
- They also say which of its slots the shaders take, worked out from the DXIL and the draw's constants.
- `test/d3d12_triangle --late-descriptor` rewrites a volatile descriptor after recording the draw that reads it.
- `test/d3d12_triangle --keep-depth` keeps its depth buffer from frame to frame.
- The replays report each buffer the capture cut at Max KB, with the size the frame bound.
- The Direct3D 12 replay names its objects as the application did, so debug-layer messages name them too.
- `test/d3d12_triangle --local-root` and `test/triangle --shader-record` put arguments in a hit group's record.
- `test/d3d12_triangle --pool` records each frame into a pool of lists reset as soon as they run.
- A Metal pixel history follows **layered** passes (the copies it draws into are arrays of the pass's `renderTargetArrayLength`, so a draw picking a layer lands in the same one) and an **indirect command buffer**'s draws (its commands run one at a time under a visibility result, which says whether each wrote the pixel).
- A Metal pixel history reports the writes to the texture that are not draws: a pass's multisample **resolve** into it, each **blit** that writes it, and a **compute** encoder that had it bound.
- `mtlinsp_triangle --layered`, `--indirect` and `--texture-writes` exercise those.
- `MTLINSP_LOG_FILE=<path>` appends the Metal library's log to a file, for an application whose stderr goes where nobody can read it (a Unity player); `--debug-log` sets it, as it already did for the Vulkan layer and the Direct3D 12 library.

### Changed
- Direct3D 12 read-backs queued by lists the captured frame never ran are dropped, instead of reported as failed.

### Fixed
- The render graph of a Direct3D 12 capture has its BeginRenderPass attachments, its split passes, and the textures and buffers its passes read.
- Launching a WebGPU page in Chrome captures the page's Direct3D 12 again, not the browser's Direct3D 11 compositing.
- A Direct3D 12 replay makes the resources a capture opened with `OpenSharedHandle`, so a WebGPU page's canvas replays.
- A multi-frame Direct3D 12 capture takes each frame's own buffer and texture read-backs rather than the first frame's.
- A Direct3D 12 capture takes what the frame found in the textures it reads before writing, so the Unity URP sample's frames replay identical.
- A Direct3D 12 capture sends a volatile descriptor the application rewrote after the draw was recorded, so the replay draws what the GPU did.
- Present latency on the frame meter no longer drops to nothing whenever the display's statistics skip a report.
- A Direct3D 12 capture of an application with async compute no longer trips the debug layer by writing one staging buffer from two queues.
- A Direct3D 12 state object with a subobject-to-exports association replays; the replay read the association's target under the wrong name.

## v0.23.0

### Added
- An application asks for a capture with a name for it: `gpu_inspector_capture_named(frames, label)` in `include/gpu_inspector.h`, and the label names the tab, the saved file and its manifest.
- The Metal library and the OpenGL ES plugin answer `include/gpu_inspector.h` too, so every API can capture from an assertion or a failed test; the header finds the library on macOS, Linux and Android as well as Windows.
- `--capture-at N` on the Metal and OpenGL ES samples, and UI test cases for the application's capture on every Windows backend.
- **Capture on hitch**: with a timing capture running, the first frame over its hitch threshold takes a frame capture of the next frame, in a tab named after the hitch.
- `--hitch-every N` on the Vulkan, Direct3D 12 and Metal samples stalls one frame in N, for a timing capture to catch.
- Dropped frames on Vulkan are measured by the display through `VK_EXT_present_timing` where the driver offers it, instead of estimated from the frame interval.
- `test/triangle --stall <ms>` sleeps every frame so vsynced presents miss refreshes, as the Direct3D 12 sample's does.
- The Windows installer ships `dxcompiler.dll` beside the Direct3D 12 capture library, so DXIL shaders have text, reflection and edits without a Vulkan or Windows SDK on the machine.
- **Present latency** on the frame meter: how long after the present call the display showed the frame, from `DXGI_FRAME_STATISTICS` on Direct3D 12 and `VK_EXT_present_timing` on Vulkan.
- An MCP session takes the captures an application asks for through `include/gpu_inspector.h`, saves them under the application's label and lists them (`get_session_status` `appCaptures`, `list_captures`).
- Direct3D 12 residency on the memory series and in memory captures: evictions, page-ins and changes of the driver's budget are marked and counted, with the bytes each named.
- `test/d3d12_triangle --evict` evicts a buffer and pages it back in on a cycle, for the residency marks.
- How-to guides for [a Unity player](docs/HOWTO_UNITY.md), [a Quest application](docs/HOWTO_QUEST.md) and [a WebGPU page](docs/HOWTO_BROWSER.md), beside the Minecraft one.
- **Viewport / Scissor**, a draw overlay that needs no replay: the rectangles in the draw's own state drawn over its render target, with what the scissor cuts away darkened. It works on a capture of any API, and on a saved one.
- `test/triangle --half-scissor` keeps the left half of the target, for that overlay.
- A Vulkan dynamic-rendering pass suspended and resumed across command buffers is timed across its parts, instead of being left out of the frame's GPU time.
- A Direct3D 12 render pass suspended across command lists is timed: every pass's queries are resolved from a list of the capture's own at the finish, instead of beside the query in the application's list where a suspended pass forbids it. A Unity frame goes from a fifth of its passes measured to all of them.
- `test/d3d12_triangle --suspend` splits its render pass across two command lists.
- Direct3D 12 passes are timed in the frame of recording before the capture as well, so an engine that builds a frame's command lists during the frame before it (Unity does) has that frame measured rather than reported without timings.
- A capture asked for while the application is live-paused captures the frame on the screen: the pause is held open for the capture's frames and closes again on the frame it captured, instead of resuming the application.
- **Validate**, a report that replays a Vulkan capture under the Khronos validation layer, whether or not the application was launched with it, with every message tied to the captured command it fired on; `get_validation` does the same with `replay: true`.
- `vkinsp_replay --validate-data <file>` writes the validation layer's messages with the command and phase each fired in.
- A capture hotkey in the application's own window: with the HUD on, F11 takes the capture the Capture button would, on Vulkan, Direct3D 12 and Metal; `VKINSP_HOTKEY` (`DXINSP_HOTKEY`, `MTLINSP_HOTKEY`) rebinds or disables it.

### Changed
- A timing or memory capture's report opens in a tab of its own beside the frame captures, instead of a band above them that pushed the capture tabs off the window.
- The application, its executable and its install directory are named `GPUInspector`, with no space; an updated install keeps the directory it was first installed into.

## v0.22.1

### Fixed
- The Linux `.deb` did not start after installing or updating to v0.22.0 on Ubuntu 24 and later. Naming the capture layer's `postinst` as electron-builder's `afterInstall` replaced its own install script rather than adding to it, so the package no longer loaded the bundled AppArmor profile, set the mode of `chrome-sandbox` or linked `/usr/bin/gpu-inspector`; without the profile the kernel denied Chromium's sandbox its user namespace and the app exited before its first window. Both scripts now do that work themselves.

## v0.22.0

### Added
- **Plugins**: a graphics API can be added as a plugin, with a backend module and a capture library (docs/PLUGINS.md).
- The plugin SDK in `src/sdk`: header-only C++ for a capture library's connection, and the backend types.
- **OpenGL ES** on Windows, from the desktop driver through WGL (Unity's `-force-gles32`) or from ANGLE, as the first plugin (`src/plugins/gles`, docs/GLES.md).
- `test/gles_triangle`, an OpenGL ES 3.0 test application on ANGLE.
- OpenGL ES on Android 10+, as an OpenGL ES layer, chosen with the Android launch's new *Graphics API* (or `--api=gles`).
- `tools/build_android_gles_triangle.py`, an OpenGL ES 3.2 test application for phones.
- OpenGL ES on Linux through EGL or GLX, preloaded into launched applications (not yet run on Linux).
- `test/gles_linux`, an OpenGL ES 3.2 test application for Linux that draws into a pbuffer.
- `dxinsp_launch.exe` takes `--dll` more than once, and calls a library's `GpuInspectorInitialize` export.
- **Direct3D 11** on Windows as a plugin (`src/plugins/d3d11`, docs/D3D11.md): the device context's calls, synthetic passes, every draw's state and read-backs, deferred contexts inlined, and pass timings.
- `test/d3d11_triangle`, a Direct3D 11 test application with deferred, compute, MSAA and discard options.
- `tools/gen_d3d11.py` generates the plugin's enum tables and vtable slots from the Windows SDK, as `gen_d3d12.py` does.
- Shader reflection attached to a plugin's shader objects is read like a Direct3D 12 pipeline's, and inlined child commands keep a plugin's own fields.

### Changed
- How an API's commands and objects read and what it can measure now come from one backend object per API (`renderer/backend.ts`).

## v0.21.0

### Added
- **Timing Capture** on Metal: every frame's wall time and category totals over minutes, which is how a hitch is told from a slow frame. The library already timed the same categories; what it lacked was the per-frame ring and the message.
- A Metal command buffer fault the session will not survive — a timeout, a page fault, revoked access — is reported as a device loss, so the diagnosis goes to the top of the log rather than among the validation messages. The per-encoder execution status was already recorded; what was missing was surfacing it as what it is.
- `MTLINSP_SIMULATE_GPU_FAULT=N` faults the Nth command buffer, so the reporting path can be exercised without hanging the GPU — the counterpart of `VKINSP_SIMULATE_DEVICE_LOST` and `DXINSP_SIMULATE_DEVICE_REMOVED`.
- The mesh view's **VS Out** on a Metal capture: the draw's vertex function run by the Metal Shading Language interpreter, which needed no replay and no second capture — the shader debugger already ran it to rasterize a pixel's inputs, and the mesh view simply never asked.
- Stencil attachments are read back on Metal, as a second read-back of the depth/stencil texture; a blit can fetch one aspect at a time.
- The render target tab opens an image's depth or stencil half rather than whichever read-back came first: two aspects of one image share an id, in all three APIs.
- The remaining Metal pixel formats: `RG8Unorm_sRGB`, the three-component formats macOS 27 added, and the stencil-only views (`X32_Stencil8`, `X24_Stencil8`). ASTC, ETC2 and EAC were already mapped, contrary to what the docs said.
- `test/metal_triangle --stencil` gives the triangle pass a combined depth/stencil attachment, with the stencil store left at `DontCare` so the capture library's forced store is what makes it readable.
- A capture's debug dump lists each render target's aspect and format, not only how many there were.
- Metal ray tracing: every acceleration structure as an object, the builds, refits and copies an acceleration structure encoder records, and the geometry, bounding boxes and instances each build read.
- A Metal acceleration structure opens in the structure tab like a Vulkan or Direct3D 12 one, with the tree, the instances, the overlaps and the mesh preview.
- Metal acceleration structures built before a capture are read back as it starts, so a bottom level an engine built at load is still drawn.
- Metal acceleration structure passes are timed, so the frame's build cost is in the pass list — they had no timing slot at all before.
- Metal intersection function tables: the function each entry holds, the buffers the table binds for them, and the pipeline's linked functions. Metal has no shader binding table, and this is what it has instead.
- Frame rules for Metal ray tracing: a geometry naming an intersection function table entry that is not there (which Metal does not check), a structure built twice in one frame, a top level built from no instances, and opaque geometry naming an intersection function.
- Metal acceleration structures count toward Memory Use, as the fourth kind beside buffers, textures and heaps.
- The ray tracing bindings Metal has on every stage: `setVertex`/`setFragment`/`setTile` acceleration structures and function tables, and the plural forms. Only compute's `setAccelerationStructure:` was recorded before, so a draw that traced rays showed nothing.
- `linkedFunctions` on a Metal render or compute pipeline: the intersection functions its traversal can call.
- `test/metal_triangle --ray-tracing` and `--static-blas`, the counterpart of `test/triangle`'s: triangle geometry in an acceleration structure, which `test/path_tracer/metal` has none of, and a bottom level built once at start-up.
- A Metal application turns up in the inspector's attach list, and steps to a free port when another inspected application has the default one — so two started by hand are both reachable without anybody choosing port numbers. An application with an inspector already attached is still missing from the list rather than shown as busy, which the Vulkan and Direct3D 12 libraries manage.
- **Sampled call stacks in a macOS timing capture**, which was Windows-only: every thread's stack 250 times a second, with whether it was running or blocked, so the hitch none of the timed calls accounts for can still be explained. Through Mach rather than a kernel trace, so it needs no privilege and no second process — and a thread already found waiting is not stopped again, which is what keeps it from causing the hitches it is there to find.
- **Ray queries in the Metal shader debugger**: `intersector::intersect` is followed rather than stepped over, with the traversal worked out over the geometry the capture read back — and for a procedural geometry the debugger steps *into* the shader's own intersection function, which is the only place "why is this sphere not hit" is answered. Metal has no hit shaders, so the one line a traced frame is about is inside the kernel.
- The buffers an intersection function table binds for its functions are read back as a capture starts. They are set once at setup, so no command of a captured frame binds them and nothing else would have read them — without them an intersection function steps with its arguments all zero.
- **Metal ray tracing replays and exports**: `mtlinsp_replay` re-creates the acceleration structures, re-runs the builds, refits and copies, fills the intersection function tables by function name, and **Export to C++** writes all of it — so a ray tracing frame can be handed to a driver team as a standalone project. Less work on Metal than on either other API: a geometry descriptor holds its buffers and a table entry names its function, where Vulkan and Direct3D 12 have device addresses and opaque identifiers to map back.
- A storage texture a compute pass wrote is read back after the frame and compared, on top of the render targets a pass ends with. Without it a frame whose work is all in compute — a path tracer, whose traced image is the whole output — compared nothing at all.
- A Metal replay says when a frame *cannot* be reproduced instead of reporting a difference: a kernel that reads the texture it writes accumulates into it, so the frame continues from contents the capture holds only as they were after it.
- **Shader editing on Metal**: a pipeline's stage recompiled from edited Metal Shading Language and bound in the running application from its next frame, with **Restore Original** to put the application's own back. Nothing has to be installed here — a Vulkan or Direct3D 12 edit is compiled by glslang or dxc on this machine, while a Metal capture holds the source the application compiled and the application's own device is the compiler, so its diagnostics come back and mark the lines of the text on screen.
- Function constants a Metal stage was specialized with are carried into an edit's recompile. Without them a library of `[[function_constant]]`-guarded variants would rebuild into a *different variant*, which compiles, draws, and looks like a clean edit.
- `--debug-view=shader-edit` works on a Metal capture, applying the edit to the running application rather than replaying: `:bad` for the compiler's diagnostics and `:compute` for a kernel, which reaches the rebuild by a different road since `newComputePipelineStateWithFunction:` has no descriptor to keep.
- **Draw overlays on Metal**, all five kinds: Highlight Draw, Depth Test, Stencil Test, Backface Cull and Wireframe, measured by drawing the pass again inside the application the way Direct3D 12's are. One draw per capture, and it lands in the same mask the Vulkan replay writes, so the render target tab and `get_draw_overlay` read all three backends the same way.
- A multisampled pass can be overlaid: the overlay draws at one sample per pixel, which is what a mask means. Only Depth Test and Stencil Test are left out there, since the runs that test start from a copy of the pass's depth and a multisampled depth attachment is not copied.
- `test/metal_triangle --inside-out` reverses the triangle's winding with back faces culled, so the draw leaves no pixel at all — the bug Backface Cull exists for, and a draw only the cull-off run can find.

- `get_draw_overlay` answers for a Metal or Direct3D 12 capture that measured one, instead of refusing every capture that is not Vulkan, and says which draw the capture measured when asked about another.
- `--debug-save-delay=<ms>` saves a debug capture later than the default four seconds, for a flow whose answer is in a second capture.

### Fixed
- A hex literal crashed the shader debugger: the numeric suffix was read greedily, so `0xFF` came apart as `0x` with an `FF` suffix and threw. Every shader with a ray mask or a bit field in it brought the debugger down before it ran an instruction.
- A Metal replay decoded an enum written under a name its own table does not have as *zero*, which for most Metal enums means "invalid": an acceleration structure built from a geometry whose vertex format decoded that way holds nothing, so every ray of the replayed frame missed and the report read like a clean run. Unknown names now fall back to what the caller asked for, which is also right for an enumerator from a newer SDK than the replay was built against.
- A measured draw overlay was dropped when the capture was saved, on Metal and Direct3D 12 both: the measurement is taken in the application and cannot be taken again from the file, so the capture was the only copy of it.
- A draw overlay of a pass with no depth or stencil attachment counted every covered pixel as *rejected* on Metal, rather than as passed: nothing to test against rejects nothing, which is what the other two backends already reported for the same pass.
- "Waiting for a swapchain image" is now "Waiting for the display": the three APIs do not agree on the noun (a swapchain image, a drawable, a waitable object), and the Vulkan one was being shown for all of them.
- A Metal pass's stencil was read back as whatever the tile memory held (0xFF, in testing): the capture library forced the store on color and depth attachments but not stencil, and an application that only tests stencil sets `DontCare`.
- Acceleration structure input buffers are read back whole on all three backends rather than truncated at `maxBufferSize` (64 KB by default), which clipped a real bottom level's geometry to its first few hundred triangles.
- `setAccelerationStructure:atBufferIndex:` in a Metal capture recorded `null` for the structure it bound: nothing tracked `MTLAccelerationStructure`, so there was no object for the reference to name.
- A stray connection reset from the target probe's own peer could fail whichever UI test happened to be running (`test/target_probe.test.js`).

## v0.20.0

### Added
- A frame rule for a shader binding table DXR will not accept: a table not on a 64-byte boundary, a stride not a multiple of 32, or a trace with no ray generation record.
- A top level's scene draws a procedural bottom level with the bounding boxes it was built from, rather than a stand-in cube per instance.
- The mesh view has RenderDoc's Arcball and Fly cameras, and Wireframe, Solid and Flat shading.
- An acceleration structure opens in a tab of its own, from the Inspect panel or from any command that names it, with the mesh view's camera and shading.
- The object list says which acceleration structures the open capture can draw, and why not for the others.
- A Direct3D 12 acceleration structure is named after the buffer it lives in.
- The mesh preview frames on where the geometry is, so one huge primitive does not shrink the rest to a speck.
- An acceleration structure built before the capture is read back as the capture starts, drawn, and built again by both replays.
- The acceleration structure tab has a tree of instances, bottom levels and geometries with their primitives, surface area and memory.
- The tree hides any row, searches by name, and draws every instance's bounding box.
- **Overlaps** lists the instances whose bounding boxes overlap and colors the scene by it.
- Both capture libraries record the size the driver gives each acceleration structure build.
- The mesh view has Points, Wireframe + Solid and Smooth shading, and flat or smooth shading from a normal attribute.
- The mesh view colors the vertices by any attribute, draws the normals, and takes its positions from any VS In attribute.
- Clicking a primitive in the mesh preview selects it, and hovering names it.
- **Zoom to Selected** and camera bookmarks (Ctrl+1-9 to keep, 1-9 to return) in the mesh and structure views.
- `test/triangle --static-blas` builds the bottom level once, at start-up.

### Fixed
- Capturing an application that binds a ray tracing acceleration structure as a root SRV shut it down: the read-back needed a barrier on a resource that may never leave RAYTRACING_ACCELERATION_STRUCTURE, which closed its command list with E_INVALIDARG.
- The D3D12 capture library asked a hit group for a shader stack size, which raised validation errors in the application's own log.
- `test/path_tracer/d3d12` laid its shader tables out back to back at the record stride, so the miss and hit tables were not 64-byte aligned and the runtime dropped every trace.
- A top level's scene draws the stand-in boxes of instances with no captured geometry beside the ones with triangles, instead of leaving them out.
- A scene draws every geometry of a bottom level, not only its first.
- Both replays upload a command group's buffer contents in one submission: a Quake II RTX frame's 238,000 took three minutes one at a time, which every View Mesh waited through, and now take two seconds.
- A Vulkan build of several geometries keeps each one's primitive count: the layer recorded only the first, and the replay and the scene used it for all.

## v0.19.0

### Added
- How-to guides in the documentation, starting with [inspecting Minecraft Bedrock](docs/HOWTO_MINECRAFT.md), which the inspector waits for rather than launches.
- **Capture child processes** in the launch window puts the Direct3D 12 capture library into every process the target starts, for a game behind its own launcher (`--follow-children`).
- Ray tracing on Direct3D 12: state objects with their exports and shader identifiers, acceleration structures with what each build read, and a trace's shader binding table resolved to the export every record runs.
- DXR replays in `dxinsp_replay`: state objects are rebuilt, a build's addresses and its instances' bottom level references are remapped, and the binding table is rebuilt with this machine's shader identifiers.
- `--ray-tracing` and `--rebuild-blas` in `test/d3d12_triangle`, the DXR counterpart of `test/triangle --ray-tracing`.
- **Attach...** on the main bar lists the applications already running with a capture library in them -- name, API, process id, port -- and attaches to the one picked, in place of the bar's port box and **Connect**.
- A capture library with no port set listens on the first free port of a small range, so several applications started by hand are all reachable at once.
- `--list-targets` names every application a capture library is serving right now, for attaching to one without knowing its port.
- **Timing Capture** on Direct3D 12: every frame's time and CPU split over minutes, as on Vulkan.
- **Sample stacks** (Windows): a timing capture samples every thread's call stack, running or blocked, and the report says what each thread was doing in the worst hitch or the stretch dragged out.
- **Memory Capture** on Vulkan and Direct3D 12: every allocation and free, and a report of what is still held, what was transient, and which frames allocated.
- `include/gpu_inspector.h`: `gpu_inspector_capture(frames)` asks for a capture from inside the application, on Vulkan and Direct3D 12.
- **Compile & Replay** in the shader editor: the open capture replayed with the edited stage, and each render target it changed shown as captured, with the edit, and where they differ (`--replace` in both replay tools).
- **Measure draws** by replay on Direct3D 12 (`dxinsp_replay --draws`), for a capture that was not taken with the option, or a file.
- **Measure shader** on Direct3D 12: variants of the stage's DXIL edited as its disassembly and assembled by dxc (`dxinsp_shader --assemble`), timed by `dxinsp_replay --ablate`.
- The Shader Flame Graph weighs Direct3D 12 stages by function and line, from the SPIR-V their HLSL compiles to.
- `--churn`, `--capture-at N` on both samples and `--heavy` on the D3D12 one, for the memory capture, the capture API and shader measurement.
- **Measure draws** on Direct3D 12: a timestamp pair, a pipeline statistics query and an occlusion query around every draw and dispatch, so the Shader Flame Graph splits a pass between its draws.
- Draw overlays on Direct3D 12: Highlight Draw, Depth Test and Wireframe, measured by issuing the draw again inside the application while its next frame is captured.
- The mesh view's VS Out on Direct3D 12: the draw's vertex shader outputs streamed out of the unmodified bytecode, with the root signature copied to allow stream output.
- **Measure hardware counters** on Direct3D 12: `dxinsp_replay --counters` reads the GPU's own counters around each render pass, as `vkinsp_replay` does for Vulkan.
- `dxinsp_replay --list-counters` names every metric the GPU offers, and the counter run says so up front when the machine keeps performance counters to administrators.

- Pixel history lists the fragments of a draw that put several on the pixel, each with the primitive it came from and what its shader wrote, and marks the one that won.
- **Stencil Test** and **Backface Cull** draw overlays, on Vulkan and Direct3D 12: the stencil test apart from the depth one, and the pixels a draw's own culling emptied.
- `get_draw_overlay` gives Claude where a draw landed and what its tests and its culling did with it.
- `test/triangle --inside-out` draws half the cube wound the other way, so its own culling throws all of it away.
- `test/triangle --no-cull` keeps the cube's back faces, so one draw puts two fragments on a pixel.

### Fixed
- An injection that worked is no longer reported as a library that would not load: the Direct3D 12 launcher loads the library and runs its initializer with one stub in the target, instead of reading a module list that a process held at start-up will not give up.
- Cancel sits at the right of every dialog's buttons; the launch and attach windows had it at the left.
- A Direct3D 12 command list that draws with the pipeline state its `Reset` named had no shaders in the Shader Flame Graph or Analyze Shaders.
- The comparison page said a lost device goes undiagnosed; both backends name the command it stopped on.
- A measurement taken inside the application is attached to the right command: a capture library names a command by its slot within its command list, which is not its index in the capture.
- The primitive a draw's pixel history reports is the one that won the pixel: the primitive-id pass tested against the depth the draw started from, so a draw whose own fragments hid one another named the wrong one.
- A Direct3D 12 draw overlay opens on the draw's own render target, not on whichever image of the capture came first, which could be one the frame only sampled.
- A Direct3D 12 mesh view draws its vertices: the records arrive after the layout they belong to, and the view only drew on the first of the two.

## v0.18.0

### Added
- An exported C++ project shows its frame in a window, run in a loop for a profiler to attach to, with its own small window source per platform; `--batch` is the headless comparison it did before.
- **Export to C++** opens the written folder, starts its dialog where the last export went, has a `{C++}` icon, and says why when the replay fails.
- **Metal replay and Export to C++** (`mtlinsp_replay`, `src/metal/replay/`, docs/REPLAY.md "Metal"): a Metal capture is re-executed on this machine's GPU, every render target it read back is compared byte for byte, and the frame is written out as a standalone CMake project of Objective-C++ that runs it again — the third backend to have both, after Vulkan and Direct3D 12. The capture bar's **Export to C++** and the MCP server's `export_cpp` now take Metal captures.

### Fixed
- An image's details no longer show an empty dark box where the histogram sits when the histogram is off.
- The launch dialog's capture options wrap onto as many rows as it needs rather than running off the side of the window.
- A D3D12 capture of a Unity URP frame holds the whole frame: lists after a suspended render pass were lost to a vtable the hooks had missed, pooled lists had no recorder, and the draws of suspended passes had no mesh, constant or texture data.
- A D3D12 capture keeps the objects a frame creates and releases within itself, which were gone by the time it was shown or saved.
- A D3D12 capture reads what `CopyBufferRegion` and buffer-to-texture copies read, and vertex and index buffers whole, so a replay has them.
- `capture_frames` no longer saves a capture cut short when the capture library pauses for seconds while it converts large textures.
- A Metal capture's color attachment holds its own contents, not the depth attachment's: a depth attachment is announced under attachment index 0, the same as color attachment 0, and `CaptureTextureData` did not carry the aspect to tell the two apart, so the depth read-back landed on the color entry. Found by the new replay, which read back a color target full of floats.
- A Metal library built ahead of time (`newLibraryWithURL:`, `newLibraryWithFile:`, `newDefaultLibrary`, `newDefaultLibraryWithBundle:` — what a shipped player uses) carries its metallib bytes in the capture, so it can be re-created without the file it was loaded from.
- Capturing a Direct3D 12 frame no longer crashes a Unity player: a list left open at the frame boundary kept the capture's queries open and failed to close, which Unity read as a lost device.

## v0.17.0

### Added
- **A web page in a browser (WebGPU)** in the launch dialog captures a page's WebGPU work as Direct3D 12: pick one of the Chrome, Chrome Canary, Edge, Brave, Firefox or Firefox Nightly installs found on this machine, type a URL, and the library goes into the browser's GPU process (docs/BROWSER.md).
- **Export to C++** writes a Vulkan capture's frame as a standalone CMake project that re-creates its objects, runs the frame and compares its render targets with the capture's, for driver bug reports (`vkinsp_replay --export`, `export_cpp`).
- **Export to C++** writes Direct3D 12 captures too, from `dxinsp_replay`, a new tool that replays a D3D12 capture and compares its render targets with the capture's.
- A Windows launch can follow the target's own child processes, which is how that works: **Follow child processes** in the launch dialog, `follow` from `launch_app`, `--follow` in `dxinsp_launch`.
- A followed launch waits for the target's whole process tree, so a target whose first process starts the real one and exits (Firefox, a game's launcher) is followed into the process that renders.
- Each of a capture's reports opens in a tab beside the capture's, with **Open in New Window** on the tab (a copy of the capture there, opened on that report) and on its context menu.
- Every report, and the render target tab, has an **Export to HTML** control: the report as it is on screen, with the application's styles in it, as a standalone file.
- The Shader Flame Graph zooms continuously with **Ctrl+Wheel** about the pointer and pans by dragging, besides the click zoom it already had.

### Fixed
- A D3D12 draw shows the textures its descriptor table held when it drew, not when the table was bound: an engine that binds first and writes descriptors after (Unity) showed the previous frame's.
- A D3D12 bundle recorded before the capture has its vertex, index and constant buffer contents in the capture, read back by the list that executes it.
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
- Metal frame stats (frame time, submit time, refresh rate), and the capture bar's options honored by the library.
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
