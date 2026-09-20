# Capture replay

[Docs index](README.md) › Capture replay

`vkinsp_replay` re-executes a Vulkan capture (`.gpucap`) on this machine's GPU, without the
application. It is the basis for the analyses that have to run a frame again with something
changed: the overdraw heatmap, pixel history, draw-call overlays, mesh output (which the shader
debugger's pixels are rasterized from), per-draw timing and shader cost by ablation. It also writes a
frame out as a C++ project ([Export to C++](#export-to-c)). Direct3D 12 and Metal captures have replay
tools of their own, `dxinsp_replay` and `mtlinsp_replay`, which replay, compare and export
([Direct3D 12](#direct3d-12), [Metal](#metal)).

```
vkinsp_replay <capture.gpucap> [--validate] [--dump <dir>] [--overdraw <dir>] [--overdraw-data <file>]
              [--pixel <image> <x> <y> [--mip <n>] [--layer <n>] [--pixel-data <file>]]
              [--draws [--draw-data <file>]] [--overlay <command> ... [--overlay-data <file>]]
              [--mesh <command> ... [--mesh-data <file>]] [--ablate <request> [--ablate-data <file>]]
              [--counters [--counter <name>]... [--counter-data <file>]] [--list-counters [--counter-data <file>]] [--trace]
              [--export <directory> [--export-data <file>]]
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
- **`--counters`:** reads the GPU's own hardware counters around every render pass (see
  [Hardware counters](#hardware-counters)). `--counter <name>` (repeatable) picks the counters;
  without any, a default limiter set is collected.
- **`--counter-draws`:** measures each draw as well as each pass. A frame with thousands of draws
  takes far longer this way, so it is off by default.
- **`--counter-backend nvperf|khr`:** forces one backend instead of picking whichever the device
  supports. Naming one the device cannot use reports how far it got and why, which is how the
  portable backend is exercised on a driver that does not offer it.
- **`--list-counters`:** lists every counter the GPU offers, without replaying the frame's work.
- **`--counter-data <file>`:** with `--counters` or `--list-counters`, writes the result as JSON.
  GPU Inspector and the MCP server's `get_hw_counters` run the tool this way.
- **`--export <directory>`:** writes the frame, as it replays, as a standalone C++ project (see
  [Export to C++](#export-to-c)). `--export-data <file>` writes a JSON summary of what was written,
  which is how GPU Inspector and the MCP server's `export_cpp` run the tool.
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

    Then it runs once more into a target of the replay's own with its fragment shader replaced by
    one writing `gl_PrimitiveID`, in a render pass of the replay's own over that target and the
    copy of the pass's depth: what is left in the pixel is the **primitive** the winning fragment
    came from (one per draw, and `gl_PrimitiveID` needs the `geometryShader` feature, which the
    replay adds to the device it creates). Finally it runs with its own pipeline, and writes.
  - **`vkCmdClearAttachments`** runs as it is.
  - After each event the pixel and the pass's depth at it are read. A multisampled target cannot be
    copied to a buffer, so its pixel is resolved into a one-pixel image of the replay's own first:
    each value is then what the pixel's samples resolve to.
  - **A shader that asks for the depth and stencil tests before it** (`EarlyFragmentTests`) has them
    on in the `shaded` query, because the hardware never runs such a shader on a fragment they
    killed; the event says so (`earlyTests`), since it changes what the shaded count means.
**A write from outside a render pass** needs none of that: it lands in the image itself, so the
pixel is read straight out of it once the command has run, as its own event.

- A **clear**, a **copy** (from an image or a buffer), a **blit** and a **resolve** say in their own
  arguments which image they write, which part of it, and in what layout, so the event is known
  before the command runs — including whether the written region covers the pixel at the mip and
  layer being followed.
- A **dispatch** or a **trace** writes through a descriptor, which no argument names. What is known
  is what was bound: the event is recorded when a descriptor set bound to that pipeline holds the
  followed image as a storage image, and it says so ("dispatched with the image bound to be
  written"). Whether the shader wrote that pixel is not knowable from outside it; the value after
  the command is, and that is what the event carries.

- **Output.** Each event lists the command index, what became of the draw's fragments, the primitive
  that won the pixel, and the pixel's value and depth after it, decoded for common formats (8-bit
  RGBA and BGRA, half and full float, `A2B10G10R10`, `B10G11R11`, and the depth formats). Draws that
  do not reach the pixel are only counted.

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
- Only the first layer of a layered framebuffer is followed.
- Per-fragment detail is partial: a draw is one event, which names the primitive of the fragment
  that won the pixel but not every fragment of the draw with its own value.
- A multisampled *depth* target cannot be resolved to be read, so a pixel of one is not followed;
  a multisampled colour target is, through the resolve of its samples.
- A dispatch or a trace is reported by what it had bound, not by what it wrote (see above).

## Per-draw timing and counters

`--draws` measures every draw and dispatch of the frame (`draw_stats.cpp`, RenderDoc's
`vk_counters.cpp` does the same). The frame replays exactly as the capture recorded it, with each
action issued between a pair of timestamps and inside a pipeline statistics query:

- **The counters are exact**: vertex and fragment shader invocations, primitives, compute
  invocations — per draw, indirect arguments included, which nothing in a capture itself reports.
  They need the `pipelineStatisticsQuery` feature, which the replay adds to the device it creates.
- **Samples passed** comes from a precise occlusion query around each draw, which is how a pass that
  executes secondary command buffers gets a depth rejection rate on a device without
  `inheritedQueries`: there the layer's own query cannot span `vkCmdExecuteCommands`, while the
  replay's sits inside the secondary. `pass_metrics.ts` sums a pass's draws and uses that where the
  capture's own counter is missing.
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

## Hardware counters

`--counters` reads the GPU's own hardware counters around every render pass and every draw
(`src/replay/src/hw_counters.cpp`). These are the counterpart of what Nsight Graphics shows, and
what docs/PROFILING.md calls **the limiters**: which unit inside the shader core a pass saturates —
SM (shader core) throughput, VRAM bandwidth, L1/texture and L2 cache, achieved occupancy, the ALU
and FMA pipes — rather than the vertex/primitive/fragment tallies a pipeline statistics query gives.
The pipeline statistics say *how much* work a pass did; these say *which unit it waited on*.

The frame is replayed once per collection pass the chosen counters need: the hardware has a fixed
number of counter slots, so a metric set that does not fit in one go is split over several replays,
and each range's counters are summed across them. A single `pct_of_peak` throughput metric already
spans enough raw counters to need dozens of passes, so asking for more of them costs little extra;
the tool prints its progress, since nothing else is printed until the replays finish.

By default a range is opened around each render pass. `--counter-draws` opens one around each draw
as well, which is what gives per-draw numbers, but the profiler serializes work at every range: on a
frame with thousands of draws that turns minutes into much longer, so it is opt-in.

Two backends supply the counters:

- **NVIDIA's Nsight Perf SDK** (`src/replay/src/nvperf.cpp`), the same one RenderDoc uses. It
  profiles named ranges, so the replay pushes a range around each render pass and, with
  `--counter-draws`, a nested one around each draw. The SDK's redistributable headers are vendored in
  `third_party/nvperf`; its host library (`nvperf_grfx_host`) is **not** shipped and is loaded at
  run time from beside the tool, from `VKINSP_NVPERF_DIR`, or from an Nsight Graphics, Systems or
  Compute install on the machine. Build it in with `-DVKINSP_NVPERF=ON` (the default when the
  headers are present).
- **`VK_KHR_performance_query`**, the portable path. `KHR` marks an extension ratified by Khronos,
  the body behind Vulkan, so this is Vulkan's own vendor-neutral way to read counters rather than a
  vendor's SDK. Mesa's AMD (RADV, which may want `RADV_PERFTEST=perfcounters`) and Intel (ANV)
  drivers offer it, as do Arm and Qualcomm mobile drivers; **NVIDIA's desktop driver does not**, so
  on NVIDIA the Nsight Perf SDK above is the only route. Its counters are command-scoped and a
  pool's queries cannot nest, so this path measures **draws, not passes**.

Both need the driver to allow GPU performance-counter access. On NVIDIA that is off for
non-administrators by default: enable it in the NVIDIA Control Panel under *Developer > Manage GPU
Performance Counters > Allow access to all users* (it persists across reboots), or run the tool as
administrator; without it the profiler returns `ERR_NVGPUCTRPERM` and the note says so.

```
vkinsp_replay frame.gpucap --list-counters          # every counter the GPU offers
vkinsp_replay frame.gpucap --counters               # the default limiter set, per pass and per draw
vkinsp_replay frame.gpucap --counter sm__throughput.avg.pct_of_peak_sustained_elapsed \
                           --counter dram__throughput.avg.pct_of_peak_sustained_elapsed
```

`--counter-data <file>` writes the result as JSON (`src/app/src/renderer/hw_counters.ts` reads it):
the counters collected, then each pass's and each draw's value of every one. GPU Inspector and the
MCP server's `get_hw_counters` run the tool this way, keeping the result with the open capture.

**What is left:** the counters are not yet folded into the GPU Bottlenecks report's own bound-stage
verdict; the `VK_KHR_performance_query` path has no portable default counter set (it takes the first
command-scoped counters) and has not yet been run against a driver that offers the extension, only
as far as its precondition check. Metal and D3D12 captures do not replay, so this is Vulkan only; on Metal, the capture
bar's **Xcode Trace** writes a `.gputrace` whose counter sets are Apple's equivalent.

## Export to C++

`--export <directory>` writes the frame as a standalone C++ project: every object, what the frame's
images and buffers held, and every command of its command buffers as plain Vulkan calls, with a
program that runs the frame and compares each render target with the capture's copy. It is for
reproducing a problem outside the application, above all in a driver bug report, where the vendor
wants something to build and run rather than a capture in someone else's format. In GPU Inspector it
is **Export to C++** in the capture bar and on a capture tab's menu. This section is the Vulkan
export; Direct3D 12 and Metal captures export the same way through their own tools
([Direct3D 12](#direct3d-12), [Metal](#metal)).

The source is emitted from the replay's own walk, not from the capture a second way. The exporter
(`src/replay/src/exporter.h`) watches the replay create the device and each object, upload each
image and buffer range, write each descriptor set, record each command, submit, and read each
target back; and it spells every one of them as it happens, with the create info the replay
actually handed the driver. So the program does what the replay did: where the replay reproduces a
fault, so does the source, and what the replay left out is left out with a comment saying why.

The spelling is generated from vk.xml, like the decoders. `tools/vkgen/emit_source.py` writes an
emitter for every struct, union, `pNext` chain and `vkCmd*` (`src/replay/gen/vk_emit.gen.*`) that
turns a decoded value into source text:

- **Structs** are designated initializers, so they read like the specification. A struct with few
  members and no pointers goes on one line; a features struct lists only what is on.
- **Pointers** become locals declared ahead of the statement: a struct by address, an array as an
  array, a `pNext` chain struct by struct.
- **Enums and flags** by name, **handles** as the variable of their object, named by type and
  capture id: `image_18` is image 18 in GPU Inspector.
- **Blobs** (SPIR-V, push constants, `vkCmdUpdateBuffer` data) and long scalar arrays are
  `Data(offset, size)` into `frame_data.bin`, which also holds the image and buffer contents and the
  captured targets. Identical contents are stored once.
- **Unions without a selector** by their largest member, as the decoder fills them, so a clear
  value is its exact bits, with the float reading in a comment.

```
vkinsp_replay frame.gpucap --export frame_cpp
export to C++: frame_cpp
  31 objects, 18 commands in 1 submission, 2 render targets compared, 2.4 MB of data, 30 files

cmake -S frame_cpp -B frame_cpp/build && cmake --build frame_cpp/build --config Release
frame_cpp/build/Release/frame --validate
device: NVIDIA GeForce RTX 4080
render targets: 2
  image18_cb7_pass0_att0 (640x480): identical to the capture (307200 texels)
  image22_cb7_pass0_att1_depth (640x480): identical to the capture (307200 texels)
```

The project needs CMake and a C++20 compiler and nothing else: it carries the Vulkan headers it was
written against (`vulkan_headers/`, embedded into the tool from `third_party/Vulkan-Headers`), opens
the loader at run time and links against nothing. An SDK a year older than these headers lacks names
the source uses (a promoted extension's struct), which is why it does not rely on one. Its hand-written part (`vk_support.*`, `main.cpp`: memory, uploads,
layout tracking, read-backs, PNG output) lives in `src/replay/export_template` as real sources and
is embedded into the tool. The generated part is split into functions of about two thousand lines
and files of about twenty-four thousand, so a large frame compiles in ordinary memory. The project's
README says how the frame differs from the application's (memory per resource, no swapchain,
pipelines one at a time from the capture's SPIR-V, no semaphores) and lists what was left out.

Checked by building and running the exported program, with the validation layer, on an RTX 4080:

| Capture | The exported program |
|---|---|
| test/triangle (render pass, compute, texture, push constants) | identical, no validation messages |
| test/triangle `--hazard` (two submissions, `vkCmdUpdateBuffer`) | identical, no validation messages |
| test/triangle `--msaa`, and with `--stencil` | identical: the multisampled colour, depth and stencil through their resolves, and the resolve target |
| test/triangle `--shader-object`, `--suspend` (dynamic rendering, suspended and resumed) | identical, no validation messages |
| test/triangle `--pipeline-library`, `--push-template`, `--stencil`, `--occluded` | identical, no validation messages |
| test/triangle `--second-queue`, `--second-device` | all 3 targets identical, no validation messages |
| test/triangle `--persistent` (frame-start contents, a mip in another layout) | all 5 targets identical, no validation messages |
| test/triangle `--ray-tracing` | the raster targets identical; the builds and the trace are left out, and the traced image is not compared (see the limits) |
| test/triangle `--descriptor-buffer` | differs in exactly the 53,759 texels the replay differs in, which does not replay descriptor buffers |
| Unity player frame (secondary command buffers, two subpasses, MRT, BC1) | all 8 replayed targets identical, no validation messages; the 8 commands the replay left out are left out |
| XR frames captured on an Adreno 740 (multiview, two layers) | differ from the capture in exactly the texels the replay differs in |

The Unity frame was also exported with parts of 150 lines and files of 700
(`VKINSP_EXPORT_PART_LINES`, `VKINSP_EXPORT_FILE_LINES`), which cuts it into 9 functions over 2
files and its objects over 5, and builds and runs the same. The generated sources of the Unity,
multisampled and ray tracing projects also pass clang 18 with `-Wall -Wextra`, which is stricter
than MSVC about narrowing and designator order.

Limits:
- The export needs the replay to run to the end, so a frame that crashes the driver is not
  written; `--trace` names the call it dies in.
- Ray tracing is not exported: a build and a trace name what they read by device address and by
  shader group handle, which the replay finds at run time, and the source has no spelling for that
  yet. Those commands are left out with a comment, and an image only a shader writes is then not
  compared: it would still hold the contents uploaded for it, and match the capture for no reason.

## Direct3D 12

`dxinsp_replay` (`src/d3d12/replay/`, Windows) re-executes a Direct3D 12 capture, and is what
**Export to C++** runs for one. It replays the frame and compares its render targets, and it writes
the frame out as a C++ project; the analyses above are `vkinsp_replay`'s and stay Vulkan-only.

```
dxinsp_replay <capture.gpucap> [--debug-layer] [--trace]
dxinsp_replay <capture.gpucap> --export <directory> [--export-data <file>]
```

- `--debug-layer` runs the replay under the D3D12 debug layer and prints its messages, grouped.
- `--trace` names each command on stderr before it is issued, to find the one a driver dies in.
- The exit code is 0 when every compared target is identical, 1 when some differ or could not be
  compared, 2 when the replay could not run. `DXINSP_REPLAY_DUMP=<directory>` writes both sides of a
  target that differs as raw bytes.

It shares the capture reader with `vkinsp_replay` (`gpucap.*`, `json.*`: no graphics API in them).
The rest is its own, because a D3D12 capture's arguments are written by hand
(`src/d3d12/src/serialize.cpp`) rather than generated from a registry. One description per struct
(`dx_reflect.h`) is visited twice: by the decoder, which fills the struct from the capture's JSON,
and by the emitter, which spells the same struct as C++.

How the frame is rebuilt:

- **Resources** are all committed, whatever they were: a placed or reserved resource needs its heap
  and its offset only to alias another, which a frame's replay does not depend on. A swap chain's
  buffers become textures of their description.
- **States.** The capture records no resource states, so each subresource starts in the state the
  frame first expects of it: the `StateBefore` of its first transition, else what its first use
  needs. Upload and readback heaps keep the state they require.
- **Contents** are what the capture read back: a sampled texture as it was when a table that holds
  it was drawn with, a buffer range where a command read it (a vertex or index buffer at its bind,
  a constant buffer at its root bind or in its table, indirect arguments at the call).
- **Descriptors.** The capture holds no `CreateShaderResourceView` calls, only what each bound
  table held. The replay writes those views into its own heaps at the same slots as it records the
  bind, and leaves a slot alone that already holds the same view. A slot given other contents after
  a draw of the same submission bound it is reported, since every write lands before the
  submission runs.
- **Root signatures** are serialized again from their description, **pipelines** created from their
  description with the capture's bytecode (a pipeline stream becomes the graphics or compute
  description it amounts to, and one loaded from a pipeline library is created from the
  description it was loaded with).
- **Command lists** are recorded from their `Reset` to their `Close` and executed where the capture
  executed them, each submission waited for. A list or an allocator the capture has no object for
  (an engine that releases them as it goes may have released one before the capture was saved) is
  made from its type. A bundle is recorded from the commands the capture inlines after its
  `ExecuteBundle`, once.
- **Read-backs** are taken where the capture took them: when a pass ends (the capture's
  `EndRenderTargets` marker, or `EndRenderPass`), a multisampled colour target through a resolve,
  depth and stencil as their planes. The top byte of a 24-bit depth texel is undefined and ignored.
  A target its render pass ends by discarding is not compared, since what it holds afterwards is
  undefined (the debug layer overwrites it), and neither is one the capture failed to read back;
  neither counts against the exit code.

Two things in the capture library exist for the replay, and improve what GPU Inspector shows too:

- A **descriptor table is snapshot when the list next draws or dispatches** with it, not when it is
  bound. A table names slots of a heap, and an engine may bind it and then write the descriptors
  (Unity does, for every draw), so a snapshot at the bind held what the slots had the frame before.
  Captures taken before this show, and replay with, the wrong textures in such a frame.
- A **bundle's snapshots are taken again by the list that executes it**, during a capture. A bundle
  is recorded once, usually before any capture (keep its recording with **Record always**), so what
  it snapshot then held no contents: its vertex and index buffers were not in the capture at all.

The exported project is the same idea as Vulkan's: `frame_create*.cpp` makes the objects,
`frame_contents.cpp` uploads the textures, `frame_commands*.cpp` uploads buffer ranges, writes
descriptors and records and executes the lists, and `main.cpp` compares each target with the
capture's copy and writes them to `out/`. Structs are declared zeroed and assigned member by
member, which is what D3D12's anonymous unions allow, leaving out members that are zero. Objects are
named by type and capture id (`texture_186`), a GPU address is its buffer's address plus an offset,
a descriptor handle is a slot of its heap. It needs CMake, a C++20 compiler and the Windows SDK, and
links `d3d12`, `dxgi` and `dxguid`. `--debug-layer` runs it under the debug layer. The hand-written
part is `src/d3d12/replay/export_template`. The sources split as Vulkan's do, with the same
`VKINSP_EXPORT_PART_LINES` and `VKINSP_EXPORT_FILE_LINES`.

Checked on an RTX 4080 with the debug layer, replayed and then exported, built and run:

| Capture | The replay, and the exported program |
|---|---|
| test/triangle/d3d12 (table of a constant buffer and a texture, root constants, static sampler) | identical, colour and depth, no debug layer errors |
| `--msaa` | the resolved colour identical; the capture does not read multisampled depth back |
| `--bundle` with **Record always** | identical: the draw is in a bundle recorded at start-up |
| `--indirect`, `--compute`, `--offscreen` (no swap chain) | identical |
| `--render-pass` (`BeginRenderPass`), `--stencil` (D24S8, depth and stencil planes) | identical |
| Unity URP sample scene (83 command lists a frame recorded by jobs, 60 render passes suspended and resumed across them, 725 draws, pooled and per-frame lists) | the final image identical in 8 captures of 10, with no debug layer errors, and the exported program the same. One of the other two was a frame of adopted lists, which has no targets read back to compare; one differed in its post-processing, which reads textures the frame overwrites |
| Unity URP player frame (9 passes, pipeline library, root constant buffer views, D32S8, BC1 and BC3, SSAO, bloom) | all 17 targets identical; the 2 a pass discards are not compared. Cut into parts of 40 lines and files of 300 it builds and runs the same |

A frame the driver cannot run ends the replay without taking the export's reason with it: a removed
device is reported once, with its reason and the submission it followed, and nothing after it is
issued; a crash writes the export summary with the exception and the command it happened at, which
is what GPU Inspector shows. `DXINSP_REPLAY_CRASH_AT=<command index>` makes one, to test that.

Limits:
- A capture taken on demand of an engine that records its lists frames ahead is not always whole.
  The capture library records a frame before the capture, adopts lists it meets without a recorder
  and takes what suspended passes read after their submission (`src/d3d12/README.md`, "Passes"),
  which makes most captures of a Unity frame replay exactly; a list recorded entirely before the
  capture was asked for is still missing, and **Record always** from launch is what avoids it.
- Ray tracing is left out (state objects, builds, `DispatchRays`), as are video, work graphs and
  meta commands: each such command is reported, and in the export is a comment where it would be.
- What the frame reads with no command naming it is not in the capture: a buffer reached through
  a GPU address inside another buffer, a descriptor indexed out of the heap directly (shader model
  6.6). Multisampled textures are not uploaded, and multisampled depth is not compared.
- Queries are issued but their results are not compared, and fences, tiled resource mappings and
  residency are not replayed.

## Metal

`mtlinsp_replay` (`src/metal/replay/`, macOS) re-executes a Metal capture, and is what **Export to
C++** runs for one. Like `dxinsp_replay` it replays the frame, compares its render targets and
writes the frame out as a project; the analyses above are `vkinsp_replay`'s and stay Vulkan-only.

```
mtlinsp_replay <capture.gpucap> [--validate] [--dump <dir>] [--trace]
mtlinsp_replay <capture.gpucap> --export <directory> [--export-data <file>]
```

- `--validate` sets `METAL_DEVICE_WRAPPER_TYPE=1` before the device is made, so the replay runs
  under Metal's API validation. Metal has no message list to read back, as D3D12's info queue is, so
  what it finds goes to stderr and a hard error aborts the process.
- `--dump <dir>` writes both copies of every compared target as raw bytes.
- `--trace` names each object and command on stderr before it is replayed, to find the one a driver
  dies in.
- The exit code is 0 when every compared target is identical, 1 when some differ or could not be
  compared, 2 when the replay could not run.

It shares the capture reader with `vkinsp_replay` (`gpucap.*`, `json.*`, `arena.h`: no graphics API
in them) and the pixel format tables with the capture library (`src/metal/src/formats.h`). The rest
is its own. A Metal capture's arguments are written by hand
(`src/metal/src/hooks_descriptors.mm`), as D3D12's are, so one description per descriptor
(`mtl_reflect.h`) is visited twice: by the filler, which sets the descriptor's properties from the
capture's JSON, and by the emitter, which spells the filled descriptor as Objective-C++. Metal's
descriptors are objects rather than C structs, so a property reaches the visitor as the value it
holds, the value a freshly allocated descriptor of the same class holds, and a block that sets it —
which is what lets the emitter write only what the application actually set. The enum name tables
the two directions share are generated from the Metal SDK headers by `tools/gen_metal_enums.py` and
committed (`src/metal/gen/`).

How the frame is rebuilt:

- **Objects** are re-created in the order the capture created them. A library is compiled from the
  Metal Shading Language the capture kept with it, or loaded from its metallib bytes; a function is
  specialized again from the constants the capture watched the application set
  (`src/metal/src/function_constants.h`), since Metal will not report them.
- **The drawable** has no window here, so its texture becomes an ordinary render target and the
  `presentDrawable:` that would have shown it is left out and reported.
- **Buffers** keep the storage mode they had, except that a memoryless one becomes private: the
  contents the capture read are written straight into a shared or managed buffer, and through a
  staging blit into a private one.
- **Contents** are what the capture read back: the textures a draw sampled, uploaded before the
  frame, and each buffer range a command bound, written before the command buffer that binds it is
  committed. The same range bound at every draw is uploaded once.
- **Encoders** come from the command stream: every command names its command buffer and its encoder
  (`CaptureCommand.encoder`), and a pass counter per command buffer matches the capture's, which is
  what the read-backs are keyed by. A parallel encoder's sub-encoder has no recorded `endEncoding` —
  its end is not the pass's (`E_endEncoding` in `src/metal/src/hooks_encoders.mm`) — so it is closed
  when the stream moves back to its parent.
- **Store actions.** A target the pass would discard is stored instead, by the same rule the capture
  applied when it took the frame (`ForceStore`), so the two read the same targets: one the capture
  declined to force is one it also declined to read.
- **Read-back.** Every target the capture read at the end of a pass is read here at the same point,
  through a blit into a staging buffer, and compared byte for byte. A multisampled attachment is
  read through its resolve, which is what the capture read.

Checked on an Apple M1 Max, replayed and then exported, built and run:

| Capture | The replay, and the exported program |
|---|---|
| test/metal_triangle (compute pass, multisampled pass through a parallel encoder resolving into a texture the next pass samples, function constants, a sampler, inline bytes) | both targets identical |
| `--occluded` (a depth attachment the pass discards, two draws) | all three targets identical, depth included |
| `--present-direct` (the drawable presented by the application rather than the command buffer) | both targets identical |

Limits:
- Acceleration structures and ray tracing, indirect command buffers, argument encoders and mesh
  shader draws are not replayed: each such object or command is reported, and in the export is a
  comment where it would be.
- Tile shading (`setImageblockWidth:height:`, `dispatchThreadsPerTile:`) is recorded by the capture
  but is not on `MTLRenderCommandEncoder` in the macOS SDK, so it is left out.
- Events and fences within the frame are replayed; the replay commits each command buffer and waits
  for it before the next, so cross-frame synchronization does not arise.
- What the frame reads with no command naming it is not in the capture — a buffer reached through a
  `gpuAddress` held in another buffer, or through an argument buffer.

Worth keeping: the first frame with a depth attachment differed in every texel of its **colour**
target, and the fault was the capture's. A depth attachment is announced under attachment index 0,
the same as colour attachment 0, and `CaptureTextureData` did not carry the aspect — so the depth
read-back matched the colour entry and landed on top of it. The Vulkan layer had always sent the
aspect for exactly this reason; the Metal one now does too (`SendTextures` in
`src/metal/src/capture.mm`). Nothing in the UI had shown it, because a depth image and a colour
image of the same pass both render as an image.

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
- **A frame captured while `replace_shader` was active, by a layer from before v0.12** reads back
  the edited shader's output while its bind names the original pipeline, which is what the replay
  draws. The layer now records the bind as the replacement (an object with the edited code, the
  original beside it as `replaced`), so such a frame replays with the edit.
- **Captures older than sampled-image read-back** have no texture contents to upload.
- **Captures older than frame-start contents** start images the frame loads or copies from at zero,
  and copy zeros from staging buffers the host wrote.

## What is left

1. **Replayable captures, the rest.** The capture holds what the frame reads before writing it
   through passes and transfers, and the replay starts every subresource in its own layout. Still
   missing:
   - The contents of multisampled images a frame loads (they cannot be read back into something a
     replay could upload yet).
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

Ray tracing replays, builds and traces alike. Pipelines and acceleration structures are made, and
`vkCmdBuildAccelerationStructuresKHR` is issued with its addresses remapped: a build names the
geometry it reads by device address, and an address from the captured process means nothing here,
so the replay uses the buffer and offset the layer recorded for each one to find its own buffer and
ask the driver where it put it. Scratch is the replay's own, since scratch holds no input, and each
build in a submission gets a stretch of its own rather than all of them sharing one: builds recorded
with nothing ordering them are legal when the application gave each its own scratch, and overlapping
them would introduce a hazard the frame never had. A build
reading memory the capture could not tie to a buffer is left out rather than issued, because the
driver rejects a build whose geometry address is not one of its buffers.

`vkCmdTraceRaysKHR` replays as well. Its shader binding table cannot be uploaded as it was
captured: every record begins with an opaque handle the captured driver gave for a shader group,
and that handle names nothing here. So the replay builds a table of its own, copies each region's
bytes into it, and rewrites every record's handle with this driver's handle for the group the
captured handle named — matched through the handle blob the layer keeps on the pipeline. The bytes
after the handle are the application's own shader record data and are copied unchanged. A record
whose handle this pipeline never gave out is reported as a problem and left as it was.

What a frame computes into an image is compared too, not only what it draws into a target: every
image the capture read back that a shader could have written (STORAGE usage) is read back again at
the end of its command buffer and compared, listed with a pass index of `-` because it belongs to
no pass. Without that, a trace that runs and a trace that produces the wrong pixels look alike.

A traced image will differ when the bottom level it traces against was built *before* the capture
began, which is the usual thing: an engine builds its bottom levels once at load. The replay
creates the structure but has nothing to build it from, so the rays miss. A capture that holds the
bottom level's own build — which `test/triangle --ray-tracing` makes, rebuilding both levels every
frame as an engine with deforming geometry does — replays it, and the traced image comes back
identical.

That comparison earns its keep. The first thing it found was not a fault in the replay at all: the
test application had no barrier between the top level's build and the trace that reads it, and on
this driver the race resolved in its favour often enough that the frame looked right every time it
was run. Synchronization validation does not report that hazard. Two runs of identical commands
disagreeing does, which is what a replay is for.

Not replayed yet: `vkCmdTraceRaysIndirect*`, the NV ray tracing commands, acceleration structure
copies, queries whose results the frame reads back, and Metal captures. Shader objects are made one at a time from their payloads, so a linked set replays
unlinked. Descriptor update templates are not created:
sets are written from the snapshots their binds carry, and a push through a template is pushed
again as plain writes from its own.

---

Previous: [Building from source](BUILDING.md) · [Docs index](README.md) · Next: [Architecture](ARCHITECTURE.md)
