# Capture replay

[Docs index](README.md) › Capture replay

`vkinsp_replay` re-executes a Vulkan capture (`.gpucap`) on this machine's GPU, without the
application. It is the basis for the analyses that have to run a frame again with something
changed: the overdraw heatmap, pixel history, draw-call overlays, mesh output (which the shader
debugger's pixels are rasterized from), per-draw timing and shader cost by ablation.

```
vkinsp_replay <capture.gpucap> [--validate] [--dump <dir>] [--overdraw <dir>] [--overdraw-data <file>]
              [--pixel <image> <x> <y> [--mip <n>] [--layer <n>] [--pixel-data <file>]]
              [--draws [--draw-data <file>]] [--overlay <command> ... [--overlay-data <file>]]
              [--mesh <command> ... [--mesh-data <file>]] [--ablate <request> [--ablate-data <file>]] [--trace]
vkinsp_replay <capture.gpucap> --check
vkinsp_replay <capture.gpucap> --serve [--validate]
```

- **Replay (the default).**
  - Re-creates the capture's objects, replays its command buffers in submission order, and reads
    back every render target the capture read back, at the same point in the frame.
  - Compares each target byte for byte with the capture's copy.
  - Exits with 0 when every target matches, 1 when some differ or could not be compared, and 2
    when the replay could not run.
- **`--validate`:** enables the Khronos validation layer and lists its messages.
- **`--dump <dir>`:** writes the captured, replayed and difference image of each compared target
  as PNG.
- **`--trace`:** prints each object and command to stderr before it is replayed, so the call a
  driver crashes on is the last line printed.
- **`--overdraw <dir>`:** measures every render pass's overdraw (see [Overdraw](#overdraw)), prints
  the numbers and writes a heatmap for each pass.
- **`--overdraw-data <file>`:** measures overdraw the same way and writes every measurement with its
  per-pixel counts into one file: a magic line (`OVERDRAW 1`), a little-endian u32 manifest length,
  a JSON manifest, then 16-bit counts the manifest names as `[offset, length]`. GPU Inspector's
  **Measure Overdraw** runs the tool this way (`src/app/src/main/replay.ts`), and so does the MCP
  server's `get_overdraw` for a Vulkan capture.
- **`--pixel <image> <x> <y>`:** follows one pixel of an image (tracker id; `--mip` and `--layer`
  pick the subresource) through the frame, and lists every pass start, draw and clear that touched
  it (see [Pixel history](#pixel-history)).
- **`--pixel-data <file>`:** with `--pixel`, writes the history as JSON: every event, including the
  draws that do not reach the pixel, with its sample counts and the texels after it as hex in the
  formats the file names. GPU Inspector's pixel history pane runs the tool this way, and so does the
  MCP server's `get_pixel_history`.
- **`--overlay <command>`:** draws one draw of the frame on its own (repeat it for several), for
  the highlight, depth test and wireframe overlays (see [Draw-call overlays](#draw-call-overlays)).
- **`--overlay-data <file>`:** with `--overlay`, writes each draw's mask in `--overdraw-data`'s
  layout (`OVERLAY 1`, one byte per pixel). GPU Inspector's render target tab runs the tool this way.
- **`--mesh <command>`:** captures what one draw's vertex shader wrote (repeat it for several), for
  the mesh view's VS Out (see [Mesh output](#mesh-output)).
- **`--mesh-data <file>`:** with `--mesh`, writes each draw's vertex records in `--overdraw-data`'s
  layout (`MESH 1`). GPU Inspector's mesh tab and the MCP server's `get_mesh_output` run the tool
  this way.
- **`--ablate <request>`:** times draws with variants of one of their shader stages (see
  [Shader cost by ablation](#shader-cost-by-ablation)) and prints each variant's time.
- **`--ablate-data <file>`:** with `--ablate`, writes the timings as JSON. GPU Inspector's
  **Measure shader** and the MCP server's `measure_shader_cost` run the tool this way.
- **`--serve`:** keeps the replay alive for many analyses of the capture (see
  [Kept alive](#kept-alive)). GPU Inspector and its MCP server run the tool this way.
- **`--check`:** only decodes every creation argument and command argument, and lists what cannot
  be rebuilt.

It builds with the layer (`VKINSP_BUILD_REPLAY`, on by default) on Windows and Linux.

## How it works

**Decoding.** The layer writes creation arguments and command arguments as JSON with a serializer
generated from vk.xml (`tools/vkgen/serialize.py`). `tools/vkgen/deserialize.py` generates the
inverse from the same registry model, as `src/replay/gen/vk_decode.gen.*`:
- a decoder for every enum, flags type, struct and `pNext` chain;
- an argument struct and decoder for every command;
- a recorder for every `vkCmd*`, which decodes the JSON and records the call into a command buffer.

The generator handles several quirks of the JSON:
- **Handles.** `{"__id"}` handles resolve to the replay's own objects.
- **`pNext` chains.** A chain is written as an array whose elements repeat the rest of the chain,
  so the elements are decoded without their nested chains and then linked.
- **Unions without a selector.** They are written with every member, and the largest member is
  decoded (for `VkClearValue`, `color.uint32`: the exact bits).
- **Truncated data.** Data the layer summarized comes back zeroed, and the decoder reports it.

**Loading.** `src/replay/src/json.*` is a JSON parser into an arena that keeps numbers as text, so
64-bit values stay exact. `gpucap.*` reads the file. The Vulkan loader is opened at run time
(`LoadGlobalFunctions`), so the replay links against no loader and the headers may be newer
than the installed runtime.

**Objects** are re-created in id order, which is creation order (`replayer.cpp`):
- **Device.** Created from the capture's own create info, on the GPU with the captured name (or
  the first discrete GPU), minus the extensions this GPU lacks. `VK_KHR_swapchain` is added so render
  passes may end in `PRESENT_SRC_KHR` without a surface.
- **Memory.** Device memory is not re-created as such: every image and buffer gets memory of its
  own. That keeps a capture replayable on another GPU with other memory types. Transfer usage is
  added to every image and buffer. External memory and DRM modifier structs are dropped.
- **Swapchain images** become ordinary images of the swapchain's format and extent. Surfaces,
  swapchains and pipeline caches are left out.
- **Pipelines and shader modules** take their code from the SPIR-V payloads the capture keeps.
  Modules are often destroyed before the capture, and the JSON cuts code over 4 KB.
- **Render passes** store every attachment, as the layer does while capturing, so the result of
  every pass can be read. Dynamic rendering attachments get the same treatment when the pass
  begins.
- **Dependencies.** An object whose arguments name objects the replay does not have is not created.
  A command that names them is left out, and so is a pass whose begin is left out, with everything
  up to its end. The driver is never handed a null handle.

**What goes in.** The replay uses only what the capture read back:
- **Sampled images** (every mip of the view the capture read) are uploaded before the frame.
- **What images held when the frame first read them** (texture kind `initial`) is uploaded after
  them, and wins where both cover a mip. The capture takes these where the frame reads an image it
  has not written whole: a pass that loads an attachment, the source of a copy or a blit
  ([Architecture](ARCHITECTURE.md), the capture's read-backs). That is state earlier frames left
  behind: a history buffer, an accumulated target, a texture updated a piece at a time.
- **Every subresource** then moves to the first layout the frame expects it in: the old layout of
  its first barrier, the layout a descriptor binds it with, the layout of a pass that renders to it,
  or the layout a copy, blit or clear names. Mips and layers of one image can start apart, as a mip
  chain being built does.
- **Buffer ranges** captured when bound, and the sources of the frame's buffer copies (read whole),
  are uploaded before the submission of the command buffer that binds or copies them. A staging
  buffer the host writes every frame arrives this way.
- **Descriptor sets** are written from the snapshot taken when they were bound. A set is only
  rewritten when its contents changed, since rewriting a bound set would invalidate the command
  buffers that bound it.

**Commands.** A submission's command buffers are recorded from their recordings in the command
list, including the secondary command buffers inlined after `vkCmdExecuteCommands`. They are
submitted without the application's semaphores and fences, and the replay waits for them. At the
end of each pass the replay copies the targets the capture read back for that pass (render pass
counter per command buffer, as the layer counts). A multisampled target goes through the resolve
the capture read it through: `vkCmdResolveImage` for color, and for depth a render pass that resolves
sample zero. After the submission the copies are compared. Only the undefined top byte of a 24-bit
depth copy is ignored.

## Kept alive

`--serve` creates the device and the capture's objects once, then answers analyses read one per line
from stdin, each by replaying the frame with that analysis added and writing the file its one-shot
flag would. Answers are JSON lines on stdout that start with `@replay ` (`Serve` in `main.cpp` has
the protocol):

```
{"id": 1, "kind": "pixel", "image": 278, "x": 400, "y": 300, "out": "history.json"}
@replay {"id": 1, "ok": true, "ms": 38.1, "problems": 0}
```

The kinds are `overdraw`, `draws`, `overlay` and `mesh` (with `commands`), `pixel`, and `replay` (the
frame alone, comparing its render targets). Every frame after the first starts where the first did
(`ResetFrameState`): command pools reset, images cleared and back in their initial layouts, sampled
textures, the images' frame-start contents and the frame's buffer ranges uploaded again. Pipeline copies made for an analysis are
kept, so the next analysis of the same draws does not make them again.

On the Unity frame, a fresh process takes 0.3 to 0.4 s per analysis. Served, setup takes 0.2 s once,
and then each analysis 35 to 150 ms; each output is byte for byte what the one-shot flag writes
(draw timings aside), and three `replay` frames in a row compare all 12 targets identical. The app
keeps one such process per open capture (`ReplayServerPool` in `src/app/src/main/replay.ts`, at most
three, stopped after five minutes idle), and falls back to a one-shot replay when a process cannot
start or dies.

## Overdraw

`--overdraw` measures each render pass right after the replay has executed it. It runs in the same
command buffer, so the buffers and descriptor sets the draws read still hold what they held for the
pass.

- **Re-issuing the pass.**
  - The pass's commands are issued again inside a pass of the replay's own. Secondary command
    buffers are inlined.
  - The state the pass inherited from earlier in its command buffer is issued first: viewports,
    bound descriptor sets, vertex and index buffers, push constants.
- **Counting.** Every graphics pipeline the pass binds is replaced by a copy whose fragment stage
  writes 1.0 into an `R16_SFLOAT` target, blended with `ONE, ONE`. The target has the size of the
  pass's framebuffer. Each pixel ends up holding the number of fragments that landed on it.
- **The copies** keep the vertex stages, rasterization and dynamic state, and drop blending and
  multisampling state.
- **Two counts per pass:**
  - **Fragments passing depth and stencil**, in draw order. The pipelines keep their tests, against
    a copy of the depth the pass started from: its contents when the pass loads depth, its clear
    value when it clears. This is the fragment shading the pass paid for, assuming early tests.
  - **Every rasterized fragment**, without depth and stencil tests.
- **Output.** Each pass reports its draws, fragments, covered pixels, the average per pixel and per
  covered pixel, the maximum, and a histogram (1, 2, 3, 4, 5-8, 9-16, 17-32, 33 and more). When the
  capture profiled the pass with pipeline statistics, it also reports the fragment shader
  invocations the capture measured. The heatmap runs black, blue, cyan, green, yellow, orange, red,
  magenta and white as the count grows.
- **Checked against pipeline statistics.** The triangle's pass counts 51,204 fragments, exactly the
  51,204 fragment shader invocations its capture measured with pipeline statistics.

Limits:
- Fragments a shader discards are counted, because the counting shader does not discard.
  Alpha-tested geometry therefore counts as opaque.
- A multiview pass is counted in its first view only.
- A pass the replay leaves out has no measurement.

A Metal capture measures the same while capturing, with no replay: the capture library draws each
pass again right after the application ends its encoder, with the application's own objects
(`src/metal/README.md`, "Overdraw").

The pipeline copies of overdraw and pixel history are made by `pipeline_copy.cpp`: the captured
create info is decoded, its shader stages rebuilt from the capture's SPIR-V, and an edit changes
the state before the copy is created. A pipeline linked from graphics pipeline libraries holds none
of their stages or state, so its copy is made whole instead of linked (`MergeLibraries`): the stages
and the state members of the parts each library's flags name, every library's dynamic states, and
their dynamic rendering formats. The copy's code comes from the linked pipeline's payloads. What
reads a pipeline's state from its record (the mesh output's topology, pixel history's scissor) finds
it in the library holding that part (`PipelineState`, `PipelineDynamic`). Checked on
test/triangle `--pipeline-library` with the validation layer: overdraw, a draw overlay, VS Out,
pixel history and ablation, no messages.

## Draw-call overlays

`--overlay <command>` shows where one draw landed, the way RenderDoc's texture viewer overlays do
(`vk_overlay.cpp`). It is recorded right after the replay has executed the draw's pass, like
overdraw, and `overlay.cpp` issues the pass again into an `R16_SFLOAT` target up to and including
the draw, three times:

- **Rasterized:** the draw alone, with the counting fragment shader and no depth or stencil tests.
- **Passed:** from a copy of the depth the pass started with, the pass's earlier draws move the
  depth and stencil without writing colour, then the draw runs with its own tests.
- **Wireframe:** the draw alone with `VK_POLYGON_MODE_LINE`, which needs the `fillModeNonSolid`
  feature the replay adds to its device.

The three fold into one byte per pixel: bit 0 rasterized, bit 1 passed depth and stencil, bit 2 an
edge. Each draw also reports its fragments and its covered, passed and rejected pixels:

```
vkinsp_replay frame.gpucap --overlay 18
draw overlays: 1
  [18] vkCmdDrawIndexed (command buffer 7, pass 0): 51076 fragments on 51076 of 307200 pixels (16.63%); 0 passed depth and stencil, 51076 rejected, wireframe drawn
```

That is the triangle's `--occluded` mode, which draws the cube a second time where it already is,
so every fragment of the second draw fails its `LESS` depth test.

Limits:
- A fragment the draw's own shader discards shows as covered and, when nothing else rejects it,
  passed. On a Unity frame the text quads show 3,480 fragments passing, while the draw's occlusion
  query (`--draws`) counts 2,089 samples: the rest were alpha-discarded.
- A pass the replay leaves out, and a draw whose pipeline cannot be copied, have no overlay.

## Mesh output

`--mesh <command>` captures what a draw's vertex shader wrote, RenderDoc's VS Out. RenderDoc turns
the vertex shader into a compute shader (`vk_postvs.cpp`); `mesh.cpp` uses transform feedback
(`VK_EXT_transform_feedback`), which the replay enables where the GPU has it. The edit to the shader
is much smaller:

- **The shader** (`xfb_patch.cpp`) gets the `TransformFeedback` capability, the `Xfb` execution
  mode on its entry point, and `XfbBuffer`, `XfbStride` and `Offset` on each output it can capture:
  `gl_Position`, and every located output of 32-bit floats or integers (scalars, vectors, matrices,
  arrays, and the members of an output block). Point size, clip distances and 64-bit outputs are
  left out.
- **The draw** is issued after the replay has executed its pass, like the overlays: the pass's
  state again, then the draw alone with a pipeline copy that uses the edited shader, has no fragment
  stage and discards rasterization. A draw with shader objects (`VK_EXT_shader_object`) binds a copy
  of its vertex shader object made from the edited SPIR-V instead, no fragment shader, and rasterizer
  discard as dynamic state; it is issued in dynamic rendering, which shader objects need, where a
  pipeline copy is made for a render pass. The topology is the one the draw's dynamic state set.
- **The buffer** is sized from the draw's arguments (three vertices per primitive for strips and
  fans, a million vertices for an indirect draw, 256 MB at most), and the counter buffer says how
  much was written. A draw that fills it is marked truncated.

The vertices are the ones the draw assembled: an indexed draw's in index order, strips and fans as
lists, every instance in turn. The replay prints the position's clip-space range:

```
vkinsp_replay frame.gpucap --mesh 17
mesh outputs: 1
  [17] vkCmdDrawIndexed (command buffer 7, pass 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST): 36 vertices, 36 bytes each
    gl_Position: offset 0, 4 floats; x [-0.870851, 0.870851], y [-1.45182, 1.45182], z [1.60923, 3.23925], w [1.69314, 3.30686], 0 with w <= 0
    fragColor: offset 16, 3 floats
    fragUV: offset 28, 2 floats
```

Checked with the validation layer on the triangle, a Unity frame (outputs of HLSLcc shaders, a sky
sphere with 2,664 of its 5,040 vertices behind the eye, a fullscreen triangle spanning -1 to 3) and a
multiview XR frame: no messages from the edited shaders or the feedback.

Limits:
- Pipelines with tessellation or geometry stages are not captured (feedback would be the last
  stage's outputs).
- In a multiview pass the draw runs in a single-view pass, so a shader that reads `gl_ViewIndex`
  gives the first view's vertices.
- A GPU without `VK_EXT_transform_feedback` (most mobile GPUs, MoltenVK) cannot capture VS Out.
- Overdraw, draw-call overlays, pixel history and ablation copy pipelines, so they leave draws with
  shader objects out.

## Pixel history

`--pixel` follows a pixel through every render pass that renders to its image
(`history.cpp`, after RenderDoc's `vk_pixelhistory.cpp`, simplified). For each such pass:

- **Before the replay executes the pass,** every attachment is copied into an image of the replay's
  own: its contents when the attachment loads, its clear value when it clears. The pixel is read
  from the copy: the pass's **start** event, with its load op.
- **After the pass,** its commands are issued again into those copies, one event at a time, each
  inside a pass that loads what the previous event left. The replay's passes are compatible with
  the captured one: the same attachments and subpasses, loading instead of clearing. Bindings and
  dynamic state are issued between them, where they stay in effect; the state the pass inherited
  from its command buffer comes first.
  - **A draw** first runs six times under occlusion queries, with a one-pixel scissor and copies of
    its pipeline that write nothing:

    | Query | Pipeline copy | A zero result means |
    |---|---|---|
    | covered | counting fragment shader, no culling, no tests | the draw does not reach the pixel |
    | facing | counting fragment shader, the pipeline's culling | culled |
    | shaded | the draw's fragment shader, no tests | discarded by the shader |
    | depth | depth test only | failed the depth test |
    | stencil | stencil test only | failed the stencil test |
    | all tests | every test | nothing written |

    Then it runs with its own pipeline, and writes.
  - **`vkCmdClearAttachments`** runs as it is.
  - After each event the pixel and the pass's depth at it are read.
- **Output.** Each event lists the command index, what became of the draw's fragments, and the
  pixel's value and depth after it, decoded for common formats (8-bit RGBA and BGRA, half and full float,
  `A2B10G10R10`, `B10G11R11`, and the depth formats). Draws that do not reach the pixel are only
  counted.

On these captures, the value after a pass's last event matches the target the capture (or the
replay) read back at that pixel:
- the triangle: the cube and the background;
- hazard, and msaa through its resolve attachment;
- the Unity player, on an image two passes render to;
- the XR frame, in both eye layers: where two overlapping draws both pass the depth test, and where
  the later one fails it (the pixels the overdraw heatmaps count 2 untested and 1 tested).

No capture yet exercises the culled, discarded and stencil outcomes.

The query counts are samples: a multisampled pass counts up to its sample count per fragment.
They are also tested against the depth and stencil from before the draw, since the copies do not
write depth. So two triangles of one draw that both pass count twice.

Limits:
- Writes outside render passes are not followed yet: clears, copies and blits into the image, and
  compute.
- A multisampled image is not followed (its resolve attachment is). Only the first layer of a
  layered framebuffer is followed.
- Per-fragment detail is missing: a draw is one event, with no values of the primitives inside it.
- A shader that writes depth, or discards after early tests, is classified as if tests ran late.

## Per-draw timing and counters

`--draws` measures every draw and dispatch of the frame (`draw_stats.cpp`, RenderDoc's
`vk_counters.cpp` does the same). The frame replays exactly as the capture recorded it, with each
action issued between a pair of timestamps and inside a pipeline statistics query:

- **The counters are exact**: vertex and fragment shader invocations, primitives, compute
  invocations — per draw, indirect arguments included, which nothing in a capture itself reports.
  They need the `pipelineStatisticsQuery` feature, which the replay adds to the device it creates.
- **Samples passed** comes from a precise occlusion query around each draw, which is how a pass that
  executes secondary command buffers gets a depth rejection rate at all: the layer's own query
  cannot span `vkCmdExecuteCommands`, the replay's sits inside the secondary. `pass_metrics.ts` sums
  a pass's draws and uses that where the capture's own counter is missing.
- **The times are not what a draw costs alone.** The GPU pipelines consecutive draws, so their
  spans overlap and add up to more than the pass takes. What they are good for is the share of a
  pass a draw accounts for.

`--draw-data <file>` writes them as JSON (`src/app/src/renderer/draw_stats.ts` reads it). GPU
Inspector's Shader Flame Graph runs the tool this way from **Measure draws**, and the MCP server's
`get_shader_flame_graph` does on first use: a pass's measured GPU time is then split between its
draws by what the replay timed, and each fragment stage takes its measured invocation count instead
of the scissor-area estimate.

```
vkinsp_replay frame.gpucap --draws
draws measured: 3, 0.021 ms of draw time, 51204 fragment shader invocations
  [5] outside a render pass: 0.0078 ms, 0 vertex, 0 primitives, 0 fragment, 1024 compute invocations
  [17] pass 0: 0.0061 ms, 24 vertex, 12 primitives, 51204 fragment, 0 compute invocations, 48000 samples passed
```

On the test triangle the fragment count matches the pipeline statistics the capture itself
measured, exactly. On a Unity frame, whose 11 draws all sit in secondaries, the per-draw sums match
the layer's per-pass counters exactly and one draw shows real rejection (3,480 fragments shaded,
2,089 samples passed).

## Shader cost by ablation

`--ablate` measures what the parts of a shader cost. The idea is to time a draw with a part of its
shader taken out: the time it saves is the part's cost.

**The variants.** `src/app/src/renderer/vulkan/spirv_ablate.ts` writes the variants of a stage, and
spirv-val checks each one before any reaches the driver:

- **The stage**, with its outputs left out (fragment and compute stages only: a vertex stage
  decides what is rasterized).
- **Each function**, with its calls removed or their results replaced.
- **Each source line**, with the values it computes replaced. This needs line information.
- **Each texture**, with every read of it replaced.

**Replacement values.** A replaced value comes from something the compiler cannot fold into a
constant: `gl_FragCoord`, the vertex index or the invocation id. Inside a loop it comes from a
value that changes every iteration, so the loop's work is not hoisted out.

**What is left out:**

- Values that decide a branch, a switch or a loop test, and everything they are computed from.
  That dependency is followed through variables by the stores a read can see, so a temporary that
  an engine's generated shader reuses for unrelated values does not tie them together.
- Lines that update a value the rest of their loop reads on the next iteration.

**The request.** The file holds a magic line (`ABLATE 1`), a u32 manifest length, and a JSON
manifest naming each target draw, its stage and `repeat`, then the variants' SPIR-V.

**How a target is timed** (`ablation.cpp`). The replay runs the frame as captured. Right before
each target draw, inside its pass and command buffer, it issues the draw again with every
variant:

- Each variant runs in a copy of the pipeline that writes no depth or stencil, so nothing after it
  changes.
- After each bind comes one untimed draw.
- Then `repeat` draws run between one pair of timestamps, and the time is divided back to a
  single draw.
- Variants rotate order each round, and the first round is a warm-up.
- A variant's time is the median of its rounds.

The captured pipeline is then bound again, and the draw runs as recorded.

```
vkinsp_replay heavy.gpucap --ablate request.bin
ablations: 1
  [17] fragment stage, pipeline 48: 0.3021 ms as captured (median of 5 rounds)
    fragment: main                           0.0000 ms, saves 0.3021 ms
    blurred(vf2;                             0.3022 ms, saves -0.0001 ms
    fbm(vf2;                                 0.0038 ms, saves 0.2982 ms
    hash(vf2;                                0.0039 ms, saves 0.2982 ms
    heavy.frag:18                            0.0039 ms, saves 0.2982 ms
    heavy.frag:29                            0.2460 ms, saves 0.0561 ms
    checker                                  0.3060 ms, saves -0.0040 ms
```

`src/app/src/main/shader_ablation_run.ts` writes the request. `src/app/src/renderer/shader_ablation.ts`
turns the answer into what each part saved. A line is charged only for what it saved beyond the
costliest measured part feeding it. On `test/triangle --heavy`, a fragment shader running 480
octaves of hash noise, the hash function measures at 98.7% of the stage, its one line at 98.7%, and
the lines that only call it at 0.

Two limits:

- The times come from one GPU and driver.
- A part the driver's optimizer had already made free measures as free.

## Where it stands

Every capture replayed so far, with its result:

| Capture | Result |
|---|---|
| test/triangle (render pass, compute, texture, push constants) | identical, color and depth |
| test/triangle `--hazard` (two submissions, `vkCmdUpdateBuffer`) | identical |
| test/triangle `--pipeline-library` (the cube pipeline linked from a vertex and a fragment library) | identical, no validation messages |
| test/triangle `--shader-object` (linked vertex and fragment shader objects, all state dynamic, dynamic rendering) | identical, no validation messages |
| test/triangle `--second-device` / `--second-queue` (a 256x256 target cleared each frame on a second VkDevice, or on a second queue) | all 3 targets identical, no validation messages: the second device's objects replay on the one device |
| test/triangle `--push-template` (the cube's uniform buffer and texture pushed through a descriptor update template) | identical, no validation messages: pushed again as plain writes from the snapshot |
| test/triangle `--msaa` | identical: the multisampled color and depth through their resolves, and the resolve target |
| test/triangle `--persistent` (images loaded and copied from what earlier frames left, a host-written staging buffer, a mip in another layout) | all 5 targets identical, no validation messages; before frame-start contents, the 3 persistent targets differed in nearly every texel |
| Unity player frame (secondary command buffers, two subpasses, `vkCmdSetVertexInputEXT`, MRT) | all 12 targets identical, 0 problems |
| Unity player frame, captured again with frame-start contents (9 passes, the last loading the depth the one before it cleared) | all 14 targets identical, no validation messages; the loaded depth was written earlier in the frame, so the capture took no copies |
| XR triangle captured on an Adreno 740, replayed on an RTX 4080 | visually identical; 0.4% of texels differ slightly (shader precision and rasterization of two GPUs) |

These cases differ for known reasons:
- **A frame captured while `replace_shader` was active** reads back the edited shader's output.
  The capture keeps the original pipeline, which is what the replay draws.
- **Captures older than sampled-image read-back** have no texture contents to upload.
- **Captures older than frame-start contents** start images the frame loads or copies from at zero,
  and copy zeros from staging buffers the host wrote.

## What is left

1. **Replayable captures, the rest.** The capture holds what the frame reads before writing it
   through passes and transfers, and the replay starts every subresource in its own layout. Still
   missing:
   - Stencil contents, and the contents of multisampled images a frame loads (neither can be read
     back into something a replay could upload yet).
   - Memory the frame reads with no command naming it: buffer device addresses, descriptor buffers,
     and buffers bound in command buffers recorded before the capture. RenderDoc diffs mapped ranges
     at each submit; here a copy is taken where a command reads, which is when the GPU sees what the
     host wrote.
   - Storage images and buffers a shader reads before it writes them, inside a pass: their read-back
     is taken when the pass ends.
   - Command buffers recorded in another order than they run can take an image's contents after a
     write rather than before it.
   - Objects the layer did not track: swapchain images of swapchains created before it loaded.
   - Live shader replacements should be recorded.
2. **Pixel history, the rest.**
   - Writes outside passes.
   - Multisampled images.
   - Per-fragment values: RenderDoc re-draws each primitive with a primitive-id shader.
   - Early fragment tests.
3. **A frame restored between analyses** clears images to zero and uploads the captured buffer
   ranges again, but a buffer the frame wrote outside those ranges (a compute shader's output, a
   `vkCmdUpdateBuffer` target) keeps what the last frame left there.

Not replayed yet: ray tracing, queries whose results the frame reads back, and Metal captures.
A ray tracing pipeline and acceleration structures are not made, and the builds and traces are
left out and listed as problems. Those commands name the captured process's device addresses. The
rest of the frame replays (test/triangle `--ray-tracing`: its raster targets are identical). Shader objects are made one at a time from their payloads, so a linked set replays
unlinked. Descriptor update templates are not created:
sets are written from the snapshots their binds carry, and a push through a template is pushed
again as plain writes from its own.

---

Previous: [Building from source](BUILDING.md) · [Docs index](README.md) · Next: [Architecture](ARCHITECTURE.md)
