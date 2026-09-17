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
picked up with **Connect** (or `npm start -- --connect=<port>`):

| Variable | Value |
|---|---|
| `DYLD_INSERT_LIBRARIES` | the path to `libmtlinsp_capture.dylib` |
| `MTLINSP_PORT` | the port to connect on |
| `MTLINSP_LOG` | optional: `1` logs the intercepted calls to the session's **Log** tab |

## What works

- **Object inspection** — the device, command queues, buffers, textures, libraries, and render and
  compute pipeline states, each with the call that created it, its arguments and its label.
  Clicking a texture reads its pixels back live. A library lists its function names, and shows the
  Metal Shading Language it was compiled from when it was compiled on the spot rather than loaded
  as a precompiled `metallib`.
- **Frame capture** — the frame's commands grouped by command buffer and pass, each draw with the
  pipeline that was bound for it, its decoded vertex and index buffers, and the pass's read-back
  colour attachments. Captures save to the same `.gpucap` files and reopen on any platform.
- **Validation** — ticking **Validation layer** in the launch dialog enables Metal's API and
  shader validation in the mode that logs a failure instead of aborting, and the messages are
  listed in the Inspect tab.
- **Bottleneck counters** — Metal's counter sets measure overdraw, fragments per primitive and
  depth rejection per pass. See [Finding GPU bottlenecks](PROFILING.md).
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

- Depth attachments and sampled images are not read back.
- Only the pixel formats `src/metal/src/formats.h` maps are decoded — no ASTC, ETC or PVRTC.
- No creation stack traces.
- Shader editing does not apply: it is built around SPIR-V and its compilers, and Metal's shaders
  are already source.
- Only Apple Silicon has been verified.

## If it does not work

See [Troubleshooting](TROUBLESHOOTING.md#macos). Almost every "starts but never connects" is the
hardened runtime dropping `DYLD_INSERT_LIBRARIES`.

---

Previous: [Vulkan](VULKAN.md) · [Docs index](README.md) · Next: [Android and Quest](ANDROID.md)
