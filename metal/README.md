# Metal capture

A first cut at capturing Metal, the way `layer/` captures Vulkan. It discovers and hooks the
Metal class tree, tracks the objects an application creates, records the command stream of a
frame on request, and streams all of it to the inspector over the same protocol the Vulkan layer
speaks. The Inspect and Capture panels both work against a Metal application today. Resource
read-back — render targets and buffer contents — is not written yet.

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
buffers, a library compiled at run time, a render pipeline, a compute pass, a render pass with an
indexed instanced draw, and a present.

## Getting in

Metal has no loader and no layer mechanism — nothing like the Vulkan loader's manifests and
dispatch chaining, and no supported extension point at all. The library is loaded by
`DYLD_INSERT_LIBRARIES` and takes the only chokepoints the API has: the C functions that hand out
a device (`MTLCreateSystemDefaultDevice`, `MTLCopyAllDevices`), interposed through a
`__DATA,__interpose` section. Everything below those is an Objective-C protocol method.

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
would not survive that.

## Two things this caught

Both were crashes or wrong data before they were fixed, and both are properties of Metal rather
than of this code, so any Metal capture library will meet them.

**Metal wraps its own objects, so one call is seen several times.** With the validation layers on
the application holds an `MTLDebugCommandBuffer` that wraps an `MTLGPUDebugCommandBuffer` that
wraps the driver's. Each forwards to the next, every level is hooked, and one `presentDrawable:`
from the application arrived three times — the frame counter advanced three times a frame and the
command counts landed on whichever observation drained them. `Reentry` in `swizzle.h` records
only the outermost call, which is the application's; the rest are Metal talking to itself. Every
level still forwards, so behaviour is unchanged.

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

Verified after all of it: three frames, `2 encoders, 1 draw, 1 dispatch` each, clean exit, no
`no original` warnings, in all three validation modes.

## What this says about the real thing

Encouraging: one interposed entry point reaches the entire tree; the frame boundary
(`presentDrawable:`) is unambiguous where Vulkan needed heuristics for OpenXR; Metal has a real
compute encoder, so the "runs of dispatches are a compute pass" rule in `capture.cpp` has no
counterpart to write; load and store actions are per-attachment and mutable, so forcing a store
during a capture replaces the whole `StoreAllRenderPass` machinery; and on Apple Silicon a shared
buffer can often be read with no copy at all.

Expensive: there is no `vk.xml`. The Vulkan layer is ~8,000 lines of which most is generated —
dispatch tables, forwarders, serializers — and none of that generator transfers. Metal also has
many overloads per operation (`drawPrimitives:` alone has several selectors, each needing its own
hook), and this macOS ships Metal 4 alongside Metal 3, which is a second API surface to decide
about.

## Talking to the UI

`transport.mm` is the Vulkan layer's wire format byte for byte, and `tracker.mm` emits the same
`AddObject` / `ObjectSetLabel` messages, so the UI needed no change at all to display Metal: it
groups the objects by type, links each to its owner, and renders the descriptor. That is the
API-neutral protocol claim in `docs/ARCHITECTURE.md` actually being tested for the first time.

What is tracked: the device, command queues, buffers, textures, libraries, and render and compute
pipeline states, each with the selector that created it and a hand-written descriptor. Command
buffers and encoders are deliberately not tracked as objects — a Metal command buffer lives for
one frame, so the object list would grow without bound; they belong to frame capture instead.

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
`CaptureFrameCommands` go out when the frame ends. Each command carries its selector, its
arguments, and `{"__id", "__class"}` references to the tracked objects it names, so the UI
resolves a bound buffer or pipeline to the object it already knows.

### Frame boundaries

There are two, and an engine may use either.

`[MTLCommandBuffer presentDrawable:]` is the documented convenience, and its boundary is the
`commit` that follows it, not the call itself. Metal presents by asking a command buffer
to, partway through encoding it, with the commit after — so arming at `presentDrawable:` starts
the recording mid-command-buffer and its first command is the *previous* frame's `commit`. Vulkan
has no such problem, since vkQueuePresentKHR is a queue operation that follows the submission.
`presentDrawable:` only marks the command buffer; its commit is the boundary.

`[MTLDrawable present]` is the other, and Unity's macOS player uses it: it presents the drawable
itself from a `addScheduledHandler:` block rather than through the command buffer, deliberately,
to avoid the frame pacing the convenience method imposes
(`PlatformDependent/OSX/MetalSurfaceHelper.mm`, case 1378985). Hooking only `presentDrawable:`
means never seeing a frame boundary in a Unity player, so the capture never arms and nothing at
all is recorded — which presents as "capture is broken" rather than "one selector is unhooked".
Unity uses the convenience method on another path in the same file, so both have to work.

The convenience method calls the drawable's own `present` internally, and does so *later and on
another thread*, so the re-entry guard cannot pair them: without an explicit record of which
drawables a command buffer was asked to present, both boundaries fire and every frame is counted
twice.

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

`"Profile passes: waiting for GPU timestamps..."` still waits forever, because no
`CapturePassTimings` is sent yet.

### Argument shapes, not only method names

Classifying commands by name gets the command tree, the pass grouping, the draw counts and the
bound pipeline. The panels that show a command's *contents* go a level deeper and read argument
names: `pBuffers`/`firstBinding`/`pOffsets` for Vulkan against `buffer`/`index`/`offset` for
Metal. So `CommandSets` carries accessors as well as sets — `vertexBuffersOf(cmd)` and
`indexBufferOf(cmd)` beside `pipelineBindPointOf` — and each returns the same neutral
`BoundVertexBuffer` / `BoundIndexBuffer` whatever the arguments were called.

The accessor is also where genuine structural differences go, not just naming ones. Vulkan binds a
range of vertex bindings with one command carrying parallel arrays; Metal binds one per call.
Vulkan binds an index buffer with its own command; Metal has no such command and names the index
buffer in the indexed draw, so `indexBufferOf` answers on the draw there and on the binding
command in Vulkan. Callers ask any command and rely on null.

One more constant needed the same treatment: the panel decided whether to show vertex and index
buffers by comparing the bind point against the literal `"VK_PIPELINE_BIND_POINT_GRAPHICS"`, so
`graphicsBindPoint` is part of the table too.

## Buffer read-back

Bound vertex buffers and an indexed draw's index buffer are read back with the capture and sent as
`CaptureBuffers` plus a `CaptureBufferData` binary frame each, which is what the UI shows under a
draw as "Vertex Buffer 0: vertices" and "Index Buffer: indices".

Much cheaper than the Vulkan layer's equivalent. That records a GPU copy into staging memory and
maps it after the frame; a Metal buffer in a shared or managed storage mode is mapped into the
process the whole time, so reading it is a `memcpy` at record time with no GPU work at all — which
is what unified memory buys. A private-storage buffer has no such pointer and is reported with an
error rather than blitted; that is the case that would need the Vulkan approach.

Ranges are capped at 64 KB, matching the layer's default, and the cap is reported as
`originalSize` so the UI can say a range was truncated.

## Render target read-back

A pass's colour attachments are blitted into staging buffers when the application ends its
encoder, and sent as `CaptureTextureFrames` plus a `CaptureTextureData` binary frame each. The
Capture panel shows the frame the application actually drew.

Three things have to be arranged for it, each the Metal counterpart of something the Vulkan layer
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
  `A command encoder is already encoding to this command buffer`. The encoder-to-command-buffer
  map is built when the encoder is created.

The capture is then sent from the command buffer's completion handler rather than at commit: the
staging holds nothing until the GPU has run the blits. `Internal` in `capture.h` keeps the
library's own Metal calls out of the recording, which would otherwise contain the commands the
capture made.

Pixel formats are named the way the protocol names them, which is Vulkan's way — the UI's decoder
is 570 lines built around those names and emitting the canonical name for the same layout reuses
all of it. The visible cost is that a Metal texture's format reads as `VK_FORMAT_B8G8R8A8_UNORM`
in the UI. A deliberate shortcut, and the obvious thing to revisit.

## Not done

Pass timings, which would fill in the profile view and clear the
`"Profile passes: waiting for GPU timestamps..."` the panel still sits in. Depth attachments are
not read back (colour only), nor are sampled images, and only the pixel formats in `FormatName`
are supported. Those are the rest of what `layer/src/capture.cpp` does. Also `DeleteObjects`
(nothing watches for released objects yet), blit and argument-buffer coverage,
`MTLIndirectCommandBuffer`, `MTKView`/`CAMetalLayer` paths other than the one the test
application uses, Intel and AMD class trees (only Apple Silicon is verified), re-signing a
hardened target as part of the launch flow, and launching a target from the UI at all — today
the application is started by hand and the UI attaches with `--connect`.

`transport.mm` also duplicates `layer/src/transport.cpp`; see the note at the top of
`transport.h` for why they are not one file yet.
