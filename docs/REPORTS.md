# Reports

[Docs index](README.md) › Reports

The **Reports** menu in a capture answers questions about the whole frame instead of one command.
Every report links back into the command list, so a number you want to understand is one click
from the draw that produced it.

Some answers are about one render target or one draw rather than the whole frame. They open in
tabs beside the capture's own: the [render target tab](#the-render-target-tab) (overdraw, draw
overlays and pixel history) and the [mesh view](#mesh-view).

For the order to use them in when a frame is slow, see
[Finding GPU bottlenecks](PROFILING.md).

## Frame Stats

Two parts:

- **Frame Bound** — the frame's GPU time next to the CPU submit time and the frame interval, with
  a verdict on which of them the frame is waiting for. This is the first thing to read: it says
  whether the GPU is the problem at all.
- **Frame Statistics** — counts for the frame: commands by kind, draws (indexed, indirect, mesh),
  dispatches, submits and command buffers, passes and attachments, pipelines and stages bound,
  descriptor sets and what they bound, memory traffic and geometry.

![Frame Stats: the Frame Bound card and the Frame Issues list, each issue linked to its command](images/frame-stats.png)

**Frame Issues** lists what the frame analysis flagged — tiny draws, redundant binds, passes
without multiview on an XR frame, and the rest — each linked to the command it is about.
Severity checkboxes and a checkbox per rule narrow the list. The same findings appear inline on
the commands they concern.

## Analyze Shaders

Static analysis of every shader the frame's draws and dispatches used, worst first. It reads the
SPIR-V directly, so no shader source is needed.

For each shader: an estimated cost broken into ALU, special functions, texture and memory
operations, weighted by loop nesting; the costliest source lines when the shader carries debug
information; and findings for the patterns that usually matter — texture samples and atomics in
loops, expensive built-ins, non-constant and integer division, derivatives inside branches,
discards, loop-invariant work that could be hoisted.

## Shader Flame Graph

The frame's GPU work as a flame graph: pass, then pipeline, then shader stage, then function, then
source line. The pass level is measured time (from **Profile passes**); inside a pass the cost
model splits it across the draws and the functions they ran. It also lists the hottest functions
and lines of the frame. Clicking a frame zooms in; a draw frame selects the draw, and a shader
frame opens the shader.

![The Shader Flame Graph of a Unity frame: passes, pipelines and the fragment stages inside them, sized by GPU time](images/flame-graph.png)

Inside a pass, the split between draws is modelled until something measures it:

- **Profile passes** counters, where the capture has them, give each pass's fragment shader
  invocations, and the fragment stages are weighted by those instead of by scissor area.
- **Measure draws** (Vulkan) replays the capture on this machine's GPU with a timer and a pipeline
  statistics query around every draw. Each draw then takes its share of the pass by its measured
  time, and each stage its measured invocation count. The measurements are saved with the capture.
  They also fill in the depth rejection figure for passes whose draws are recorded into secondary
  command buffers, which the capture itself cannot measure (see
  [Finding GPU bottlenecks](PROFILING.md)).

Use it to find which shader function is eating a pass, rather than which pass is eating the frame.

## GPU Bottlenecks

Every pass, measured in the terms a bottleneck is usually described in:

- GPU time
- how many times each pixel was shaded (overdraw)
- how large the triangles were (fragments per primitive)
- whether the depth test was rejecting work
- which stage the pass waits on

Each number is shown with what normally causes it. The counters come from a pipeline statistics
query on Vulkan and from Metal's counter sets on macOS; which of them are available depends on the
API and the GPU.

![The GPU Bottlenecks report: per-pass GPU time, overdraw and fragments per primitive, with what to look at](images/bottlenecks.png)

[Finding GPU bottlenecks](PROFILING.md) is the walkthrough, including the thresholds each number
is judged against.

## Render Graph

The same frame as a dependency graph: every pass, the resources it reads and writes, and which
pass produced each of those. It is drawn as a resource lifetime chart, with the selected pass's
producers and consumers beside it.

![The Render Graph: the frame's passes, the resources between them, and the suggestions it raises](images/render-graph.png)

It answers:

- why a pass is there at all, and what would break if it went
- what the frame reads from before the capture began
- which passes write something that nothing ever reads
- the frame's critical path

## The render target tab

**Open in Tab** under any of a pass's render targets (in a draw's details, or the pass's) shows the
target in a tab of its own: the image at any zoom on the left, and the history of whichever pixel
you click on the right. The **overlay list** in its toolbar draws over the image: **Overdraw**,
**Highlight Draw**, **Depth Test** or **Wireframe**. **Reports → Overdraw** opens it on the frame's
first measured pass with the overdraw on.

### Overdraw

How many fragments landed on each pixel, drawn over the pass's render target. Two numbers per
pass: fragments that passed the depth and stencil tests, and every rasterized fragment. Hovering
shows the counts under the pointer.

![The overdraw heatmap over a pass's render target, with the counts per pixel and the colour scale](images/overdraw.png)

How it is measured depends on the API:

- **Vulkan** — pick **Overdraw** in the overlay list, or press **Measure Overdraw** in a pass's
  details. The capture is replayed on this machine's GPU, drawing each pass again with a counting
  shader. The application does not need to be running, but a GPU that can replay the capture does.
  See [Capture replay](REPLAY.md#overdraw).
- **Metal** — tick **Overdraw** in the capture bar before capturing. The measurement happens
  inside the captured frame.

The counting shader does not discard, so fragments the real shader would have thrown away are
still counted and alpha-tested geometry counts as opaque. A multiview pass is counted in its first
view only.

### Draw-call overlays

Where one draw landed, over its pass's render target: pick **Highlight Draw**, **Depth Test** or
**Wireframe** from the render target tab's overlay list, or press **Highlight Draw** under a draw's
render targets. The draw list beside it (with **‹** and **›**) steps through the pass's draws, and
**Go to Draw** selects the draw in the command list. Hovering a pixel says what the draw did there,
and the line under the list counts the pixels it covered, passed and had rejected.

![Highlight Draw on a Unity frame: the menu buttons' draw in magenta, the rest of the frame darkened](images/draw-overlay.png)

- **Highlight Draw** — the draw's pixels in a flat colour, the rest of the image darkened.
- **Depth Test** — green where the draw's fragments passed the depth and stencil tests, red where
  they were rejected.
- **Wireframe** — the draw's triangles as lines.

Vulkan only: the capture is replayed on this machine's GPU with the draw drawn on its own (see
[Capture replay](REPLAY.md#draw-call-overlays)). As with overdraw, a fragment the draw's own shader
discards still shows as covered.

## Mesh view

A draw's mesh, as RenderDoc's Mesh Viewer shows it: press **View Mesh** in a draw's details. The tab
has a wireframe preview over a table of the draw's vertices:

- drag to turn the mesh, use the wheel to zoom, and double-click (or **Reset View**) to frame it again
- click a row of the table to mark that vertex in the preview
- the draw list steps through the pass's draws and keeps the view, so their meshes line up

![The mesh view's VS In: a Unity sky sphere's 5,040 vertices as a wireframe, over the table of its positions](images/mesh-view.png)

- **VS In** — the vertices the draw read, decoded from the captured vertex and index buffers, with
  the attributes named from the vertex shader. The preview draws the attribute that looks like a
  position.
- **VS Out** — what the vertex shader wrote: `gl_Position` and every output, drawn in normalized
  device coordinates inside the outline of the view volume. The status line counts what keeps a
  mesh from being seen: primitives outside the view volume, vertices behind the eye, triangles with
  no area and NaN positions. Vulkan only: the capture is replayed with the vertex shader writing
  its outputs to a buffer (see [Capture replay](REPLAY.md#mesh-output)).

![The mesh view's VS Out: the test application's cube in normalized device coordinates, inside the outline of the view volume, over its clip-space positions and outputs](images/mesh-output.png)

VS Out is the place to look when a draw ran but nothing appeared. A mesh entirely outside the
volume, or behind the eye, points at the matrices that placed it; triangles with no area at a scale
of zero; NaN positions at a uniform that was never set. `get_mesh_output` gives Claude the same.

### Pixel history

Click a pixel in the render target tab. The **Pixel History** pane beside it
lists every clear and draw that touched that pixel, in order, with what became of the draw's
fragments — not reached, culled, discarded, failed the depth test, failed the stencil test,
written — and the pixel's value and depth after each one.

![Pixel history: the clear and the draw that touched the clicked pixel, with the value after each](images/pixel-history.png)

This is the report for "why is this pixel the wrong colour".

- **Vulkan** — the capture is replayed on this machine's GPU, so the application need not be
  running.
- **Metal** — another frame is captured while following the pixel, so the application must still
  be running.

Current limits: writes outside render passes (copies, blits, compute) are not followed,
multisampled images are followed through their resolve attachment, and a draw is one event with no
detail per primitive inside it. The full list is in [Capture replay](REPLAY.md#pixel-history).

---

Previous: [Capture](CAPTURE.md) · [Docs index](README.md) · Next: [Finding GPU bottlenecks](PROFILING.md)
