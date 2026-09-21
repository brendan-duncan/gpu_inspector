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

An application the inspector cannot launch itself can be started with these variables set, then
picked up with the port box of **Attach...** on the main bar (or `npm start -- --connect=<port>`):

| Variable | Value |
|---|---|
| `DYLD_INSERT_LIBRARIES` | the path to `libmtlinsp_capture.dylib` |
| `MTLINSP_PORT` | the port to connect on |
| `MTLINSP_LOG` | optional: `1` logs the intercepted calls to the session's **Log** tab |

`MTLINSP_PORT` has to be set and has to be free: unlike the Vulkan and Direct3D 12 libraries,
this one neither steps to a free port nor answers `--list-targets`, so its port is typed rather
than read off a list, and probing it would take the connection from an attached inspector. The
two pieces that are missing are marked in `src/metal/src/transport.mm`.

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
  categories are the CPU timeline's, so a frame that stopped for a pipeline compile says so. What a
  Windows capture also gets and this does not is sampled call stacks: the sampler is Windows-only in
  all three backends.
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

On Metal, [pixel history](REPORTS.md#pixel-history) is measured by capturing another frame while
following the pixel, so it needs the application to still be running.

## What is not there yet

- `mtlinsp_replay` does not replay acceleration structure builds or traces yet, so **Export to
  C++** on a ray tracing frame leaves them out.
- Ray queries are not in the shader debugger: a kernel that traverses a scene can be stepped, but
  `intersector::intersect` is not followed into.
- Shader editing is not wired up. The usual reason given — that it is built around SPIR-V — is only
  half of it: a Metal capture holds the Shading Language the application compiled, so recompiling an
  edited copy is *easier* than on either of the other two. What is missing is the plumbing to swap
  the recompiled function into the pipeline and re-run the frame, which on Metal means rebuilding
  the pipeline state rather than patching a module.
- PVRTC textures are not decoded (the format is mapped, but the UI has no decoder for it); every
  other pixel format Metal has is. It is an iOS format, so a macOS capture will not hold one.
- No **CPU sampling**: a timing capture records where the frame's *calls* went, but does not sample
  the threads' call stacks. This is not a Metal gap as such — the sampler is Windows-only in all
  three backends, so a Vulkan capture on Linux lacks it too.
- Only Apple Silicon has been verified.

## If it does not work

See [Troubleshooting](TROUBLESHOOTING.md#macos). Almost every "starts but never connects" is the
hardened runtime dropping `DYLD_INSERT_LIBRARIES`.

---

Previous: [Vulkan](VULKAN.md) · [Docs index](README.md) · Next: [Android and Quest](ANDROID.md)
