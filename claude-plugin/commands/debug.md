---
description: Debug a rendering problem in a GPU Inspector capture, down to the draw and the state that causes it
argument-hint: "[capture path or id] <what looks wrong>"
---

Find the cause of a rendering problem in a GPU Inspector capture. `$ARGUMENTS` holds a capture
(path or id; if absent, pick one from `list_captures` and say which) and a description of what
looks wrong: an object missing, a black or garbage target, wrong colors, flickering geometry.
Follow the rendering-bugs method of the `gpu-capture-analysis` skill.

1. **Open and summarize.** Call `get_capture_summary`, then `get_validation`. If a validation
   error could explain the symptom, check it first: `get_command` on the command it fired on.
2. **Find where the image goes wrong.** `list_textures`, then `read_texture` on the targets pass by
   pass, following the passes' order, until one shows the problem. Look at the returned image and
   at `uniform`, the channel ranges, and NaN counts.
3. **Find the draw.** Call `list_commands` with that `pass` (and `kind: "draw"`). If the object has a
   debug label, use `label`. Otherwise narrow down by pipeline, vertex count or bound textures.
4. **Check what the draw read.** Call `get_command`.
   - **Fixed-function state:** cull mode and front face, depth test, write and compare op, blend and
     color write mask.
   - **Viewport and scissor.**
   - **Decoded uniforms and push constants:** matrices, colors, alpha, counts.
   - **Bound images:** `read_texture` on their texture numbers.
5. **Check the geometry** with `read_vertices`: bounds, NaNs, indices within the vertex data.
6. **Check the shaders** with `get_shader`: `source` or `glsl`, and `reflection` to confirm the
   bindings the shader expects are the ones bound.
7. **Compare with a correct draw.** If a similar draw renders correctly, call `get_command` on both
   and compare.
8. **Report.**
   - The cause, with the evidence: command index, object ids, the values that are wrong.
   - The fix in the application's terms.
   - How sure you are.

   If the capture cannot show the cause, say what to capture next: buffer contents cut by the size
   limit, a missing validation layer, a frame that did not show the problem.
