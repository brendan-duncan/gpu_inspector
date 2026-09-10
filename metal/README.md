# Metal capture

Capturing Metal, the way `layer/` captures Vulkan. It discovers and hooks the Metal class tree,
tracks the objects an application creates and releases, records the command stream of a frame on
request with its render targets, bound buffers and GPU pass timings, and streams all of it to the
inspector over the same protocol the Vulkan layer speaks. The Inspect and Capture panels both
work against a Metal application.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
cd build/bin
MTLINSP_LOG=1 DYLD_INSERT_LIBRARIES=./libmtlinsp_capture.dylib ./mtlinsp_triangle --frames 3
```

From the UI, which launches the target itself: **Launch...** → **This computer**, and choose the
`.app`. Or from the command line, which is what the tests use:

```
cd app && npm start -- --launch=/path/to/Player.app --launch-args="-screen-width 800"
```

`--connect=<port>` still attaches to an application started by hand, for anything the launch path
does not cover. `MTLINSP_LOG=1` logs the intercepted calls to the session's Log tab,
`MTLINSP_PORT` moves the listener off 47531.

## Launching

`app/src/main/metal.ts` handles the two things that make this different from registering a Vulkan
layer:

* **A `.app` is a directory.** dyld needs the binary inside it, named by `CFBundleExecutable` in
  the bundle's `Info.plist`, falling back to the bundle's own name — which is what Unity produces.
  The session and the recent list keep showing the `.app` the user picked.
* **The hardened runtime silently wins.** dyld drops `DYLD_*` for such a process unless it carries
  `com.apple.security.cs.allow-dyld-environment-variables` and
  `com.apple.security.cs.disable-library-validation`. That is checked with `codesign` *before*
  spawning, because otherwise the target starts perfectly and simply never connects, which is a
  far worse thing to debug than a message. The message says how to re-sign.

Locally built players are normally ad-hoc signed without the hardened runtime and need none of
that. Re-signing is deliberately not done for the user: it rewrites their application bundle and
invalidates its signature and notarization.

`test/metal_triangle` is the target, the Metal counterpart of `test/triangle`: a device, a queue,
buffers in shared and private storage, a library compiled at run time, two render pipelines and
a sampler, a compute pass, a multisampled pass through a parallel encoder resolving into a
texture, a pass to the drawable that samples it with inline constants bound, and a present. That
is one of everything the read-back has a path for.

## Getting in

Metal has no loader and no layer mechanism — nothing like the Vulkan loader's manifests and
dispatch chaining, and no supported extension point at all. The library is loaded by
`DYLD_INSERT_LIBRARIES` and takes the only chokepoints the API has: the C functions that hand out
a device (`MTLCreateSystemDefaultDevice`, `MTLCopyAllDevices`, `MTLCopyAllDevicesWithObserver`,
`CGDirectDisplayCopyCurrentMetalDevice`), interposed through a `__DATA,__interpose` section.
Everything below those is an Objective-C protocol method.

An application can also get its device without calling any of them — a `CAMetalLayer`'s
`preferredDevice`, an `MTKView`'s default — and then nothing would ever be hooked. So the
`nextDrawable` hook registers the layer's device too, since every device that draws to a window
passes through it. RenderDoc registers the layer's device at the same spot.

**Code signing decides whether this is possible at all**, measured on macOS 15 / Apple Silicon
with a dylib whose constructor logs:

| Target signing | Injected |
|---|---|
| ad-hoc signed (a Unity development build's default) | yes |
| hardened runtime, no entitlements (any notarized application) | **no**, the variable is dropped silently |
| hardened runtime + `allow-dyld-environment-variables` + `disable-library-validation` | yes |

So "works with any uninstrumented application", which the Vulkan layer can claim on Windows and
Linux, does not carry over. What carries over is "works with any application you can re-sign",
`codesign --force --sign - --entitlements …` with those two keys, which covers the Unity players
that are the target. It invalidates notarization, so it is something to do to a development
build, not to a shipping application. The same shape as the Android requirement that the
application be debuggable.

`test/metal_triangle` is built as a plain executable for this reason: ad-hoc signed, no hardened
runtime, injectable as-is.

## The class tree

Metal's API is protocols. The objects behind them are concrete classes the driver owns, they are
not exported by Metal.framework (of its 318 exported Objective-C classes, all are descriptors),
and they differ per GPU. So they are discovered at run time: the interposed entry point hooks the
class of the device it returns, and every hook that returns another Metal object hooks the class
of that, once. No private class is ever named in the source.

What that finds on an M1 Max — and how much it changes with Metal's own validation layers on,
which is the argument against hard-coding any of it:

| | plain | `MTL_DEBUG_LAYER=1` | `+ MTL_SHADER_VALIDATION=1` |
|---|---|---|---|
| device | `AGXG13XDevice` | `MTLDebugDevice` | `MTLDebugDevice` |
| queue | `AGXG13XFamilyCommandQueue` | `MTLDebugCommandQueue` | `MTLDebugCommandQueue` |
| command buffer | `AGXG13XFamilyCommandBuffer` | `MTLDebugCommandBuffer` | `MTLDebugCommandBuffer` wrapping `MTLGPUDebugCommandBuffer` |
| render encoder | `AGXG13XFamilyRenderContext` | `MTLDebugRenderCommandEncoder` | as debug |
| compute encoder | `AGXG13XFamilyComputeContext` | `MTLDebugComputeCommandEncoder` | as debug |
| library | `_MTLLibrary` | `MTLDebugLibrary` | as debug |

The objects are never wrapped, only their classes hooked. That is the same decision the Vulkan
layer makes for handles (`docs/ARCHITECTURE.md`, "Handles: pass-through") and it matters more
here: CoreAnimation and MetalKit inspect the objects they are handed, and a proxy class of ours
would not survive that. RenderDoc's Metal driver wraps, and has to add Metal's private
`MTLTextureImplementation` protocol to its proxy so that the driver's own assert passes — the
problem, met.

The hooks are in four files by the class they intercept: `hooks_device.mm` for the device and
what it creates (heaps, libraries, textures and buffers included, since each of those creates
objects of its own), `hooks_command_buffer.mm` for queues, command buffers and the parallel
render encoder, `hooks_encoders.mm` for the render, compute and blit encoders, and
`hooks_descriptors.mm` for the JSON a descriptor becomes. RenderDoc's `*_bridge.mm` files
enumerate every method of each protocol and were the checklist; about two hundred selectors are
hooked.

## Three things this caught

All three were crashes or wrong data before they were fixed, and all are properties of Metal
rather than of this code, so any Metal capture library will meet them.

**Metal wraps its own objects, so one call is seen several times.** With the validation layers on
the application holds an `MTLDebugCommandBuffer` that wraps an `MTLGPUDebugCommandBuffer` that
wraps the driver's. Each forwards to the next, every level is hooked, and one `presentDrawable:`
from the application arrived three times — the frame counter advanced three times a frame and the
command counts landed on whichever observation drained them. `Reentry` in `swizzle.h` records
only the outermost call, which is the application's; the rest are Metal talking to itself. Every
level still forwards, so behaviour is unchanged. The library's own Metal calls — read-back blits,
staging buffers — are issued from inside a hook and so are nested by construction; `Internal`
marks the ones issued from elsewhere (the image read-back on the transport thread) so that
neither the tracker nor the capture takes them for the application's.

**A hooked implementation can belong to an ancestor shared with sibling classes.**
`class_getInstanceMethod` walks up, so hooking "the render encoder's `endEncoding`" may really be
rewriting an implementation on a base class that the compute encoder and the command buffer
inherit too. Metal's class trees are shaped that way as a rule, not as a corner case.

This took two attempts, and the first one is worth recording because it looks right. Keying the
saved originals by `Method` rather than `(class, selector)` fixes the sibling that finds nothing
and jumps through a null pointer — `SIGSEGV` under `MTL_SHADER_VALIDATION=1`. But it still leaves
the ancestor's implementation rewritten for every class that inherits it, and a subclass with its
own override that calls `super` then resolves to the override's `Method`, which is not the one
that was replaced: the same null, from a different direction. That is what
`no original for setLabel: on AGXG13XFamilyCommandBuffer` was.

The fix is not to touch the ancestor at all. `Hook` uses `class_addMethod` to install an override
on the named class alone, and the implementation it shadows — what calls used to reach — is the
original. When the class already has its own implementation there is no sharing to worry about
and it is replaced in place. Siblings are then unaffected by construction, and a per-class key
with a walk up from the receiver is sound again.

**Except for one shape: a hooked subclass whose override calls `super` into a hooked ancestor.**
Both levels run the same replacement, and resolving "the original" from the receiver's class
twice returns the subclass's override twice, forever. So `Reentry` keeps a per-thread stack of
the calls in progress, and a nested observation of the same selector on the same object resolves
from above where the outer one resolved. The table of originals is an immutable snapshot swapped
by `Hook` and read without a lock, because every intercepted draw and bind reads it — the
previous mutex was the most contended thing in the library. Only Apple Silicon's class tree has
been checked for the super-chain shape; Intel and AMD trees are where it would show up.

Verified after all of it: three frames, `2 encoders, 1 draw, 1 dispatch` each, clean exit, no
`no original` warnings, in all three validation modes. (With the multisampled pass and the
parallel encoder the test application now logs `3 encoders, 2 draws, 1 dispatch` a frame.)

## What this says about the real thing

Encouraging: one interposed entry point reaches the entire tree; the frame boundary is a present
where Vulkan needed heuristics for OpenXR; Metal has a real compute encoder, so the "runs of
dispatches are a compute pass" rule in `capture.cpp` has no counterpart to write; load and store
actions are per-attachment and mutable, so forcing a store during a capture replaces the whole
`StoreAllRenderPass` machinery; and on Apple Silicon a shared buffer can often be read with no
copy at all.

Expensive: there is no `vk.xml`. The Vulkan layer is ~8,000 lines of which most is generated —
dispatch tables, forwarders, serializers — and none of that generator transfers. Metal also has
many overloads per operation (`drawPrimitives:` alone has several selectors, each needing its own
hook), and this macOS ships Metal 4 alongside Metal 3, which is a second API surface to decide
about.

## Talking to the UI

`transport.mm` is the Vulkan layer's wire format byte for byte, and `tracker.mm` emits the same
`AddObject` / `DeleteObjects` / `ObjectSetLabel` messages, so the UI needed no change at all to
display Metal: it groups the objects by type, links each to its owner, and renders the
descriptor. That is the API-neutral protocol claim in `docs/ARCHITECTURE.md` actually being
tested for the first time.

What is tracked: the device, command queues, buffers, textures and texture views, heaps and what
is sub-allocated from them, libraries and functions, sampler and depth-stencil states, render and
compute pipeline states in every spelling of their creation (engines ask for reflection with the
pipeline, and build many asynchronously — an unhooked spelling means `setRenderPipelineState:`
resolves to null in every capture), fences, events, argument encoders and indirect command
buffers, each with the selector that created it and a hand-written descriptor. A pipeline's
descriptor carries its vertex layout, blend state and attachment formats, which is what makes a
captured vertex buffer decodable.

**Lifetime.** Every tracked class gets a `dealloc` hook, installed the same way as the others,
and an object's death is a `DeleteObjects`. That is what makes keying the side table by pointer
safe: without it a freed buffer's address, reused for the next one, answered with the old
object's id and the new object was never announced — drawable textures and per-frame temporaries
hit that constantly. It is also what keeps the table from growing for as long as a game runs.
RenderDoc takes the same signal from its wrapper's `dealloc`. Objects are otherwise held weakly,
so the tool never keeps a texture resident.

Command buffers and encoders live for a frame, so they are not tracked as objects — except that
while a capture is recording, a command buffer is announced the first time a command is recorded
against it and dropped through the `dealloc` hook once the application lets go. Every recorded
command names its command buffer in `object`, the way a Vulkan capture does, so the UI groups the
tree by it, numbers passes within it, and matches render targets and timings to it. The encoder
the command was issued on is named in `encoder` with an id from the same sequence, never
announced.

Both paths are exercised by the test application, which allocates a labelled buffer every second
on top of what it creates at start-up: objects created before the UI connects arrive in the
snapshot with their labels resolved, and objects created after it connects arrive as `AddObject`
followed by `ObjectSetLabel`, because Metal applications label a resource on the line after they
create it.

One cosmetic thing the UI gets wrong: it pluralizes type names by appending "s", so the object
tree says "MTLLibrarys". Vulkan type names never hit that case.

## Frame capture

`capture.mm` follows the Vulkan layer's model: the UI's `Capture` message arms the library, the
next frame's commands are recorded as they are encoded, and `CaptureFrameResults` plus batched
`CaptureFrameCommands` go out when the frame's command buffers have completed. Each command
carries its selector, its arguments, and `{"__id", "__class"}` references to the tracked objects
it names, so the UI resolves a bound buffer or pipeline to the object it already knows.

### Frame boundaries

There are two, and an engine may use either.

`[MTLCommandBuffer presentDrawable:]` is the documented convenience, and its boundary is the
`commit` that follows it, not the call itself. Metal presents by asking a command buffer to,
partway through encoding it, with the commit after — so arming at `presentDrawable:` starts the
recording mid-command-buffer and its first command is the *previous* frame's `commit`. Vulkan has
no such problem, since vkQueuePresentKHR is a queue operation that follows the submission.

`[MTLDrawable present]` is the other, and Unity's macOS player uses it: it presents the drawable
itself from a `addScheduledHandler:` block rather than through the command buffer, deliberately,
to avoid the frame pacing the convenience method imposes
(`PlatformDependent/OSX/MetalSurfaceHelper.mm`, case 1378985). Hooking only `presentDrawable:`
means never seeing a frame boundary in a Unity player, so the capture never arms and nothing at
all is recorded — which presents as "capture is broken" rather than "one selector is unhooked".
RenderDoc hooks only the convenience method and would miss such a player entirely.

The drawable's own present has a problem of its own: it arrives on Metal's scheduled-handler
thread, after the commit, by which time the next frame may already be encoding. A capture armed
or ended there starts and stops partway into a frame. So once an application has been seen to
present that way, the boundary is taken earlier and on the encoding thread instead: at the commit
of the command buffer that rendered into the drawable's texture, which is the one whose handler
will present it. `nextDrawable` remembers which drawable owns which texture, the render-encoder
hook notes which command buffer draws into one, and `commit` decides. The present itself then
only confirms a frame already counted, and a `present` marker is recorded before the `commit` so
both paths read the same in the UI. The convenience method also calls the drawable's own present
internally, later and on another thread, so the re-entry guard cannot pair them; the drawables a
command buffer was asked to present are remembered, by pointer and by `drawableID`, so that call
is recognised and not counted twice. An engine that draws into the drawable from two command
buffers before presenting would have the second land in the next frame; none of the targets do.

**`addCompletedHandler:` is only legal before a command buffer is committed.** The read-back
needs a completion to know the staging holds pixels, and the obvious place to ask for one — the
frame boundary — is too late on the drawable path, because that arrives from a scheduled handler
after the commit. Metal asserts and aborts the application. So the handler is registered in the
commit hook, before the commit is forwarded, and the capture is sent when the last command buffer
of the frames completes. `test/metal_triangle --present-direct` renders the way Unity does, which
is the only way this path gets tested.

The UI classifies commands — which are draws, which open a pass, which bind a pipeline — by
matching method names, and those were Vulkan's. It now picks a table per capture instead:
`app/src/renderer/command_sets.ts` defines the interface, `vulkan/command_sets.ts` and
`metal/command_sets.ts` fill it in, and `CaptureData.sets` selects by the `api` the library
reports in `CaptureFrameResults` (or that a `.gpucap` recorded — the file format already had the
field, hard-coded to `"vulkan"`). With that, a Metal capture groups into passes, counts its draws
and dispatches, and resolves the pipeline bound at each draw.

### Argument shapes, not only method names

Classifying commands by name gets the command tree, the pass grouping, the draw counts and the
bound pipeline. The panels that show a command's *contents* go a level deeper and read argument
names: `pBuffers`/`firstBinding`/`pOffsets` for Vulkan against `buffer`/`index`/`offset` for
Metal. So `CommandSets` carries accessors as well as sets — `vertexBuffersOf(cmd)` and
`indexBufferOf(cmd)` beside `pipelineBindPointOf` — and each returns the same neutral
`BoundVertexBuffer` / `BoundIndexBuffer` whatever the arguments were called.

The accessor is also where genuine structural differences go, not just naming ones. Vulkan binds a
range of vertex bindings with one command carrying parallel arrays; Metal usually binds one per
call, and `setVertexBuffers:offsets:withRange:` when it binds a range. Vulkan binds an index
buffer with its own command; Metal has no such command and names the index buffer in the indexed
draw, so `indexBufferOf` answers on the draw there and on the binding command in Vulkan. Callers
ask any command and rely on null. The vertex layout comes from the pipeline in both APIs, from
`pVertexInputState` in one and `vertexDescriptor` in the other; each attribute of the latter
carries the protocol's format name beside Metal's, so the decoder needs no table of its own.

One more constant needed the same treatment: the panel decided whether to show vertex and index
buffers by comparing the bind point against the literal `"VK_PIPELINE_BIND_POINT_GRAPHICS"`, so
`graphicsBindPoint` is part of the table too.

## Buffer read-back

Bound buffers — vertex, fragment, compute, tessellation-factor, indirect-argument — and an
indexed draw's index buffer are read back with the capture and sent as `CaptureBuffers` plus a
`CaptureBufferData` binary frame each, which is what the UI shows under a draw as "Vertex Buffer
0: vertices" and "Index Buffer: indices". Inline constant blocks (`setVertexBytes:` and the rest)
are recorded the way the UI reads push constants, with the bytes inline.

Three storage modes, three ways to read:

* **Shared** is mapped into the process the whole time, so it is a `memcpy` at bind time with no
  GPU work at all — which is what unified memory buys, and much cheaper than the Vulkan layer's
  equivalent.
* **Managed** has a CPU copy that lags a GPU write, so `synchronizeResource:` is encoded at the
  end of the pass and the bytes are read once the frame completes. RenderDoc does the same.
* **Private** has no CPU copy at all. It is blitted into a staging buffer at the end of the pass,
  the way the Vulkan layer reads every buffer. This is the mode that matters: Unity keeps its
  vertex and index buffers private on macOS, so without it the primary target had nothing to
  show.

The blits go at `endEncoding`, beside the render targets', because a command buffer allows one
encoder at a time. A range bound at every draw — a uniform block — is read once per capture, keyed
by buffer, offset and size; ranges are capped at 64 KB, matching the layer's default, and the cap
is reported as `originalSize` so the UI can say a range was truncated.

## Render target read-back

A pass's colour and depth attachments are blitted into staging buffers when the application ends
its encoder, and sent as `CaptureTextureFrames` plus a `CaptureTextureData` binary frame each.
The Capture panel shows the frame the application actually drew.

Things that have to be arranged for it, each the Metal counterpart of something the Vulkan layer
does:

* **The attachment has to survive the pass.** `storeAction` `DontCare` leaves it undefined, so
  during a capture the store is forced on. Far less machinery than the layer's store-everything
  render pass, because a store action is a mutable field — but the application's own descriptor is
  copied first rather than written to, since it owns and reuses that object.
* **The drawable has to be readable.** A `framebufferOnly` `CAMetalLayer` hands out textures that
  cannot be a blit source, and the flag has to be off before the drawable exists. So it is turned
  off in the `nextDrawable` hook, always — the counterpart of the layer adding `TRANSFER_SRC` to
  every image, a small cost paid all the time so that a capture can be taken at any moment.
* **The blit needs the command buffer, and it has to be free.** A command buffer allows one
  encoder at a time, so the read-back's blit encoder can only be created *after* the hook forwards
  the application's `endEncoding` — doing it before raises
  `A command encoder is already encoding to this command buffer`. For a parallel render encoder
  that is the parallel encoder's own end, not a sub-encoder's.
* **Not everything can be a blit source.** A multisample texture cannot be copied to a buffer, so
  a multisample attachment is read through its resolve texture when the pass resolves, and
  reported as unreadable when it does not; a memoryless attachment has no contents after the
  pass; a `framebufferOnly` drawable seen before the layer hook was in place cannot be copied
  either. Each of these was a Metal validation failure, which aborts the application rather than
  failing the read, so they are checked first. What cannot be read is reported with the reason,
  because an empty Render Targets section with no reason given is the hardest kind of gap to
  notice. The attachment's `level`, `slice` and `depthPlane` are honoured rather than assumed
  zero.

The capture is then sent from the command buffer's completion handler rather than at commit: the
staging holds nothing until the GPU has run the blits.

See "Pixel formats" below for how a format is named and which ones can be read back.

## Frame timing and capture options

`frame_stats.mm` is the Vulkan layer's frame report: at every frame boundary the interval since
the last is accumulated, and ten times a second a `FrameStats` message carries the average,
shortest and longest frame, the CPU time spent in `commit` (the counterpart of the layer's
vkQueueSubmit time), and the refresh period. That last one comes from the display when
CoreGraphics or AppKit will say — the built-in panel of an Apple Silicon laptop answers only
through `NSScreen`, which is asked through the runtime so that AppKit is not linked into an
application that may not have it — and from the layer's interval estimate otherwise, verbatim.
A `CAMetalLayer` with display sync off is Vulkan's immediate mode, and no refresh is reported
for it. The frame counter the report carries is the one a queued capture names.

The `Capture` message's options are read the way the layer reads them: `atFrame` waits for
that frame (a frame already passed captures the next), `maxBufferSize` truncates a range,
`maxBufferTotal` is the per-capture budget past which further ranges are reported as over it,
`maxTextureSize` skips an attachment larger than it, and `captureTextures`, `captureBuffers`
and `profilePasses` switch the read-backs and the timestamps off. Sampled images and creation
stack traces have no Metal counterpart yet, so those switches are ignored.

## Stack traces

With the launch dialog's "Stack traces" on, the tracker takes the return addresses at every
object creation (`stacktrace.mm`, `backtrace`, cheap), and a capture with the capture bar's
switch on takes them at every recorded command. Symbols are looked up only when the UI asks
(`RequestStacktraces` for objects, `RequestSymbols` for a command's addresses), through `dladdr`:
an exported symbol, demangled, when one is within reach, else the module and offset, which is
what an engine's own frames come to in a stripped build. Frames inside this library, Metal, its
driver and the Objective-C runtime are marked internal, so the application's call is the first
frame shown, the way the Vulkan loader's frames are hidden. The host-side symbolizer the Android
path uses reads ELF objects, so a stripped Mach-O module stays at module and offset.

## Validation messages and the leak report

Vulkan has a debug messenger; Metal has three things that say the same kinds of things, and
`validation.mm` turns each into the UI's `ValidationMessage`, deduplicated by text with repeat
counts sent at frame end, so the Inspect panel's message list and the session bar's counter work
unchanged:

* **A command buffer's error**, read in a completed handler the library adds to every command
  buffer while a client is connected. Command buffers are then made with encoder execution
  status on — the plain `commandBuffer` forms are opened through the descriptor form, the way
  compute encoders are for timing — so a GPU fault names the encoder it happened in and the
  debug signposts before it. Apple documents a small cost to the option, which is why it is only
  paid with someone watching.
* **Metal's own validation layer.** `MTL_DEBUG_LAYER=1` aborts on the first error by default;
  `MTL_DEBUG_LAYER_ERROR_MODE=nslog` logs instead, through NSLog, which the library interposes
  for every image but itself. A line naming one of the layer's classes or its assertion form is
  forwarded, with the method it names as the message's id the way a VUID is; the application's
  own NSLog calls pass through untouched. The launch dialog's "Validation layer" sets those
  variables, plus `MTL_SHADER_VALIDATION=1`, for the target; a variable already set wins.
* **Shader logs** (`MTLLogContainer` on a completed command buffer), as info messages.

At process exit the tracker sends the Vulkan layer's `LeakReport` for whatever the application
never released — everything but the device, the queues and command buffers — and the transport
is flushed synchronously, since the sender thread would not get another turn. Vulkan sends its
at device destruction; a Metal application has nothing to destroy, so exit is the moment.

## Frame Issues

Frame Stats' Frame Issues card runs a rule set over the captured commands; the Vulkan rules are
keyed by Vulkan names, so a Metal capture gets its own (`app/src/renderer/metal/frame_analysis.ts`),
chosen by the capture's `api`. They are the same findings where the two APIs have the same
mistakes — a first-use load, a store nothing reads, a multisampled attachment stored rather than
resolved, redundant binds, tiny draws — read straight off the pass descriptor's load and store
actions, plus what Xcode's Insights flag that a tile-based GPU cares about most: a texture only
ever cleared and discarded or resolved, which could be `MTLStorageModeMemoryless` and never touch
memory at all, and two back-to-back passes on the same target where the second loads what the
first stored, which one render encoder would have kept in tile memory. A load of an attachment
whose previous pass did not store it is reported as undefined contents, the one correctness
rule in the set.

## An Xcode trace of the frame

Xcode's shader debugger and per-line shader profiler cannot be reproduced outside Apple's
tooling; what can be done is to hand them the frame the inspector is looking at. The capture
bar's **Xcode Trace** button asks the library (`gpu_trace.mm`) to have `MTLCaptureManager` write
the next frame as a `.gputrace` document, started and stopped at the frame boundary so it holds
exactly one frame, beside the Desktop by default; the Log tab says where. Metal only allows that
for a process started with `METAL_CAPTURE_ENABLED=1`, which the launch path sets.

## Pass timings

The Metal counterpart of `vkCmdWriteTimestamp` is a counter sample buffer, `MTLCounterSampleBuffer`
over the device's timestamp counter set. A render pass samples into it at its stage boundaries
through `sampleBufferAttachments` on the pass descriptor — start of vertex work, end of fragment
work — and a compute or blit pass likewise through its own descriptor. That is one more reason the
descriptor is copied while recording; and since the plain `computeCommandEncoder` and
`blitCommandEncoder` have no descriptor to carry a sample buffer, while recording they are opened
through the descriptor forms with a descriptor that says the same thing, and the command is
recorded under the selector the application called. A GPU that cannot sample at stage boundaries
but can at draw, dispatch or blit boundaries takes the samples from the encoder instead
(`sampleCountersInBuffer:atSampleIndex:withBarrier:` at its beginning and end); Apple Silicon is
the former kind.

The samples are resolved once the frame's command buffers have completed, mapped to nanoseconds
with two CPU/GPU timestamp pairs taken around the capture, and sent as `CapturePassTimings` with
the same keys the Vulkan layer uses, so the Profile view and the frame statistics needed no
change. RenderDoc's Metal driver has no timing code.

A render pass samples all four stage boundaries, so beside the pass's span the vertex and
fragment stages' own spans are sent (`vertexMs`, `fragmentMs`) and shown in the pass header.
On a tile-based GPU the two overlap, so they can sum to more than the whole. Two more counter
sets ride on the same descriptor, each in a sample buffer of its own on the next free attachment
slot: the statistic set (vertex, fragment and kernel invocations, clipper counts) and the
stage-utilization set (cycles per stage), differenced over the pass and sent as `counters` and
`utilization`, which the pass header shows as a tooltip. That is Xcode's per-encoder counters
view. A GPU without a set leaves it out; the encoder-boundary fallback samples the statistic set
beside the timestamps and has no stage split.

## Reflection

What turns a captured buffer's bytes into named fields. The Vulkan side gets that from the
SPIR-V it parses in the UI; Metal has no SPIR-V, but a pipeline created with the argument-info
and buffer-type-info options comes back with an `MTLRenderPipelineReflection` that says the same
things: per stage, the buffers, textures and samplers by index, and for each buffer the struct
it points at, member by member, with offsets.

So every pipeline creation asks for it, whether or not the application did (`reflection.mm`,
and the pipeline hooks in `hooks_device.mm`). The forms without an options argument are redirected
to the form with one, through the hook for that selector, which is nested and so only forwards;
the forms with one get the reflection options added and, when the application passed no
reflection out-parameter, one of the library's own. The application sees exactly what it asked
for. Two generations of the API describe the reflection — `MTLArgument`, deprecated in macOS 13,
and the `MTLBinding` protocols that replaced it — with the same property names for everything
used, so it is read through selectors and works on either.

The layouts are written in the shape the UI's own reflection has (`ReflType` in
`spirv_reflect.ts`: scalar, vector, matrix, array, struct, opaque, with sizes and offsets), and
ride along in the pipeline's descriptor as `reflection`, keyed by stage. Metal matrices are
column-major with three-row columns padded to four elements, and `device float *data` becomes a
runtime-sized array of the pointee. On the UI side `metal/reflection.ts` reads that into the
same `ShaderResource` objects the Vulkan side builds, so a draw's stage buffers — everything
`setVertexBuffer:`, `setFragmentBuffer:`, the compute encoder's `setBuffer:` and the inline
`set*Bytes:` forms bound, minus the vertex-stage slots the vertex descriptor lays out, which are
vertex buffers — render as typed blocks with the Format editor, and a pipeline object in the
Inspect panel gets a Reflection section per stage.

## Argument buffers

Metal's bindless path: an argument buffer's bytes hold, for each member the shader declared, a
buffer's GPU address, a texture's or sampler's resource id, or an inline value. Two things make
them readable. The reflection marks each handle member with what it is (`metal: "pointer"`,
`"texture"`, `"sampler"`, a function table) and its eight bytes, with a nested argument buffer's
struct along as `element`; and every buffer, texture and sampler the tracker announces carries
its `gpuAddress` or `gpuResourceID`, as hex strings since they are 64-bit. A draw's bound
argument buffer is then shown member by member (`app/src/renderer/metal/argument_buffer.ts`):
a pointer resolves to the buffer whose range holds the address, with the offset into it, and a
texture or sampler to the object with that id, each a link. That is Xcode's argument buffer view.
Only Apple GPUs encode handles this way (Tier 2 argument buffers); a Tier 1 encoding is
driver-defined and shows as unresolved values.

## Memory

Xcode's memory viewer lists every resource with what Metal set aside for it. Here every buffer
and texture is announced with its `allocatedSize` (alignment and padding included, which is
why it can exceed a buffer's `length`), the heap it was sub-allocated from with its
`heapOffset`, and `aliasable`; a heap with its `size`, `usedSize` and `currentAllocatedSize`,
re-sent as an `ObjectUpdate` after each sub-allocation. `setPurgeableState:` and
`makeAliasable` are hooked on the resource classes and send updates too, and the tracker
replays the latest update per key after the object in a snapshot, so a UI that connects late
sees the same state. The frame stats report carries the device's `currentAllocatedSize` and
`recommendedMaxWorkingSetSize`, which is what the memory meter in Inspect shows for a Metal
session: the driver's total of the working set, then heaps, textures and buffers summed from
the objects (a texture view and a buffer-backed texture share their parent's storage and are
not counted), and the object's own view has a Memory row with the heap link and the state.
A volatile or empty resource still counts until Metal reclaims it, so the state is shown rather
than subtracted. Not tracked: a heap's usage going down when a sub-allocation is released
(the next sub-allocation refreshes it).

## Texture views

Clicking an `MTLTexture` in the Inspect panel reads it back live: the UI's `RequestImage` is
answered with `ImageData` and the pixels (`image.mm`). A blit into a staging buffer rather than
`-getBytes:`, because anything worth looking at — a render target above all — is in private
storage and has no contents the CPU can see. It runs on a command queue of the library's own, so
a read-back does not queue behind whatever the application has already scheduled, and waits for
completion on the transport's receiver thread. That thread is a plain `std::thread` with no
autorelease pool, so the message handler makes one; without it the command buffer, the encoder
and the texture leaked on every click.

The same guards as the render-target read-back apply — multisample, memoryless, `framebufferOnly`,
and the mip and layer being asked for having to exist — because Metal validation aborts the
application on any of them. A depth-stencil texture is read one aspect at a time, depth. The
queue and staging buffer the read-back makes are the library's own and are not announced to the
UI as the application's objects.

Objects are held **weakly**, which is what makes this safe to offer. Retaining every texture a
game creates would keep hundreds of megabytes of VRAM alive for as long as the inspector is
attached — the tool would change what it is measuring. A weak reference reads nil once the
application lets go, and "the application has released this texture" is the honest answer.

The Inspect panel needed two small things beside that: `MTLTexture` added to the types that get
an Image section, and a Metal branch where the panel reads a texture's shape, since Metal spells
those `mipmapLevelCount` and `arrayLength` where Vulkan has `mipLevels` and `arrayLayers`. The
object list orders both APIs device-first from one shared list — the type names are disjoint, so
no API detection is needed.

## Pixel formats

`formats.h` keeps two separate answers to "what format is this", because they are wanted for
different reasons:

* **Metal's own name** — `MTLPixelFormatBGRA8Unorm` — is what a descriptor shows in the Inspect
  panel. It is what the application wrote and what the documentation calls it; a bare `80` is not
  something anyone can act on. The same goes for `MTLTextureType2D`, `MTLStorageModePrivate`,
  `MTLVertexFormatFloat3` and the load and store actions.
  These are generated from the SDK's `MTLPixelFormat.h` and cover all 139 formats Metal has, so
  a format the read-back cannot handle still says what it is.
* **The protocol's name** — `VK_FORMAT_B8G8R8A8_UNORM` — travels with pixel data, because the
  UI's decoder is 570 lines built around those names and an identical memory layout can reuse all
  of it. 66 formats are mapped, including the BC family; the UI decodes BC1–BC5 and recognises
  BC6H and BC7. Vertex formats get the same pair, for the same reason.

Block-compressed formats are sized by block rather than by pixel, rounded up to whole blocks, so
a BC1 read-back asks for the right number of bytes and a row pitch the GPU accepts.

A combined depth-stencil texture cannot be copied to a buffer whole: the blit picks one aspect
with `MTLBlitOptionDepthFromDepthStencil`, and what lands in the buffer is that aspect alone —
four bytes of depth per pixel for `MTLPixelFormatDepth32Float_Stencil8`, not eight.
`DepthReadbackDetails` answers for that.

A format with no mapping is reported by name — `unsupported pixel format MTLPixelFormatASTC_4x4_LDR`
— rather than silently producing nothing. ASTC, ETC, PVRTC, the XR formats and the YUV formats are
in that group: Metal has them, Unity can produce them, and nothing here reads them yet.

## Library contents

Selecting an `MTLLibrary` shows what is in it. Two cases, and an engine uses both — Unity's
`GpuProgramsMetal.mm` has `CreateMTLLibraryFromSource` and `CreateMTLLibraryFromBinary` side by
side:

* **Compiled here** (`newLibraryWithSource:options:error:`, or its asynchronous form): the Metal
  Shading Language is attached verbatim as a blob and shown as text.
* **Loaded precompiled** (`newLibraryWithData:error:`, `newLibraryWithURL:error:`,
  `newLibraryWithFile:error:`, `newDefaultLibrary`): the metallib is AIR bitcode. Its bytes are
  attached so a capture or a bug report carries them, but nothing here disassembles it — that
  needs Apple's Metal tooling — so the panel says so rather than showing noise.

Either way the descriptor carries `functionNames`, read off the library itself. That is the part
that always works, and for a shipped metallib it is the only way to see what is inside without a
disassembler. The Inspect panel lists them in a Functions section, each linked to the
`MTLFunction` object once the application has made one from it, and the source is shown with
Metal Shading Language highlighting (`code_editor.ts` knows the language; the cross-compiled MSL
view of a Vulkan shader uses it too).

Names in the object list: a Metal object that has a name of its own — a function's entry point,
the device's GPU — is listed by it when the application gave it no label, rather than as a
number. A pipeline's descriptor names its functions as references to the tracked `MTLFunction`
objects, with the plain name beside each, so the pipeline's Dependencies section links to them
and a function's Dependents section lists the pipelines built from it.

In the capture tree a command shows the arguments worth reading beside its name, in the muted
style the Vulkan tree uses: a `setLabel:` or `pushDebugGroup:` its label, a draw its primitive
type and counts, a bind its slot and the object's name, a pass its first attachment, and for the
rest the scalars and references, a few of them (`summarize` in `metal/command_sets.ts`).

Deliberately not the Vulkan shader section, which is built around SPIR-V: reflection,
cross-compilation to GLSL and HLSL, and shader editing. MSL is already source, and none of those
apply to it.

## Not done

Stencil attachments are not read back (colour and depth are), nor are sampled images, and only
the pixel formats in `PixelFormatDetails` are supported. What an argument buffer points at is resolved one level
deep: the buffers it names are not themselves read back. Resource state and acceleration structure encoders are
recorded as passes without their commands. `MTLIndirectCommandBuffer` contents are not read.
Intel and AMD class trees are unverified (only Apple Silicon is), and so is the encoder-boundary
timing path those GPUs would take. Re-signing a hardened target is left to the user, on purpose.

`transport.mm` also duplicates `layer/src/transport.cpp`; see the note at the top of
`transport.h` for why they are not one file yet.
