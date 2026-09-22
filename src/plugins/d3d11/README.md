# Direct3D 11 plugin

Captures Direct3D 11 applications on Windows: the calls each device context makes, the passes they
draw, and what every draw was given. It is the second plugin written against the plugin SDK
(docs/PLUGINS.md), after OpenGL ES, and takes the shape the Direct3D 12 library has where the two
APIs are alike (vtable hooks, DXGI swap chains, DXBC reflection) and the OpenGL ES plugin's where
they are not (a state machine with no command buffers, synthetic passes, a state snapshot on every
draw).

```
plugin.json            the manifest: the backend module and the library the launcher injects
CMakeLists.txt         d3d11insp_capture, into build/plugins/d3d11/bin
compile_check.cmd      compiles one source on its own
gen/                   enum name tables and vtable slots, generated from the Windows SDK (tools/gen_d3d11.py)
src/
  platform_win32.cpp   GpuInspectorInitialize; d3d11.dll loaded and its two entry points patched
  hook.cpp             the vtable patcher and MinHook, as the D3D12 library has them
  hooks_device.cpp     D3D11CreateDevice[AndSwapChain]; the device's Create* methods, one object each
  hooks_context.cpp    the device context: every Set*, draw, dispatch, clear, copy and map
  hooks_dxgi.cpp       the factory's CreateSwapChain*; the swap chain's Present, ResizeBuffers, GetBuffer
  hooks_object.cpp     Release and SetPrivateData, which every object gets
  state.cpp            objects and their descriptions; the shadow state of each context
  capture.cpp          the capture: recording, passes, read-backs, the state at each draw, timings
  serialize.cpp        the D3D11 and DXGI descriptors as JSON
  formats.cpp          DXGI formats as VK_FORMAT names, and their layouts
  shader_reflect.cpp   DXBC reflection through d3dcompiler_47.dll
  server.cpp           the connection (the SDK's server) and the requests it answers
ui/backend.ts          the backend module: how the inspector reads the library's captures
```

## What the library does

**Getting in.** The inspector's launcher injects the library before the target's first
instruction and calls `GpuInspectorInitialize`. The library loads `d3d11.dll` and patches its two
exported entry points in place (MinHook): `D3D11CreateDevice` and `D3D11CreateDeviceAndSwapChain`.
The device, the resources, views, shaders and state objects it hands out, the DXGI factory and the
swap chains are vtable patches, once per distinct vtable, with the originals saved so a hook can
forward; the mechanism is the D3D12 library's (`hook.cpp`), including its adoption of vtables
something in the process copied. No DXGI export is hooked: the factory a device's adapter belongs
to is patched at device creation, and every factory object shares that vtable, so the swap chains
made through any factory are seen.

**The device context is the exception.** Its vtable is not the class's but the object's own,
eight bytes into the context, and the runtime rewrites entries in it as the pipeline state
changes (the draws above all, which it swaps between validating and fast paths), so a patch there
is undone by the next state change. The application is therefore given a proxy in its place:
`RecordingContext` (`hooks_context.cpp`) implements `ID3D11DeviceContext4` with every method
forwarding to the real context (`gen/d3d11_context_proxy.gen.h`, generated) and the recorded ones
overridden. `D3D11CreateDevice`, `GetImmediateContext` and `CreateDeferredContext` in every version
return the proxy, `QueryInterface` on it answers the context interfaces with itself and everything
else (`ID3DUserDefinedAnnotation`, `ID3D11Multithread`, video) with the real object's, and the
library's own calls go to the real context and are never seen. A deferred context's proxy dies
with its last reference; the immediate context's lives until the device goes, since an
application may fetch and release the context every frame.

A device created by ANGLE's `libGLESv2.dll` is left alone: that is an OpenGL ES application on
ANGLE's Direct3D 11 backend, which the OpenGL ES plugin captures (`D3D11INSP_ANGLE=1` captures
the device underneath instead). The port is opened when a device gets its first swap chain rather
than when it is created, so a device an application makes for video decoding, or Chrome's media
device beside its WebGPU one, never takes the session's port from the API that draws.

**Objects.** Every buffer, texture, view, shader, input layout, state object, query, command list,
device, context and swap chain is announced with the description it was created from (`pDesc`
under the D3D11 member names, enums by name, flags as `A | B`). A texture also carries its
`VK_FORMAT_*` spelling, a view what it covers of its resource, an input layout its elements with
`D3D11_APPEND_ALIGNED_ELEMENT` resolved. A shader carries its bytecode as a blob named
`<stage>:main` and its reflection under `reflection.<stage>`, the JSON the D3D12 library attaches
to a pipeline state, which the inspector reads for the shader views, the flame graph and the typed
constant buffers. The debug name (`SetPrivateData` with `WKPDID_D3DDebugObjectName`) is the label.

**State.** Each device context has a shadow of what is bound on it, followed from the
application's `Set*` calls rather than asked of the runtime (a deferred context cannot be asked).
`ClearState`, `ExecuteCommandList` without state restoration, `FinishCommandList` and
`SwapDeviceContextState` clear it. Every draw and dispatch carries a `state` snapshot: the
shaders, each stage's constant buffers, shader resources and samplers, the compute or pixel UAVs,
the input assembler (topology, input layout with its elements, vertex buffers, index buffer), the
render targets, the rasterizer, blend and depth-stencil state with their dynamic values, and the
draw's own counts.

**Read-backs.** What a draw reads is copied on the GPU where the draw is met, into staging
resources of the library's own (`CopySubresourceRegion`), and mapped once the last frame is over:
the frame is not stalled mid-pass. Each range or texture is copied once per contents, where the
contents are a generation the library bumps at every write it sees (a map for writing, an update,
a copy, a clear, a draw with the resource as a target). Vertex buffers are read from their offset
to the end for an indexed draw, and to the last vertex named for a non-indexed one; the index
buffer for the indices drawn; constant buffers whole (or the window `*SetConstantBuffers1` names);
buffer views their range; textures at their base mip, every slice. A multisampled texture is
resolved first. Everything is capped by the capture's buffer and image budgets.

**Passes.** Direct3D 11 has none. The library begins one at the first draw into, or clear of, the
bound render targets and ends it when `OMSetRenderTargets` binds different ones, at `ClearState`,
at an executed command list, when the event (`BeginEvent`) it began in ends, or at `Present`.
Binding the same targets again keeps the pass open, which is what engines that set their targets
before every draw need. `BeginRenderPass` and `EndRenderPass` are recorded around it (`synthetic:
true`). When the pass ends its render targets and its depth target are copied into staging
textures; a depth-stencil target becomes two entries, its depth and its stencil byte. A
`DiscardView` of a target reads it first and marks it discarded, and a clear before the first draw
marks it cleared, both written into the pass's `BeginRenderPass` so the render graph knows what
never had to be loaded or stored. A run of dispatches with no render pass open is a compute pass,
which the inspector brackets itself; the library ends its own on the same commands the backend
lists in `COMPUTE_PASS_END`. Passes are numbered per context and per frame, which is how the
inspector counts them.

**Deferred contexts.** A deferred context records into a stream of its own until
`FinishCommandList`, when the recording is frozen into the command list. `ExecuteCommandList` on
the immediate context records the list's commands as its `children`, which the inspector inlines
the way it inlines Vulkan secondaries. The read-backs a deferred context's draws asked for were
recorded into the deferred context too, so they run with the list and hold what its draws read;
its passes count in the immediate context's sequence from the point of execution, and their
render targets are filed there. A list finished before the capture began is
`<unrecorded command list>`.

**Timings.** With `profilePasses`, a `D3D11_QUERY_TIMESTAMP_DISJOINT` brackets each immediate
context's frame and a `D3D11_QUERY_TIMESTAMP` is written where every render and compute pass
begins and ends. The passes of a deferred context are not timed. A frame the GPU reports as
disjoint gets no timings.

## What it does not do yet

- **Live image read-back** in the Inspect tab (`RequestImage`) and creation stack traces.
- **An object released while still bound** (its count at zero, alive only through the pipeline)
  is forgotten, and a draw naming it shows it as unknown; the D3D12 library has the same gap.
- **Timings of deferred contexts' passes**, and multisampled depth targets, which Direct3D 11
  cannot resolve.
- **Shared resources** (`OpenSharedResource`) and `D3D11On12CreateDevice` devices are not tracked.
- **Tiled resources** and video (decoder, processor) interfaces are not recorded.
- The shader views show a shader's disassembly (the inspector's shader tool reads DXBC) but there
  is no shader debugger for DXBC.

## Testing it

`test/d3d11_triangle` is a Direct3D 11 application with two passes, an indexed textured cube into an
offscreen target with a depth buffer and a fullscreen pass to the swap chain; `--msaa`, `--deferred`
(the first pass on a deferred context), `--compute`, `--discard` and `--debug-layer` exercise the
rest. The app launches it like any Windows target, and the library goes in beside the others:

```
node tools/run_electron.mjs . --launch=<build>\bin\Release\d3d11insp_triangle.exe --debug-capture --debug-command=19
```

`src/app/test/d3d11_plugin.test.js` checks the backend against a capture in the library's format,
including a deferred context's inlined draw.
