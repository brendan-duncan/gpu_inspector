# Direct3D 12 capture

Capturing Direct3D 12, the way `src/vulkan/` captures Vulkan and `src/metal/` captures Metal. A library
injected into the application hooks the D3D12 and DXGI objects it creates, tracks every object
with the call that made it, records the command stream of a frame on request with its render
targets, bound buffers and textures and GPU pass timings, and streams all of it to the inspector
over the protocol the Vulkan layer speaks. The Inspect and Capture panels work against a D3D12
application without knowing which API produced the objects.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 && cmake --build build --config Release
cd build\bin\Release
set DXINSP_LOG=1
dxinsp_launch.exe --dll dxinsp_capture.dll -- dxinsp_triangle.exe --frames 3
```

From the UI nothing changes: **Launch...** → **This computer** starts *every* Windows target with
both the Vulkan layer enabled and the D3D12 library injected, and whichever API the application
uses connects. There is no "which API" field, because the application already knows.

## Getting in

D3D12 has no loader layers. What it has is a handful of exported entry points that every device
and every swap chain passes through — `D3D12CreateDevice`, `CreateDXGIFactory`, `CreateDXGIFactory1`,
`CreateDXGIFactory2` — and COM objects whose methods are reached through a vtable. So:

* **The library is injected at process start.** `dxinsp_launch.exe` creates the target suspended,
  loads `dxinsp_capture.dll` into it with a remote `LoadLibraryW` thread, runs the library's
  exported `DxinspInitialize` in a second remote thread, and only then resumes the main thread.
  Injection therefore precedes the application's first instruction; there is no race with a
  device created in a static initializer. The launcher inherits its standard handles to the target,
  waits for it and exits with its exit code, so the inspector's process handling (log capture,
  `taskkill /T`, exit status) sees one process tree. A target the library cannot be injected into
  (a 32-bit executable, a protected process) is still started, with the reason on stderr, so a
  Vulkan application launched the same way keeps working.
* **Or the launcher waits for the application to start** (`--watch`, below), for an application the
  inspector does not start itself.
* **Or it follows the target's own children** (`--follow <text>`), for an application that renders
  in a process it starts itself. A Chromium browser's WebGPU and compositing work is in its GPU
  process, which the browser spawns: `--follow --type=gpu-process` injects into every descendant
  whose command line holds that text, frozen as it appears exactly as a watched process is, which
  puts the hooks in well before the child's `D3D12CreateDevice` (Chrome's GPU process is caught
  some 6 ms after it starts and makes its device a few hundred milliseconds later). The children
  that do not match — a browser's renderers and utility processes — are left alone, and a child
  that never creates a device never opens the port, so only the one that renders connects. A
  pattern can also exclude (`--follow !--use-gl=disabled`, for the second `--type=gpu-process`
  Chrome starts to collect GPU information and then exits).
* **The entry points are hooked inline** (MinHook, `third_party/minhook`, BSD-2-Clause): the
  library loads `d3d12.dll` and `dxgi.dll` itself at initialization and patches the four exports,
  which covers a static import, a `LoadLibrary` + `GetProcAddress` (Unity), and the Agility SDK
  (`D3D12Core.dll` is loaded *by* the system `d3d12.dll`, whose export is still the entry).
* **Everything below is a vtable patch.** The objects are never wrapped, the same decision the
  Vulkan layer makes for handles and the Metal library for classes: CoreAnimation-style problems
  have D3D12 counterparts (the debug layer keeps its own wrappers; DXGI queries the queue it is
  given for private interfaces), and a proxy of ours would have to survive all of them. Instead
  the first object of each kind that comes out of a creation call has its vtable's entries
  replaced (`hook.h`, `VtableHook`), once per distinct vtable. All command lists of a class share
  one vtable, so the first hooks them all; the debug layer's wrapper classes have vtables of their
  own and are discovered the same way. Per-object state lives in side tables keyed by the
  interface pointer (`tracker.h`), and `Release` is hooked so a count reaching zero drops the
  entry before the address is reused.
* **The library's own D3D12 calls run through the same hooks.** Read-back command lists, staging
  resources, query heaps and fences are made on the application's device, so every hook first asks
  `Internal()` (a per-thread depth set by `ScopedInternal`) and forwards without recording or
  tracking when the call is ours.

**The runtime swaps a command list's vtable with its state.** Found on the first render-pass
capture, which recorded `BeginRenderPass` and nothing until after `EndRenderPass`: D3D12Core gives
a list inside a `BeginRenderPass` region a vtable of its own (one whose entries refuse what a pass
forbids), and puts the ordinary one back at `EndRenderPass`; a closed list and a reset one have
theirs too. A vtable patched at creation covers none of those. So the command list hooks call
`HookCommandList` again after forwarding `Reset`, `Close` and `BeginRenderPass`, which patches
whatever vtable the object has then, once per distinct vtable (six turned up in one run of the
test application). The per-vtable originals mean a call made inside the pass forwards to the
render-pass entry it would have reached. The device's interface versions do not do this; a
runtime implementing an older version simply has a shorter vtable, which is why every installer
probes the newest interface the object answers to before it writes any slot.

**A vtable may be a copy of one we patched.** A Unity player died of a stack overflow a few
seconds in: something in the process copies an already patched vtable into heap memory, and a call
arriving on an object that uses the copy found no registry entry for it. The lookup then fell back
to reading the slot out of the vtable to find the original, and that slot holds our own
replacement, so `Hook_Release` forwarded to itself until the stack ran out. Every replacement we
install is now remembered (`IsOurs`), and a vtable holding one with no entry of its own is matched
back to the vtable it was copied from (`AdoptCopy`) and registered with that vtable's saved
originals: first on the object-identity slots, then, with a warning, on the replacement itself.
A copy whose source cannot be found refuses the call rather than recursing. Eighteen copies turn
up in a 45-second run of the Unity player.

Slot numbers and method signatures come from the SDK's C-style vtable structs, generated by
`tools/gen_d3d12.py` into `gen/d3d12_vtables.gen.h`, together with `gen/d3d12_enums.gen.*`, the
name tables of every D3D12, DXGI and debug-layer enum. That is the D3D12 counterpart of `vk.xml`;
the struct serializers are written by hand (`serialize.cpp`), as they are for Metal.

The listener checks the system's TCP table before it binds, because `SO_REUSEADDR` lets a second
listener take an address that is already served on Windows: two inspected applications would both
bind 47531 and the inspector would reach whichever the stack routed to. The default port steps to
the next free one and logs where it went; a port named by `DXINSP_PORT` is refused with an
explanation instead, since whoever chose it is waiting on that one. The table is read rather than
probed with a connect, which these one-client-at-a-time servers would answer by dropping the
inspector. It also settles the case of a Vulkan application whose driver creates a D3D12 device,
where both capture libraries live in the one process and the Vulkan layer has the port.

`DXINSP_PORT` moves the listener off 47531, `DXINSP_LOG=1` logs the intercepted calls to the
session's Log tab (`DXINSP_LOG_FILE` appends them to a file, since a GUI application has no
stderr), `DXINSP_STACKTRACES` takes a stack at every object creation, `DXINSP_RECORD_ALWAYS`
records every command list whether or not a capture is in progress, `DXINSP_DEBUG_LAYER=1`
enables the D3D12 debug layer before the device is created (the launch dialog's "Validation
layer"), and `DXINSP_FRAME_BOUNDARY=submit|present` forces the frame boundary (see "Frame
boundary" below).

`compile_check.cmd <file.cpp>` compiles one source of the library on its own (no link), for
checking a file against the headers without building the whole library.

## Waiting for an application to start

A game behind its launcher, a Unity player started from the editor: the inspector never runs the
executable, so it cannot create it suspended. The Vulkan layer has the loader's implicit layer
mechanism for that; D3D12 has no loader at all, and a device that already exists cannot be reached
afterwards (there is no enumeration API, and the hooks are on the calls that make one). So the same
launcher watches instead:

```
dxinsp_launch.exe --watch <image name or full path> --dll <dxinsp_capture.dll>
                  [--env NAME=VALUE]... [--timeout <seconds>] [--poll <ms>] [--once]
```

It polls `EnumProcesses` every `--poll` milliseconds (2 by default, with a one millisecond timer
for the duration of the watch, or `Sleep` would round that up to a scheduler tick) and injects into
the first process whose image name matches, case insensitively — the whole path when one was given.
Only a pid never seen before is asked for its name (`QueryFullProcessImageNameW`), which is what
keeps a poll to a fraction of a millisecond; a toolhelp process snapshot costs several, and those
milliseconds are the application's head start. Each pid is examined once, and its own process is
skipped. `--once` injects into one process and then stands in for it: the launcher waits for that
process and exits with its exit code, which is what the app's session wants
(`waitForD3D12Application` in `src/app/src/main/main.ts`); without it the watch goes on, injecting
into every matching process that starts. `--timeout` ends a watch that caught nothing with exit
code **3**; **2** is a usage error, **1** nothing to inject with, **0** (or the application's own
code) an injection.

Two things make this beat the application to its device:

* **The process is held still while the library goes in**, and let go once the hooks are installed.
  `dxinsp_triangle` has a device some fifteen milliseconds after its first instruction, and a remote
  `LoadLibraryW` of this library takes about a hundred, so without the hold the hooks arrive after
  the device and nothing is captured. What decides the race is therefore not how long the injection
  takes — the application is held throughout — but the milliseconds before it was caught. It is
  stopped with `NtSuspendProcess` the moment it is seen, before anything else is asked of it, since
  even enumerating its threads costs more than the application has left; the hold is handed over to
  per-thread suspensions only once the injecting thread exists, because that thread is one of the
  target's own and has to keep running.
* **Let go in bursts.** A process this young is mostly loading libraries, so the remote
  `LoadLibraryW` may be waiting for a loader lock the held application will never release. Whenever
  the remote thread makes no progress for 5 ms the application is let go for a burst and held again;
  bursts start at 200 µs and double, so it is given the least running time that releases the lock
  rather than a fixed slice of it. The injection line says how many bursts it took — none for the
  test application, a hundred or so for a Unity player, which loads a great many libraries.

The watched process was started by someone else, so it cannot inherit `DXINSP_PORT` and the rest
from the inspector: `--env NAME=VALUE` entries are written into the target as a double-null
terminated environment block and handed to `DxinspInitialize`, which applies them before anything
reads a setting (`src/main.cpp`, `SetConfigValue`). They are kept inside the library rather than
written into the process environment with `SetEnvironmentVariable`: a process caught a millisecond
or two after its first instruction may still be in its loader, building that environment, and a
variable set there then is lost — which showed up as a watched application that connected sometimes
and ignored its port the rest of the time. `ConfigValue` reads them first and the environment after,
so a launch, which inherits the variables, is unchanged.

It remains a race with the application's start, and the launcher says how it went: the age of the
process when the library went in, and a warning when that was more than a second (the application
was already running, and if it had a device by then nothing will be captured). Since the library
opens its port only once a device exists, a connection is the proof that it was in time; the app's
session says so when none comes (`D3D12_DEVICE_WAIT_MS` in `main.ts`).

## Talking to the UI

`transport.cpp` is the Vulkan layer's wire format byte for byte, and `tracker.cpp` emits the same
`AddObject` / `DeleteObjects` / `ObjectSetLabel` / `ObjectUpdate` messages, so the UI needed only
to learn the D3D12 spellings of a few things (`src/app/src/renderer/d3d12/`).

**Types** are the interface names the application sees: `IDXGIAdapter`, `IDXGISwapChain`,
`ID3D12Device`, `ID3D12CommandQueue`, `ID3D12CommandAllocator`, `ID3D12GraphicsCommandList`,
`ID3D12Resource`, `ID3D12Heap`, `ID3D12DescriptorHeap`, `ID3D12RootSignature`,
`ID3D12PipelineState`, `ID3D12StateObject`, `ID3D12Fence`, `ID3D12QueryHeap`,
`ID3D12CommandSignature`, `ID3D12PipelineLibrary`. A versioned interface (`ID3D12Device10`) is
tracked under its base name. The device's parent is its adapter; everything else's is the device,
except a swap chain's back buffers (`GetBuffer`), whose parent is the swap chain.

**The creating call** is the method name (`CreateCommittedResource`, `CreateGraphicsPipelineState`,
`CreatePipelineState` for a stream, `GetBuffer`, `D3D12CreateDevice`) and **the arguments** are the
call's parameters under their D3D12 names, so `pDesc` is the descriptor the Inspect panel shows
(`VulkanObject.descriptor` reads `pDesc` for a D3D12 object). Enums are written by name
(`DXGI_FORMAT_R8G8B8A8_UNORM`), flags as `A | B`, and references to tracked objects as
`{"__id", "__class"}`. Two D3D12 values are not objects and are written resolved:

* a **descriptor handle** as `{"heap": {ref}, "index": N}` (`"ptr": "0x.."` when the heap is not
  ours to know), and
* a **GPU virtual address** as `{"address": "0x..", "buffer": {ref}, "offset": N}`, resolved through
  the address ranges of every buffer the tracker knows (`descriptors.h`, `AddressMap`).

Shader bytecode in a pipeline's descriptor is summarized (`{"__bytes": N}`) and attached as blobs
named `<stage>:<entry>` with the UI's stage names — `vertex`, `fragment` (the pixel shader),
`tess_control` (hull), `tess_eval` (domain), `geometry`, `compute`, `task` (amplification),
`mesh` — so the shader views, the capture's shader sections and the flame graph find them where
they find a Vulkan pipeline's. Beside the bytecode the descriptor carries `reflection`, keyed by
stage (below).

## Frame capture

`capture.cpp` follows the Vulkan layer's model (`docs/ARCHITECTURE.md`, "Frame capture"): the UI's
`Capture` message arms the library, the next present starts the recording, every command list
recorded during the frame gets a `CommandRecorder`, `ExecuteCommandLists` freezes each list's
commands and records which ran in which order, and the next present after the last frame waits for
the GPU, maps the staging buffers and streams `CaptureFrameResults` + `CaptureFrameCommands`,
`CaptureTextureFrames` + `CaptureTextureData`, `CaptureBuffers` + `CaptureBufferData`,
`CapturePassTimings` and `CaptureComplete`. `api: "d3d12"` in `CaptureFrameResults` picks the
command tables in the UI.

**Frame boundary.** `IDXGISwapChain::Present` / `Present1`, which is what a D3D12 application has
in place of `vkQueuePresentKHR`. Frames are counted from the first present; `FrameStats` goes out
every 100 ms with the frame time, the CPU time inside `ExecuteCommandLists` (`submitMs`), the
monitor's refresh period (`EnumDisplaySettings` on the monitor of the swap chain's window,
`refreshSource: "monitor"`), whether the present syncs (`presentMode`), and `frameBoundary:
"present"`.

Not every D3D12 renderer presents. Chrome's GPU process runs its WebGPU work through Dawn, whose
D3D12 device renders into textures the compositor presents rather than calling `Present` itself,
so that device's `IDXGISwapChain::Present` is never seen — the same shape as an OpenXR Vulkan
application, where the runtime composites and the layer falls back to a submission boundary
(`src/vulkan/src/layer.cpp`, `OnSubmitForFrames`). The boundary is therefore decided **per device**
over its lifetime (`capture.cpp`, `DeviceFrame`): a device that presents is delimited by its
presents; one that goes `kSubmitsWithoutPresent` (60, the layer's threshold) submissions without
ever presenting is delimited by every `ExecuteCommandLists` from then on, with `frameBoundary:
"submit"` and no refresh period. `DXINSP_FRAME_BOUNDARY=submit` forces the submit boundary for
every device and ignores presents for framing — for the case where the compositor presents on a
hooked D3D12 device but the work to capture is Dawn's, which does not — and `=present` keeps the
present-only behavior. A queued "capture frame N" counts the boundaries a device actually has, so
it lands on the right one either way.

**Several devices.** A process can hold more than one D3D12 device: a game and a background copy
device, or Dawn's WebGPU device beside the compositor's. Each keeps its own frame boundary and
frame count. A capture's frames are the home boundary's — the swap chain that started a
present-delimited capture, or the device that started a submit-delimited one — and any other
device's presents or submits are recorded but land in the frame the home is on. By default a
process that presents anywhere has its capture started by a present, so a background device's
submit boundary does not hijack it (the guard the Vulkan layer applies in `OnFrameEnd`); the
`submit` override lifts that, to target a device that never presents while another one does. Each
participating device is waited for at the finish through its own fence, and its passes carry its
own timestamps.

**The stream.** Each command's `object` is its command list; `ExecuteCommandLists`, `Signal` and
`Wait` name the queue; `Present` names the swap chain. Per frame the order is the submission
order: the `ExecuteCommandLists` entry, then each of its lists' commands, then the next. A bundle
runs inside `ExecuteBundle` and arrives as its `children`, which the UI inlines the way it inlines
Vulkan secondaries. A list executed during the capture but recorded before it (no recorder) is
one `<unrecorded command list>` entry, unless `DXINSP_RECORD_ALWAYS` is on. The same goes for a
bundle recorded before the capture: `ExecuteBundle` carries its commands as `children` only when the
bundle had a recorder when it was recorded, which for a bundle an engine records at start-up means
record-always from launch (the launch dialog's "Record all command buffers"), read at the bundle's
creation rather than at the first capture for that reason. Such a bundle's snapshots were taken when
no capture was on, so they hold no contents, and they are of another time anyway: during a capture
the list that executes the bundle takes them again (`RecordedCommand::refresh`), with the copies
recorded into that list, which is how the bundle's vertex and index buffers get into the capture. A
table the bundle set before any root signature of its own is then read with the executing list's.
A list is recorded
under the method the application called (`DrawIndexedInstanced`, `SetGraphicsRootDescriptorTable`,
`ResourceBarrier`, ...) with its parameters under their names.

**Passes.** D3D12 has no render pass unless the application uses `BeginRenderPass`, and most do
not: render targets are bound with `OMSetRenderTargets` and draws follow. So the library synthesizes
the boundaries the UI's pass model needs, and says so in the stream:

* `OMSetRenderTargets` and `BeginRenderPass` begin a render pass. The recorded command carries the
  targets resolved: each handle as `{heap, index, resource: {ref}, view: {the RTV/DSV desc}}`.
* A pass ends at the next `OMSetRenderTargets`, at `EndRenderPass`, or at `Close`. Where the
  application made no call there, the library records a synthetic `EndRenderTargets` command so
  the UI's `PASS_END` has something to close the pass on. It is the only command in the stream the
  application did not make, and it carries no arguments.
* A dispatch inside a render pass stays in it (D3D12 allows the interleaving and engines use it);
  a run of dispatches outside one is a compute pass, opened by the dispatch and closed by the next
  barrier, render pass begin, event, bundle or `Close`, exactly the Vulkan layer's rule, with the
  UI's `COMPUTE_PASS_END` listing the same commands.
* Passes are numbered per command list, render and compute in separate sequences, which is what a
  `CaptureTextureInfo.passIndex` and a `PassTiming.passIndex` refer to.
* A render pass the application **suspends** across command lists
  (`D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS` / `_RESUMING_PASS`, which Unity's URP uses) carries
  almost nothing of the library's. Between a suspension and its resume Direct3D allows no copy and
  no `ResolveQueryData`: one there makes `Close` return `E_FAIL`, and an application that checks it
  -- Unity does -- treats that as a lost device and exits. So a split pass has no render target
  read-back, and the app's `suspended-pass` finding says how many there were.
  **It is timed, though.** A timestamp is a single `EndQuery`, which is allowed inside a pass
  region, so each segment takes one at each end -- the begin where the pass begins, the end
  *before* `EndRenderPass` is forwarded (`EndSplitPassTimestamp`), since after that the pass is
  suspended and the runtime takes nothing. What used to make this impossible was the resolve, which
  the library wrote into the application's list beside the query; it now resolves every pass's
  queries itself, from a list of its own, once the frame's work has been waited for
  (`Impl::ResolveQueries`). Counters are still not taken: `BeginQuery` / `EndQuery` pairs for
  pipeline statistics and occlusion are not allowed inside a render pass region at all, which is
  why no pass of the render-pass API has them. Measured on a Unity URP player, where 46 to 74 of a
  frame's ~58 pass segments are suspended: the frame went from about 11 timed passes to all 58.
  What its draws *read* is still taken: the copies of the buffers and textures they bind are held
  per list (`RecorderSlot::afterSubmit`) and recorded into a list of the library's own, executed on
  the same queue right after the submission. That is after the draws, which is right for what a
  pass reads and does not write (meshes, constants, sampled textures) and wrong for a texture the
  frame goes on to overwrite, such as temporal anti-aliasing's history. Unity's URP draws its whole
  scene in such passes, so without this a capture had every draw and none of their data.
* The runtime changes a list's vtable with its state, and **after a pass that ends suspended** the
  list is on one that `BeginRenderPass`'s re-hook had not seen: its `Close` and everything after
  went unrecorded, and so did its next `Reset`, which left the list with no recorder for the frame
  after. `EndRenderPass` re-applies the hooks as `BeginRenderPass` does.
* A list with **no recorder when a call arrives is adopted** (`CaptureManager::Adopt`): an engine
  that pools its lists resets one as soon as it has run, frames before it records into it again,
  so that `Reset` is long past when a capture is armed. The recorder is made at the first call seen,
  with a `Reset` that names no allocator and carries `adopted: true`. What state such a list is in
  is unknown (it may be inside a pass), so it is treated as a suspended pass is: recorded, with its
  copies after the submission, and of the queries only the passes' timestamps, which Direct3D
  allows in any state. No pipeline statistics or occlusion, since the application may have a query
  of its own open from before. Which lists come back adopted is up to the pool: on the URP player
  40 to 60 of a frame's lists did in about one capture of three, and before they took timestamps
  such a capture timed 11 to 32 of its 58 passes. `test/d3d12_triangle --pool` adopts every time
  (the `d3d12-pool` UI case).
* **Contents are taken in the frame of recording before the capture as well** (`TakesContents`). An
  engine records a frame's lists during the frame before, and a list recorded then and run in the
  captured frame would otherwise bind buffers the capture never read. Entries no captured list ran
  are dropped -- the ones that frame queued, and the ones the captured frame queued in lists it
  recorded ahead for the next, which used to be reported as a third of a Unity capture's buffers
  failing to read back -- an entry several lists asked for gets its frame from any
  of them (`sharedBy`), and a list recorded again lets go of what its last recording queued.
* **So are the queries** (`BeginPass`, `BeginDrawQueries`): a pass's timestamps and statistics go
  into the list as it is recorded, so timing only what is recorded once the capture has started
  measures nothing of a frame whose lists were built the frame before. The entries they make carry
  `warmup` and become the capture's only if their list runs in it, exactly as the read-backs do; one
  that does not keeps `frame == UINT32_MAX` and is not sent. The query counters therefore start over
  when the capture is *armed* (`RequestCapture`) rather than when it starts, since the warm-up frame
  is what hands out the first slots -- starting over after it would give the captured frame's passes
  the same slots and overwrite what they measured.
* **What a copy reads is captured whole**: the source range of `CopyBufferRegion`, of a
  `CopyTextureRegion` from a buffer, and of a buffer `CopyResource`. An engine fills its per-frame
  constant buffers that way, and a replay copying from a source it has nothing for overwrites
  good constants with zeros. **Vertex and index buffer views are whole too**, since a view says
  exactly what a draw reads and a mesh cut at `maxBufferSize` draws as part of itself; the
  capture's total buffer budget still bounds both.

**A pass the capture began outlives the capture.** A capture ends at a frame boundary, and an
engine that builds its command lists on worker threads (Unity again) has a dozen of them open at
that moment, several in the middle of a pass -- a pass this capture began, whose `PIPELINE_STATISTICS`
and `OCCLUSION` queries it began with it. A command list closed with a query still open returns
`E_FAIL` from `Close`, which Unity reports as `Device failed error (80004005)` and then exits, so
those queries have to be ended even though the capture is over and nothing records any more. The
recorders of lists still open therefore survive the end of the capture (`Impl::Finish` keeps them,
where it drops every recorder whose list is closed), the hooks reach them through
`EndOpenPass` / `OnBeforeClose` rather than through `RecorderFor`, which answers only while
something records, and each one is dropped at its own `Close`. `EndPass` ends the queries whichever
state the capture is in; only the timing they measured is thrown away.

**Render target read-back.** When a render pass ends the library appends to the application's
list: transition barriers of every color target and the depth target into `COPY_SOURCE`,
`CopyTextureRegion` into a readback-heap staging buffer, and barriers back to the states they were
in. The state each resource was in comes from `resources.h`, which follows the application's
`ResourceBarrier` / `Barrier` calls per list while it records and applies them at
`ExecuteCommandLists`; a render target found in an unknown state is assumed to be in
`RENDER_TARGET` (`DEPTH_WRITE` for the depth target, or `DEPTH_READ` for a read-only DSV), which is
what it must be to be bound. A multisampled target is resolved (`ResolveSubresource`) into a
single-sampled texture of the capture's first; depth is resolved by copying sample zero through a
compute shader the library compiles once per device (D3D12 cannot resolve depth with
`ResolveSubresource`). A depth-stencil target is read back twice, its depth plane and its stencil
plane (plane 1, one byte per texel, `aspect: "stencil"`), each an entry of its own; a multisampled
stencil is not resolved. Pixel data travels under the protocol's format
names: `formats.cpp` maps each `DXGI_FORMAT` to the `VK_FORMAT_*` spelling the UI's decoders read,
so an identical memory layout reuses all of them; the DXGI name stays in the descriptors. A target
larger than `maxTextureSize`, or of a format with no mapping, is reported with the reason.

**Bound buffers and textures** are read back like the Vulkan layer's: every root descriptor table
bound with `SetGraphicsRootDescriptorTable` / `SetComputeRootDescriptorTable` and every root view
(`SetGraphicsRootConstantBufferView` and the rest) gets a `descriptors` snapshot on the binding
command in the shape of a Vulkan descriptor set snapshot, so `draw_state.ts` reconstructs a
draw's bindings unchanged. A root view is snapshot at its bind. A table is snapshot when the list
next draws with it (a graphics table) or dispatches (a compute one), or at `Close` if nothing did:
a table names slots of a heap, and what is in them counts when the GPU reads them. An engine may
bind the table and write its descriptors afterwards (Unity does, for every draw), and a snapshot at
the bind then holds what the slots had the frame before (`CommandRecorder::DeferSnapshot`):

```
descriptors: { bindPoint: "graphics" | "compute", sets: [ {
    set: <root parameter index>, descriptorSet: {ref to the ID3D12DescriptorHeap} | null (a root view),
    layout: {ref to the ID3D12RootSignature},
    bindings: [ { binding: <range index>, type: "D3D12_DESCRIPTOR_RANGE_TYPE_SRV" | "..._CBV" | "..._UAV" | "..._SAMPLER"
                                             | "D3D12_ROOT_PARAMETER_TYPE_CBV" | "..._SRV" | "..._UAV",
                  register: <base shader register>, space: <register space>, stages: "D3D12_SHADER_VISIBILITY_ALL",
                  descriptors: [ { buffer: {ref}, offset, range, data }            // a CBV, or a buffer SRV/UAV
                               | { resource: {ref}, view: {the SRV/UAV desc}, data }  // a texture SRV/UAV
                               | { sampler: null, samplerDesc: {...} }             // a sampler
                               | null ] } ] } ] }
```

The heap contents behind a table come from `descriptors.cpp`, which follows every
`Create*View`, `CreateSampler`, `CopyDescriptors` and `CopyDescriptorsSimple` into a record per
heap slot (`CPU handle - heap start` over the increment size), the way the Vulkan layer follows
`vkUpdateDescriptorSets`; the root signature's ranges (`D3D12CreateVersionedRootSignatureDeserializer`
at `CreateRootSignature`) say which slots a table covers and which registers they are. Buffer
ranges named by a snapshot, by `IASetVertexBuffers` / `IASetIndexBuffer` (whose GPU addresses the
`AddressMap` resolves to resources) and by `ExecuteIndirect` are queued for read-back
(`QueueBufferCapture`, truncated to `maxBufferSize`), copied at the end of the pass they were
bound in — a copy may not interrupt a render pass's attachments — and referenced by id in `data`
and `bufferData`. Textures an SRV or UAV names are read back once per resource per capture
(`QueueTextureCapture`, every mip with all its slices, under `maxImageTotal`) and referenced in the
descriptor's `data`; a `CaptureTextureInfo` of `kind: "sampled"` carries them. Root constants are
`SetGraphicsRoot32BitConstants` / `SetComputeRoot32BitConstants` commands whose arguments carry
the bytes inline (`pValues`, with `offset` and `size` in bytes and `stageFlags` naming the bind
point), which is the UI's push constant shape. Vertex and index buffer views carry the resolved
`buffer` and `offset` beside the address, and the layout comes from the pipeline's `InputLayout`.

**Pass timings** are `EndQuery` timestamps around every render and compute pass in a
`D3D12_QUERY_HEAP_TYPE_TIMESTAMP` heap of the capture's, resolved into a readback buffer at pass
end, scaled by the queue's `GetTimestampFrequency`, and sent as `CapturePassTimings` with the
Vulkan keys. Beside them a `PIPELINE_STATISTICS` query per render pass gives the counters the GPU
Bottlenecks report reads (`inputVertices`, `inputPrimitives`, `vertexInvocations`,
`clipperInvocations`, `clipperPrimitives`, `fragmentInvocations`, the Vulkan layer's names for the
same quantities), and an `OCCLUSION` query the samples that passed the depth and stencil tests
(`fragmentsPassed`). A pass whose list has a query of the application's open is not counted.

**Stack traces** (`stacktrace.cpp`, the Vulkan layer's DbgHelp code): every tracked object keeps
its creation stack with `DXINSP_STACKTRACES`, every recorded command carries its return addresses
with the capture option `stacktraces`, and the UI symbolizes on request. Frames inside this
library, `d3d12.dll`, `D3D12Core.dll`, `dxgi.dll` and the driver's user-mode DLLs are marked
internal.

**Multiple devices, queues and swap chains.** Each device keeps its own capture state (query
heaps, staging, resolve textures); a capture's frames are those of the swap chain that presented
first. Copy and compute queues take part like graphics queues; a list executed on one is read back
after that execution.

## Live requests

* `RequestImage {id, mip, layer}` reads one subresource of an `ID3D12Resource` back
  (`image_readback.cpp`): a command list of the library's own executed on the device's first direct
  queue after the application's work, a fence wait on the transport thread, and `ImageData` with
  the pixels under the protocol's format name.
* `RequestDescriptorSet {id}` answers for an `ID3D12DescriptorHeap` with the first 4096 written
  slots as `bindings`, in the snapshot shape above.
* `RequestBlob`, `RequestStacktraces`, `RequestSymbols`, `RequestSnapshot`, `Settings` and `Ping`
  as the Vulkan layer answers them.
* `ReplaceShader {pipeline, stage, spirv}` takes DXBC or DXIL bytecode (the field keeps its name;
  the app compiles HLSL with `dxc -T <profile>` for a D3D12 session): the library keeps a copy of
  every pipeline's description, creates a replacement with the stage swapped, registers it as an
  object of its own (`"<name> (edited)"`) and binds it instead of the original in `SetPipelineState`
  from then on; `RestoreShader` drops it. A pipeline made from a stream (`CreatePipelineState`) is
  rebuilt from its stream. One loaded from a pipeline library is rebuilt too: the library only
  caches the driver's compiled blob under a name, and `LoadGraphicsPipeline` is still given the
  whole description, which is the path that matters because Unity's D3D12 player loads every
  graphics pipeline that way.

## Shaders

Reflection turns a captured buffer's bytes into named fields. The Vulkan side parses SPIR-V in the
UI; for DXBC and DXIL the library does it in the process, at pipeline creation (`shader_reflect.cpp`):
`D3DReflect` from `d3dcompiler_47.dll` (always in System32) for DXBC, and `IDxcUtils::CreateReflection`
from `dxcompiler.dll` for DXIL, which the library looks for beside itself (where the installer
puts Electron's copy, `src/app/tools/after_pack.cjs`), in the Vulkan SDK (`%VULKAN_SDK%\Bin`),
in the Windows SDK's `bin\<version>\x64` and on `PATH`. Both give the same
things — every constant buffer's members with offsets, every bound resource with its register and
space, the stage's inputs and outputs, a compute shader's thread group size — written in the shape
the UI's own reflection has (`ReflType`) and keyed by `space` and `register`, which is what the
snapshot's bindings carry too. `src/app/src/renderer/d3d12/reflection.ts` reads it into the same
`ShaderReflection` the SPIR-V path builds, so a draw's constant buffers render as typed blocks,
the Reflection section works on a pipeline object, and the flame graph weighs its stages.

The Inspect panel's shader views need text: `dxinsp_shader.exe`, built beside the library from
the same reflection code, disassembles a bytecode file (`D3DDisassemble` for DXBC, DXC's
disassembler for DXIL) and finds its HLSL; `src/app/src/main/shader_tools.ts` runs it the way it
runs `spirv-dis`. There is no DXIL decompiler — `dxc` has no decompile mode, `spirv-cross` cannot
read DXIL — so the only HLSL there can be is the HLSL the build kept, and where dxc put it
depends on one flag:

* `-Zi` **embeds** the source in the container (`ILDB`), which is what `EmbeddedSources` reads
  through `IDxcPdbUtils::Load`. `-Qembed_debug` is not needed for this; it only silences dxc's
  warning about having no `-Fd` to write a PDB to.
* `-Zs` keeps the source **out** of the container and writes it to a **PDB** instead, named after
  the shader hash when the build used `-Fd <dir>\`.
* Neither leaves no source anywhere in the container.

So `--sources <container> [--pdb <file>]... [--pdb-dir <dir>]...` looks for that PDB when the
container has none of its own (`FindShaderSources`): the `ILDN` part holds the exact file name dxc
wrote (a bare `<hash>.pdb` for `-Fd <dir>\`, or the path `-Fd <file>` was given), and the `HASH`
part holds the 16-byte shader hash that names it — the hash of the *module*, so it is the same for
a container built with `-Zi`, with `-Zs` or with no debug flags at all. Each directory is searched
for that file name a few levels down, and then every `.pdb` lying in it is opened and matched by
the hash `IDxcPdbUtils::GetHash` reports, which catches a build that renamed the file. An entry the
tool read out of a PDB carries `"from"` beside its `name` and `text`, so the UI can say where the
source came from; when there is none anywhere the array is empty and the reason goes to stderr.
The array's last entry, `{"compile": {"mainFile", "entryPoint", "target", "defines", "args"}}`,
is how dxc was run, from the same debug information (`IDxcPdbUtils::GetMainFileName`, `GetDefine`,
`GetArg`; `ShaderCompileInfo`): what compiling the same source again needs, less the file options
and the debug flags, which the caller supplies itself.

The directories come from the session's **symbol directories** — the same build output stack
traces are symbolized against (`src/app/src/main/main.ts`'s `inspector:shaderText`, the MCP
server's `set_search_paths` `symbolDirs`), since a shader PDB is exactly that kind of file.

A stage with no HLSL anywhere still opens in the shader editor: `src/app/src/renderer/d3d12/
hlsl_stub.ts` writes one out of the reflection — every `cbuffer` with its members at their real
offsets (`packoffset`), every SRV, UAV and sampler at its register and space, the entry point with
the input and output signature's semantics and a compute stage's real `[numthreads]` — with a body
that compiles and does something harmless. It declares what the original declared, so `dxc` turns
it into a binding-compatible replacement and **Compile & Apply** works on a shipped shader.

**The shader debugger** steps a D3D12 stage as that HLSL compiled to SPIR-V. There is no DXIL
interpreter in the app (the interpreters are SPIR-V's and MSL's), so `compileHlslForDebugging`
(`src/app/src/main/shader_tools.ts`) takes the sources and the compile entry above, writes the
files out under their own names so `#include` resolves, and runs `dxc -spirv` on the main file with
the build's defines and arguments, `-fspv-debug=line -fspv-debug=source` (the text and a line per
instruction embedded), `-fspv-reflect` (every stage variable keeps its HLSL semantic),
`-fvk-use-dx-layout` (constant buffers at their D3D offsets, so the captured bytes read right),
`-O0` (the locals survive) and the register shifts of `src/app/src/shared/hlsl_debug.ts`: `t`
registers become bindings from 65536, `s` from 131072, `u` from 196608, a space its descriptor set.
`src/app/src/renderer/d3d12/shader_debug.ts` steps the result in the SPIR-V interpreter and undoes
the shift when the interpreter asks for a binding: the register is looked up in the draw's root
descriptor table and root view snapshots (keyed by register and space, above), in the root
constants set for the root signature parameter naming that `b` register, or among the root
signature's static samplers (`pStaticSamplers` of the object's `pDesc`). A vertex input is paired
with the input layout's element by semantic, a pixel input with the vertex shader's output by
semantic, since dxc numbers each stage's locations by declaration order; a pixel's inputs come from
the vertex shader run in the interpreter over the draw, as on Metal, rasterized with D3D's
conventions. dxc wraps the HLSL entry point in a SPIR-V one that loads the inputs and calls
`src.<name>`; stepping starts inside the latter. A shader with no HLSL anywhere cannot be
debugged, and the tab says so.

Replayed for D3D12 (`replay/src/dx_measure.cpp`): per-draw timings and counters, and shader cost
by ablation, whose variants are DXIL edited as its disassembly and assembled again by dxc
(`AssembleDxil` in `src/shader_reflect.cpp`, `dxinsp_shader --assemble`). Overdraw, pixel history,
draw overlays and mesh output are measured in the application while it captures instead; see
"Overdraw" and "Pixel history" below.

## Validation messages

With `DXINSP_DEBUG_LAYER=1` the library calls `D3D12GetDebugInterface` and `EnableDebugLayer`
before forwarding `D3D12CreateDevice`, then takes the device's `ID3D12InfoQueue`. Where
`ID3D12InfoQueue1` exists (Windows 11, or the Agility SDK) `RegisterMessageCallback` delivers each
message as it is produced, so a message fired inside a command list method while the library
records it is attached to that command (`command: {commandBuffer, slot}`) like a Vulkan validation
message; otherwise the queue is drained at every present. Messages become `ValidationMessage`
(severity from `D3D12_MESSAGE_SEVERITY`, `types` from the category, `idName` the
`D3D12_MESSAGE_ID` name), deduplicated by text with counts sent as `ValidationCount`, and the
first 2000 unique ones are kept for a UI that connects later. D3D12 messages name objects only in
their text, so `objects` is empty.

The device's `Release` reaching zero sends the Vulkan layer's `LeakReport` for whatever is still
tracked under it. A D3D12 child holds a reference on its device, so the report only fires for an
application that releases its device last; one that exits with objects alive reports nothing.

## Device sections

`D3D12CreateDevice` records the adapter (`DXGI_ADAPTER_DESC3`: name, vendor and device ids, memory)
on the `IDXGIAdapter` object, and the device's feature level plus the `CheckFeatureSupport`
results (`D3D12_OPTIONS` through the latest the runtime answers, `SHADER_MODEL`, `ARCHITECTURE1`,
`ROOT_SIGNATURE`) as an `ObjectUpdate` named `features`, which the Inspect panel lists as a
sections of the device the way it lists a physical device's limits.

## Replay and Export to C++

`replay/` builds `dxinsp_replay.exe`, which re-executes a capture on this machine's GPU, compares
every render target with the capture's copy, and with `--export <directory>` writes the frame as a
standalone C++ project ([docs/REPLAY.md](../../docs/REPLAY.md#direct3d-12)). It links none of the
capture library: it shares `formats.*`, `log.*` and the generated enum tables with it, and the
capture reader with `src/replay`.

| File | What it holds |
|---|---|
| `src/dx_reflect.h` | one description of every D3D12 struct a capture holds, visited by the two below |
| `src/dx_decode.*` | the visitor that fills a struct from the capture's JSON: enums and flags by name, objects by id, GPU addresses and descriptor handles resolved to the replay's own |
| `src/dx_source.*` | the visitor that spells a struct as C++ |
| `src/dx_replayer.*` | objects, contents, initial states, descriptors, command lists, read-backs and their comparison |
| `src/dx_exporter.*` | the project: sections, parts and files, the data file, `frame_objects.*`, CMake and README |
| `export_template/` | the exported project's hand-written part (`main.cpp`, `dx_support.*`), embedded into the tool |

A command the replayer does not issue is reported with its index and left out, never guessed at;
adding one is a case in `DxReplayer::IssueCommand`, and a struct is a `Reflect` in `dx_reflect.h`.

## Test application

`test/d3d12_triangle` is the D3D12 counterpart of `test/triangle`: a window and a swap chain, a
depth buffer, a root signature with a constant buffer table, a texture SRV, a static sampler and
root constants, a vertex/pixel pipeline compiled with `dxc` at build time (`-Zi`, so the source
view has something to show), a compute dispatch into a UAV, and one instanced draw per
frame, re-recorded every frame. `--frames N`, `--msaa`, `--bundle` (the draw in a bundle),
`--indirect` (through `ExecuteIndirect`), `--render-pass` (`BeginRenderPass` instead of
`OMSetRenderTargets`), `--compute`, `--leak` and `--offscreen` exercise the paths above.
`--offscreen` creates no swap chain and never presents, rendering into its own targets the way
Chrome's Dawn WebGPU device does, which is what drives the submit frame boundary; a capture of it
is a full frame all the same.

## Overdraw

With **Overdraw** in the capture bar (the `Capture` message's `overdraw`), every render pass of the
capture is issued a second time into the application's own command list and each pixel ends up
holding how many fragments landed on it (`overdraw.cpp`). The Vulkan counterpart, `vkinsp_replay
--overdraw` (`docs/REPLAY.md`), first has to rebuild the frame from a capture file; the Metal library
measures the same way this does (`src/metal/src/overdraw.h`). Nothing is rebuilt here: the library is
in the process with the application's own objects.

* **Recording the pass.** While such a capture records, every command-list call that shapes what a
  pass rasterizes is also kept as a closure holding its arguments, with a reference on each object it
  names: `SetPipelineState` (and `Reset`'s and `ClearState`'s initial pipeline, which is the only
  bind many engines make), `SetGraphicsRootSignature`, `SetDescriptorHeaps`, the root table, view and
  constant setters, `IASetPrimitiveTopology`, `IASetVertexBuffers`, `IASetIndexBuffer`,
  `RSSetViewports`, `RSSetScissorRects`, `OMSetStencilRef`, `OMSetFrontAndBackStencilRef`,
  `OMSetBlendFactor`, `OMSetDepthBounds`, the two clears, and the draws (`DrawInstanced`,
  `DrawIndexedInstanced`, `DispatchMesh`, and `ExecuteIndirect`, which is kept only so it can be
  reported as not counted). Barriers, queries and everything that does not change rasterization are
  not kept. A bundle's kept calls are inlined into the list that executes it, since a bundle sets
  state on that list and draws with it. Each call carries a key saying which earlier call it undoes
  (`OpKey` in `overdraw.h`), which is what lets the state at a point be rebuilt from the calls still
  in effect there rather than from every call before it.
* **Where it starts.** A pass that has a depth-stencil attachment gets a copy of it in the
  `OMSetRenderTargets` / `BeginRenderPass` hook, where the list is outside a render-pass region and
  the application has not drawn yet. `BeginRenderPass` says whether it preserves, clears or discards
  it, and a clear is applied to the copy instead of the copy being taken; `OMSetRenderTargets` has no
  such thing, so the attachment is copied and the application's own `ClearDepthStencilView` inside
  the pass is issued against the copy as well.
* **Drawing it again.** When the capture ends the pass -- after its queries and its render-target
  read-back, so neither counts the measurement -- the kept calls are issued into the same list: the
  calls still in effect at the pass's start, then the pass's own, in order. Twice, into an
  `R16_FLOAT` target of the pass's size: once with the depth copy bound and the pipelines' own
  depth-stencil state, once with no depth-stencil attachment and the tests off.
* **The pipelines.** A pipeline state cannot be copied, only its description, which `shader_edit.cpp`
  already keeps from every creation. The counting copy replaces the pixel shader with one that
  returns 1.0, gives it one `R16_FLOAT` target blended `ONE + ONE` writing red only, one sample, and
  the measurement's depth-stencil format. For a description the copy is field edits; for a pipeline
  stream every subobject the application wrote is kept, the ones the measurement changes are patched
  in place, and the ones it needs that the stream did not carry are appended, since the runtime
  refuses a stream that describes one field twice. Copies are cached on the pipeline and released
  with it. The counting shader is compiled once per container kind: `ps_5_0` through `D3DCompile`
  from `d3dcompiler_47.dll`, which every Windows has, and `ps_6_0` through `dxcompiler.dll` for a
  pipeline whose other stages are DXIL, because D3D12 refuses a pipeline that mixes the two.
* **Putting it back.** Unlike a Metal encoder, a D3D12 command list keeps its state across passes, so
  the calls still in effect at the end of the pass are issued again once the measurement is drawn.
  The render targets are not: a pass only ends where the application is about to bind others, has
  left a render-pass region, or is closing the list.
* **Results.** The counts are read back once the capture's lists have run and sent as
  `CaptureOverdraw` with per-pass totals, covered pixels, maximum, draws and a histogram, plus one
  `CaptureOverdrawData` frame of little-endian 16-bit counts per measurement. Counts larger than
  `maxTextureSize` go without their pixels.

Everything the measurement makes -- the count target, the depth copy, the counting pipelines, the
readback buffer, the descriptor heaps -- is kept until the capture's command lists have completed,
because a copy recorded into the application's list outlives the pass that recorded it.

Limits, reported rather than silently dropped:
- Fragments a shader discards are counted: the counting shader does not discard.
- An `ExecuteIndirect`'s draws are not counted: their arguments are in a buffer the GPU reads, so
  they can be issued neither one at a time nor with a pipeline of the measurement's.
- A pass that executes a bundle recorded before the capture cannot be issued again, since the
  bundle's calls were never kept.
- A multisampled pass is counted single-sampled and without its depth and stencil tests, and a
  layered pass without them too (its depth-stencil attachment is not copied).
- A pipeline whose copy does not build is a skipped draw, with the reason on the pass.

## Pixel history

A pixel's history is every pass start, clear and draw of the frame that touched one pixel of a render
target, what each draw's fragments at the pixel met, and the value and depth after each
(`pixel_history.cpp`). The Vulkan counterpart replays a capture file (`vkinsp_replay --pixel`).

* **Asking.** The `Capture` message's `pixelHistory` (`{texture, x, y, mip, layer}`) names an
  `ID3D12Resource` from an earlier capture. In the app the pixel is clicked in a render target's tab,
  which captures the next frame. A swap chain's back buffer -- or a resource no longer alive -- follows
  whichever back buffer the captured frame renders into, the way a Metal capture follows the next
  drawable.
* **Where it starts.** Every pass whose color attachment is that resource at that mip and slice gets
  copies of all its attachments, textures of the library's own of the same formats and size, with the
  followed pixel copied into them before the pass begins (or cleared, where a real render pass clears
  the attachment at its start).
* **One draw at a time.** After the application's pass has ended and the capture has read its render
  targets back, the copies are bound and the pixel is read: the pass's start. Then the pass's kept
  calls are issued in order, and at every draw a one-pixel scissor and the draw six times under
  occlusion queries with copies of its pipeline that add one step each -- its primitives with no
  culling and no tests, with its cull mode, with its own pixel shader (so a discard shows), with the
  depth test, with the stencil test, and with both. None of those writes anything: color writes are
  off and the depth and stencil write masks are cleared. Then the draw itself, with the application's
  pipeline, and the pixel is read again. Cull mode and the depth-stencil state are pipeline state in
  D3D12 rather than encoder state as in Metal, so the six steps are six pipeline copies rather than
  two. A `ClearRenderTargetView` or `ClearDepthStencilView` of one of the pass's attachments is an
  event of its own, since a D3D12 pass has no load action.
* **Results.** At the end of the capture the counts and texels go out as `CapturePixelHistory`, in
  the JSON `vkinsp_replay --pixel-data` writes, which the app's pixel history pane and
  `get_pixel_history` read the same way.

Limits:
- Multisampled and layered passes are noted and not followed.
- An `ExecuteIndirect`'s draws are not followed; the values after one may be missing its writes.
- Only the first 1024 draws of a pass are followed. Each costs seven draws in the captured frame.
- Writes outside render passes (copies, compute) are not events.

## Not done

A multisampled stencil is not read back (nor a multisampled depth, outside the measurements'
compute resolve). Sampler feedback, video and the work graph and mesh shader nodes are recorded as
commands and objects but their contents are not read back. Ray tracing is read back
(`src/raytracing.h`); what it leaves out is the build of a structure written before the capture
began, which is where an engine builds its bottom levels, so those show no geometry and a replay of
them traces an empty scene. Enhanced barriers (`Barrier`) are tracked for state only as far as their layouts map to
legacy states. A table a bundle set before any root signature of its own has contents only in a
capture that executes the bundle, where the executing list's root signature reads it. Only x64 targets are
injected. A process that is **already running** cannot be attached to: the hooks go on the entry
points before the device exists, and a device that exists cannot be found afterwards. `--watch`
covers what the implicit layer covers for Vulkan — an application the inspector did not start — but
it catches it at its start, so the watch has to be running before the application is launched.
