---
name: gpu-capture-analysis
description: >-
  Interpret GPU Inspector frame captures (.gpucap) of Vulkan and Metal applications — the command
  stream, passes, the state bound at a draw, render targets, buffers, shaders, validation messages,
  Frame Issues, GPU Bottlenecks and the render graph — to debug rendering problems and find what
  limits a frame, including in running applications the tools launch, capture and edit shaders of.
  Use with the gpu-inspector MCP tools whenever a .gpucap file or a GPU Inspector capture comes up,
  or a native Vulkan or Metal rendering or GPU performance problem does.
---

# GPU Inspector capture analysis

GPU Inspector records frames of native applications: the Vulkan layer intercepts every Vulkan call,
and on macOS the Metal capture library does the same for Metal. A capture saved as `.gpucap` holds
one or more frames and everything needed to inspect them without the application. The
`gpu-inspector` MCP tools read those files with the analyses GPU Inspector itself runs, so what you
report matches what the user sees in its Capture tab.

## What a capture holds, and what it may lack

- **Commands** in submission order, per command buffer, with secondary command buffers inlined
  where `vkCmdExecuteCommands` ran them. Every command has an index; findings, messages and the
  UI all refer to commands by it.
- **Passes**: render passes, and on Vulkan the runs of dispatches outside a render pass, which are
  timed as compute passes. On Metal every encoder is a pass.
- **Objects** the commands reference, each with the call that created it and its full arguments
  (the create info), later updates (memory bindings, descriptor contents) and dependencies.
- **Read-backs**: every render pass attachment at the end of its pass, images bound in descriptor
  sets, and every bound buffer range (vertex, index, indirect, uniform, storage). Buffer ranges
  stop at the capture's buffer size limit, 64 KB unless the user raised it: `truncatedFrom` says so.
- **Only when enabled at capture time**: pass GPU timings and counters ("Profile passes"),
  validation messages (the "Validation layer" launch option), recording stacks ("Stack traces").
  `get_capture_summary`'s `notes` say what is missing. Tell the user how to capture again when a
  missing piece is what the question needs.
- **From this machine**: two things a capture may only name can be read from the user's source
  and build trees. Ask the user where those trees are, and pass them to `set_search_paths`:
  - the source of a shader compiled with line information but no text
  - the functions and lines of stack frames named by module and offset

  With sources, the shader analyses quote the code of their costliest lines.

## Reading the tools' answers

- Object references read `VkImage#12 "GBuffer Albedo"`: type, id, and the name when it has one.
  Pass the id to `get_object`.
- **Three numberings, never mixed**: command indices (`command`, `index`, `i`, `boundAt`); pass
  numbers (`pass`, used by `get_bottlenecks`, `list_commands` and `get_command`); render graph node
  numbers (`node`, used only by `get_render_graph`). Captured images are `texture` numbers
  (`read_texture`); captured buffer ranges are `data` ids (`read_buffer`).
- A missing field means not measured or not applicable, never zero. Measurements are rounded to
  four significant digits.
- Decoded values: vectors are arrays, matrices arrays of **columns**, structs objects.
- Long lists page with `offset` and `limit`; `nextOffset` means there is more.

## Orientation

Start with `get_capture_summary`: counts, the frame timing with its Frame Bound verdict, the
slowest passes, Frame Issues by severity, validation, and notes. If no path was given,
`list_captures` shows the captures GPU Inspector saved or opened recently.

## Performance: what limits the frame

This follows GPU Inspector's `docs/PROFILING.md`. Work from the frame down, and name the evidence at
each step.

1. **Is the GPU the problem?** The Frame Bound verdict compares the GPU span of the passes and the
   CPU submit time with the budget, which is the display refresh period with vsync on and the frame
   interval without. GPU bound: carry on. CPU bound: the application's submission is the limit
   (fewer command buffers and state changes), not the GPU. Vsync bound: nothing to fix.
2. **Which pass?** `get_bottlenecks` ranks the timed passes. Work on the slowest one first. A 12 ms
   shadow pass matters more than an inelegant 0.3 ms bloom pass.
3. **Which stage?** On Metal the vertex/fragment split names it (`bound`). On Vulkan infer it from
   the counters: vertices and draws against fragment work.
4. **Why?** These are the measured causes. Thresholds are starting points, not laws.

   | Measurement | Healthy | Flagged | Usual cause | Fix |
   |---|---|---|---|---|
   | overdraw (fragment runs per pixel) | about 1.2 | above 2 | stacked transparency, repeated full-screen passes, opaque drawn back to front | fewer/larger particles, merge full-screen effects, sort opaque front to back or a depth prepass |
   | fragments per primitive | above 4 | below 4 | dense meshes drawn small (microtriangles) | mesh LOD, culling; not the shader |
   | depth rejection (Metal) | high | below 25% with overdraw above 1.5 | fragments shaded then replaced | front-to-back sort, depth prepass |
   | many tiny draws | | 32+ draws of ≤12 vertices | per-draw overhead | instancing, merged geometry |

   `get_overdraw` measures it per pixel: a Metal capture taken with `overdraw` carries it, and a
   Vulkan capture is replayed on this machine's GPU the first time (slow for a large frame, and it
   needs `vkinsp_replay` built). It ranks the passes, and with `pass` returns the heatmap, which
   shows *where* on screen the fragments stack up.
   Compare its two counts. Many more rasterized fragments than fragments passing depth means the
   depth test is rejecting work, which is cheap only when it rejects before the fragment shader
   runs. Discarded fragments count in both.

5. **Rules without counters**: `get_frame_issues`. Most bear on tiled mobile and XR GPUs, where
   loading and storing attachments costs as much as shading:
   - `clear-outside-pass`: a clear command followed by a pass that loads the image.
   - `color-load`: a load before anything in the frame wrote the image.
   - `unread-store` / `depth-store` / `color-store`: a store that nothing reads.
   - `depth-transient` / `transient-candidate` / `memoryless-candidate`: targets that never need
     memory outside the tile.
   - `msaa-store`: a multisampled attachment stored although it is resolved.
   - `mergeable-passes`: a pass that loads exactly what the previous one stored.
   - `stereo-without-multiview`: one pass per eye.
   - Redundant pipeline, descriptor and buffer binds.
   - Barrier rules: inside a render pass, directly after another barrier, or ALL_COMMANDS.
   - `overwritten-before-read`: a result replaced before anything reads it.
   - `unmipped-texture`: large sampled textures without mips.

   Confirm each finding against the command it names with `get_command`, and against the render
   graph with `get_render_graph` (`node`). Say whether it is real here or a pattern that may be
   intentional.
6. **Shaders** (Vulkan only):
   - `analyze_shaders` ranks the stages in use by uses times modeled cost.
   - `get_shader_flame_graph` shows where the frame's shading work goes: each stage's modeled cost
     times its invocations, by pass, pipeline, stage, function and source line, with the hottest
     functions and lines. Pass it the slowest pass from step 2 with `pass`.
     - With every pass timed, its numbers are milliseconds, but only each pass's total is measured:
       the split inside a pass is modeled.
     - Vertex and compute invocation counts are exact. Fragment counts come from the scissor area,
       so they are an upper bound that ignores overdraw.
   - `get_shader` with view `analysis` gives costs per function and source line, and findings such
     as texture samples in loops, non-constant division and derivatives in branches.

   The cost is a *model*, not a measurement: use it to compare shaders, and never present a modeled
   split as measured time.
7. **Confirm the fix** with `compare_captures` on a capture taken after the change. A change that
   moves no measured number did not address the bottleneck.

## Rendering bugs: something looks wrong

1. **Validation first.** `get_validation`. Errors are real bugs, often the whole answer, and most
   name the command they fired on.
2. **Find where it goes wrong.** `list_textures` then `read_texture`, which returns the image,
   pass by pass: the first pass whose target shows the problem is where to look. Check the numbers
   `read_texture` gives. `uniform: true` means every texel is the same, so nothing was drawn or the
   clear shows through. `nan` or `infinite` counts point at a shader dividing by zero or at bad
   input. Depth that is all 1.0 means nothing passed the depth test or nothing was written.
3. **Find the draw.** `list_commands` with `pass` (or `label`, `kind: "draw"`). For one wrong
   pixel, `get_pixel_history` names it directly: the last draw that wrote the pixel, and the draws
   that reached it but were culled, discarded or failed the depth or stencil test. A Vulkan capture
   is replayed for it, which takes seconds. A Metal capture answers only for the pixel it was taken
   with: capture again with `capture_frames` and `pixelHistory: { texture, x, y }` (a render
   target's id from `list_textures`), then ask the new capture.
4. **Check what the draw read** with `get_command`. The usual suspects:
   - **Fixed-function state**: `cullMode` and `frontFace` (winding flipped by a negative scale or
     viewport), depth test, write and compare op (reversed-Z against a LESS compare), blend factors
     and `colorWriteMask`, `rasterizerDiscardEnable`.
   - **Viewport and scissor**: zero size, off target, or a flipped height.
   - **Uniform values**: matrices (arrays of columns: check for identity, zeros, NaN, or a
     projection that swaps rows and columns), colors and alpha of 0, counts of 0.
   - **Bindings**: a descriptor pointing at the wrong image or buffer, the `data` values of a stale
     range, a sampled image whose read-back shows it is empty.
   - **Geometry**: `read_vertices`. Bounds all zero or NaN mean uninitialized data. A huge range
     means a wrong stride or format. Indices out of range, and a first vertex or index past the
     data, are problems too.
   - **Shaders**: `get_shader`. `source` when it is embedded, `glsl` or `hlsl` otherwise, and
     `reflection` to check the bindings the shader expects against what `get_command` shows bound.
5. **Compare with a draw that works**: `get_command` on both, and diff the state.

## Live applications

`launch_app` starts an application with the capture library in it; `attach_app` connects to one
already listening.

On Android:
- `list_android_devices` finds the device and the package.
- `launch_android_app` starts the package with the layer over adb.
- The package must be debuggable (a development build).

The capture library serves one client, so attaching takes it over from GPU Inspector.

- **Before capturing,** `get_live_frame_stats` says whether the frame meets the display refresh or is
  bound by submission. It measures no GPU time.
- **`capture_frames`** saves a `.gpucap` and returns its summary. The capture tools take the
  returned `capture` id.
  - Capture while the application shows the problem: ask the user to get it there, or use
    `delaySeconds` or `atFrame`.
  - No commands means the application reuses command buffers recorded earlier: capture again with
    `recordAlways: true`.
  - On Metal, `overdraw: true` measures every pass's overdraw per pixel (`get_overdraw`). It slows
    the captured frame, so take timings from a capture without it.
- **Object ids are the same** in the live session and in its captures, so a pipeline id from
  `get_command` is what `replace_shader` takes.
- **A shader experiment:**
  1. Get the stage's source with `get_shader` (`source`, else `glsl`) and edit it.
  2. Call `replace_shader`. A compile failure comes back with the compiler's errors, by line.
  3. Call `capture_frames` again, then `compare_captures`, and `read_texture` on the same target
     before and after.
  4. Call `restore_shader` to undo it.

  The replacement exists only in the running process: report the source change for the application
  to make.
- **Between captures:**
  - `read_live_image` shows an image as it is now. It is quicker than a capture for a look at a
    target after `replace_shader`.
  - `get_live_descriptor_set` shows what a set binds now.
  - `list_live_objects` and `get_live_object` show the live objects. A live object's creation stack
    is where a leak or a misconfigured object came from.
- **When a session misbehaves,** `get_session_log` has the application's output and the capture
  library's own log.
- **Clean up** with `stop_app` when done.

## Vulkan and Metal differences

- Metal binds buffers per stage by index (`stageBuffers`), not through descriptor sets. Its
  pipeline reflection names each slot. Vertex buffers are the slots the vertex descriptor lays out.
  An argument buffer's members are resolved (`argumentBuffer`) to the buffer with an offset, the
  texture or the sampler each one holds. A value that no tracked object claims is a stale or wrong
  handle.
- Metal passes can have the vertex/fragment split, depth rejection and stage utilization.
  Vulkan pipeline statistics give invocation and primitive counts only.
- SPIR-V tools (`analyze_shaders`, `get_shader` source, analysis and cross-compilation) are Vulkan
  only. Metal libraries built from source carry their MSL, which `get_shader` on an `MTLFunction`
  shows at its definition. For per-line Metal costs, GPU Inspector's Xcode Trace button writes a
  `.gputrace`.

## Reporting

- Lead with the answer, then give the evidence.
- For each finding give:
  - its severity
  - what it is
  - where: command indices, object ids and pass labels, so the user can find it in GPU Inspector
  - the measured numbers
  - why it matters in this frame
  - the concrete fix
- Keep three kinds of claim apart: confirmed (validation errors, measured costs), heuristic (a
  rule's pattern that may be intentional) and modeled (shader cost).
- Say what the capture could not show, and which capture option would show it.
