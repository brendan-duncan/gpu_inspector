# GPU Inspector — Architecture

[Docs index](README.md) › Architecture

A cross-platform (Windows, Linux, macOS) graphics inspector for native applications, the native
counterpart of [WebGPU Inspector](https://github.com/brendan-duncan/webgpu_inspector). Vulkan was
the first API: it works with any uninstrumented Vulkan application by interposing a Vulkan layer,
and it targets Unity Vulkan players first. The UI and protocol are API-neutral (see Multi-API
below), so another API is another capture library speaking the same protocol. Metal is the second
one, described in `src/metal/README.md`, and Direct3D 12 the third, described in `src/d3d12/README.md`;
what follows is the Vulkan side.

## Design decisions

| Topic | Decision |
|---|---|
| Capture model | WebGPU Inspector style: commands are recorded in-process during the captured frame and GPU resource contents (render targets, buffers, textures) are read back at capture time. There is no replay. Captures can be saved and reopened for viewing. |
| Attach model | Launch from the inspector first: the layer is enabled per-process through environment variables, nothing is registered. For applications started elsewhere (an editor, a game behind its launcher) the layer can be registered as an implicit layer for the user (`main/implicit_layer.ts`: the registry under HKCU on Windows, a manifest in `implicit_layer.d` on Linux) and loads into any process started with `VKINSP_ENABLE=1`, which a session then waits for. A Vulkan layer cannot join a process after its instance exists, so "attach" means "start the application with the variables set". Direct3D 12 has no registration of any kind, so its counterpart is a watch: `dxinsp_launch.exe --watch <image>` polls for the process and injects the capture library as it starts, freezing it until the hooks are in (`src/d3d12/README.md`). Both catch an application at its start; neither joins one that is already running. |
| Targets | Local processes, and Android devices through adb (see Android below). The layer and UI talk over TCP, so other remote targets can be added without changing the protocol. |
| Device features the layer adds | Three, each optional and each with a fallback to the application's own create info if the driver refuses: a refresh-period extension (`refresh_rate.h`), dynamic rendering for multisampled depth read-back (`depth_resolve.h`), and `pipelineStatisticsQuery` for the per-pass GPU counters (`pipeline_stats.h`). |
| Native language | C++20, CMake. MSVC on Windows, GCC/Clang on Linux, Clang on macOS. |
| UI | Electron, written in TypeScript throughout (esbuild bundles, `tsc` type-checks), including the widget library ported from WebGPU Inspector. |
| Distribution | electron-builder installers (Windows NSIS, Linux .deb, macOS .dmg) bundling the capture library under `resources/layer`, built by a GitHub Actions workflow on version tags, with electron-updater self-update from the GitHub releases. See `docs/RELEASING.md`. |
| Handles | Pass-through. The layer never wraps Vulkan handles; it keeps side tables keyed by handle and uses the loader's dispatch pointer (first word of each dispatchable handle) to find its per-instance/per-device state. This is what RenderDoc's `vk_dispatchtables.cpp` does for tables, and it avoids RenderDoc's 20k+ lines of handle unwrapping. |
| Code generation | Everything mechanical is generated from `vk.xml` (Vulkan-Headers submodule): dispatch tables, forwarding entry points, object create/destroy hooks, and JSON serializers for every struct, enum, bitmask and command signature. |
| Multi-API | Vulkan first, Metal second, Direct3D 12 third. The UI and protocol are API-neutral (objects with a class, a descriptor and dependencies; commands with arguments; passes; resources), so another API is another capture library speaking the same protocol. `src/d3d12/` is a library injected at process start by `dxinsp_launch.exe` that hooks the D3D12 and DXGI entry points and vtables, synthesizes the pass boundaries D3D12 does not have, and speaks the Vulkan layer's wire format byte for byte (`src/d3d12/README.md`). On Windows every local target is started with both the layer and the library, and the API is known by which one connects. |
| Reference code | WebGPU Inspector (MIT), RenderDoc (MIT), GFXReconstruct (Apache-2.0). Adapted files name their origin; see `THIRD_PARTY_LICENSES.md`. |

## Repository layout

Everything that is built lives under `src/`; everything around the build stays at the root.

```
src/app/        the Electron UI (main, renderer, preload) and the MCP server of the plugin
src/vulkan/     the Vulkan capture layer, and the code generated from vk.xml
src/metal/      the Metal capture library (macOS)
src/d3d12/      the Direct3D 12 capture library, its launcher and its shader tool (Windows)
src/replay/     vkinsp_replay, which re-executes a Vulkan capture
test/           the test applications each backend is exercised against
tools/          the generators, the build and setup scripts, and the UI test harness
docs/           this documentation
claude-plugin/  the Claude Code plugin, with the MCP server bundle committed in server/
third_party/    submodules (Vulkan-Headers, MinHook)
build/          CMake's binary tree; every native target lands in build/bin
```

The top-level `CMakeLists.txt` adds `src/vulkan`, `src/metal`, `src/d3d12` and `src/replay` per
platform; the app is built with npm from `src/app`.

## Components

```
+---------------------------+          TCP (JSON + binary frames)          +--------------------------+
|  Target application       |  <---------------------------------------->  |  Inspector app (Electron)|
|                           |                                              |                          |
|  vulkan-1 loader          |                                              |  main process:           |
|    +-- VK_LAYER_INSPECTOR_capture  (src/vulkan/)                         |    launches target with  |
|    |     dispatch + forwarders (generated)                               |    layer env, owns socket|
|    |     object tracker                                                  |  renderer:               |
|    |     frame capture + GPU readback                                    |    object database,      |
|    |     transport (TCP server)                                          |    inspect / capture UI  |
|    +-- ICD (driver)                                                      |    (WebGPU Inspector     |
+---------------------------+                                              |     widgets)             |
                                                                           +--------------------------+
```

The right half is the same for every API. On macOS `src/metal/` takes the layer's place, inserted by
dyld (`src/metal/README.md`). On Windows `src/d3d12/` sits beside the layer: `dxinsp_launch.exe` starts the
target suspended and injects `dxinsp_capture.dll`, which hooks `D3D12CreateDevice` and
`CreateDXGIFactory*` and patches the vtables of the objects they hand out; `dxinsp_shader.exe`
gives the app DXBC/DXIL disassembly, embedded HLSL and reflection the way `spirv-dis` and
`spirv-cross` give it SPIR-V's (`src/d3d12/README.md`).

### src/vulkan/ — the Vulkan layer

* `src/layer.cpp` — loader negotiation, `vkCreateInstance`/`vkCreateDevice` chaining, dispatch
  registry, `vkGet*ProcAddr`, frame boundary at `vkQueuePresentKHR`.
* `gen/` — generated by `tools/gen_vulkan.py`:
  * `vk_dispatch.gen.h` — `InstanceDispatch` / `DeviceDispatch` function pointer tables.
  * `vk_entry.gen.cpp` — one forwarding entry point per Vulkan command. Command-buffer
    commands call the command recorder after forwarding; object-creating and destroying
    commands call the tracker.
  * `vk_serialize.gen.{h,cpp}` — `ToJson()` for every struct, `ToString_*` for every enum,
    `Flags_*` for every bitmask, `PNextToJson()`, and `ArgsToJson_vk*()` for every command.
  * `vk_commands.gen.h` — command ids and handle type ids.
* `src/json_writer.h` — streaming JSON writer. Handles are written through a `HandleResolver`
  as `{"__id": N, "__class": "VkImage"}`, the same shape WebGPU Inspector uses.
* `src/tracker.*` — live object database: id, class, parent, creating command and its
  serialized arguments (the descriptor), label. Emits `AddObject` / `DeleteObject` /
  `ObjectSetLabel`. On connect, the whole live set is sent as a snapshot, so the UI can
  connect at any time.
* `src/transport.*` — TCP server on `VKINSP_PORT`. Frames are `u32 length, u8 kind, payload`;
  kind 0 is UTF-8 JSON, kind 1 is `u32 headerLength, JSON header, raw bytes`.
* `src/descriptors.*` — descriptor set contents: follows `vkCreateDescriptorSetLayout`,
  `vkAllocateDescriptorSets`, `vkUpdateDescriptorSets`, update templates and push descriptors, so
  a bind command during a capture can carry a snapshot of what each bound set contained. The
  Inspect panel asks for a set's current contents with `RequestDescriptorSet {id}`, answered by
  an `ObjectUpdate` carrying `bindings` (updates are not streamed: engines rewrite thousands of
  sets per frame).
* `src/capture.*` — frame capture (see below).

Enabling the layer for a process (what the app does when launching):

```
VK_ADD_LAYER_PATH=<dir containing VK_LAYER_INSPECTOR_capture.json>
VK_LOADER_LAYERS_ENABLE=VK_LAYER_INSPECTOR_capture
VKINSP_PORT=<port>
VKINSP_LOG=1            (optional, stderr + debugger logging)
VKINSP_LOG_FILE=<path>  (optional, also append the log to a file; GUI apps such as Unity players have no stderr)
```

### Android

The same layer, built with the NDK (`tools/build_android.py`), runs inside Android applications.
Android differs from the desktop in how a layer gets into a process and how it is configured, and
the approach follows RenderDoc's Android support (`renderdoc/android/android.cpp`), minus its
device-side server:

* **Loader.** Android's loader has no manifests: it dlopens every `libVkLayer*.so` in its layer
  directories and resolves `vkEnumerateInstance{Layer,Extension}Properties` and the two
  `GetProcAddr`s by name, so the Android build exports them (end of `layer.cpp`).
* **Frame boundaries.** An OpenXR application never presents (the runtime composites), so
  the layer switches, after sixty submissions without a present, to ending frames at the
  application's `vkWaitForFences` following a submission (or at every submission when it never
  waits); see `EndFrame` in `layer.cpp`. Multiview passes read back the view mask's layers.
  `test/android_triangle` is the phone counterpart of the desktop triangle: a NativeActivity
  with a `VK_KHR_android_surface` swapchain (FIFO, the surface's pre-transform undone in the
  projection), a transient unstored depth buffer and one instanced draw, built debuggable by
  `tools/build_android_triangle.py`; on a Quest it only runs as a background 2D panel without
  a window. `test/xr_triangle` is an OpenXR NativeActivity (one stereo swapchain, a multiview pass with
  `gl_ViewIndex`, a transient unstored depth buffer, one instanced draw) that
  `tools/build_xr_triangle.py` builds against the Khronos loader AAR and packages debuggable
  twice: the same library as `com.brendanduncan.xrtriangle` and as `...xrtriangle.slow`, which
  reads its package name through JNI and renders with deliberate inefficiencies (a pass per
  eye, a clear command plus loadOp LOAD, a stored depth buffer, a draw and a bind per triangle,
  a wasteful fragment shader) for the frame analysis and shader analysis to flag; both verified
  on a Quest 3. Its manifest declares hand tracking as an input
  option, since the Quest shell otherwise refuses to launch an application until controllers
  are on, and a launch check dialog left behind by such a refusal blocks later launches until
  the shell restarts.
* **Connecting.** The layer's abstract socket is `@vkinsp:<port>:<package>` (the package from
  `/proc/self/cmdline`), so an application launched earlier with the layer and left running
  cannot answer for a new one on the same port. The launcher force-stops the package and
  waits for its process to be gone before starting it, since a dying instance still holds the
  socket, and warns when `/proc/net/unix` still lists the name; the layer's listener retries a
  bind refused with `EADDRINUSE` for 30 s (also on the desktop); and a connection attempt
  refused on the host is checked against `adb forward --list`, because adb drops a device's
  forwards when the device reconnects (a headset's USB link blips with its power state).
  After the start, `_launchDiagnostics` reads `dumpsys power` and `dumpsys window` and logs
  what would keep the application from running: a device that is asleep (an OpenXR session
  stays idle until the headset is worn) and a headset shell's launch check dialog
  ("controllers required"), which a launch attempted without controllers or tracked hands
  leaves behind and which blocks every later launch until the shell restarts.
* **Getting into the process.** `src/app/src/main/android.ts` uses Android's GPU debug layer settings
  (`settings put global enable_gpu_debug_layers 1`, `gpu_debug_app <package>`,
  `gpu_debug_layers VK_LAYER_INSPECTOR_capture`). On Android 10+ the layer comes from the
  **layer APK** (`build/android/gpu_inspector_layer.apk`, a package with no code that only carries
  the library, named in `gpu_debug_layer_app` the way RenderDoc's own APK is); the inspector
  installs it when the device's copy has a different version (the version name is a hash of the
  library, installed `--force-queryable` and declaring `forceQueryable`: Android 11+'s package
  visibility would otherwise hide it from the target and the loader would not find it). On
  Android 9 the library is copied into the application's data directory with
  `run-as`, which the loader searches too. Either way the application must be debuggable, or the
  device rooted: Android permits nothing else.
* **Transport.** The layer listens on an abstract Unix socket (`@vkinsp:<port>`), which needs no
  INTERNET permission in the target (most applications lack it and TCP sockets fail with
  EACCES); `adb forward tcp:<port> localabstract:vkinsp:<port>` reaches it.
* **Configuration.** An Android app inherits no environment. `ConfigValue()` in `layer.cpp` maps
  each `VKINSP_*` variable to a `debug.vkinsp.*` system property (`VKINSP_PORT` ->
  `debug.vkinsp.port`), which `adb shell setprop` can set without root; RenderDoc's `debug.rdoc.*`
  properties are the same idea.
* **Log.** The layer logs to logcat (tag `vkinsp`), which the session streams into its Log tab
  together with native crash dumps; the process is watched with `pidof`. Closing the last session
  on a device deletes the debug layer settings again.
* **Not needed.** RenderDoc runs a remote server on the device to start packages, copy the capture
  file back and replay it there. Captures here stream straight over the socket and there is no
  replay, so the layer is the only device-side component. Replay-based features (see TODO.md)
  would need a device-side replay process for Android, since a desktop GPU cannot replay a Mali or
  Adreno capture faithfully.

The generated serializers already cover `VK_KHR_android_surface` and the Android hardware buffer
extensions (guarded by `VK_USE_PLATFORM_ANDROID_KHR`). The readback path invalidates mapped
memory, so non-coherent host-visible memory types, common on mobile GPUs, are read correctly, and
swapchain images only get `TRANSFER_SRC` when the surface supports it. What differs is cost: a
tiled GPU has to flush its tiles for the per-pass attachment copies, so the captured frame is
much slower than on the desktop.

### Frame capture

1. The UI sends `Capture {frameCount, atFrame?}`; the layer arms at the next `vkQueuePresentKHR`,
   or, with `atFrame`, when the device's present counter reaches that frame (frame 0 starts with
   its first `vkBeginCommandBuffer`, so the very first frame can be captured whole). The launch
   dialog's "Queued Capture" uses this (or a timer in the UI for "after N seconds") to capture
   automatically once the application connects; `--capture-frame=N` / `--capture-after=S` do the
   same from the command line.
2. During the captured frame every `vkBeginCommandBuffer` attaches a `CommandRecorder`; the
   generated forwarders serialize each `vkCmd*` call's arguments into it.
3. At `vkCmdEndRenderPass` / `vkCmdEndRendering` the layer appends its own commands to the
   application's command buffer: barriers plus `vkCmdCopyImageToBuffer` of each attachment into a
   staging buffer, then barriers back to the pass's final layout. This is the in-process
   equivalent of WebGPU Inspector's pass-end readback. To make it possible, `vkCreateImage` and
   `vkCreateSwapchainKHR` get `TRANSFER_SRC` added to their usage, and `vkCreateBuffer` gets
   `TRANSFER_SRC` (the object record keeps the application's own arguments: the generated
   forwarders serialize what was passed before a pre-hook substituted it). An attachment with
   storeOp DONT_CARE has undefined contents after the pass, so `vkCreateRenderPass*` also
   creates a store-everything copy of any pass with such an attachment (`StoreAllRenderPass` in
   `hooks.cpp`, straight through the dispatch table so it never appears as an object; store ops
   do not take part in render pass compatibility, so the application's framebuffers and
   pipelines work with it) and the begin pre-hooks substitute it, or rewrite a
   `VkRenderingInfo`'s attachments, while a capture is being recorded (or under "record
   always", whose recordings a later capture may show); the post-hooks and the command record
   see the original. A command buffer recorded before the capture began carries no copies, so
   every recorder keeps its passes (`RecordedPass`, with whether copies were recorded) and
   `ReadBackAfterSubmit` copies the attachments of the others when the buffer is submitted
   during the capture: a command buffer of the layer's (the live read-back's pool) with the
   same `RecordImageCopy`, submitted right behind the application's on the same queue and
   waited for, the layouts taken from the layout tracker after the submission.
   - **Split submissions.** When such a buffer is followed by others in the same submission,
     the submit pre-hooks (`PreHook_vkQueueSubmit`, `PreHook_vkQueueSubmit2`) split the
     submission after it, so a later buffer cannot overwrite a target before it is read back.
     Each part up to such a buffer is submitted with `ReadBackSubmitted` right behind it. The
     application's own call submits the last part, with its fence.
   - **Semaphores.** A submit info split across parts waits on its semaphores in its first part
     and signals them in its last. Timeline semaphore values follow the semaphores.
   - **Left whole.** A submission whose infos carry any other extension structure is not split.
   - **What the post-hooks see.** They record the submission as the application made it.
   - **Limits.** Two passes of one such buffer writing the same image still read back
     after both, and such buffers have no pass timings.

   The captured frame pays for the extra stores (a Quest's
   stereo pass took 5.1 ms captured against 2.9 ms live), which the pass timings of a capture
   include. Multisampled attachments are resolved (`vkCmdResolveImage`, color only) into
   a temporary single-sampled image owned by the capture before the copy; dynamic rendering's
   resolve targets are captured too. `RecordImageCopy` in `capture.cpp` records the barriers,
   resolve and copy for every image read-back, including the live one. Depth cannot go through
   `vkCmdResolveImage`: `src/depth_resolve.*` resolves it with an empty dynamic rendering pass
   (the multisampled image as depth attachment, loadOp LOAD, the temporary image as its
   resolve target with `VK_RESOLVE_MODE_SAMPLE_ZERO_BIT`, which every implementation supports)
   through image views owned by the capture. For that the layer enables dynamic rendering at
   device creation when the physical device offers it: the feature alone on 1.3, plus
   `VK_KHR_dynamic_rendering` below 1.3 and its `VK_KHR_depth_stencil_resolve` /
   `VK_KHR_create_renderpass2` dependencies below 1.2 (a device the application created with
   its own `VkPhysicalDeviceVulkan13Features` or dynamic rendering feature struct is left as is
   and used when the flag is on). A depth-stencil attachment is read back as two textures, its
   depth (`aspect: "depth"`) and its stencil (`"stencil"`, one byte per texel), each its own copy;
   the resolve pass resolves both aspects, so a multisampled stencil goes the way depth does.
4. Bound buffers are read back too, like WebGPU Inspector captures the buffers of each bind group
   and vertex/index binding. `vkCmdBindDescriptorSets` (and push descriptors) gets a
   `descriptors` snapshot of every bound set, taken from the descriptor tracker: per binding the
   type and per element the buffer range (with the dynamic offset applied), image view, sampler,
   layout or texel buffer view. Every buffer range in it, every `vkCmdBindVertexBuffers` /
   `vkCmdBindIndexBuffer` range and every indirect argument buffer is queued for readback
   (`CaptureManager::QueueBufferCapture`), truncated to `maxBufferSize` (64 KB by default). The
   copy is recorded at once outside a render pass and at the end of the pass otherwise (transfers
   are not allowed inside one); secondary command buffers hand their pending copies to the primary
   that executes them. A dynamic rendering pass suspended at the end of one command buffer and
   resumed in another (`VK_RENDERING_SUSPENDING_BIT` / `RESUMING_BIT`) may have nothing recorded
   between its parts, so the suspended part gets no copies, timestamps, queries or attachment
   read-back at all: its pending copies go to the device's capture record and the part that resumes
   the pass records them after it ends, with its own and the attachment read-back; such a pass is
   neither timed nor counted. The binding command references each capture by id (`data` in the
   snapshot, `bufferData` for vertex/index/indirect bindings).
   The contents the frame starts from are read back too, since a replay needs them and the read-backs
   above only see what the frame shows on its way. The transfer commands get pre-call hooks: the
   source of `vkCmdCopyBuffer` and `vkCmdCopyBufferToImage` (and their `2` forms) is queued whole
   (not truncated to `maxBufferSize`) into the command's `bufferData`, which covers a staging buffer
   the host writes every frame. For images, the capture keeps a state per subresource: untouched,
   read, or written whole (a clear, a copy or blit over the whole extent, a pass that does not load
   the attachment and whose render area covers it). The first read of an untouched subresource (a
   render pass or `vkCmdBeginRendering` attachment with loadOp LOAD or NONE, the source of a copy,
   blit or copy to a buffer) copies it before the command runs (`SnapshotImageRead`, one mip per
   texture entry of `kind: "initial"`, recorded outside the pass in the pre-hook, under the
   `maxImageTotal` budget with the sampled images), and the command carries the capture ids in
   `imageData`. Reads of what the frame wrote itself take nothing: a Unity frame, whose one loading
   pass loads the depth the pass before it cleared, takes no copies at all. A depth-stencil image's
   stencil is tracked and taken apart from its depth (a render pass's `stencilLoadOp`, dynamic
   rendering's `pStencilAttachment`). Multisampled contents are not taken, and the state follows recording order, so
   command buffers recorded in another order than they run can take a copy after a write.
5. `vkQueueSubmit` records which command buffers ran in which order; the command buffer's record
   is frozen at submit so later re-recording does not disturb the capture.
6. At the next present the layer waits for the frame's work, maps the staging memory, and streams
   `CaptureFrameCommands`, `CaptureTextureFrames` + `CaptureTextureData`, then `CaptureBuffers` +
   `CaptureBufferData` messages, then `CapturePassTimings`, then (a capture with `overdraw`)
   `CaptureOverdraw` + `CaptureOverdrawData`, then (a capture with `pixelHistory`)
   `CapturePixelHistory`. `CaptureComplete` comes last, whichever
   sections the capture had, so a client waiting for the capture (the MCP server) knows the stream
   has ended. The Metal and D3D12 libraries do the same, and they are the ones that measure the last
   two, in the application while it captures, since neither can be replayed.
7. Secondary command buffers arrive as `children` of their `vkCmdExecuteCommands` entry; the UI
   inlines them into the primary's command stream (Unity records every draw in secondaries).
8. Multi-frame captures: every command and render target carries a frame ordinal (a render target
   belongs to the frame its command buffer was submitted in); the UI shows one tab per frame.
9. Profile passes: with `profilePasses` the layer creates a timestamp `VkQueryPool` for the
   capture and brackets every render pass with `vkCmdResetQueryPool` + `vkCmdWriteTimestamp`
   (top of pipe, before the pass, in a pre-hook since resets are not allowed inside a pass) and
   `vkCmdWriteTimestamp` (bottom of pipe, after it). Results are read at finish and sent as
   `CapturePassTimings` (start and duration in ms, using `timestampPeriod`). Vulkan has no
   compute pass, so runs of dispatches outside a render pass are timed as one "compute pass":
   a dispatch opens it (pre-hook, begin timestamp before the dispatch) and the next barrier,
   event wait, render pass begin, debug label, secondary execution or the end of the command
   buffer closes it (pre-hooks, end timestamp before that command). Each command buffer counts
   its compute passes separately from its render passes (`kind` in the timing).
   - **Several devices.** A capture keeps a `DeviceCapture` for each device the application
     records on, holding that device's query pools, staging chunks and resolve images, since none
     can be used by another device's command buffers. Each read-back and timing names its device.
   - **Timings across devices.** A pass's start is measured from the earliest pass of its own
     device, since the devices' clocks differ.
   - **Frames.** The capture's frames are those of the device it started on. Once any device has
     presented, a present starts the capture, not another device's substitute frame boundary (a
     compute device waiting on its fences). Another device numbers its submissions from the frame
     it joined at.
   - **Queues.** Queues of one device share its pools.
   - **Replay.** `vkinsp_replay` replays every device's objects on its own single device.

   The UI groups
   the same runs into "Compute N" blocks by applying the same rule to the command stream. The UI
   shows timings as pass durations in the command tree, the pass timeline above the list, and
   the Frame Bound card and Pass Timings of Frame Stats. `FrameStats` also carries the CPU time inside `vkQueueSubmit`
   per frame (`submitMs`, measured by pre/post hooks), the "submit" line of the frame time meter,
   and a refresh-rate estimate (`EstimateRefreshMs`): with a FIFO present mode the display
   consumes at most one present per refresh, so the last 240 intervals add up to at least one
   period per present (less a start-up allowance of 8 the driver queued before blocking), and
   every interval other than a queued present (near zero) is a whole number of periods; of the
   common rates the sum allows, the one the intervals fit best (the median distance to the
   nearest multiple, within 8%: frames ended by a fence wait jitter by a millisecond where a
   vblank is exact) is the estimate, a faster rate replacing a slower one only when it fits
   clearly better. The layer logs the rate whenever it changes. Per report, the refreshes elapsed minus the frames presented accumulate in a signed
   deficit (a report bounded by a queued present is one short, the next one long); its growth is
   `dropped`, its high-water mark `droppedTotal`, both reset when the estimate changes. The UI
   uses the period as the frame budget (timeline marker, Frame Bound card) and shows the dropped
   frames in the meter and the session bar; without vsync the frame interval stays the budget.
   A 30 fps application on a 60 Hz display reads as 30 Hz, which is why the estimate is the
   last resort: `src/refresh_rate.*` asks the display first. At device creation the layer
   enables `VK_EXT_present_timing` (plus `VK_KHR_present_id2`, `VK_KHR_calibrated_timestamps`
   and the `presentTiming` / `presentId2` features chained into the create info; the instance
   gets `VK_KHR_get_surface_capabilities2`, tried and dropped if the loader lacks it) when the
   physical device offers it, else `VK_GOOGLE_display_timing`; a failed creation retries with
   the application's own create info. Swapchains get `VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT`
   when the surface supports timing, and `vkGetSwapchainTimingPropertiesEXT` /
   `vkGetRefreshCycleDurationGOOGLE` give the refresh period, re-queried every 120 presents
   (monitor changes). On Windows without a driver source the current mode of the monitor showing
   the application's largest window (`EnumDisplaySettings`) stands in. `FrameStats` carries
   `refreshSource` and `displayRefreshMs`. An enabled Khronos validation layer older than the
   layer's headers does not know `VK_EXT_present_timing` (the SDK 1.4.304 one crashes on it),
   so the extension stays off then; `VKINSP_NO_REFRESH_EXTENSIONS=1` disables both.
10. Every capture opens in its own tab of the Capture panel (`CaptureView` in `capture_panel.ts`
   owns one capture's data and views), as WebGPU Inspector does; earlier captures stay open for
   comparison until their tab is closed. Layer messages go to the most recently requested capture.

#### Command details

Selecting a draw or dispatch shows what WebGPU Inspector shows for one: the pipeline state, each
shader stage's reflection (entry point, inputs/outputs, resources), every bound descriptor set
with its bindings, the parsed contents of uniform and storage buffers, vertex buffers decoded by
the pipeline's vertex input layout (attribute names from the vertex shader), the index buffer,
push constants, and the pass's render targets (`renderer/capture_command_info.ts`). Binding
commands show the same for what they bind. Buffer contents are typed by SPIR-V reflection
(`renderer/vulkan/spirv_reflect.ts`, a parser of the module's declarations: types with member
offsets and array/matrix strides, decorations, variables, entry points) run on the SPIR-V the
layer keeps for each shader module / pipeline stage (`renderer/shader_cache.ts` fetches it once
with `RequestBlob`). The type can be overridden per binding with GLSL struct declarations
(`renderer/vulkan/buffer_layout.ts` computes std140/std430 offsets), the way WebGPU Inspector's
Format button takes WGSL. Images bound in descriptor sets are read back by the capture too
(`CaptureManager::QueueImageCapture`): when a bind or push descriptor command is recorded, every
sampled / storage / input-attachment image view it binds is queued once per view per capture
(every mip of the view back to back, each with all its layers, block-compressed formats
included; `mips` in the texture entry, and the viewer cuts the selected mip out of the data)
under a byte budget
(`maxImageTotal`, 256 MB), and the copy is recorded when the pass ends, like the buffer copies,
with the layout the descriptor promised (the layout tracker's when it says UNDEFINED). The
descriptor carries the texture capture id in `data`, the texture entry says `kind: "sampled"`,
and the binding shows the captured contents with the image viewer a click away; a live thumbnail
is the fallback when the read-back failed or images were not captured. The Inspect tab's image
viewer falls back to the most recent capture's contents when no application is connected, so
capture files show their textures. The read-back render
targets appear twice: as a thumbnail strip beside the command list (one tile per render pass
with every attachment, clicking it selects the pass's begin command, WebGPU Inspector's frame
images), and in the selected pass's Render Targets section, where clicking a target opens the
image viewer (`renderer/image_view.ts`, zoom, channels, exposure, auto range, texel values under
the mouse) on the captured pixels in place. That viewer takes a captured source instead of live
read-backs, so it works the same on loaded capture files.

### Live image readback

The Inspect panel shows the contents of a `VkImage` / `VkImageView` (`RequestImage {id, mip,
layer}` -> `ImageData`). To transition an image for the copy, the layer must know its current
layout: `LayoutTracker` (`src/image_readback.*`) records the transitions each command buffer
makes (pipeline barriers, render pass final layouts, dynamic rendering attachment layouts) and
applies them at submit, keeping one layout per image. Requests are served just before
`vkQueuePresentKHR` on the presenting queue: barrier to `TRANSFER_SRC`, copy to a host buffer,
barrier back, fence wait, send. Compressed formats are copied as blocks and decoded in the UI
(BC1–BC5 today). Multisampled images go through a temporary resolve image (color only). The
command buffer for this is allocated by the layer itself, which bypasses the loader trampoline
that stamps a new dispatchable object's dispatch pointer, so the layer copies the device's
pointer onto it: layers below (the validation layer) look their per-object state up by it and
crash on an unknown one.

The viewer (`renderer/image_view.ts`) follows WebGPU Inspector's texture viewer: decoding is
split into raw texel values (`decodeTexels`) and a display pass (`displayTexels`) that applies
channel selection, exposure, auto range and sRGB encoding of linear data. Hovering the canvas
shows the texel coordinates and values; clicking pins them in the info line; Ctrl + wheel zooms;
Copy puts the displayed image on the clipboard as PNG. Display settings are remembered per image.
An owner can also hand it an overlay to blend over the image and a callback for the pixel clicked,
which is what the render target tab below is built from.

### The in-app HUD and live pause

Two features that the three capture libraries share almost entirely, because neither of them has
any graphics API in the part worth sharing. Both live in headers under `src/vulkan/src`, which
`src/d3d12` and `src/metal` already add to their include path for `json_writer.h`.

`hud_text.h` is the HUD itself: a 5x7 bitmap font written out as binary literals (so the glyph is
legible in the source), and `BuildHud`, which turns the frame-time figures into a list of
`{rectangle, colour}` — one for the panel and one for every lit pixel of every glyph, with runs
merged. Expanding text to rectangles on the CPU is what keeps the per-backend code small: a backend
only has to draw flat axis-aligned rectangles, which needs no font atlas, no sampler and no
descriptors, only a vertex buffer and two shaders that transform and interpolate. A four-line panel
is about 700 rectangles, drawn as 700 instances of a four-vertex triangle strip.

`frame_pause.h` is the pause: a mutex, a condition variable and a step count. Each library calls
`Wait()` at its frame boundary — `vkQueuePresentKHR`, `IDXGISwapChain::Present`, the commit that
presents a Metal drawable — *after* the frame has been presented, so the frame the user is left
looking at is the complete one. `SetPaused(true)` deliberately grants one step: most of a frame's
wall time is spent inside the present call itself (under FIFO, waiting for vblank), so a pause
request usually arrives after the HUD has already drawn the frame in flight, and freezing on that
frame would show one with no PAUSED badge on it. Letting one more frame through means the frozen
frame is drawn knowing it is paused. `Generation()` counts the times the application has actually
been held, which is how each HUD knows to throw away the frame interval that spans a pause instead
of reporting a five-second frame.

Per backend, all that is left is putting the rectangles on the screen:

- **Vulkan** (`src/vulkan/src/hud.*`) draws from `vkQueuePresentKHR`, in a render pass that loads
  and stores the swapchain image in `PRESENT_SRC`. Submitting on the presenting queue is *not*
  enough to order the overlay before the present: the present waits on semaphores the application's
  rendering already signalled, so the presentation engine could read the image while the overlay is
  still drawing. The overlay's submission therefore waits on the application's present semaphores
  and signals one of its own, and the present is handed a rewritten `VkPresentInfoKHR` that waits
  on that. Its command buffers need the loader's dispatch pointer copied onto them, for the same
  reason as live image readback above.
- **D3D12** (`src/d3d12/src/hud.*`) draws from the `Present` hook, transitioning the back buffer
  out of `PRESENT` and back. Ordering is free here: DXGI puts the present on the timeline of the
  queue the swap chain was created with, so a command list executed on that queue beforehand is
  already ordered before it.
- **Metal** (`src/metal/src/hud.*`) appends a render pass to the command buffer the application is
  still encoding when it calls `presentDrawable:` — the present does not happen until that command
  buffer completes, so there is no extra submission and no synchronisation of its own. It compiles
  its MSL at run time, which is why it has no generated header.

The Vulkan and D3D12 shaders cannot be compiled at run time, and a release build deliberately has
no shader compiler, so their SPIR-V and DXBC are generated by `tools/gen_hud_shaders.py` and
committed (`src/vulkan/shaders/`, `src/d3d12/shaders/`). Each header records a hash of the shader
it came from, so the release workflow can catch a shader edited without regenerating its header
without needing a compiler of its own.

### The capture's render target tab

`renderer/capture_texture_view.ts` is WebGPU Inspector's capture texture viewer
(`devtools/capture_texture_viewer.js`): one render target of one pass, the image on the left and
the pixel history on the right. The toolbar's overlay list blends something over the image.
**Overdraw** is the pass's heatmap (`renderer/overdraw.ts` for the ramp and the counts, transparent
where nothing landed), with both counts in the tooltip. **Highlight Draw**, **Depth Test** and
**Wireframe** are RenderDoc's draw overlays for one of the pass's draws, painted from the replay's
mask (`renderer/draw_overlay.ts`); a pass of up to 48 draws has them all drawn in one replay, so
stepping through them is immediate. A Vulkan capture is replayed for each the first time it is
needed. Clicking a pixel follows it through the frame in the pane beside it
(`renderer/pixel_history_view.ts` in its compact mode) — replayed for a Vulkan capture, and for a
Metal capture the one the capture was taken with, with a button to capture the next frame
following another. One such tab per capture, retargeted as other render targets are opened.

### The capture's mesh tab

`renderer/mesh_view.ts` is RenderDoc's Mesh Viewer for one draw, opened from **View Mesh** in its
details. VS In decodes the captured vertex and index buffers through the pipeline's vertex layout
(`renderer/mesh_input.ts`, over `draw_state.ts`); VS Out parses the replay's transform feedback
records (`renderer/mesh_output.ts`), which also counts what keeps geometry from being seen. The
preview (`renderer/mesh_preview.ts`) is a WebGL2 wireframe with an orbit camera; VS Out is drawn in
normalized device coordinates with y and z negated so it faces the viewer the way the render target
does. A pass of up to 16 draws has all its draws captured in one replay. One such tab per capture.

### The shader debugger

Two languages, one debugger. `renderer/debug/program.ts` is the seam: a `DebugProgram` is a shader
(its source, where each instruction came from in it, how to name and print a value) and a
`DebugInvocation` is one run of it. The stepping, the tab and `debug_shader` are written against
those two and import neither back end. What is genuinely language-neutral lives beside them —
`debug/values.ts` (scalars, composites, pointers, the buffer-backed cells a block is read through),
`debug/sampling.ts` (filters, wrap modes, mips, comparison and cube maps, over a captured Vulkan
image or Metal texture alike) and `debug/quad.ts` (a fragment's 2x2 pixel quad in lockstep, for
derivatives and implicit LOD).

`renderer/spirv/` is a SPIR-V interpreter, after RenderDoc's (`spirv_debug.cpp`):

- `module.ts` parses a module into instructions in module order, so the ordinals match the source
  locations of `vulkan/spirv_debug.ts` and the instructions of `spirv-dis`.
- `interpreter.ts` runs one invocation an instruction at a time, with explicit frames rather than
  JavaScript recursion, so it can stop anywhere.
- `values.ts` reads uniform and storage blocks lazily from their bytes (std140 / std430 offsets from
  the decorations), with stores laid over them.
- `program.ts` is its `DebugProgram`.

`renderer/msl/` is a Metal Shading Language interpreter, which a Metal capture needs because its
shaders arrive as the source the application compiled rather than as an IR:

- `lexer.ts` tokenizes, with the preprocessor a generated shader needs; a token produced by
  expanding a macro keeps the line of the *use*, so a breakpoint lands where the reader sees it.
- `parser.ts` builds a syntax tree of the C++ subset shaders are written in.
- `types.ts` is MSL's type system and its C layout rules — where `float3` is sixteen bytes and
  `bool` is one, which is what makes a Metal uniform block read differently from a SPIR-V one.
- `lower.ts` flattens the tree to a linear instruction list with explicit jumps (`ir.ts`), applying
  MSL's arithmetic conversions once so the interpreter is component-wise. There is no SSA and there
  are no basic blocks: a debugger needs to stop anywhere, not to be optimized.
- `interpreter.ts` runs that list with a program counter and explicit frames, the same shape as the
  SPIR-V one; `stdlib.ts` is the `metal::` library, `program.ts` its `DebugProgram`. An invocation
  is specialized with the function constants the draw's `MTLFunction` was built with, so a library
  of `[[function_constant]]`-guarded variants steps the one that ran.

`renderer/shader_debug_setup.ts` builds a session from a capture and holds the rasterizer both APIs
share; `renderer/metal/shader_debug.ts` holds the Metal half. A vertex's inputs are decoded by
`mesh_input.ts`. A fragment's are rasterized from the draw's vertex shader outputs — near-plane
clipping, the draw's culling and depth compare to pick the covering triangle, and
perspective-correct, flat or noperspective interpolation — which a Vulkan draw gets from the
replay's transform feedback (`mesh_output.ts`) and a Metal draw by running its own vertex shader in
the interpreter, since Metal has no replay. The rasterizer state comes from the pipeline on Vulkan
and from commands on the encoder on Metal, and their clip-space Y points opposite ways, which is
why `RasterState` names both. `renderer/shader_debugger.ts` steps a session by source line or
instruction with breakpoints and per-line values, without a UI. The capture's debugger tab
(`renderer/shader_debugger_view.ts`) and the MCP server's `debug_shader` (`mcp/debug_tools.ts`) are
both built on it.

SPIR-V built without line information can be debugged through a translation. `DebugContext.translate`
swaps the stage's module for one `main/shader_tools.ts` `decompileForDebugging` makes:
`spirv-cross --force-temporary` decompiles the entry point, one statement per value rather than
folded expressions, and `glslangValidator -g` compiles it back under the name `decompiled.glsl`
with the text and an `OpLine` per instruction. The source maps above then give it lines like any
other module. The translation keeps the original's sets, bindings, locations, interpolation
decorations and spec constant ids, so the same bindings and inputs drive it. Compilers do not
guarantee it computes what the original does, so the session's `original` starts the same
invocation of the capture's module, and `compareWithOriginal` checks the two once both finish. The
tab runs the original a slice at a time, off the stepping. The capture's module still decides the
invocation's shape (a compute shader's local size).

### src/replay/ — capture replay

`vkinsp_replay` re-executes a `.gpucap` on this machine's GPU without the application, and is
the base for overdraw, draw overlays, mesh output and pixel history. Its pieces:
- decoders for the layer's JSON, generated from vk.xml (`tools/gen_replay.py`,
  `tools/vkgen/deserialize.py`), the inverse of the layer's serializers
- a Vulkan loader opened at run time
- object re-creation with resource-level memory
- command replay in submission order
- per-pass read-backs compared with the capture's own
- analyses that issue a pass again after the replay has executed it, with edited copies of its
  pipelines (`pipeline_copy.cpp`): overdraw (`overdraw.cpp`), draw-call overlays (`overlay.cpp`),
  mesh output through transform feedback (`mesh.cpp`, with the vertex shader edited by
  `xfb_patch.cpp`) and pixel history (`history.cpp`)

It also measures a frame's draws one at a time (`draw_stats.cpp`, `--draws`): each is issued
between two timestamps and inside a pipeline statistics query, which is where the Shader Flame
Graph's per-draw weights and exact fragment counts come from (`renderer/draw_stats.ts`).

The app runs it for a Vulkan capture's analyses through one process kept alive per capture
(`vkinsp_replay --serve`; `ReplayServerPool` in `src/app/src/main/replay.ts`). A capture view names its
capture by a key: the main process asks for the serialized bytes the first time a key is used,
writes them to a temporary file, and keeps the replay and the file until the view releases the key
(its tab closed, or the capture rebuilt). Each analysis is a request line; the data file it writes
is parsed by `renderer/overdraw.ts`, `pixel_history.ts`, `draw_overlay.ts`, `mesh_output.ts` or
`draw_stats.ts`, or by `shader_ablation.ts` for the shader variants `vulkan/spirv_ablate.ts` wrote
(the request's SPIR-V goes in an input file). The MCP server uses the same pool from `get_overdraw`,
`get_pixel_history`, `get_mesh_output`, `debug_shader` (a pixel's inputs), `get_shader_flame_graph`
and `measure_shader_cost`. `src/app/tools/stage_layer.mjs` ships the tool beside
the layer.

See [REPLAY.md](REPLAY.md).

### src/app/ — Electron UI

* `src/shared/protocol.ts` — typed definitions of every message (layer <-> UI, main <-> renderer).
* `src/main/` — sessions (process launch with the layer environment, stdout capture, TCP client),
  windows, IPC bridge. Some of it has no Electron in it and is shared with the MCP server:
  - `launch_env.ts`: finding the layers, and the launch environment
  - `layer_protocol.ts`: the socket framing
  - `shader_tools.ts`: SPIR-V text through the SDK's `spirv-dis` and `spirv-cross`, and the shader
    compilers
* `src/mcp/` — the MCP server of the Claude Code plugin (see MCP server below).
* `src/renderer/` — `inspector_window.ts` (launch toolbar, one tab per session),
  `session_panel.ts` (a session's object database and its Inspect / Capture / Log tabs),
  `inspect_panel.ts` (live objects), `capture_panel.ts` (frame capture: command list, render
  targets), `capture_command_info.ts` (the selected command: bound state, descriptor sets, buffer
  contents, shaders), `buffer_data_view.ts` (typed buffer values), `shader_cache.ts` (SPIR-V
  reflection on demand), `args_view.ts` (argument trees), `render_graph.ts` / `frame_graph.ts` /
  `render_graph_view.ts` (the frame's pass dependency graph and its chart). `widget/` and
  `utils/` are TypeScript ports of WebGPU Inspector's widget library and helpers; `src/vulkan/` holds
  the object model, database, texture decoding, SPIR-V reflection, vertex format decoding, the
  buffer layout parser, the render pass decoder and the frame rules; `src/metal/` the Metal command
  tables, reflection and resource source; `src/d3d12/` the D3D12 command tables and the reader of the
  reflection the D3D12 library sends with each pipeline.

#### Frame Stats

The Frame Stats button of a capture tab replaces the command details with statistics of the
capture (`capture_statistics.ts`, after WebGPU Inspector's): commands by kind, passes and
attachments, pipelines and stages bound, descriptor sets and what they held, push constants,
memory traffic (update/fill/copy bytes, and what the capture read back), and geometry. Vertex,
triangle, line and point counts follow each draw's bound pipeline's topology; indirect draws
count from their captured argument buffers.

#### Validation messages

The layer registers its own `VK_EXT_debug_utils` messenger on every instance
(`src/validation.*`), enabling the extension in `vkCreateInstance` when the application did not
(the loader implements it, so it is always available), and forwards what the validation layer or
the driver reports as `ValidationMessage` (severity, message id name and number, text, the frame,
and the objects the message names, resolved to tracked ids so the UI can link them). Messages are
kept in the layer as well: a UI that connects later receives them after the object snapshot, and
repeats of one message (engines re-issue the same mistake every frame) are counted rather than
resent, the counts going out with the frame tick as `ValidationCount`. Unique messages are capped
at 2000 per process. Only messages of layers *below* ours in the chain reach the messenger, which
is why the launcher enables the Khronos validation layer itself: the launch dialog's "Validation
layer" checkbox adds `VK_LAYER_KHRONOS_validation` to the enabled layers and its directory (the
Vulkan SDK, or the distribution's `explicit_layer.d`) to the layer path, since `VK_LAYER_PATH`
replaces the loader's own explicit-layer search. The Inspect tab lists the messages above the
object groups (WebGPU Inspector's "Validation Errors"), marks the objects they name in the object
list and in their details, and the session bar counts errors and warnings; messages are also
written to the Log tab and saved in capture files. The triangle test application's
`--bad-scissor` option provokes one for testing.

A message that fires inside a `vkCmd*` call while the layer records that command buffer (a
capture in progress, or "record always") is attached to the command: the messenger callback runs
during the call, before the post-hook appends the command, so the command in flight is the
recorder's current count (`ValidationLog::CurrentCommand`, which only dereferences handles the
tracker knows). The reference (`command: {commandBuffer, slot}`) travels with the message, and
every captured command carries its `slot` (its position in the command buffer's recording;
inlined secondary commands their position in the secondary). A repeat during a capture moves
the reference to that recording and resends the message in full at the next frame tick, so the
capture shows the link even for messages first seen long before. The capture's command rows get
a severity marker and the command details a Validation section (`validationForCommand` in the
object database). The launcher sets `VK_LAYER_DUPLICATE_MESSAGE_LIMIT=0` alongside the
validation layer: its default limit (10) would silence the message before the captured frame.

Synchronization validation ("Sync validation" in the launch dialog: `VK_LAYER_VALIDATE_SYNC`,
plus `VK_LAYER_ENABLES` for older layers) reports hazards between submissions at
`vkQueueSubmit`, with the queue as the only object; the text names the submitted command buffer
("entry 0, VkCommandBuffer 0x...") and the command ("command: vkCmdDrawIndexed"). When no object
of the message is a command buffer, `CurrentCommand` takes the handles from the text, and for a
recorder that has ended (the buffer is being submitted) finds the command by its sequence number
("seq_no", older layers) or else the first command of that name in the recording. Such messages
also carry per-submission counters ("submit: 37, batch: 0"), which are stripped from the text
before the repeat lookup, or every frame would be a new message. The capture's own read-back
barriers can resolve a hazard in the captured frame (the buffer copy's barrier orders a draw
after an earlier unsynchronized write), so a hazard reported every other frame can be missing
from the captured one. The triangle test application's `--hazard` option submits its vertex
update unsynchronized ahead of the frame's draw.

#### Capture windows

A capture file can be shown in a window of its own: `openCaptureWindow` in `main.ts` opens a
`BrowserWindow` with `?capture=<path>` (and `temp=1` for a hand-over), and the renderer in that
window opens the path as a file session with no launcher, as it does for `?session=`. A live
session's capture tab serializes the capture and hands the bytes over; the main process writes
them to a temporary `.gpucap` (removed at quit) and opens it. "Move to Main Window" sends the
path back to the main window (`inspector:openCapture`) and closes the capture window. The file
session itself is renderer-only, so moving one is reopening the file elsewhere.

#### Source view in captures

`shader_source_view.ts` renders an embedded source file with line numbers and highlighting
(`renderSourceLines`, shared with the Inspect tab's Source view, which adds the jump to the
disassembly) and a whole module's source with its summary line and file bar
(`renderEmbeddedSource`). The captured command's shader sections
(`CommandInfoView._renderShader`) fetch the stage's SPIR-V on first expansion and show the
source, the Shader Cost and the Performance Analysis sections under the reflection; a finding's
line link opens the source at that line.

#### Shader Flame Graph

`frame_cost_tree.ts` (no DOM) builds the tree `frame_flamegraph.ts` renders with the flame
graph widget (`widget/flamegraph.ts`, ported from WebGPU Inspector). It walks the capture's
commands with the same pass numbering as the command list (render passes and compute runs per
command buffer and frame), tracks the bound pipeline per stream and bind point and the dynamic
scissor, and makes one item per draw or dispatch. Per pipeline the panel gathers a `StageModel`
per stage (the SPIR-V analysis of `spirv_analysis.ts`, plus the workgroup size from reflection
for compute). Invocation counts: `vertexCount x instanceCount` / `indexCount x instanceCount`,
indirect draws summed from the captured argument buffer, dispatches `groups x workgroup size`;
fragment invocations are unknowable without rasterization and are estimated from the scissor
area clipped to the render area (an upper bound without overdraw), or left unweighted when the
estimate is switched off. A stage's cost is the entry point's modeled per-invocation cost times
its invocations; its children are the entry's callees with their inclusive costs (recursion
cut, children squeezed to fit their parent). Items are grouped per pipeline (or per draw), the
32 costliest per pass kept and the tail collapsed into a "+ more" frame that keeps its cost.
When every pass has a measured duration the tree is in milliseconds: each pass subtree is
scaled to its measured time, so the root and pass widths are real and only the split within a
pass is modeled; otherwise it stays in modeled op units.

#### Stack traces

`src/stacktrace.*` captures raw return addresses in the hot path (`CaptureStackBackTrace` on
Windows, `_Unwind_Backtrace` elsewhere; 32 frames) and symbolizes them only on request. Every
tracked object keeps its creation stack when `VKINSP_STACKTRACES` is set (the launch dialog's
"Stack traces", on by default); with the capture option `stacktraces` every recorded command
carries its addresses as `stack: ["0x...", ...]` (strings: 64-bit values are not JSON-safe
numbers). The UI asks for symbols lazily: `RequestStacktraces {ids}` returns the objects'
symbolized frames (`Stacktraces`, with `available` saying whether the layer collects any), and
`RequestSymbols {addresses}` resolves a capture's addresses (`Symbols`), both cached in the
object database and saved in capture files (`symbols`, `stacks`) so a file session answers them
itself. Symbols come from DbgHelp on Windows (one mutex around it; `SymRefreshModuleList` before
each batch for modules loaded since) and `dladdr` on Linux/Android (exported names only). A
symbol further than 64 KB from the address is the nearest export of a module without symbols
and is dropped for module+offset. DbgHelp looks for a PDB beside its module and along
`_NT_SYMBOL_PATH`, which finds nothing for a build that keeps its symbols elsewhere, so the
launch passes the symbol directories to the capture libraries as well
(`VKINSP_SYMBOL_PATH` / `DXINSP_SYMBOL_PATH`, `SymSetSearchPath` in front of what DbgHelp
works out for itself). One return address can stand for several source functions: DbgHelp
answers with the function the compiler emitted, and the inlined ones are a separate walk
(`SymAddrIncludeInlineTrace`, `SymQueryInlineTrace`, then `SymFromInlineContext` per context),
so the frame takes the innermost — the one the reader means — and the rest, ending with the
emitted function, become its `inlinedInto`, the same shape the host symbolizer produces. Frames from the innermost up to the outermost loader/layer
frame are marked `internal` (the driver's frames sit between them) and hidden behind a toggle,
so the first frame shown is the application's call into Vulkan. Every frame carries its offset
from the module base, and frames without a source location get a second pass on the host
(`main/symbolize.ts`, through `inspector:symbolize`): the launch configuration's "Symbol
directories" (Android section; the last ones used serve capture files) are searched a few
levels deep for a file named like the module, the largest copy taken as the unstripped one,
and `llvm-symbolizer` from the NDK (the one on `PATH`, or `addr2line`, otherwise) turns the
offsets into functions, files and lines; its JSON output also gives the callers a function
was inlined into, kept as `inlinedInto` and shown as indented steps under the frame. The
results replace the cached frames, so a capture file saves them; `--debug-expand-stacks` opens
the section as soon as a command is shown.

#### Leak report

`vkDestroyDevice` and `vkDestroyInstance` first ask the tracker for the objects still alive
under the owner (`Tracker::SendLeakReport`): everything in its child tree except what the
application cannot destroy itself (queues, physical devices, swapchain images) and what is
freed with its pool (descriptor sets, command buffers). The report goes out as `LeakReport`
(owner, count, counts by type, the first 2000 objects with their names and creating commands)
just before the `DeleteObjects` cascade, so the Inspect tab's Leaked Objects group keeps the
names after the objects are gone; the session bar counts them next to the validation counts,
and the Log tab records the summary.

#### Capture files

A capture can be saved (the Save button of the capture bar, or the tab's context menu) and
reopened without the application (Open Capture... in the launch bar, or by dropping the file on
the window), the way WebGPU Inspector saves `.wgpuc` files. `renderer/capture_format.ts` defines
the `.gpucap` format: an ASCII `GPUCAP 1` header line, a u32 manifest length, a JSON manifest,
then the raw payloads (render target pixels, buffer ranges, SPIR-V) the manifest references as
`[offset, length]`; the header and manifest are readable in a text editor, and the payloads stay
binary so large captures do not grow by a third as base64. The manifest carries the objects the
capture references, closed over their dependencies and owners so every link resolves (their
creation arguments, labels, updates such as memory bindings and descriptor contents, and whether
they had already been destroyed), the command list with the secondaries inlined, the render
targets, the buffer ranges, the pass timings, and the frame and submit times behind the Frame
Bound card. Saving (`renderer/capture_file.ts`) fetches the SPIR-V of the referenced pipelines
and modules from the layer first (`RequestBlob`, cached in `ObjectDatabase.blobData`).

A loaded file becomes a session of its own (`FileSessionPanel`): its object database is built
from the manifest with the same snapshot path as a live connection, and its Capture tab holds the
loaded capture. Its `send()` answers `RequestBlob` from the file, so shader views, reflection,
parsed buffer contents and the source view work as they do live; anything only a running
application could answer (image read-back, descriptor contents, shader edits) is declined and
the panels say so. "Open in New Tab" on a capture tab goes through the same serialization in
memory and opens an independent copy. `--debug-save=<file>` after `--debug-capture` and
`--debug-open=<file>` exercise the round trip unattended.

#### Shader editor

The Edit button on a shader payload (Inspect > VkPipeline or VkShaderModule > Shader) edits the
text currently shown (SPIR-V disassembly, GLSL or HLSL from `spirv-cross`) and compiles it with
the SDK's `spirv-as`, `glslangValidator` or `dxc` (`compileShader` in `main.ts`, target
environment matched to the module's SPIR-V version). WebGPU Inspector can rebuild a pipeline in
the page; here the application's `VkPipeline` is immutable, so the layer does it
(`src/shader_edit.*`): it keeps a deep copy of every pipeline's create info (known pNext
structs included, unknown ones dropped with a note), and `ReplaceShader {pipeline, stage,
spirv}` creates a new module and a replacement pipeline from that copy with the stage swapped.
From then on `vkCmdBindPipeline` (a pre-hook) binds the replacement instead of the original;
`RestoreShader` drops it. The replacement is registered as an object of its own ("<name>
(edited)", with the new code as its stage payload) so captures and reflection see it: a
`vkCmdBindPipeline` (or `vkCmdBindShadersEXT`) recorded while the edit is active names the
replacement in its arguments, with the application's original as `replaced` (a post-hook rewrites
the record), so the capture, its analyses and its replay carry what the frame drew. Retired
replacements are destroyed at a later present after `vkDeviceWaitIdle`, since command buffers
may still reference them. The stages an edit leaves alone are not given the application's own
modules, which it may have destroyed as soon as the pipeline existed. They get temporary modules
made from the SPIR-V the tracker keeps with the pipeline, and those are destroyed once the
replacement exists. Editing a module applies to every pipeline that uses it. Command
buffers recorded before the edit keep binding the original until they are re-recorded.

- **Graphics pipeline libraries.** A pipeline linked from libraries shows their stages: the
  creation hook attaches the libraries' stage payloads to it. Its record holds its libraries'
  records, so an edit can make every library again, with the edited stage in whichever library
  holds it, and link the replacement from those. The rebuilt libraries live as long as the
  replacement, which the spec requires of a library.
- **Ray tracing pipelines** are shown but not edited. Each stage's code is a payload named with
  its index in `pStages` (`miss:main#1`), since a ray tracing pipeline usually has several stages
  of one kind and its shader groups refer to them by index. `shaderGroups` and
  `bindingTableRegions` (`renderer/shader_cache.ts`) read the groups and a trace command's table
  regions for `renderer/ray_tracing_view.ts` and `get_command`.
  - **Acceleration structures:** the build hooks put each build's geometries and primitive counts
    on the structure as a `build` update.
  - **Descriptors:** acceleration structure descriptors are tracked from
    `VkWriteDescriptorSetAccelerationStructureKHR`.
- **Shader objects** (`VK_EXT_shader_object`). Each `VkShaderEXT` gets its SPIR-V as a
  `<stage>:<entry>` payload, and its create info is recorded. An edit makes a replacement shader
  object, and a `vkCmdBindShadersEXT` pre-hook binds it instead of the original. A shader created
  linked to others must be bound with the rest of its set, so the whole set is made again,
  unlinked, and every member is substituted. The protocol is unchanged: `ReplaceShader` carries
  the shader object's id in `pipeline`. Shaders created from a binary cannot be edited.

  In the app, a draw's state (`renderer/draw_state.ts`) carries the shader objects bound in place of
  a pipeline and the dynamic state that stands in for a pipeline's (`dynamicValue` reads it where a
  pipeline declares that state dynamic). Reports that group draws by pipeline id group these by a
  program key instead (`ShaderProgram` in `renderer/shader_cache.ts`): the pipeline's id, or a
  negative key per set of shader objects, followed through a frame by `ProgramTracker`.
  `stateStages` gives a draw's stages from either source, so the command details, the analysis,
  the flame graph and the shader debugger read one lookup.

The editor (`renderer/code_editor.ts`) is a
textarea over a highlighted copy of its text with a line-number gutter and a find bar; a failed
compile marks the offending lines (`parseCompileErrors` reads glslangValidator's
`file:line:`, dxc's `file:line:col: error:` and spirv-as's `error: line: col:` forms), the
log's lines jump to them, and the editor goes to the first one.

#### Device sections

`vkEnumeratePhysicalDevices` has a hook that queries each physical device's properties (with
its limits), memory heaps and types, queue families, features and extensions and attaches them
to the `VkPhysicalDevice` object as an `ObjectUpdate` (replayed in snapshots and saved in
capture files), so a capture from another machine says what that GPU offered.
`renderer/device_info_view.ts` renders them as sections on the physical device (a filterable
limits table, memory with sizes and flags, queue families, supported features with a "show all"
toggle, a filterable extension list), on the device (enabled extensions, queues, and the enabled
features flattened across `pEnabledFeatures` and every feature struct in the pNext chain, each
member tagged with its struct), and on the instance (application info, layers, extensions).

#### Shader reflection

Every shader payload in the Inspect tab (a module, or a pipeline stage) has a Reflection section
above its code: the SPIR-V version, each entry point with its stage, workgroup size, inputs and
outputs, the resources by set and binding with struct members, offsets and sizes, and the push
constant block. It is the same `reflectSpirv` result the capture view uses per draw, rendered by
`renderer/shader_reflection_view.ts`, so a module explains what it expects without a capture.

#### Source roots

A module that carries line information but no text (dxc `-Zi`, a build that strips the text)
names its files in `OpString` / `DebugSource`; `resolveSourcesFromHost` in
`shader_source_view.ts` asks the main process for them (`inspector:shaderSource`,
`main/shader_sources.ts`): an absolute name that exists is read as is, otherwise each source
root of the launch configuration (the last ones used serve capture files) is indexed once by
file name, a few levels deep, and the candidate sharing the most trailing path components with
the name wins ("triangle/wave.comp" prefers `.../triangle/wave.comp`). The text fills the parsed
debug info in place, marked `fromHost`, so the Source view, the per-line costs and the findings'
links work as with embedded text; the summary says the source came from this machine. The
triangle test application ships its compute shader that way: `tools/strip_shader_source.py`
drops the text and makes the file name relative to `test/` after glslc, so the source root
`test/` (the `sources` case of `tools/ui_tests.py`) is what finds it.

#### Shader analysis

WebGPU Inspector analyzes WGSL source; here the same questions are answered from the SPIR-V
the layer holds (`renderer/vulkan/spirv_analysis.ts`), so no shader source is needed. One pass
over the module classifies every instruction of every function into ALU, special-function
(divisions, transcendentals from `GLSL.std.450`), texture and memory operations (loads and
stores whose pointer's storage class is a buffer, image or workgroup memory, traced through
access chains), weights them by the loop nesting the structured control flow gives
(`OpLoopMerge` / `OpSelectionMerge` and their merge blocks; unknown trip counts count as 8),
and sums them up the call graph per entry point. The same walk raises findings for the patterns
WebGPU Inspector's analyzer flags (texture samples, expensive builtins, non-constant division,
atomics, barriers and storage accesses inside loops, derivatives inside branches, discards,
integer division), located to a source line through the debug information when there is one.
`renderer/shader_analysis_view.ts` shows the result as the Shader Cost and Performance Analysis
sections of a shader payload in the Inspect tab, and as the capture's "Analyze Shaders" report,
which resolves the pipeline bound for each draw and dispatch to count uses per shader.

Loop-invariant detection records every value-producing instruction with the loops it sits in,
and every loop's stored variables; after the walk, a candidate inside a loop is invariant when
all its inputs are constants, global variable addresses, values defined outside the loop, loads
(through invariant indices) of variables the loop never stores to and that are not storage,
shared or image memory, or invariant operations themselves (phis and calls never are; cycles
resolve to variant). The workgroup memory rule sums the sizes of Workgroup variables with a
scalar layout computed from the type declarations.

#### Frame analysis

`renderer/vulkan/frame_analysis.ts` runs rules over a whole capture rather than one shader:
one walk of the commands builds a record per render pass (attachments with their load and
store ops, formats, sample counts, the image behind each view and its usage, the view mask
from `VkRenderPassMultiviewCreateInfo`, `VkSubpassDescription2` or `VkRenderingInfo`, the draws
with the pipeline bound for each) and notes the clear commands, the images copies read and
the image views descriptor snapshots bind. The rules then flag what costs most on a tiled GPU:
a clear command followed by a pass loading the same image, a color attachment loaded before
the frame wrote it, a depth attachment stored that nothing can read (its usage has no sampled,
input-attachment or storage bit; `TRANSFER_SRC` does not count, since old captures carry the
layer's own addition of it), a depth attachment neither loaded nor stored without
`TRANSIENT_ATTACHMENT` usage (the severity depends on the device having a lazily allocated
memory type, from the physical device's memory properties), a multisampled attachment stored
although it is resolved, a multisampled attachment stored without a resolve for sampling,
barriers inside a render pass, directly after another or from ALL_COMMANDS to ALL_COMMANDS, redundant
pipeline, descriptor set and vertex or index buffer binds (compared against what the command
buffer has bound), push constants re-pushed with the bytes the range already holds, dispatches
of one workgroup and many tiny draws; and the XR-specific one: passes without multiview whose
draw sequence (pipelines and vertex counts), target size and attachment formats match another
pass rendering to a different image or layer, which is one pass per eye. Findings of a rule
over many commands are folded into one that names the first and counts the rest, but
`byCommand()` maps every affected command to its findings: the capture panel computes the
analysis once per capture, marks the affected rows with a flag after the call number, and the
command details show a Performance section. `renderFrameStats` in `frame_stats_view.ts`
shows the list as the Frame Issues card with links that select the command, behind severity
checkboxes (the shader findings' hide classes) and one checkbox per rule that fired.

Every own-cost charge of the analysis is also charged to the source line of the instruction
(`locations[ordinal]` from the debug info), giving `FunctionAnalysis.lines` (costliest first):
the Shader Cost section's "Costliest lines" list and the flame graph's line frames under a
function come from it.

#### Render graph

The same commands, read as a dependency graph instead of a list. `renderer/render_graph.ts` is
the model and has no capture in it: it is given the frame's passes with what each one reads and
writes and builds the graph. Two things make that a graph and not "which passes touched image
12". Resources are identified per subresource — the mip level and array layer a pass actually
touched — so a bloom chain that writes mip N and reads mip N-1 of one image is a chain and not a
node with a self-loop. And every write starts a new *version* of the resource, with edges running
from a version's producer to its readers, the SSA shape a render graph compiler uses: without it
a pass that loads an attachment and stores it again is a cycle. Version 0 is what a resource held
on entry to the capture, so reads of it are reported as external inputs (the previous frame, a
host upload, a pass outside the captured range) rather than as edges. From the graph come the
critical path (one backwards sweep, since the nodes are in execution order and the edges run
forward with them), the resources read from before the frame, and the passes whose every write no
later pass reads — stated as that and not as "dead", because the host or the next frame may read
it, and because a binding the capture cannot see may too.

The extraction is split the way `command_sets.ts` is. `renderer/frame_graph.ts` is API-neutral
and does the segmentation: it walks the commands, cuts them into passes exactly as the capture
panel's command tree does (render passes counted per command buffer, compute passes as runs of
dispatches outside one, both keyed with `passKey()` so a node finds its GPU timing, all of it
restarting per captured frame), folds the accesses collected for a pass so that five hundred
draws sampling one shadow map are one edge, and asks a `ResourceSource` what each command
touches. The sources are `renderer/vulkan/frame_resources.ts` and
`renderer/metal/frame_resources.ts`: attachments (Vulkan's through `vulkan/pass_info.ts`, which
`frame_analysis.ts` shares, so the graph and the frame rules read the same load and store ops),
the descriptor sets snapshotted at each draw and dispatch — or, for Metal, what the encoder had
bound — and the transfer commands, which name their two ends outright. What a source cannot
resolve it counts rather than guesses at: bindings through shader objects or Metal's argument
buffers are not in the capture, and the view says the graph is a lower bound on the frame's edges
instead of implying those passes read nothing. Descriptor buffers used to be in that group and are
now decoded (`src/vulkan/src/descriptor_buffer.h`), arriving as an ordinary set snapshot; only one
the layer could not read — memory it had no host mapping for, or descriptors made before it
attached — still counts as hidden. Storage bindings are counted
read-write for the same reason — without shader reflection a read-only storage buffer is
indistinguishable from one the shader writes — and the view says so.

`renderer/render_graph_view.ts` draws it. Not as a node-link diagram: a real frame has hundreds
of passes and thousands of edges and lays out as a hairball whatever the algorithm. The main view
is a resource lifetime chart — passes along the top in execution order, one row per resource, a
bar across the passes where it is live, marked (filled for a write, outlined for a read, colored
by usage class) at every pass that touched it. It needs no layout pass, scales to any frame, and
"what does this pass depend on" is read up its column. The node-link drawing is kept for the one
part small enough to be legible: the selected pass, its immediate producers on the left and its
consumers on the right, with the resource on each edge — the "why is this pass here" question a
graph is really asked. Rows and columns cross-highlight with the selection, and every pass and
resource links back to the command list and the Inspect tab.

#### Render graph rules

`renderer/render_graph_analysis.ts` runs rules over the graph rather than over the command stream,
and the difference is what they can say. The per-command rules answer "does anything read this?"
with a proxy — the Vulkan pass rules from the image's *usage flags* (a sampled bit means something
*could* read it, not that anything did), the Metal ones from a read set kept per whole texture and
unversioned, which a mip chain or a target written twice in a frame defeats. The graph knows the
answer outright: a version of a subresource with no readers. So `unread-store` states it, and the
rules it replaces (`depth-store`, `color-store`, and Metal's adjacent-pass `mergeable-passes`) are
listed in `SUPERSEDED_RULES` and dropped by `analyzeFrame` when a graph is available, rather than
reported a second time in other words.

The rest exist only because the graph does. `overwritten-before-read` finds a version replaced by a
write that keeps nothing of it with no reader in between — work done and thrown away, which needs
versioning to see. `transient-candidate` finds a resource written and then read only by the pass
that immediately follows and never presented or copied: it never has to reach memory at all
(`TRANSIENT_ATTACHMENT` with `LAZILY_ALLOCATED` memory or an input attachment; `MTLStorageModeMemoryless`).
`mergeable-passes` is the exact form of that pairing, where the second pass loads precisely what
the first stored to the same targets. `subpass-candidate` is the other form: a render pass that
reads nothing but the previous render pass's attachments, at the size it renders, could be that
pass's second subpass with input attachments. Whether it could depends on its shaders, which the
graph does not have. The Vulkan analysis passes it `filtersInput`, which checks each draw's bound
descriptors against its fragment shader's reads (`textureReads` in `vulkan/spirv_ablate.ts`). A
shader that reads the input more than once, or in a loop, filters it (a blur, ambient occlusion)
and needs a texture, so that pass is not reported. `oversynchronized-barrier` compares what the frame *declares*
it depends on with what it does: a barrier is questioned only when it names resources (a global
memory barrier says nothing to check), changes no image layout and moves nothing between queue
families (both required whatever the data does), and every resource it names is untouched on one
side of it. That last test is deliberately weaker than "no dependency edge": a barrier may be
guarding a write-after-read or write-after-write, neither of which is an edge, so the rule only
fires when there is nothing on one side at all.

Every rule that rests on "nothing reads this" drops a confidence level and says so when any pass
in the frame has bindings the capture could not resolve, since the graph is then a lower bound on
the reads. The findings are the same `FrameFinding` the other analyses produce, so they render in
the Render Graph view's own Suggestions card, in Frame Stats' Frame Issues, and as the flag on the
command rows, with no separate plumbing. Because the graph is API-neutral, so are the rules: one
implementation serves Vulkan, Metal and Direct3D 12, and only the wording of each fix names an
API, from `RenderGraph.api`.

#### Shader source maps

WebGPU shaders are their own source; SPIR-V is not, but compilers can embed the source and a
line mapping as debug information, in one of two forms: the core `OpSource` / `OpSourceContinued`
text with `OpLine` per instruction (glslc / glslangValidator `-g`), or the
`NonSemantic.Shader.DebugInfo.100` extended instruction set with `DebugSource` /
`DebugSourceContinued` and `DebugLine` (dxc `-fspv-debug=vulkan-with-source`, glslang `-gVS`;
glslang's `-gV` alone embeds only the file name and the line mapping).
`renderer/vulkan/spirv_debug.ts` parses both into one model: the embedded files, the language,
generator and `OpModuleProcessed` strings, and for every instruction of the module the source
line it came from. The Inspect panel then adds a **Source** view (the embedded text with line
numbers, one button per file when includes were embedded, and the default view when a source is
present), annotates the SPIR-V disassembly with `; file:line  <source line>` wherever the line
changes, and links the two: clicking an instruction shows its source line, clicking a source line
highlights its instructions. The mapping ties the module's instruction ordinals to `spirv-dis`
output, which prints one instruction per statement in module order (an `OpSource` text spans
several lines of one statement, which the splitter tracks by its string literal). Editing from
the Source view compiles the embedded GLSL or HLSL with its original entry point instead of
`spirv-cross` output, minus glslang's `// OpModuleProcessed` and `#line` prefix, which a
compiler rejects ahead of `#version`; other embedded languages (Slang, WGSL, ...) are shown but
not compiled.
Applications that strip debug information (release builds, most engines) get the summary line
saying so and the flags that embed it.

#### Meters

The Inspect tab's top row follows WebGPU Inspector's meters: a frame time plot (average and
longest frame of each 100 ms `FrameStats` interval the layer reports, plus the CPU time inside
`vkQueueSubmit` per frame), an object count plot with
a type selector, and memory totals. Vulkan makes memory explicit, so "Device Memory" is the sum
of the live `VkDeviceMemory` allocations (what the application actually holds), while the image
and buffer figures are estimates from their formats and sizes (`objectMemoryBytes` in
`vulkan_object.ts`), the way WebGPU Inspector estimates texture and buffer memory.

#### Theme

`src/renderer/css/theme.css` defines every color as a token: the dark palette on `:root`, the
light palette under `:root[data-theme="light"]`. The other stylesheets only use tokens. The
theme is a user setting (Theme picker in the main window's toolbar, stored in `settings.json`)
and applies to every open window at once. Each window also gets it as a `?theme=` query parameter
so the first paint uses the right palette (`renderer/theme.ts`). `INSPECTOR_THEME=light|dark` in
the environment overrides the setting for one run (used by the screenshot test aids). Adding a
theme means adding its palette block to `theme.css` and its name to `THEMES` in `protocol.ts`.

#### Sessions

A *session* is one inspected application: the target process (when launched by the inspector),
the TCP connection to its layer, its status and log. Sessions live in the main process; the
renderer holds the object database and capture data for each session it displays.

* Any number of sessions can run at once. Each gets its own port: the configured one, or the next
  free port when it is taken (by another session or by a previous instance of the target that is
  still exiting).
* Relaunching a session waits for the old process to exit before starting the new one, so the
  new connection cannot reach the old process. Connection attempts are retried while the target
  is alive, including after a lost connection, until the connect deadline.
* Every session is displayed by exactly one window. "Open in New Window" (in the session tab's
  context menu) moves it into a window of its own; closing that window moves it back to the main
  window; closing the main window ends the application. A window that picks up an already
  connected session sends `RequestSnapshot` and the layer resends its live object list.
* Closing a session tab terminates the application. Stop terminates it but keeps the tab, so it
  can be relaunched in place.
* Message vocabulary follows WebGPU Inspector's `actions.js` (`AddObject`, `DeleteObject`,
  `ObjectSetLabel`, `CaptureFrameResults`, `CaptureFrameCommands`, `CaptureBuffers`,
  `CaptureBufferData`, `CaptureTextureFrames`, `CaptureTextureData`).

#### MCP server

`src/mcp/` gives Claude Code, or any Model Context Protocol client, the captures the app saves,
read with the app's own analyses. `claude-plugin/` packages it as a plugin with a skill and
commands, and `.claude-plugin/marketplace.json` makes the repository the plugin's marketplace.

**Loading.** The server reads a `.gpucap` the way `FileSessionPanel` does: `parseCaptureFile`,
then `ObjectDatabase.loadObjects`, then `CaptureData.load`.

**Analyses.** On the loaded capture it runs the renderer's modules as they are:
- `analyzeFrame` with the render graph, for Frame Issues
- `collectPassMetrics` and its advice, for GPU Bottlenecks
- `CaptureStatistics`
- `frameRenderGraph`
- `draw_state.ts`, for the state bound at a command
- SPIR-V reflection, debug information and analysis
- the texture decoders

**What was split out of the UI for it:**
- The format, from the save path (`capture_format.ts`).
- The statistics and the Frame Bound verdict, from their card (`capture_statistics.ts` against
  `frame_stats_view.ts`).
- The bottleneck advice, from its report (`pass_metrics.ts`).
- The state reconstruction, from the command details view (`draw_state.ts`).
- The Vulkan command summaries, into `VULKAN_SETS.summarize`.
- The debug group names (`labelNameOf`).
- The pipelines a frame used (`pipelineUses`).
- `shaderText`, out of `main.ts` (`main/shader_tools.ts`).

**Bundle.** The build bundles the server into one dependency-free file,
`claude-plugin/server/gpu-inspector-mcp.mjs`. It is committed, since a plugin installs from the
repository as it is. The build fails if a UI module (widgets, views, panels, anything calling the
preload API) would end up in that bundle.

**Protocol.** `stdio_server.ts` implements only the four methods a tools-only stdio server needs:
`initialize`, `ping`, `tools/list` and `tools/call`. It does not use the SDK, whose server brings a
schema validator, an HTTP stack and a schema library with it.

**Answers.** Tool answers are written for a model to read (`describe.ts`):
- Object references as `VkImage#12 "name"`, and inline payloads as their size.
- Measurements rounded, fields that were not measured left out.
- Every list paged and every answer capped.
- Buffer and push constant contents decoded through the shader's reflection.
- Vertices decoded through the pipeline's layout, with per-attribute bounds.
- Images as PNG with their statistics.

**Store.** Captures stay open in `capture_store.ts`, with their analyses computed on first use.
`list_captures` adds the app's recent captures from its settings file.

**Live sessions.** `live_session.ts` drives running applications without the app.
- **Launch and connect.** The launch environment comes from `main/launch_env.ts` (the Vulkan layer)
  or `main/metal.ts` (the Metal library). Messages are framed by `main/layer_protocol.ts`. Both
  modules are shared with `main.ts`.
- **Finding the capture library.** The server looks in the build tree of a checkout (the one the
  bundle sits in, or `GPU_INSPECTOR_ROOT`), then in an installed GPU Inspector, then
  `INSPECTOR_LAYER_DIR`.
- **Session state.** Messages feed an `ObjectDatabase` as a session's do.
- **Captures.** A capture streams into a `CaptureData` until the capture library's
  `CaptureComplete`. A library built before that message existed gets a quiet stream after the
  commands and buffers instead. The capture is saved with `serializeCapture`, which needs only a
  connection now that the stack and symbol requests are UI-free (`stack_requests.ts`), and it opens
  in the store like any file.
- **Shaders.** `replace_shader` compiles with `main/shader_tools.ts` for the stage's SPIR-V
  version, then sends `ReplaceShader`.
- **One client.** The capture library serves one client at a time, so attaching to an application
  the app is connected to takes it over.
- **Android.** A package starts through `main/android.ts`'s `AndroidTarget`, as in the app, and is
  reached over the adb forward.
  - adb accepts a forwarded connection before the layer listens on the device. So a session counts
    as connected only once the capture library sends something. A connection that closes before
    then is retried, and the forward is re-created when connections are refused.
  - Stopping the session also turns the package's debug layer settings off.
- **Between captures.** Some tools read a running application directly:
  - `read_live_image` (`RequestImage`)
  - `get_live_descriptor_set` (`RequestDescriptorSet`)
  - the live object tools, which fetch creation stacks with `stack_requests.ts`

  The image and object details share their code with `read_texture` and `get_object`.
- **Search paths.** `search_paths.ts` resolves where the files named by debug information and stack
  frames live on this machine: the `set_search_paths` tool, then the environment, then the app's
  settings (`sourceRoots`, `symbolDirs`). It uses `main/shader_sources.ts` for shader files and
  `main/symbolize.ts` for stack frames. The shader analyses and the flame graph quote the code of a
  line through it.

## Building

Prerequisites and the one-command setup are in the [README](../README.md); `tools/setup.sh` does
the whole thing on Linux. What the pieces are:

```
# layer + test application
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release        # Linux
cmake -S . -B build -G "Visual Studio 17 2022" -A x64          # Windows
cmake --build build --config Release
# -> build/bin/{lib,}VkLayer_inspector_capture.{so,dll} + VK_LAYER_INSPECTOR_capture.json

# Android layer (NDK) + layer APK; needs the Android SDK, an NDK and a Java runtime
python tools/build_android.py [--abi arm64-v8a,x86_64]
# -> build/android/lib/<abi>/libVkLayer_inspector_capture.so, build/android/gpu_inspector_layer.apk

# app
cd src/app && npm install && npm start        # builds with esbuild, then launches Electron
npm run build                              # the bundles only, including claude-plugin/server/gpu-inspector-mcp.mjs
npm test                                   # unit tests: pass metrics, texture decoders, the MCP server
npm run typecheck                          # tsc
npm run watch                              # rebuild on change
npm run dist                               # installer (electron-builder), see docs/RELEASING.md
npm run icons                              # re-render assets/icon.{ico,png} from assets/icon.svg

# test application (re-records every frame; built by the top-level CMake)
build/bin/vkinsp_triangle --frames 600     # window is resizable; --msaa, --bad-scissor, --leak, --occluded, --persistent, --heavy, --prerecord, --push-template, --second-device, --second-queue, --pipeline-library, --shader-object, --suspend, --stencil, --ray-tracing
```

On Linux the layer serializes the surface arguments of each windowing system whose headers CMake
finds (`xcb/xcb.h`, `X11/Xlib.h`, `wayland-client.h`); a missing one is reported at configure time
and only costs that platform's surface arguments. `npm start` goes through
`src/app/tools/run_electron.mjs`, which clears `ELECTRON_RUN_AS_NODE` — terminals that are themselves
Electron apps (VS Code's) set it, and it would make the `electron` binary run as plain Node.

The window icon takes two paths on Linux. `_NET_WM_ICON`, the icon the window carries, must fit in
X11's maximum request size (256 KB), so `main.ts` scales `assets/icon.png` down to 128x128 before
handing it to `BrowserWindow`; at its native 512x512 it is 1 MB and Chromium drops it silently.
GNOME's dock ignores that property anyway and matches the window's `WM_CLASS` (`gpu-inspector`,
from the app's package name) against installed `.desktop` files, which is what
`tools/install_desktop_entry.sh` writes.

Debug aids: `npm start -- --launch=<exe> --record-always --debug-capture --screenshot=<png>`
captures a frame automatically and writes a screenshot (one per window), and
`--quit-after-screenshot` exits once it is written; `--debug-relaunch`, `--debug-multi` and
`--debug-detach` exercise relaunching, two simultaneous sessions and a session window;
`--debug-log=<file>` mirrors the session log to a file, the layer log to `<file>.layer.log` and any
malformed layer message to `<file>.badjson`; `--debug-launch-dialog[=android]` opens the launch
dialog at startup; `--launch-android=<package> --device=<serial>` launches on an Android device;
`--debug-dump=<json>` writes what the renderer knows at screenshot time (each session's state,
object and validation counts, refresh source, frame boundary, symbols, the Metal memory
breakdown, and per capture the command, draw, pass, texture and buffer counts, the frame findings,
the commands carrying stacks or validation messages, the CPU timeline's categories and the
Timeline card's lanes: `debugState()` on the session and capture views, read through
`window.__inspectorDebugState`); `--debug-capture-with=overdraw,stacks` turns on the capture
options that are off by default, as `--debug-capture-without=<list>` turns the default ones off;
`--debug-mouse=x,y[;x,y...]` clicks points on the window before
the dump, and `--debug-settle=<ms>` waits after them for work a click set going (a pixel history's
replay). `tools/ui_tests.py` builds its cases on these flags: the triangle application's options
(plain, `--msaa`, `--offscreen`, `--bad-scissor` with the validation layer, `--hazard` with sync
validation, stacks), the reports (render graph, bottlenecks, and the render target tab measuring
overdraw, following a clicked pixel, and drawing `--occluded`'s hidden draw with the depth test
overlay via `--debug-view=overlay:depth:last`; the mesh tab's VS In and VS Out via
`--debug-view=mesh:in` and `mesh:out`; the shader debugger via
`--debug-view=debugger:pixel|vertex|compute[:<command>[:<lines>|end[:decompiled]]]`), and saved captures with expected findings, each a UI run
whose dump and log are checked. `python tools/inspector_client.py --capture
--record-always --save out.json` talks to the layer without the UI.

Regenerate `src/vulkan/gen` (done automatically by CMake when vk.xml or the generator changes):

```
python tools/gen_vulkan.py --xml third_party/Vulkan-Headers/registry/vk.xml --out src/vulkan/gen
```

---

Previous: [Capture replay](REPLAY.md) · [Docs index](README.md) · Next: [Releasing](RELEASING.md)
