# Metal capture — spike

A first cut at capturing Metal, the way `layer/` captures Vulkan. It is a spike: it discovers and
hooks the Metal class tree and logs a frame's commands. It does not track objects, read anything
back, or speak the inspector's protocol yet.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
cd build/bin
MTLINSP_LOG=1 DYLD_INSERT_LIBRARIES=./libmtlinsp_capture.dylib ./mtlinsp_triangle --frames 3
```

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

## Two things the spike caught

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
`class_getInstanceMethod` walks up, so hooking "the render encoder's `endEncoding`" may really
replace an implementation on a base class that the compute encoder inherits too. Keying the
saved originals by `(class, selector)` then leaves the sibling with nothing to find: its hook
fires, the lookup fails, and the forward jumps through a null pointer — `SIGSEGV`, reproducibly,
under `MTL_SHADER_VALIDATION=1`. The originals are keyed by `Method` instead, which is what was
actually replaced.

Verified after both fixes: three frames, `2 encoders, 1 draw, 1 dispatch` each, clean exit, in
all three validation modes.

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

## Not done

Object tracking, resource read-back, the TCP transport and the inspector's protocol, blit and
argument-buffer coverage, `MTLIndirectCommandBuffer`, `MTKView`/`CAMetalLayer` paths other than
the one the test application uses, Intel and AMD class trees (only Apple Silicon is verified),
and re-signing a hardened target as part of the launch flow.
