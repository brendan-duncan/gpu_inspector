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
- **`--validate`:** enables the Khronos validation layer and lists its messages. **`--validate-data
  <file>`** writes them as JSON, each with the captured command the replay was re-issuing when it
  fired and the phase (`setup`, `frame` or `submit`), which is what the Validate report and the MCP
  server's `get_validation` with `replay: true` read. The layer's settings come from the
  environment as for a launch (`VK_LAYER_VALIDATE_SYNC=true` for synchronization validation).
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
- **`--replace <request>`:** replays the frame with other code for some pipelines' stages (see
  [A shader edited in the capture](#a-shader-edited-in-the-capture)). Not with `--serve`, which
  made its pipelines before the request arrived.
- **`--target-data <file>`:** writes every compared render target with how far it is from the
  capture's copy, and the pixels of the ones that differ. The shader editor's **Compile & Replay**
  runs the tool this way.
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
- **Discards.** Where the pipeline's fragment shader discards (`OpKill`, `OpTerminateInvocation`,
  a demote to a helper) or writes depth, the sample mask or the stencil reference, the copy runs
  that shader itself, edited to count (`count_patch.cpp`): its color outputs become private
  variables, and an output of its own at location 0 is written 1.0 first thing. Its discards and
  depth then decide what is counted, as they decided what was drawn. A shader that writes memory (a
  storage buffer or image, an atomic) keeps the constant shader, since drawing it again would repeat
  the writes, and the replay says so among its problems. Every other shader counts the same either
  way, so it keeps the cheaper constant one.
- **Multiview.** A multiview pass (an XR frame's eyes) is counted in every view. The count target
  and the depth copy have a layer per view, and the counting render pass has the pass's view mask
  (`VkRenderPassMultiviewCreateInfo`, or `viewMask` in dynamic rendering, where the copies take it
  too). Each view is a measurement of its own, with its `view` index, and its own heatmap. Checked
  on two XR frames captured on an Adreno 740: the views' tested counts add up to the draw's
  occlusion count exactly (69,252 + 67,826 = 137,078, and 62,368 + 62,073 = 124,441), and so do
  test/triangle `--multiview` in a render pass (67,698 + 67,723 = 135,421) and with
  `--dynamic-rendering` (64,973 + 64,969 = 129,942), and with `--alpha-test --occluded` there, whose
  discards and fully rejected second draw leave 41,309 + 41,295 = 82,604 + 0.
- **Layered passes.** A pass whose framebuffer has several layers, drawn into through `gl_Layer`
  (single-pass cube maps and shadow cascades), is counted in every layer the same way: the count
  target and the depth copy have the framebuffer's layers, the counting framebuffer (or
  `layerCount` in dynamic rendering) has them too, and each layer is a measurement of its own with
  its `view` index and `"layered": true`. test/triangle `--layered` checks it: 68,095 + 68,098 =
  136,193, the draw's occlusion count, in a render pass; its layers also split as expected in
  dynamic rendering, with shader objects and with `--alpha-test --occluded`, with no validation
  messages.
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
  51,204 fragment shader invocations its capture measured with pipeline statistics. With
  `--alpha-test`, whose shader discards the checker's dark squares, it counts 25,342 of the 50,655
  invocations: exactly the samples the draw's occlusion query (`--draws`) says passed.

Limits:
- A shader that discards and writes memory counts every fragment it rasterized.
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

A draw bound with shader objects (`VK_EXT_shader_object`) has no pipeline to copy
(`shader_objects.cpp`):

- **The copy** is the draw's own shader objects with the fragment stage bound to one of the
  replay's shaders. That shader is made with the draw's descriptor set layouts and push constant
  ranges, since every shader object bound together must share them.
- **What a pipeline copy bakes in** is set as dynamic state right before the draw, over what the
  application set: blending, color writes, one sample, the tests, culling, the polygon mode, and a
  scissor for every viewport.
- **Dynamic rendering.** Shader objects draw only in dynamic rendering, so a pass of the replay's
  own that holds one begins with `vkCmdBeginRendering`. The pipeline copies drawn beside it are made
  for dynamic rendering too.
- **Mixed passes.** A pipeline copy holds statically some state the application left dynamic. So
  the application's dynamic state is set again before the next shader-object draw, and a
  `vkCmdSet*` for a state the bound pipeline holds statically is not issued.
- **The geometry stage.** Pixel history turns on the `geometryShader` feature for `gl_PrimitiveID`.
  With it on, a shader-object draw must bind the geometry stage, so the replay binds it to none
  after each of the application's binds.

Pixel history does the same for its six variants, the primitive-id pass and the per-fragment runs.
Checked on test/triangle `--shader-object` and `--mixed` (a shader-object draw and a pipeline draw
in one pass) with the validation layer: overdraw, draw overlays, VS Out, pixel history and
ablation, no messages. The tested overdraw count matches the fragment shader invocations each
capture measured.

## Draw-call overlays

`--overlay <command>` shows where one draw landed, the way RenderDoc's texture viewer overlays do
(`vk_overlay.cpp`). It is recorded right after the replay has executed the draw's pass, like
overdraw, and `overlay.cpp` issues the pass again into an `R16_SFLOAT` target up to and including
the draw, five times:

- **Rasterized:** the draw alone, with the counting fragment shader and no depth or stencil tests.
- **Passed:** from a copy of the depth the pass started with, the pass's earlier draws move the
  depth and stencil without writing color, then the draw runs with its own tests.
- **Wireframe:** the draw alone with `VK_POLYGON_MODE_LINE`, which needs the `fillModeNonSolid`
  feature the replay adds to its device.
- **Stencil:** the same as Passed with the depth test off, so what it reports is the stencil test's
  doing alone. Only where the pass's depth-stencil format has a stencil aspect.
- **Back-facing:** the draw with nothing culled and a shader that writes only for back faces
  (`kBackFaceFragmentSpirv`), which marks the pixels its own cull mode emptied.

They fold into one byte per pixel: bit 0 rasterized, bit 1 passed depth and stencil, bit 2 an edge,
bit 3 passed the stencil test alone, bit 4 culled away — a back face landed there and no front one
did, so a pixel of a closed mesh never carries it. Each draw also reports its fragments and its
covered, passed, rejected, stencil-rejected and culled-away pixels:

```
vkinsp_replay frame.gpucap --overlay 18
draw overlays: 1
  [18] vkCmdDrawIndexed (command buffer 7, pass 0): 51076 fragments on 51076 of 307200 pixels (16.63%); 0 passed depth and stencil, 51076 rejected, wireframe drawn
```

That is the triangle's `--occluded` mode, which draws the cube a second time where it already is,
so every fragment of the second draw fails its `LESS` depth test.

A draw whose shader discards is drawn with its own shader, as overdraw counts it (above), so
Rasterized, Passed and Stencil leave out what it discards; Wireframe and Back-facing are its geometry
alone. On a Unity frame the text quads now show 2,089 fragments passing, the 2,089 samples the draw's
occlusion query (`--draws`) counts; the constant shader had shown 3,480, its whole glyph quads.

Limits:
- A shader that discards and writes memory shows as covered where it discarded.
- A pass the replay leaves out, and a draw whose pipeline cannot be copied, have no overlay.

## Mesh output

`--mesh <command>` captures what a draw's last stage before rasterization wrote: RenderDoc's VS Out,
or GS Out and DS Out past a geometry or tessellation shader. RenderDoc turns the vertex shader into a
compute shader (`vk_postvs.cpp`); `mesh.cpp` uses transform feedback (`VK_EXT_transform_feedback`),
which the replay enables where the GPU has it. The edit to the shader is much smaller:

- **The shader** (`xfb_patch.cpp`) is the last stage before rasterization: the geometry shader if the
  draw has one, else the tessellation evaluation shader, else the vertex shader, since transform
  feedback records that stage's output. It gets the `TransformFeedback` capability, the `Xfb`
  execution mode on its entry point, and `XfbBuffer`, `XfbStride` and `Offset` on each output it can
  capture: `gl_Position`, `gl_Layer` and `gl_ViewportIndex`, and every located output of 32-bit
  floats or integers (scalars, vectors, matrices, arrays, and the members of an output block). Point
  size, clip distances and 64-bit outputs are left out.
- **Past a geometry or tessellation stage** the primitives come out as lists: the topology reported
  is the list that stage emits, from its execution modes (a geometry shader's output primitive; a
  tessellator's domain, or points in point mode, from the evaluation shader or else the control
  shader). A geometry shader's buffer is sized from its declared vertices per primitive; a
  tessellator's has room for 1,024 vertices per input vertex.
- **Multiview.** Transform feedback cannot be active in a multiview pass, so a multiview draw is
  issued once per view, in a pass without a view mask, with `gl_ViewIndex` in every stage made that
  view's constant (the built-in input becomes a private variable initialized to it). Each view is a
  mesh of its own, with its `view` index.
- **Layered passes.** The pass the draw is issued in has the framebuffer's layers, so a shader that
  writes `gl_Layer` is valid there, and `gl_Layer` is among the outputs captured.
- **The draw** is issued after the replay has executed its pass, like the overlays: the pass's
  state again, then the draw alone with a pipeline copy that uses the edited shader, has no fragment
  stage and discards rasterization. A draw with shader objects (`VK_EXT_shader_object`) binds a copy
  of its last pre-rasterization shader object made from the edited SPIR-V instead, no fragment
  shader, and rasterizer discard as dynamic state; it is issued in dynamic rendering, which shader objects need, where a
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
multiview XR frame: no messages from the edited shaders or the feedback. The stages past the vertex
one and the views, on test/triangle, with pipelines and with shader objects, all with no validation
messages:
- `--geometry`: each of the geometry shader's 72 vertices is the input triangle's, or the copy it
  shifts right by 0.3w, exactly.
- `--tessellation`: 216 vertices, six triangles a patch, each at the barycentric mix of its patch's
  corners that its written `gl_TessCoord` names, to float rounding; with `--geometry` too, the 432
  the geometry shader makes of them.
- `--multiview`, with `--geometry` (`gl_ViewIndex` read in the vertex shader, the geometry shader
  captured) and with `--tessellation`: view 1 is view 0 shifted by the 0.24w the shaders put
  between them, exactly. On the Adreno XR frames each eye's projected mesh covers the same pixels as
  that eye's overdraw.
- `--layered`: 72 vertices, `gl_Layer` 0 for the first instance and 1 for the second.

Limits:
- Mesh shader pipelines are not captured.
- A GPU without `VK_EXT_transform_feedback` (most mobile GPUs, MoltenVK) cannot capture VS Out.
- A tessellated draw that emits more than 1,024 vertices per input vertex is marked truncated.

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

**Multiview.** A pixel in any view of a multiview pass (an XR frame's eyes) is followed in that view
alone. The pass is replayed with its view mask narrowed to the one view, through a single-view copy
of its render pass (or `viewMask` in dynamic rendering), and every pipeline bound meanwhile is a copy
made for it (`ViewPipeline`), as are the variants, the primitive-id pass and the per-fragment runs.
The shaders then see the view's own index, and the queries and the one-pixel scissor meet that
view's fragments only; views never read one another's layers, so nothing the followed view sees
changes. The attachments' copies hold the layer of that view. On two XR frames captured on an Adreno
740, pixels in both eyes come out at exactly the values the capture read back for their layers, with
as many fragments as each eye's overdraw counted there, and no validation messages; so do both
views of test/triangle `--multiview`, in a render pass and in dynamic rendering. Before this, any
pixel history of such a frame crashed the replay. In the single-view pass the history also binds
the pass's descriptor sets, buffers and push constants again after each pipeline it binds: the
validation layer reports what was bound before a multiview pass began as unbound in it, although
the draws find it.

**Layered passes.** A pixel in any layer of a pass layered through `gl_Layer` is followed in that
layer. Its draws land in every layer at once, so every pipeline bound is copied with its last
pre-rasterization stage (geometry, else tessellation evaluation, else vertex) edited
(`layer_patch.cpp`): where it finishes a vertex, `gl_Position` moves outside the clip volume unless
the vertex's `gl_Layer` is the followed one, so a primitive aimed at another layer is clipped whole.
A shader that writes no layer draws into layer 0, and is clipped entirely when another layer is
followed. Shader objects get the same edit on their last pre-rasterization stage. The attachments'
copies have every layer of the framebuffer. On test/triangle `--layered`, in a render pass, in
dynamic rendering, with shader objects and with `--alpha-test --occluded`, a pixel in either layer
comes out at exactly the value the capture read back for that layer, with no validation messages,
and a pixel only the other layer's cube covers shows the draw with no fragments. A shader whose
code could not be edited is noted, and its draws are counted in every layer.

Limits:
- A mesh shader pipeline's draws in a layered pass are counted in every layer, not only the
  followed one.
- A multisampled *depth* target cannot be resolved to be read, so a pixel of one is not followed;
  a multisampled color target is, through the resolve of its samples.
- A dispatch or a trace is reported by what it had bound, not by what it wrote (see above).

## Per-draw timing and counters

`--draws` measures every draw, dispatch and ray trace of the frame (`draw_stats.cpp`, RenderDoc's
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
  changes. For a draw bound with shader objects, the variant is the stage's shader object made
  again with its code, and the depth and stencil writes are turned off as dynamic state.
- After each bind comes one untimed draw.
- Then `repeat` draws run between one pair of timestamps, and the time is divided back to a
  single draw.
- Variants rotate order each round, and the first round is a warm-up.
- A variant's time is the median of its rounds.

The captured pipeline or shader object is then bound again, and the draw runs as recorded.

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
frame_cpp/build/Release/frame                      # a window, the frame run in a loop until it is closed
frame_cpp/build/Release/frame --batch --validate   # once, headless, compared with the capture
device: NVIDIA GeForce RTX 4080
render targets: 2
  image18_cb7_pass0_att0 (640x480): identical to the capture (307200 texels)
  image22_cb7_pass0_att1_depth (640x480): identical to the capture (307200 texels)
```

### The window, and `--batch`

Run with no arguments, the exported program opens a window and runs the frame in it again and
again until the window is closed (or Escape is pressed): a stand-in for the application that a
profiler or a frame debugger can be attached to, which needs a present to tell one frame from the
next. The same holds for the three APIs:

- **What is shown** is the swapchain image (swap chain buffer, drawable) the frame wrote last, which
  is what the application presented, or the frame's last color target for a renderer that never
  presents. The replay names it (`FrameOutput`), with the layout or state the frame leaves it in. It
  is copied to a swapchain of the program's own, so the frame itself still never presents: a blit in
  Vulkan, which converts formats, and a copy in Direct3D 12 and Metal, where the program's swapchain
  takes the output's own format. An output no swapchain can hold (multisampled, a format no display
  takes) is said so, and the program runs as `--batch`.
- **Between two runs** `RestoreFrame` puts back what the frame changed: command pools or allocators
  reset, and every image or resource moved from the layout or state the frame leaves it in to the
  one its first barrier expects (the replay knows both ends). Contents are not restored, since that
  would upload every texture again each frame; a texture the frame reads and then overwrites holds
  the last run's result from the second frame on, as it would in the application.
- **Buffer uploads are gathered** into one command buffer and one wait per submission. A frame binds
  thousands of ranges, and a staging buffer and a wait for each made a Unity frame loop at under 5
  frames a second; gathered, it runs at the display's rate.
- **The window itself** is `frame_window.h` with one source per platform beside it, and nothing of a
  graphics API in any of them: `frame_window_win32.cpp`, `frame_window_x11.cpp` (Xlib, which needs the
  X11 development files; without them CMake builds the program without its window) and
  `frame_window_cocoa.mm` (an `NSWindow` whose view is a `CAMetalLayer`, for Metal and for Vulkan
  through MoltenVK). The project's `CMakeLists.txt` picks the one for the platform it builds on. They
  live in `src/replay/export_template` and all three exporters embed what they need.
- `--frames <n>` closes the window after that many frames, `--no-vsync` presents without waiting for
  the display, and the title shows the frame rate. Validation and debug layer messages are printed
  once each, so a looping frame does not repeat them.

`--batch` is what the program did before it had a window: the frame once, without one, each render
target the capture read back compared byte for byte with the capture's copy and written to `out/`,
and an exit code of 0 only when all are identical. It is the mode to script, and what runs by
itself where no window can be opened. With a window the read-backs are not taken at all.

The project needs CMake and a C++20 compiler and nothing else: it carries the Vulkan headers it was
written against (`vulkan_headers/`, embedded into the tool from `third_party/Vulkan-Headers`), opens
the loader at run time and links against nothing. An SDK a year older than these headers lacks names
the source uses (a promoted extension's struct), which is why it does not rely on one. Its hand-written part (`vk_support.*`, `main.cpp`: memory, uploads,
layout tracking, read-backs, PNG output) lives in `src/replay/export_template` as real sources and
is embedded into the tool. The generated part is split into functions of about two thousand lines
and files of about twenty-four thousand, so a large frame compiles in ordinary memory. The project's
README says how the frame differs from the application's (memory per resource, a swapchain of the
program's own, pipelines one at a time from the capture's SPIR-V, no semaphores) and lists what was left out.

Checked by building and running the exported program, with the validation layer, on an RTX 4080,
both as `--batch` and in its window for 90 frames. The window adds no validation message to any of
these, and only the Win32 window has been run: the Xlib and Cocoa ones are written and not yet built.

| Capture | The exported program |
|---|---|
| test/triangle (render pass, compute, texture, push constants) | identical, no validation messages |
| test/triangle `--hazard` (two submissions, `vkCmdUpdateBuffer`) | identical, no validation messages |
| test/triangle `--msaa`, and with `--stencil` | identical: the multisampled color, depth and stencil through their resolves, and the resolve target |
| test/triangle `--shader-object`, `--suspend` (dynamic rendering, suspended and resumed) | identical, no validation messages |
| test/triangle `--pipeline-library`, `--push-template`, `--stencil`, `--occluded` | identical, no validation messages |
| test/triangle `--second-queue`, `--second-device` | all 3 targets identical, no validation messages |
| test/triangle `--persistent` (frame-start contents, a mip in another layout) | all 5 targets identical, no validation messages |
| test/triangle `--ray-tracing`, `--ray-tracing --static-blas` | all 3 targets identical, the traced image included, with the validation layer and over 300 frames in the window; the bottom level built before the capture is built ahead of the frame |
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

Ray tracing is exported on both APIs. A build and a trace name what they read by device address and
by shader group handle (Vulkan) or shader identifier (Direct3D 12), which mean nothing in another
process, so the source spells each as what the program finds at run time rather than as a number.
On Vulkan:

- An address inside a buffer is `BufferAddress(buffer_N) + offset`: every device address the source
  spells goes through the exporter, which knows where the replay's driver put each buffer and so
  which buffer and offset the replay's own address was.
- Scratch is `BuildScratch(info, primitiveCounts)`, sized by the program's driver, which may want
  more than the replay's did.
- A top level's instances go up as captured with each instance's bottom-level reference rewritten
  to the program's own structure (`UploadInstances`).
- A trace passes the captured binding table records and the captured group handles to
  `TraceRays`, which builds the table the way the replay does, with the program's driver's handles.
- A bottom level built before the capture is built in a command buffer of its own at the start of
  the frame, from what the layer read back of its inputs.

On Direct3D 12 the same, in its terms. A GPU address was already spelled as its buffer's
`GetGPUVirtualAddress()` plus an offset; what is new is:

- A state object is written as the subobject array the replay made it from: each description a
  local, a library's DXIL a range of the data file, an association's subobject the element it
  names, and `Device5()->CreateStateObject`.
- A top level's instances go up through `UploadInstances`, with each bottom level spelled as the
  program's own buffer. Scratch for a structure built before the capture is `BuildScratch(inputs)`,
  sized by the program's device; the frame's own builds use the application's scratch, as the replay does.
- `DispatchRays` builds each region of its binding table with `BindingTable(stateObject, exports,
  ...)`: the captured records, with each record's identifier replaced by this runtime's
  `GetShaderIdentifier` for the export the captured identifier named. The captured identifiers
  and their export names are in the source beside the call.
- Structures built before the capture are built in `UploadContents`, in a list of their own, from
  what the library read back of their inputs.

Checked by building and running what it wrote on an RTX 4080: `d3d12_triangle --ray-tracing` and
`--ray-tracing --rebuild-blas` identical with the debug layer, in `--batch` and over 300 frames in
the window. The cubes sample the traced image, so the color target is what shows the trace: the
same program with its `DispatchRays` removed differs in 1,015 texels.

## A shader edited in the capture

`--replace <request>` replays the frame with other code for some pipelines' stages, which is how
the shader editor's **Compile & Replay** runs an edit in a capture rather than in the application
([Inspect](INSPECT.md#editing-a-shader)). Both `vkinsp_replay` and `dxinsp_replay` take it. The
request is in `--ablate`'s layout (`REPLACE 1`, a manifest, then the code it names):

```json
{"replacements": [{"pipeline": 48, "stage": "fragment", "payload": [0, 7288]}]}
```

Every pipeline made from that object runs the replacement — SPIR-V, or a DXBC/DXIL container — the
copies the other analyses make included. With `--target-data <file>` the render targets the capture
read back are compared as always, and the file (`TARGETS 1`) lists each with its differing texels
and carries the replayed pixels of the ones that differ, in the layout of the capture's own
read-back of them, which is how GPU Inspector decodes them. The exit code is 0 when the replay ran:
a frame replayed with an edit is meant to differ.

What differs is measured against the *capture*, not against an unedited replay, so a capture that
does not replay exactly to begin with shows that difference too; replay it without `--replace`
first to know. A replacement the driver refuses leaves its pipeline out, and the report's problems
name it (`pipeline 48: ...`).

## Direct3D 12

`dxinsp_replay` (`src/d3d12/replay/`, Windows) re-executes a Direct3D 12 capture. It replays the
frame and compares its render targets, writes the frame out as a C++ project (**Export to C++**),
reads the GPU's own counters around each render pass (**Measure hardware counters**), measures
every draw (**Measure draws**), times a draw with variants of a shader (**Measure shader**), and
replays with an edited one ([above](#a-shader-edited-in-the-capture)). Overdraw, pixel history,
draw overlays and the mesh view's VS Out are not replayed: a D3D12 capture gets those from the
capture library, measured while the frame was recorded
([Direct3D 12](D3D12.md#measuring-draws-overlays-and-meshes)).

```
dxinsp_replay <capture.gpucap> [--debug-layer] [--trace]
dxinsp_replay <capture.gpucap> --export <directory> [--export-data <file>]
dxinsp_replay <capture.gpucap> --counters [--counter <name>]... [--counter-data <file>]
dxinsp_replay <capture.gpucap> --list-counters [--counter-data <file>]
dxinsp_replay <capture.gpucap> --draws [--draw-data <file>]
dxinsp_replay <capture.gpucap> --ablate <request> [--ablate-data <file>]
dxinsp_replay <capture.gpucap> --replace <request> [--target-data <file>]
```

- `--draws` puts a timestamp pair, a pipeline statistics query and an occlusion query around every
  draw and dispatch and writes the file `vkinsp_replay --draw-data` writes
  ([Per-draw timing](#per-draw-timing-and-counters)). Queries are resolved by the command list in
  D3D12 rather than read by the host, so each list resolves what it used before it closes, and the
  results are read after the submission's wait. A bundle's draws cannot hold queries: the bundle is
  measured whole, and the time goes to the first draw in it. Statistics and occlusion are a direct
  list's; a compute list's dispatches carry timings only.
- `--ablate` is [Shader cost by ablation](#shader-cost-by-ablation) with DXIL containers where that
  request has SPIR-V, timed the same way: the draw issued again before it runs as captured, with a
  copy of its pipeline per variant that writes no depth or stencil, rotated over rounds, medians
  compared. The variants come from `src/app/src/renderer/d3d12/dxil_ablate.ts`, which edits the
  module's disassembly; `dxinsp_shader --assemble <module.ll> --out <file>` assembles the text into
  a container and has dxc validate and sign it, which is what makes it a shader the runtime takes
  ([Direct3D 12](D3D12.md#shader-cost-by-ablation)).

- `--debug-layer` runs the replay under the D3D12 debug layer and prints its messages, grouped.
- `--trace` names each command on stderr before it is issued, to find the one a driver dies in.
- `--counters` collects the counters [above](#hardware-counters), a range per render pass, writing
  the same `gpu-inspector-hw-counters` file `vkinsp_replay --counter-data` writes; `--counter`
  names the metrics to collect instead of the default set, and `--list-counters` names every metric
  the GPU offers. Three things differ from the Vulkan side:
  - **NVIDIA only.** Vulkan falls back to `VK_KHR_performance_query` where the driver has it;
    Direct3D 12 has no portable counter API, so this is NVIDIA's Nsight Perf SDK or nothing.
  - **Passes, not draws.** There is no `--counter-draws`: a D3D12 capture's per-draw numbers come
    from the capture library's own queries (**Measure draws**), so nesting a range per draw here
    would double the collection passes for numbers that already exist.
  - **A refusal looks different.** Counter access has to be allowed on the machine (NVIDIA Control
    Panel, *Developer > Manage GPU Performance Counters*, for all users) or the replay run as
    administrator. Vulkan is told `ERR_NVGPUCTRPERM` when the session begins; Direct3D 12's driver
    accepts the session and then never finishes the first profiled submission, so the replay checks
    the permission before it starts and gives up on a submission that has not finished in 30
    seconds rather than waiting for good.

  The SDK's own range profiler ships for Vulkan only, so its Direct3D 12 half — the seven calls its
  state machine makes — is implemented in `src/d3d12/replay/src/dx_nvperf.cpp` over the
  `NVPW_D3D12_*` entry points, which mirror the Vulkan family call for call.
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
  `EndRenderTargets` marker, or `EndRenderPass`), a multisampled color target through a resolve,
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
links `d3d12`, `dxgi` and `dxguid`. `--debug-layer` runs it under the debug layer, and `--batch` runs it
once without its window ([The window, and `--batch`](#the-window-and---batch)). The hand-written
part is `src/d3d12/replay/export_template`. The sources split as Vulkan's do, with the same
`VKINSP_EXPORT_PART_LINES` and `VKINSP_EXPORT_FILE_LINES`.

Checked on an RTX 4080 with the debug layer, replayed and then exported, built and run, as `--batch`
and in its window for 90 frames (no debug layer errors in either):

| Capture | The replay, and the exported program |
|---|---|
| test/triangle/d3d12 (table of a constant buffer and a texture, root constants, static sampler) | identical, color and depth, no debug layer errors |
| `--msaa` | the resolved color identical; the capture does not read multisampled depth back |
| `--bundle` with **Record always** | identical: the draw is in a bundle recorded at start-up |
| `--indirect`, `--compute`, `--offscreen` (no swap chain) | identical |
| `--render-pass` (`BeginRenderPass`), `--stencil` (D24S8, depth and stencil planes) | identical |
| Unity URP sample scene (83 command lists a frame recorded by jobs, 60 render passes suspended and resumed across them, 725 draws, pooled and per-frame lists) | the final image identical in 8 captures of 10, with no debug layer errors, and the exported program the same. One of the other two was a frame of adopted lists, which has no targets read back to compare; one differed in its post-processing, which reads textures the frame overwrites |
| Unity URP player frame (9 passes, pipeline library, root constant buffer views, D32S8, BC1 and BC3, SSAO, bloom) | all 17 targets identical; the 2 a pass discards are not compared. Cut into parts of 40 lines and files of 300 it builds and runs the same |

### Ray tracing

DXR replays: state objects, acceleration structure builds, traces, copies and postbuild info
(`src/d3d12/replay/src/dx_raytracing.cpp`). Most of a frame replays because everything it names has
an id; ray tracing names almost nothing that way, and three kinds of number have to be translated.

**Addresses.** A build reads its geometry, its instances and its scratch from GPU virtual
addresses, and a trace reads its binding table from three more. The capture library resolved each
one to the buffer and offset that owned it, so the ordinary address decode turns them into this
machine's and that half is free.

**Addresses inside buffers.** An instance description holds the address of the bottom level it
places, eight bytes in the middle of a 64-byte record that nothing resolved, because the
application wrote them into memory rather than passing them to a call. The replay rewrites them:
each structure object the capture minted carries the address it was built at, which gives captured
address to buffer and offset to this machine's address, and the instances go into a buffer of the
replay's own rather than over the application's, which may be an upload heap it rewrites every
frame.

**Shader identifiers.** A binding table record begins with the 32 bytes the captured runtime gave
for an export, and this runtime gives different ones for the same state object. So the table is
rebuilt the same way: captured identifier, to the export it named (the capture kept that list), to
this runtime's identifier for that name. A record whose identifier no export gave out is reported
and left alone.

**Local root arguments.** What follows the identifier is the arguments of the local root signature
the state object associates with that export: root constants, and root views and descriptor tables
as the captured process's GPU addresses and GPU descriptor handles. Which bytes are which is in
the capture after all -- the state object's associations name the local root signature, and its
parameters give the layout -- but the numbers can only be turned into objects where the capture
library can see them. So the library does it at the end of the frame, once the table's read-back
is there (`ResolveLocalRootArguments`, src/d3d12/src/raytracing.cpp): a table to its heap, slot and
the descriptors the table covers, a view to its buffer and offset. What they name is bound nowhere
else, so nothing else read it back: the library reads each root view's range and each buffer a
table's descriptors view, then and there. The command carries the result as `localRootArguments`,
and the replay writes its own heap slot and buffer address in each place, the descriptors into its
heap and the read-backs into its buffers. Export to C++ does the same through `BindingTable`'s
patches.

Limits: what was read back is the end of the frame's contents, which is right for what local
arguments point at (materials, per-object constants) and wrong for a buffer the frame writes before
the trace reads it; a texture a local table's descriptor views is not read back (its descriptor
is); and an association made inside a DXIL library, rather than in the state object's description,
is not seen, so its records are copied as they were.
`test/d3d12_triangle --local-root` gives the tinted hit group a root constant, a root CBV and a
table of one CBV, all three read in its color: identical, and with the rewrite left out of the
exported program 2,636 texels differ.

A buffer holding an acceleration structure is **created** in
`D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE` rather than moved into it: a barrier into
that state is rejected outright, and a structure never leaves it. So the structures are found before
any resource is made, and the state inference leaves their buffers alone.

Checked on an RTX 4080, under the debug layer:

| Capture | The replay |
|---|---|
| `test/d3d12_triangle --rebuild-blas` (a state object with two hit groups, both levels built every frame, a 4-record binding table, a 256x256 trace whose image the cubes then sample) | identical, color and depth, no problems and no debug layer messages |
| `--ray-tracing` (the same frame with the bottom level built once, before the capture) | identical: the bottom level is built before the frame from what was read back when the capture began |
| `test/path_tracer/d3d12` (three procedural bottom levels and a top level, all built at start-up) | all four structures built before the frame, no problems |

An engine builds its bottom levels once at load, so a captured frame usually holds no build of them.
The capture library remembers what each structure's last build read, and when a capture starts it
reads those ranges back, behind the first submission. The replay builds every structure the frame
uses but does not build itself from them before replaying the frame — bottom levels first, since a
top level's instances name them — and says how many it built. What was read back is what those
buffers held as the capture began: right for geometry that does not change, and not for a buffer the
application rewrote after the build. A structure whose inputs were not read back (a capture library
older than this, or memory that is not a buffer) is still left empty, and the replay reports it
against the instance that named it.

The comparison is what found the one real defect along the way, and it was in the replay: a build's
inputs are read back under their field names (`VertexBuffer`, `IndexBuffer`, `InstanceDescs`) rather
than in the flat `bufferData` list, and nothing was uploading them. The replay then built a bottom
level out of uninitialized memory, which looks exactly like a correct replay of a frame that had
nothing in it.

Not replayed: an opacity micromap array build, and `DispatchRays` whose binding table the capture
did not read back. What does replay is exported to C++ as well ([Export to C++](#export-to-c)): the
exported program looks its own shader identifiers up at run time by export name.

**What the frame found.** A frame reads resources it does not write first: a depth buffer it loads
rather than clears, the history texture temporal anti-aliasing reads and then overwrites, what
earlier frames left in a target. The capture library takes each such texture as the frame found it,
before the submission that first reads it (`kind: "initial"`, src/d3d12/README.md), and the replay
uploads those before the frame, ahead of any read-back taken later. On the Unity URP sample
(800x600, `-force-d3d12`) that was the difference between 12 of 14 targets differing -- the TAA
history read back after the frame overwrote it, so every pass from TAA on drifted -- and all 14
identical; in another frame, a back-buffer depth the frame only loads, left to whatever a new
allocation held. `test/d3d12_triangle --keep-depth` never clears its depth after the first frame:
identical, where without the uploads both targets differ.

**Bindless descriptors.** A shader of model 6.6 can take a descriptor straight out of the heap
(`ResourceDescriptorHeap[i]`), and then no root table says which slots it reads: a replay that only
writes what tables name leaves those slots empty, and the draw samples nothing without a word. The
capture library sends the heaps such shaders index with each submission that uses them -- the slots
written since it last sent them, as they were when the list was submitted, which is when the GPU
reads them -- and reads back what their views name (src/d3d12/README.md, "Directly indexed heaps").
The replay writes them into its heap after recording the submission's lists and before executing
them (`WriteHeapDescriptors`), and Export to C++ writes the same `Create*View` calls there. Slots
whose resource the replay lacks are counted into one problem per submission, since a bindless heap
keeps views the frame never reads. `test/d3d12_triangle --bindless` multiplies the cubes' color by
a texture that only a heap slot names: replay and export identical, debug layer clean; with the
slot writes left out, 33,419 texels differ and the replay reports nothing, which is what made this
worth doing.

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
- Video, work graphs and meta commands are left out: each such command is reported, and in the
  export is a comment where it would be.
- What the frame reads with no command naming it is not in the capture: a buffer reached through
  a GPU address inside another buffer. Multisampled textures are not uploaded, and multisampled
  depth is not compared.
- A buffer read-back cut at the capture's **Max KB** leaves the rest of the range as the replay's
  own buffer holds it, and the replay reports each such buffer with the size the frame bound; a
  simulation over a large buffer (a WebGPU page's particles) needs the setting raised to replay.
- A resource another device or process shared (`OpenSharedHandle`: a browser's canvas, which Dawn
  renders a page into) is made as one of the replay's own, from the description the capture keeps.
- Descriptors are right as of each submission, which is when the GPU reads them: a table's
  snapshot is taken at the draw, and a volatile slot the application rewrites before submitting is
  sent again with the submission (`test/d3d12_triangle --late-descriptor`). Two draws of one
  submission can never see different contents in one slot, on the GPU or here.
- Queries are issued but their results are not compared, and fences, tiled resource mappings and
  residency are not replayed.

## Metal

`mtlinsp_replay` (`src/metal/replay/`, macOS) re-executes a Metal capture, and is what **Export to
C++** runs for one. Like `dxinsp_replay` it replays the frame, compares its render targets and
writes the frame out as a project; it runs none of the analyses above.

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
- **The drawable** has no window in the replay, so its texture becomes an ordinary render target and
  the `presentDrawable:` that would have shown it is left out and reported. The exported program
  copies that texture to a drawable of its own window's layer
  ([The window, and `--batch`](#the-window-and---batch)); that part is written and has not been
  built on a Mac yet.
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
- **Storage textures.** A texture a compute pass of the frame wrote is read back after the frame and
  compared too, in a command buffer of the replay's own. A render pass's attachments have a pass end
  to be read at; a storage texture does not — the capture read it because a later pass *sampled* it
  — so without this a frame whose work is all in compute (a path tracer, whose traced image is the
  whole output) would compare nothing at all. Only textures bound writable to a compute encoder: one
  the frame merely reads was uploaded from the capture, so comparing it would compare the upload
  with itself.

  A kernel that *reads* the texture it writes accumulates into it, and then neither that texture nor
  anything else the same kernel wrote can be reproduced: the frame continues from contents nothing
  captured, since the capture holds them only as they were *after* the frame. Those are reported as
  not compared, with that reason. The verdict comes from the pipeline's own reflection, which says
  whether a texture slot is `read_write`.
- **Acceleration structures** are re-created at the size the capture recorded — a build writes into
  a structure the application had already sized, so the size is what has to match — and the builds,
  refits and copies are replayed with the descriptor rebuilt from the capture's JSON
  (`mtl_raytracing.h`). This is far less work than the same job on either other API, and the reason
  is in the API rather than in the code: a `VkAccelerationStructureGeometryKHR` names its vertices
  by *device address*, so the Vulkan replay has to have recorded every buffer's address range and
  map an address back to the buffer it fell in, with all the ways that can go wrong. A Metal
  geometry descriptor holds the `id<MTLBuffer>` and an offset, which the replay resolves the way it
  resolves every other reference. The scratch buffer is the replay's own, sized by asking *this*
  driver what the descriptor needs, rather than the application's — a build given too little scratch
  is undefined rather than an error.
- **Intersection function tables** are made from the pipeline they belong to and filled by function
  name. This is where DXR's and Vulkan's replays do their hardest work: both write a table into GPU
  memory as opaque identifiers — 32-byte export identifiers, or group handles — so the replay has to
  have recorded every identifier the capture's pipeline produced and substitute its own into the
  buffer's bytes. A Metal table is set through the API one entry at a time and an entry names its
  function, so the whole of that rewrite collapses into looking the name up among the pipeline's
  linked functions and asking for a handle. The linked functions themselves have to be carried onto
  the pipeline descriptor, or the table exists and cannot be filled — and then every ray misses.

Checked on an Apple M1 Max, replayed and then exported, built and run:

| Capture | The replay, and the exported program |
|---|---|
| test/metal_triangle (compute pass, multisampled pass through a parallel encoder resolving into a texture the next pass samples, function constants, a sampler, inline bytes) | both targets identical |
| `--occluded` (a depth attachment the pass discards, two draws) | all three targets identical, depth included |
| `--present-direct` (the drawable presented by the application rather than the command buffer) | both targets identical |
| `--ray-tracing` (a triangle bottom level, an instance top level over two copies of it, and a compute trace into a storage texture) | all three targets identical, the traced image included |
| test/path_tracer/metal `--rebuild` (bounding box geometry, an intersection function table, linked functions) | every command replayed; the traced image is reported as unreproducible rather than compared, because the frame accumulates into it |

Limits:
- Indirect command buffers, argument encoders and mesh shader draws are not replayed: each such
  object or command is reported, and in the export is a comment where it would be.
- Curve and motion geometry is replayed but not exported: the exported project would need a newer
  SDK and a keyframe array per buffer, which is more transcription than a repro case has needed.
  An opaque triangle intersection function is Metal's own rather than the application's, set by
  signature rather than by a function, and the replay has no way to name it.
- Tile shading (`setImageblockWidth:height:`, `dispatchThreadsPerTile:`) is recorded by the capture
  but is not on `MTLRenderCommandEncoder` in the macOS SDK, so it is left out.
- Events and fences within the frame are replayed; the replay commits each command buffer and waits
  for it before the next, so cross-frame synchronization does not arise.
- What the frame reads with no command naming it is not in the capture — a buffer reached through a
  `gpuAddress` held in another buffer, or through an argument buffer.

Worth keeping, twice over.

The first ray tracing frame replayed with **every ray missing**, and nothing said so: every object
was created, every build issued, the trace dispatched, and the report read like a clean run with a
black image. The fault was two lines away from the ray tracing work. `ParseEnum` resolves an enum
written by name, falls back to reading it as a number, and `strtoll` on a name that is not a number
returns *zero* — which for most Metal enums is a meaningful value, usually "invalid". The capture
writes a geometry's vertex format under its `MTLVertexFormat` name (the two enums are the same 42
values under two names), the replay looked it up in `MTLAttributeFormat`, the name did not resolve,
and the build got `MTLAttributeFormatInvalid` vertices. A structure built from those holds nothing.
`ParseEnum` now returns the caller's fallback for a string that is neither a known name nor a
number, which is the honest answer for an enumerator from a newer SDK too; and the geometry formats
are read under either name. The general lesson is the one the comment there now carries: a decoder
whose failure mode is a valid-looking zero hides itself.

The first frame with a depth attachment differed in every texel of its **color**
target, and the fault was the capture's. A depth attachment is announced under attachment index 0,
the same as color attachment 0, and `CaptureTextureData` did not carry the aspect — so the depth
read-back matched the color entry and landed on top of it. The Vulkan layer had always sent the
aspect for exactly this reason; the Metal one now does too (`SendTextures` in
`src/metal/src/capture.mm`). Nothing in the UI had shown it, because a depth image and a color
image of the same pass both render as an image.

## Where it stands

Every capture replayed so far, with its result:

| Capture | Result |
|---|---|
| test/triangle (render pass, compute, texture, push constants) | identical, color and depth |
| test/triangle `--hazard` (two submissions, `vkCmdUpdateBuffer`) | identical |
| test/triangle `--pipeline-library` (the cube pipeline linked from a vertex and a fragment library) | identical, no validation messages |
| test/triangle `--shader-object` (linked vertex and fragment shader objects, all state dynamic, dynamic rendering) | identical, no validation messages |
| test/triangle `--mixed` (the cube drawn with shader objects, then again with its pipeline, in one pass) | identical, no validation messages |
| test/triangle `--multiview` (both views of a two-layer target in one pass, blitted side by side), with and without `--dynamic-rendering` | both layers identical, no validation messages |
| test/triangle `--layered` (the same two-layer target through a layered framebuffer, the cube as two instances writing `gl_Layer`), with `--dynamic-rendering` and with `--shader-object` | both layers identical, no validation messages |
| test/triangle `--geometry` and `--tessellation` (the cube through a geometry shader, and tessellated), together, with `--shader-object` and with `--multiview` | identical, no validation messages |
| test/triangle `--dynamic-rendering` (the main pass in dynamic rendering, drawn with the cube's pipeline) | identical, no validation messages |
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
captured handle named — matched through the handle blob the layer keeps on the pipeline. A record
whose handle this pipeline never gave out is reported as a problem and left as it was.

**Shader record data.** The bytes after the handle are the application's own, and can hold device
addresses (a GLSL `buffer_reference`, a raw `uint64_t`), which are the captured process's. Unlike
D3D12's local root signature, nothing in the API gives the record a layout, so the work is split.
At the end of the frame, once the table's read-back has landed, the layer looks at every 8-byte
value of every record and keeps those that fall in a buffer with a device address, as that buffer
and offset, and reads the buffer back from there (`ResolveRecordAddresses`,
src/vulkan/src/capture.cpp): it is named nowhere else, so nothing else read it. The command
carries the result as `recordAddresses`. The replay then keeps only the values the group's shaders
actually read as 64-bit values, from the `ShaderRecordBufferKHR` block of their SPIR-V (a buffer
reference, a 64-bit integer or a `uvec2` member), so that a constant which happens to look like an
address is left alone, and writes its own buffer's address in their place. Export to C++ does the
same through `BindingTableRegion`'s patches (`RecordPatch`).

Limits: what was read back is the end of the frame's contents, at most 16 MB from the address on;
a buffer reference stored inside another buffer, rather than in the record, is not followed; and
an array member of the record block is not looked into.
`test/triangle --shader-record` puts a tint buffer's address in the hit record: replay and export
identical, validation clean; with the patch left out of the exported program 8,192 of 65,536
traced texels differ.

What a frame computes into an image is compared too, not only what it draws into a target: every
image the capture read back that a shader could have written (STORAGE usage) is read back again at
the end of its command buffer and compared, listed with a pass index of `-` because it belongs to
no pass. Without that, a trace that runs and a trace that produces the wrong pixels look alike.

A bottom level built *before* the capture began, which is the usual thing — an engine builds its
bottom levels once at load — is built by the replay before the frame, from what the layer read back
of its last build's inputs when the capture started ("acceleration structures built before the
capture" in the report). `test/triangle --ray-tracing --static-blas` makes that frame and replays
identical, traced image included; without `--static-blas` the test rebuilds both levels every frame,
as an engine with deforming geometry does. The same limits as on D3D12 apply: what was read back is
what the input buffers held when the capture began, and a host build
(`vkBuildAccelerationStructuresKHR`) has host pointers for inputs, which a later capture cannot read.

That comparison earns its keep. The first thing it found was not a fault in the replay at all: the
test application had no barrier between the top level's build and the trace that reads it, and on
this driver the race resolved in its favor often enough that the frame looked right every time it
was run. Synchronization validation does not report that hazard. Two runs of identical commands
disagreeing does, which is what a replay is for.

Not replayed yet: `vkCmdTraceRaysIndirect*`, the NV ray tracing commands, acceleration structure
copies, and queries whose results the frame reads back. Shader objects are made one at a time from their payloads, so a linked set replays
unlinked. Descriptor update templates are not created:
sets are written from the snapshots their binds carry, and a push through a template is pushed
again as plain writes from its own.

---

Previous: [Building from source](BUILDING.md) · [Docs index](README.md) · Next: [Architecture](ARCHITECTURE.md)
