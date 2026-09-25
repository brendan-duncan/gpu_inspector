# How to debug and profile ray tracing

[Docs index](README.md) › Ray tracing

A ray traced frame is harder to read than a rasterized one. A trace names regions of memory rather
than the shaders it runs, and an acceleration structure is opaque: the driver owns its layout and
nothing reads it back. This guide covers what the inspector shows instead, in the order you usually
need it: is the scene what you think it is, do the rays run the shaders you think they do, what
does it cost, and did the change fix it.

Everything here works on Vulkan (`VK_KHR_ray_tracing_pipeline`), Direct3D 12 (DXR) and Metal,
except where a section says otherwise. It was checked on Vulkan and Direct3D 12 with
`vkinsp_triangle --ray-tracing` and `dxinsp_triangle --rebuild-blas`. The
[Metal](METAL.md) side is described from its own documentation.

## Before you capture

- **Capture a frame that builds the structures you want to see.** The view of an acceleration
  structure is what it was *built from*, and the capture library records that as the build goes
  by. An engine builds its bottom levels once, at load, so a later frame only uses them. The capture
  still knows what each structure *is*, and it reads the last build's inputs back as the capture
  starts, but a buffer the application has rewritten since then no longer holds what was built.
  To see a structure exactly, capture a frame that rebuilds it
  ([which ones can be viewed](INSPECT.md#which-ones-can-be-viewed)).
- **Turn on Buffers and Images** in the capture bar. A trace's binding table is read back as a
  buffer, and replay needs the geometry and the images the rays write.
- **Profile passes** stays on (it is on by default): it is what times the builds and the traces.

## Is the scene what you think it is?

Select the top level acceleration structure in **Inspect**, or any command that uses it: its
details have an **Acceleration Structures** section that says whether the command **builds**,
**copies**, **queries** or **traces** each structure. The object shows:

- **Acceleration Structure**: the build, with each geometry's primitive count, vertex format,
  stride and index type. The addresses a build reads are resolved back to buffer and offset.
- **Instances**: each instance's bottom level, transform, mask, custom index, hit group offset and
  flags. A mask of 0, a wrong hit group offset or a transform that puts an instance somewhere else
  are the usual problems, and they show up here.

**View in a Tab** opens the scene in 3D ([Acceleration structures](INSPECT.md#in-a-tab-of-its-own)):

- **Tree** lists top level, instances, bottom levels and geometries, with primitives, surface
  area and memory summed up the tree. A bottom level that is much bigger than it looks, or that
  holds more triangles than you meant, stands out here.
- **Instances** is the instance list as a table.
- **Overlaps** lists the pairs of instances whose bounding boxes overlap, most overlapped first.
  **Heatmap** colors the scene by it. A ray through a region where many boxes overlap descends into
  every one of them, so this is where traversal gets expensive, and it is the closest thing here to
  a traversal heat map. The GPU's own per-ray traversal counts are readable only through NVIDIA's
  tools.

A procedural bottom level is drawn as its bounding boxes, since its shape exists only in its
intersection shader. An instance whose bottom level has nothing in the capture is drawn as a box.

## Do the rays run the right shaders?

Select the trace (`vkCmdTraceRaysKHR` or `DispatchRays`). Its **Shader Binding Table** section
lists each region, raygen, miss, hit and callable, with its record count and stride, and then what
each record runs: the shader group on Vulkan, or the **export name** on Direct3D 12
([Shader binding tables](INSPECT.md#shader-binding-tables)). The pipeline's **Shader Groups**
section lists the groups and the stages each one names.

The two faults to look for:

- **A record whose handle matches nothing.** The panel says *no shader of this pipeline has this
  handle*. Those rays run the wrong shader or none. It happens when a table was filled from another
  pipeline, or from handles fetched before the pipeline was rebuilt.
- **A Direct3D 12 table the runtime drops.** Every table must start on a 64-byte boundary and
  every record stride must be a multiple of 32. **Frame Stats** reports a table that breaks either
  rule (*binding-table-alignment*), and also a trace with no ray generation record
  (*empty-binding-table*). Without the debug layer, the runtime drops such a trace without any
  message, and the frame just looks empty.

The bytes after each handle are the application's own record data. On Direct3D 12 they are the local
root signature's arguments, resolved to the heap slots and buffers they name. On Vulkan, device
addresses among them are resolved to buffer and offset ([Capture](CAPTURE.md#reading-the-frame)).
On Metal, the intersection function table the geometry indexes is shown instead
([Metal: intersection function tables](INSPECT.md#metal-intersection-function-tables)), and Frame
Stats reports an offset outside it.

## What did the rays write?

A trace writes a storage image (a UAV on Direct3D 12), not a render target, so look at the image,
not a pass's attachments:

- **The image itself.** Open it from the trace's bound resources, or from the pass that samples it
  later.
- **Pixel history** (Vulkan) lists a trace that had the image bound for writing as an event on
  the pixel, *traced with the image bound to be written*, next to the draws, clears and copies that
  touched it ([Pixel history](REPORTS.md#pixel-history)).
- **Validation.** Launch with **Validation layer** (Vulkan) or the debug layer (Direct3D 12). On
  Vulkan, messages about a trace are attached to the trace command. After a device loss on
  Direct3D 12, the breadcrumbs name the `DispatchRays` or build that was running.

The [shader debugger](REPORTS.md#shader-debugger) steps vertex, pixel and compute shaders. It does
not step ray generation, hit or miss shaders. On Metal it does follow a compute kernel's ray
queries, `intersect` included ([Ray queries](REPORTS.md#ray-queries-metal)).

## What does it cost?

**Each build and each trace is timed.** Outside a render pass, the capture library times
acceleration structure builds and ray traces as compute passes, the same way it times a run of
dispatches: from the first one to the next barrier. So a typical frame reads as a bottom level build
pass, a top level build pass and a trace pass, each with its own GPU time. They show in the
**Timeline**, in **Frame Stats**' pass list and in **Reports → GPU Bottlenecks**. On the test apps:

| | Bottom level build | Top level build | Trace |
|---|---|---|---|
| `vkinsp_triangle --ray-tracing` | 0.051 ms | 0.029 ms | 0.021 ms |
| `dxinsp_triangle --rebuild-blas` | 0.044 ms | 0.022 ms | 0.031 ms |

Things to look for:

- **Builds that should not be there.** A bottom level rebuilt every frame when its geometry never
  changes costs its full build time every frame. A build pass is the compute pass whose first
  command is the build. On Metal, Frame Stats reports a structure built twice in one frame (*accel-rebuilt-twice*)
  and a top level with no instances (*accel-empty-top-level*).
- **A trace that costs more than its size suggests.** Open the top level in a tab and check
  **Overlaps**. Stacked or nested instances make every ray that passes through them descend into
  all of them. The **Tree** shows which bottom levels have the most triangles.
- **Each trace on its own.** A replay can time every trace separately.
  `vkinsp_replay frame.gpucap --draws` and `dxinsp_replay frame.gpucap --draws` list each trace with
  its time, next to the draws and dispatches ([Per-draw timing](REPLAY.md#per-draw-timing-and-counters)).
  The pipeline statistics counters do not count ray tracing shader invocations, so those read 0 for
  a trace.

**Frame Stats** also counts the frame's ray tracing launches under API Activity.

What is not here: per-ray traversal and intersection counts, which only NVIDIA's driver exposes,
and [shader cost by ablation](REPLAY.md#shader-cost-by-ablation), which takes draws and dispatches
only.

## Did the fix work?

**Replay the frame** ([Capture replay](REPLAY.md)). A ray tracing frame replays on all three APIs.
The builds are reissued at this machine's addresses, and the binding table is rebuilt with this
driver's shader handles. Record data is translated too: Direct3D 12 local root arguments and Vulkan
record addresses. Bottom levels built before the capture are built first, from what was read back.
The replay compares what the traces wrote as well as the render targets: every storage image the
capture read back is read again and compared, so a trace that runs but writes the wrong pixels is
reported. Both test frames above replay identical.

**Export to C++** writes the frame as a program that builds the same structures and traces the same
rays ([Export to C++](REPLAY.md#export-to-c)). You can change it and run it without the
application.

Shader editing works for ray tracing shaders only through the exported program: a ray tracing
pipeline is shown in the capture but not edited in place.

Not replayed yet on Vulkan: `vkCmdTraceRaysIndirect*`, the NV ray tracing commands, acceleration
structure copies, and queries whose results the frame reads back. A replay that leaves one out
names it in its problems. On Direct3D 12: opacity micromap builds, and a `DispatchRays` whose
binding table the capture did not read back.

## Test applications

| Application | What it exercises |
|---|---|
| `vkinsp_triangle --ray-tracing` | Both levels built every frame, a trace into a storage image the cubes sample |
| `vkinsp_triangle --shader-record` | A hit record holding a buffer's device address |
| `dxinsp_triangle --ray-tracing`, `--rebuild-blas` | A state object with two hit groups; the bottom level built once, or every frame |
| `dxinsp_triangle --local-root` | A local root signature whose arguments the hit record holds |
| `vkinsp_path_tracer`, `dxinsp_path_tracer`, `mtlinsp_path_tracer` | Three procedural bottom levels under one top level: intersection shaders and a closest hit per material. `--rebuild` rebuilds them every frame |

## See also

- [Inspect: acceleration structures and shader binding tables](INSPECT.md#acceleration-structures)
- [Capture replay: Direct3D 12 ray tracing](REPLAY.md#ray-tracing)
- [Finding GPU bottlenecks](PROFILING.md): the general profiling workflow
