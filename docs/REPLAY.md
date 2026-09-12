# Capture replay

[Docs index](README.md) › Capture replay

`vkinsp_replay` re-executes a Vulkan capture (`.gpucap`) on this machine's GPU, without the
application. It is the basis for the analyses that have to run a frame again with something
changed: the overdraw heatmap, pixel history, and later per-draw timing and shader debugging
(TODO.md, "Replay-based features").

```
vkinsp_replay <capture.gpucap> [--validate] [--dump <dir>] [--overdraw <dir>] [--overdraw-data <file>]
              [--pixel <image> <x> <y> [--mip <n>] [--layer <n>] [--pixel-data <file>]] [--trace]
vkinsp_replay <capture.gpucap> --check
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
  **Measure Overdraw** runs the tool this way (`app/src/main/replay.ts`), and so does the MCP
  server's `get_overdraw` for a Vulkan capture.
- **`--pixel <image> <x> <y>`:** follows one pixel of an image (tracker id; `--mip` and `--layer`
  pick the subresource) through the frame, and lists every pass start, draw and clear that touched
  it (see [Pixel history](#pixel-history)).
- **`--pixel-data <file>`:** with `--pixel`, writes the history as JSON: every event, including the
  draws that do not reach the pixel, with its sample counts and the texels after it as hex in the
  formats the file names. GPU Inspector's pixel history pane runs the tool this way, and so does the
  MCP server's `get_pixel_history`.
- **`--check`:** only decodes every creation argument and command argument, and lists what cannot
  be rebuilt.

It builds with the layer (`VKINSP_BUILD_REPLAY`, on by default) on Windows and Linux.

## How it works

**Decoding.** The layer writes creation arguments and command arguments as JSON with a serializer
generated from vk.xml (`tools/vkgen/serialize.py`). `tools/vkgen/deserialize.py` generates the
inverse from the same registry model, as `replay/gen/vk_decode.gen.*`:
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

**Loading.** `replay/src/json.*` is a JSON parser into an arena that keeps numbers as text, so
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
- **Sampled images** (every mip of the view the capture read) are uploaded before the frame. Each
  image is then moved to the first layout the frame expects: the old layout of its first barrier,
  the layout a descriptor binds it with, or the initial layout of a pass that renders to it.
- **Buffer ranges** captured when bound are uploaded before the submission of the command buffer
  that binds them.
- **Descriptor sets** are written from the snapshot taken when they were bound. A set is only
  rewritten when its contents changed, since rewriting a bound set would invalidate the command
  buffers that bound it.

**Commands.** A submission's command buffers are recorded from their recordings in the command
list, including the secondary command buffers inlined after `vkCmdExecuteCommands`. They are
submitted without the application's semaphores and fences, and the replay waits for them. At the
end of each pass the replay copies the targets the capture read back for that pass (render pass
counter per command buffer, as the layer counts). After the submission the copies are compared.
Only the undefined top byte of a 24-bit depth copy is ignored.

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
(`metal/README.md`, "Overdraw").

The pipeline copies of overdraw and pixel history are made by `pipeline_copy.cpp`: the captured
create info is decoded, its shader stages rebuilt from the capture's SPIR-V, and an edit changes
the state before the copy is created.

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
- **The times are not what a draw costs alone.** The GPU pipelines consecutive draws, so their
  spans overlap and add up to more than the pass takes. What they are good for is the share of a
  pass a draw accounts for.

`--draw-data <file>` writes them as JSON (`app/src/renderer/draw_stats.ts` reads it). GPU
Inspector's Shader Flame Graph runs the tool this way from **Measure draws**, and the MCP server's
`get_shader_flame_graph` does on first use: a pass's measured GPU time is then split between its
draws by what the replay timed, and each fragment stage takes its measured invocation count instead
of the scissor-area estimate.

```
vkinsp_replay frame.gpucap --draws
draws measured: 3, 0.021 ms of draw time, 51204 fragment shader invocations
  [5] outside a render pass: 0.0078 ms, 0 vertex, 0 primitives, 0 fragment, 1024 compute invocations
  [17] pass 0: 0.0061 ms, 24 vertex, 12 primitives, 51204 fragment, 0 compute invocations
```

On the test triangle the fragment count matches the pipeline statistics the capture itself
measured, exactly.

## Where it stands

Every capture replayed so far, with its result:

| Capture | Result |
|---|---|
| test/triangle (render pass, compute, texture, push constants) | identical, color and depth |
| test/triangle `--hazard` (two submissions, `vkCmdUpdateBuffer`) | identical |
| test/triangle `--msaa` | the resolve target identical; the multisampled target is not compared yet |
| Unity player frame (secondary command buffers, two subpasses, `vkCmdSetVertexInputEXT`, MRT) | 8 of 9 targets identical; the last pass renders to a swapchain image the capture never tracked, and is left out |
| XR triangle captured on an Adreno 740, replayed on an RTX 4080 | visually identical; 0.4% of texels differ slightly (shader precision and rasterization of two GPUs) |

These cases differ for known reasons:
- **A frame captured while `replace_shader` was active** reads back the edited shader's output.
  The capture keeps the original pipeline, which is what the replay draws.
- **Captures older than sampled-image read-back** have no texture contents to upload.

## What is left

1. **Replayable captures.** The capture has to hold the state a frame starts from, not only what
   it read back on the way. That means resource contents at frame start: images never read back,
   buffers never bound in the frame, and host writes to mapped memory between submissions.
   - Diffing mapped ranges at each submit, as RenderDoc does, covers the host writes.
   - It also needs initial layouts per subresource.
   - It needs objects the layer did not track: swapchain images of swapchains created before it
     loaded.
   - Multisampled targets should be compared through a resolve.
   - Live shader replacements should be recorded.
2. **Pixel history, the rest.**
   - Writes outside passes.
   - Multisampled images.
   - Per-fragment values: RenderDoc re-draws each primitive with a primitive-id shader.
   - Early fragment tests.
3. **Speed.** Both analyses are already wired into the app and the MCP server: the capture's
   render target tab draws the overdraw over the image and follows the pixel you click beside it,
   with `get_overdraw` and `get_pixel_history` for Claude. But each pixel replays the whole frame
   again; keeping one replay process alive between requests would make it quicker.

Not replayed yet: pipeline libraries, ray tracing pipelines and shader objects, descriptor update
templates and push descriptors with templates, queries whose results the frame reads back, and
Metal captures.

---

Previous: [Building from source](BUILDING.md) · [Docs index](README.md) · Next: [Architecture](ARCHITECTURE.md)
