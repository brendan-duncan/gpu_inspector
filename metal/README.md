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

With the UI, which attaches to the port the library listens on rather than launching anything:

```
(cd build/bin && DYLD_INSERT_LIBRARIES=./libmtlinsp_capture.dylib ./mtlinsp_triangle &)
cd app && npm start -- --connect=47531
```

`MTLINSP_LOG=1` logs the intercepted calls to stderr, `MTLINSP_PORT` moves the listener off
47531.

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

The frame boundary is `commit`, not `presentDrawable:`. Metal presents by asking a command buffer
to, partway through encoding it, with the commit after — so arming at `presentDrawable:` starts
the recording mid-command-buffer and its first command is the *previous* frame's `commit`. Vulkan
has no such problem, since vkQueuePresentKHR is a queue operation that follows the submission.
`presentDrawable:` only marks the command buffer; its commit is the boundary.

The UI classifies commands — which are draws, which open a pass, which bind a pipeline — by
matching method names, and those were Vulkan's. It now picks a table per capture instead:
`app/src/renderer/command_sets.ts` defines the interface, `vulkan/command_sets.ts` and
`metal/command_sets.ts` fill it in, and `CaptureData.sets` selects by the `api` the library
reports in `CaptureFrameResults` (or that a `.gpucap` recorded — the file format already had the
field, hard-coded to `"vulkan"`). With that, a Metal capture groups into passes, counts its draws
and dispatches, and resolves the pipeline bound at each draw.

`"Profile passes: waiting for GPU timestamps..."` still waits forever, because no
`CapturePassTimings` is sent yet.

## Not done

Resource read-back: render targets and buffer contents, and the pass timings that would fill in
the profile view. Those are the rest of what `layer/src/capture.cpp` does. Also `DeleteObjects`
(nothing watches for released objects yet), blit and argument-buffer coverage,
`MTLIndirectCommandBuffer`, `MTKView`/`CAMetalLayer` paths other than the one the test
application uses, Intel and AMD class trees (only Apple Silicon is verified), re-signing a
hardened target as part of the launch flow, and launching a target from the UI at all — today
the application is started by hand and the UI attaches with `--connect`.

`transport.mm` also duplicates `layer/src/transport.cpp`; see the note at the top of
`transport.h` for why they are not one file yet.
