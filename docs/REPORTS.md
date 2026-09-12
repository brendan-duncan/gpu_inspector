# Reports

[Docs index](README.md) › Reports

The **Reports** menu in a capture answers questions about the whole frame instead of one command.
Every report links back into the command list, so a number you want to understand is one click
from the draw that produced it.

For the order to use them in when a frame is slow, see
[Finding GPU bottlenecks](PROFILING.md).

## Frame Stats

Two things:

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
and lines of the frame.

Use it to find which shader function is eating a pass, rather than which pass is eating the frame.

## GPU Bottlenecks

Every pass measured in the terms a bottleneck is usually described in:

- GPU time
- how many times each pixel was shaded (overdraw)
- how large the triangles were (fragments per primitive)
- whether the depth test was rejecting work
- which stage the pass waits on

![The GPU Bottlenecks report: per-pass GPU time, overdraw and fragments per primitive, with what to look at](images/bottlenecks.png)

with what normally causes each. The counters come from a pipeline statistics query on Vulkan and
from Metal's counter sets on macOS; which of them are available depends on the API and the GPU.

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

## Overdraw

How many fragments landed on each pixel, drawn over the pass's render target. Two numbers per
pass: fragments that passed the depth and stencil tests, and every rasterized fragment. Hovering
shows the counts under the pointer.

![The overdraw heatmap over a pass's render target, with the counts per pixel and the colour scale](images/overdraw.png)

How it is measured depends on the API:

- **Vulkan** — press **Measure Overdraw**. The capture is replayed on this machine's GPU, drawing
  each pass again with a counting shader. The application does not need to be running, but a GPU
  that can replay the capture does. See [Capture replay](REPLAY.md#overdraw).
- **Metal** — tick **Overdraw** in the capture bar before capturing. The measurement happens
  inside the captured frame.

Fragments a shader discards are counted, since the counting shader does not discard, so
alpha-tested geometry counts as opaque. A multiview pass is counted in its first view only.

## Pixel history

Click a pixel in a render target and press **Pixel History**: every clear and draw that touched
that pixel, in order, with what the draw's fragments met — not reached, culled, discarded, failed
the depth test, failed the stencil test, written — and the pixel's value and depth after each one.

![Pixel history: the clear and the draw that touched the clicked pixel, with the value after each](images/pixel-history.png)

This is the report for "why is this pixel the wrong colour".

- **Vulkan** — the capture is replayed on this machine's GPU, so the application need not be
  running.
- **Metal** — the next frame is captured again while following the pixel, so the application must
  still be running.

Current limits: writes outside render passes (copies, blits, compute) are not followed,
multisampled images are followed through their resolve attachment, and a draw is one event with no
detail per primitive inside it. The full list is in [Capture replay](REPLAY.md#pixel-history).

---

Previous: [Capture](CAPTURE.md) · [Docs index](README.md) · Next: [Finding GPU bottlenecks](PROFILING.md)
