# Metal (macOS)

[Docs index](README.md) › Metal

On macOS the inspector captures Metal applications. Metal has no layer mechanism, so the capture
library is inserted into the application by dyld (`DYLD_INSERT_LIBRARIES`) and hooks the Metal
objects from there. The Inspect and Capture tabs work the same way as they do for Vulkan.

Metal support is newer than the Vulkan layer and does less; [what is not there
yet](#what-is-not-there-yet) has the current list, and `src/metal/README.md` the details.

A macOS build also opens `.gpucap` files taken anywhere, and inspects
[Android devices](ANDROID.md) over adb, exactly as the Windows and Linux builds do.

## Launching an application

Press **Launch...**, keep *Run On* on **This computer**, and choose the application's `.app`
bundle (the inspector finds the executable inside it) or a plain executable. `DYLD_INSERT_LIBRARIES`
is set for that process alone; nothing is registered system-wide.

## Code signing decides whether this works

dyld silently ignores `DYLD_*` variables for a process that uses the hardened runtime, unless the
application carries both of these entitlements:

- `com.apple.security.cs.allow-dyld-environment-variables`
- `com.apple.security.cs.disable-library-validation`

No notarized application does. The inspector checks the signature with `codesign` **before**
launching and tells you, rather than leaving a session waiting for a connection that can never
arrive; the message includes the `codesign --force --sign - --entitlements ...` command that adds
the two keys.

**A locally built player — a Unity development build, the main target here — is normally ad-hoc
signed without the hardened runtime and needs none of this.**

Re-signing is deliberately not done for you: it rewrites the application bundle and invalidates
its signature and notarization. Do it to a development build, not to a shipped copy.

## Starting an application by hand

An application the inspector cannot launch itself can be started with just the library set, then
picked up from the list in **Attach...** on the main bar:

| Variable | Value |
|---|---|
| `DYLD_INSERT_LIBRARIES` | the path to `libmtlinsp_capture.dylib` |
| `MTLINSP_PORT` | optional: the port to listen on, when one in particular is wanted |
| `MTLINSP_LOG` | optional: `1` logs the intercepted calls to the session's **Log** tab |
| `MTLINSP_LOG_FILE` | optional: a path the log lines are appended to as well, for an application whose stderr goes somewhere nobody can read (a Unity player) |

`MTLINSP_PORT` is optional because the library steps to a free port when another inspected
application has the default one, so several started by hand are all reachable without anybody
choosing numbers — and each answers the probe the attach list sends, so it turns up by name with
its pid and its port. A port named in `MTLINSP_PORT` is used as given and never stepped off, since
that is where whoever named it is waiting.

One difference from the other two libraries: an application with an inspector *already attached*
is missing from the list rather than shown as busy. Its session runs on the thread that would
otherwise be accepting the probe, so the probe times out
([the reason, and what fixing it needs](../src/metal/README.md)).

## What works

- **Object inspection** — the device, command queues, buffers, textures, libraries, and render and
  compute pipeline states, each with the call that created it, its arguments and its label.
  Clicking a texture reads its pixels back live. A library lists its function names, and shows the
  Metal Shading Language it was compiled from when it was compiled on the spot rather than loaded
  as a precompiled `metallib`.
- **Frame capture** — the frame's commands grouped by command buffer and pass, each draw with the
  pipeline that was bound for it, its decoded vertex and index buffers, and the pass's read-back
  color attachments. Captures save to the same `.gpucap` files and reopen on any platform.
- **Validation** — ticking **Validation layer** in the launch dialog enables Metal's API and
  shader validation in the mode that logs a failure instead of aborting, and the messages are
  listed in the Inspect tab.
- **Bottleneck counters** — Metal's counter sets measure overdraw, fragments per primitive and
  depth rejection per pass. See [Finding GPU bottlenecks](PROFILING.md).
- **Timing Capture** — every frame's wall time and where its CPU went, over minutes, for finding
  [a hitch rather than a slow frame](PROFILING.md#step-1c-a-hitch-rather-than-a-slow-frame). The
  categories are the CPU timeline's, so a frame that stopped for a pipeline compile says so. With
  **Sample stacks** on it also samples every thread's call stack 250 times a second and whether it
  was running or blocked, which is what answers the hitch none of the timed calls accounts for — the
  application's own work between them. On macOS that goes through Mach (`task_threads`,
  `thread_suspend`, `thread_get_state`) and walks frame pointers, which the arm64 ABI guarantees are
  there; it needs no privilege and no second process, unlike the kernel traces this competes with.
- **GPU faults** — a command buffer that fails is reported with its error and, where the driver
  supplies them, the state of every encoder in it: which one faulted, which were affected, which
  never ran, and the debug signposts each had passed. That is Metal's answer to Vulkan's
  breadcrumbs and DRED's command lists, and unlike either it costs no per-draw markers — the option
  is set on the command buffer and is always on while the inspector is attached. A fault the session
  will not survive (a timeout, a page fault, revoked access) is also reported as a device loss, so
  the diagnosis goes to the top of the log rather than among the validation messages.
- **Mesh view** — a draw's **VS In** from the vertices it read, and its **VS Out** from running the
  draw's own vertex function in the Metal Shading Language interpreter (`renderer/msl/`), the same
  one the shader debugger steps. Vulkan gets its outputs from a replay and Direct3D 12 by streaming
  them out of the running application; Metal has no replay that serves analyses, so it interprets
  instead — which also means the outputs need no second capture. Draws longer than 20,000 vertices
  are cut off, and the view says so rather than showing a short mesh as a whole one.
- **Shader editing** — a pipeline's stage recompiled from edited Metal Shading Language and bound
  in the running application from its next frame: **Edit** under a render or compute pipeline
  state's Shader section, then **Compile & Apply**. Metal's is the least trouble of the three
  backends, and for a reason worth knowing: a Vulkan or Direct3D 12 edit has to be compiled here,
  by glslang or dxc, so it needs the Vulkan SDK on this machine — while a Metal capture holds the
  Shading Language the application itself compiled, and the device that compiled it is in the
  application. So nothing has to be installed, and the compiler's own diagnostics come back and
  mark the lines of the text on screen. Function constants the stage was specialized with are
  carried across, which matters for a library of `[[function_constant]]`-guarded variants: without
  them the recompile would quietly build a different variant. **Restore Original** binds the
  application's own pipeline again.

  What Metal does not have is Direct3D 12's and Vulkan's **Compile & Replay** — running the edit
  against the captured frame instead of the live application — because that needs a replay that
  serves analyses, which `mtlinsp_replay` is not yet. A pipeline the inspector never saw created
  cannot be rebuilt either, and says so.
- **Draw-call overlays** — where one draw landed over its pass's render target, and what the depth
  test, the stencil test and its own culling did with it: Highlight Draw, Depth Test, Stencil Test,
  Backface Cull and Wireframe ([Draw-call overlays](REPORTS.md#draw-call-overlays)). Measured by
  drawing the pass again inside the application, so asking for one captures the next frame — see
  [Measuring a draw inside the application](#measuring-a-draw-inside-the-application).
- **Depth and stencil read-back** — both aspects of a pass's depth/stencil attachment, each its own
  entry in the render targets. Two read-backs of one texture, because a blit may fetch depth or
  stencil but not both; the library also forces the store on, since an application that only *tests*
  stencil leaves `storeAction` at `DontCare` and there would otherwise be nothing in memory to read.
- **Ray tracing** — every `MTLAccelerationStructure` as an object, the builds, refits and copies an
  acceleration structure encoder records, and what each build read: a geometry descriptor names its
  buffers outright, so the vertices, indices, bounding boxes and instance descriptions a structure
  was built from are read back and drawn. A top level opens in a tab of its own with its instances,
  the tree of bottom levels and geometries under it, and where their boxes overlap
  ([Reports](REPORTS.md)); a structure built before the capture is read back as the capture starts,
  so a bottom level an engine built at load is still legible. An acceleration structure pass is
  timed like any other, so the frame's build cost is in the pass list.

  **Ray queries are in the shader debugger.** Metal has no hit shaders — a kernel holds an
  `intersector` and calls `intersect` — so the one line a traced frame is about is inside the
  shader, and the debugger follows it: the traversal runs on the CPU over the geometry the capture
  read back, and `intersection_result` comes back with the instance, the geometry, the primitive,
  the barycentrics and the distance. For a *procedural* geometry it goes further and calls the
  shader's own intersection function, stepping into it like any other call — which is the only way
  to answer "why is this sphere not hit", since a bounding box says nothing about what is in it.

  A ray tracing frame replays and exports: `mtlinsp_replay` re-creates the structures, re-runs the
  builds and refits, fills the intersection function tables by function name, and compares the
  traced storage texture as well as the render targets
  ([Capture replay](REPLAY.md#metal)). This is less work on Metal than on either other API, because
  a geometry descriptor names its buffers outright and a table entry names its function — where
  Vulkan and Direct3D 12 have device addresses and opaque identifiers to map back.

  Metal has no shader binding table and no hit shaders — a kernel traverses the scene itself — so
  in place of the other two backends' binding table view there is the **intersection function
  table**: its entries by index with the function each one holds, the buffers it binds for them,
  and the pipeline's linked functions. Because the entries are set through the API rather than
  written into GPU memory, the capture knows them exactly, and a geometry naming an entry that is
  not there is reported as a frame issue — something Metal itself does not check.
- **Where the CPU went, and the Timeline** — the calls the capture library times, so a Metal frame
  can be called CPU- or GPU-bound and the threads and the passes can be drawn on one axis. Metal's
  categories are the shortest of the three backends: `commit` is the submit, `waitUntilCompleted`
  and `waitUntilScheduled` are waiting for the GPU, and `CAMetalLayer`'s `nextDrawable` is waiting
  for the display, which is where a display-paced Metal frame spends its time. There is deliberately
  no present span — `presentDrawable:` only schedules and returns at once, so timing it would record
  a call that never waits. `sampleTimestamps:gpuTimestamp:` relates the two clocks, which is what
  puts the passes on the same axis as the calls that committed them.
- **Memory over time** — the device's own `currentAllocatedSize` against its
  `recommendedMaxWorkingSetSize`, sampled each frame. See
  [Memory](PROFILING.md#memory-how-much-from-which-heap-and-which-way-it-is-going).
- **Memory by what is holding it** — Metal has no heap table to enumerate and no residency figure
  separate from `currentAllocatedSize`, so the Memory Use section cannot show the per-heap
  breakdown the other two backends have. What Metal does give is every resource's `allocatedSize`,
  so the breakdown is by object kind instead: buffers, textures, and the heaps themselves. A
  resource created *from* a heap is left out of the total and reported apart from it — its bytes
  are part of the heap's reservation, and counting both would count them twice — and a heap using
  much less than it reserved is called out, since that is memory the process has taken and is not
  using. The total is what the inspector has seen created, which is less than the device's own
  figure in the series: that includes whatever the driver allocated behind the objects.

## Metal-only capture options

Two controls appear in the capture bar on macOS:

| Control | What it does |
|---|---|
| **Overdraw** | Draws every render pass a second time with a counting fragment shader, so the capture records how many fragments landed on each pixel, with and without the depth and stencil tests. Costs GPU and CPU time in the captured frame. See [Overdraw](REPORTS.md#overdraw) |
| **Xcode Trace** | Writes the next frame as a `.gputrace` document to open in Xcode's Metal debugger, for shader debugging and per-line profiling. The path is printed in the **Log** tab |

## Measuring a draw inside the application

Vulkan answers "what did this draw do" by replaying the capture on this machine's GPU. Metal has no
replay that serves analyses, so the capture library measures inside the running application instead:
it keeps every call a render pass records, and draws the pass again — up to the draw in question,
then that draw on its own into a target of its own. This is the same machinery overdraw uses, with a
different fragment function and a different target.

What follows from that:

- **The application has to still be running.** A `.gpucap` opened later holds whatever was measured
  when it was taken, and nothing more.
- **Asking captures another frame.** Choosing an overlay, or following a pixel, records the
  application's *next* frame with the measurement in it, and opens that capture's own tab on the
  answer. The draw is named by its pass and its ordinal within it, not by its command index, because
  the new frame numbers its commands afresh.
- **One draw per capture.** A tab opened on a draw that was already measured does not ask again.
- **It costs the captured frame time**, as overdraw does: five extra runs of the pass for an
  overlay.

[Pixel history](REPORTS.md#pixel-history) and [draw overlays](REPORTS.md#draw-call-overlays) both
work this way. What a Metal pixel history covers, beyond a pass's ordinary draws:

| Also reported | How |
|---|---|
| A **multisampled** pass | The copies it draws into take the pass's sample count, and the two the pixel is read from resolve into single-sample copies first — nothing can be blitted out of a multisampled texture |
| A **layered** pass | The copies are arrays of the pass's `renderTargetArrayLength`, so a draw that picks a layer with `render_target_array_index` lands in the same one, and the pixel is read from the layer the request named |
| An **indirect command buffer**'s draws | Its commands are executed one at a time under a visibility result. Each carries its own pipeline, which the library never saw created and so cannot copy, so the event says whether the command wrote the pixel but not where its fragments stopped. The form whose range comes from a buffer is not followed |
| The pass's **multisample resolve** into the texture | One "resolve" event, read out of the resolve target once the pass has stored |
| A **blit** that writes the texture | One event per command, read back on the application's own blit encoder right behind the write |
| A **compute** encoder with the texture bound | One event per *encoder*, not per dispatch: a compute encoder cannot be interrupted to read a texture, so the pixel is read once the application closes it. As in a Vulkan replay, what the shader wrote is not knowable from outside it — the event says the texture was bound to be written and carries the value after |

Direct3D 12 does the same thing for the same reason
([Measuring draws, overlays and meshes](D3D12.md#measuring-draws-overlays-and-meshes)); the mesh
view is the exception, since the Shading Language interpreter can compute a draw's vertex outputs
without running anything on the GPU.

## What is not there yet

- `mtlinsp_replay` leaves curve and motion geometry out of **Export to C++** (it replays both), and
  does not set an opaque triangle intersection function — Metal's own rather than the application's,
  named by signature rather than by a function.
- PVRTC textures are not decoded (the format is mapped, but the UI has no decoder for it); every
  other pixel format Metal has is. It is an iOS format, so a macOS capture will not hold one.
- Only Apple Silicon has been verified.

## If it does not work

See [Troubleshooting](TROUBLESHOOTING.md#macos). Almost every "starts but never connects" is the
hardened runtime dropping `DYLD_INSERT_LIBRARIES`.

---

Previous: [Vulkan](VULKAN.md) · [Docs index](README.md) · Next: [Android and Quest](ANDROID.md)
