# Inspect

[Docs index](README.md) › Inspect

The **Inspect** tab is the live view of the application: every object it has created, as it
creates them, with the arguments it created them with. Nothing has to be captured first.

## Finding an object

Objects are listed by type — images, buffers, pipelines, shader modules, descriptor sets, device
memory and the rest — with a count beside each type. On Metal the types are `MTLTexture`,
`MTLBuffer`, `MTLLibrary` and so on instead.

- **Search** matches a name, label, type, format or id.
- **Filters** narrows further: images by format, size, layers and usage; buffers by size and
  usage; shaders and pipelines by stage; descriptor sets by what they bind.
- **Only objects used in the last capture** hides everything the captured frame did not touch,
  which is usually the fastest way to a short list.
- The back and forward buttons walk through the objects you have visited.

## Reading an object

Selecting an object shows:

- **Arguments** — the full creation call, with every struct member and `pNext` chain decoded.
- **Dependencies** — what it was made from and what uses it, each a link. An image view links to
  its image, a pipeline to its layout, shaders and render pass.
- **Memory** — for images, buffers and device memory: the allocation it is bound to, the offset
  and the heap.
- **Stack trace** — where the application created it, when **Stack traces** was ticked in the
  launch dialog. Symbols come from the application's debug information; on Android, from the
  directories given as **Symbol directories**.

Destroyed objects are marked as such rather than disappearing, so a dangling reference still says
what it pointed at.

## Textures

Selecting an image opens an **Image** view that reads the current pixels back from the running
application. The toolbar has the mip level and array layer, zoom, **Smooth** (filter instead of
showing texels), a refresh button to read the image again, and a button that copies what is shown
as a PNG. The value of the texel under the pointer is shown as you move it.

## Descriptor sets

A descriptor set shows its layout and, under **Contents**, what is bound to each binding right
now — **Refresh** reads it again from the application. Bindings that were never written say so.
Buffers are decoded with the types the shader declares, so a uniform block reads as its fields
rather than as bytes.

## Shaders

A shader module or pipeline has a **Shader Code** section per stage with these views:

| View | What it is |
|---|---|
| **Source** | The original source the compiler embedded in the SPIR-V. Shown only when the shader carries it — see [shader sources](VULKAN.md#shader-sources) |
| **SPIR-V** | Disassembly, from `spirv-dis` |
| **GLSL**, **HLSL** | Decompiled by `spirv-cross` |
| **Reflection** | Entry points, inputs and outputs, resources and push constants, read from the SPIR-V itself |

On Metal, a library shows the Metal Shading Language it was compiled from, when it was compiled on
the spot rather than loaded as a precompiled `metallib`.

### Editing a shader

**Edit** opens the shown text in an editor. **Compile & Apply** compiles it with the Vulkan SDK's
compilers on this machine and swaps it into the running application — the next frame it draws uses
your version. **Restore Original** binds the application's own pipeline again. An edited stage is
marked `[edited]` in its heading.

GLSL, HLSL and SPIR-V assembly can all be edited; which compiler is needed depends on which one
you edit (`glslangValidator`, `dxc`, `spirv-as`). A shader with embedded source is edited as that
source.

This works for Vulkan on the desktop and on [Android](ANDROID.md), where the shader is compiled
here and sent to the device. It does not apply to [Metal](METAL.md).

## Validation messages

With **Validation layer** ticked at launch, the errors and warnings it reports are listed in a
**Validation Messages** section, each linked to the objects it names. With **Sync validation**,
synchronization hazards appear the same way, linked to the command that caused them.

On Metal this is Metal's own API and shader validation, in the mode that logs a failure instead of
aborting.

## Leaks

**Leaked Objects** lists objects that were created and never destroyed, with the creation
arguments last known for each and their stack traces when those were recorded.

## Frame time

The **Frame Time** graph plots the application's recent frames: the average, the maximum, and the
CPU time spent inside `vkQueueSubmit`. It is the quickest check of whether a change you made in
the application or in a shader actually moved anything.

---

Previous: [Android and Quest](ANDROID.md) · [Docs index](README.md) · Next: [Capture](CAPTURE.md)
