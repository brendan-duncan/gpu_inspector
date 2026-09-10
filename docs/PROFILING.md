# Finding GPU bottlenecks with GPU Inspector

A frame that takes too long is taking too long *somewhere*. This walks through finding where,
using what a capture measures, and says what each measurement means when it is bad. It follows the
same shape as Apple's and Unity's guidance for Xcode's Metal frame capture, because the reasoning
is the same; what differs is that the numbers here come out of a capture you can save, reopen and
compare.

**Vulkan and Metal.** Almost all of this works for both. The Metal library samples Metal's counter
sets around every pass and the Vulkan layer a pipeline statistics query beside its timestamps, so
overdraw, fragments per primitive and the geometry counts come out of either. Two measurements are
Metal only and are marked where they appear: the vertex and fragment spans of a pass, which need
timestamps at its stage boundaries and have no portable Vulkan equivalent, and depth rejection,
which needs the count of fragments that survived the depth test — Metal's statistic set has it and
Vulkan's pipeline statistics do not.

On Vulkan the counters need the `pipelineStatisticsQuery` device feature, which an application
that does not profile itself has no reason to enable. The layer adds it at device creation, and
falls back to creating the device exactly as the application asked if the driver refuses.
`VKINSP_NO_PIPELINE_STATISTICS=1` turns that off.

## Before you capture

Turn on **Profile passes** in the capture bar. The timings and counters this whole document rests
on are sampled *during* the capture; they cannot be recovered afterwards, and a capture taken
without them shows durations of nothing at all.

Capture while the application is doing the thing that is slow. A menu screen tells you nothing
about a battle. If the slow moment is hard to reach by hand, set **Capture at frame** and let it
arm itself.

Capture more than one frame if the frame time is uneven. Reports total over the captured frames,
and one frame of a stutter is easier to read next to two that are fine.

## Step 1: is the GPU even the problem

Open **Reports → Frame Stats**. The Frame Bound card at the top compares three numbers: the frame
interval the application actually achieved, the CPU time it spent submitting work, and the GPU
time its passes took.

- GPU time close to the frame interval: the GPU is the limit. Carry on.
- CPU submit time close to the frame interval: the application is limited by its own submission,
  not by the GPU. Fewer, larger command buffers and fewer state changes; the rest of this document
  is not your problem.
- Both well under the frame interval: something else sets the pace. Usually vsync, which is fine,
  or the application waiting on something that is not the GPU.

The meter in the session bar shows the same comparison live, without capturing.

## Step 2: which pass

Open **Reports → GPU Bottlenecks**. The Passes table lists every pass, slowest first, with what
was measured over it.

Work on the slowest pass. This sounds obvious and is routinely ignored: a 12 ms shadow pass and a
0.3 ms bloom pass are not equally worth an afternoon, however inelegant the bloom pass is.

The report names the slowest pass at the top of **What to look at**, with the stage it waits on
and the first thing to try. Everything below that is a specific measured problem, worst first.

## Step 3: which stage (Metal only)

The **Vertex / fragment** column is a bar: blue is the vertex stage's own span, orange the
fragment stage's. On a tile-based GPU, which is every Apple GPU, the two stages of one pass
overlap — the fragment stage of a tile starts as soon as that tile's geometry is binned — so the
two spans add up to more than the pass duration. The longer of the two is what the pass waits on.

The **Verdict** column says which, and hovering it says why. Four answers:

| Verdict | Meaning |
|---|---|
| **Vertex bound** | The pass waits on geometry: too many vertices, or too much work per vertex. |
| **Fragment bound** | The pass waits on shading: too many fragments, or too much work per fragment. |
| **Target write bound** | Most of the pass's GPU cycles went to writing the attachment rather than shading it. Needs the stage-utilization counters. |
| **Balanced** | Neither dominates. The win is usually removing the pass, not tuning it. |

## Step 4a: a vertex-bound pass

Two things make a vertex stage slow, and they need different fixes.

**Too many vertices.** The Passes table's *Draws* column and Frame Stats' Geometry section say how
much geometry the pass submitted. If the count is large, the answer is less geometry: mesh level of
detail so distant objects use cheaper meshes, culling so objects outside the view are never
submitted, and merging many tiny draws so the GPU is not restarted for each of them. The
`tiny-draws` rule in Frame Issues flags the last of these.

**Too much work per vertex.** If the vertex count is reasonable and the stage is still slow, the
cost is in the shader. The GPU Bottlenecks report divides the vertex stage's time by its
invocations, so you can compare a pass against itself after a change.

To read the shader itself, select the draw and open its pipeline, or find the function in Inspect:
a Metal function shows its library's source scrolled to the definition. The static cost model
behind **Reports → Analyze Shaders** and the **Shader Flame Graph** works on SPIR-V, so those two
reports are for Vulkan captures; for a Metal shader, the Xcode trace below has per-line costs.

A common cause on Unity projects is per-vertex evaluation of something that could be per-fragment,
or the reverse. Moving lighting work between stages changes which stage pays; the report tells you
which one can afford it.

## Step 4b: a fragment-bound pass

This is the more common case, and there are three measurements that name the cause. The first two
come from the GPU counters either backend samples; the third, depth rejection, is Metal only (see
*What cannot be measured here* below).

### Overdraw

**Overdraw** is fragment shader runs per pixel of the render target. A frame doing well sits near
**1.2**: most pixels shaded once, a few twice. The report flags anything above **2**.

High overdraw is one of three things:

1. **Stacked transparency.** Transparent surfaces cannot be depth-rejected, so every one of them
   shades every pixel it covers. Particles and UI are the usual offenders. Fewer, larger, more
   opaque particles beat many faint ones.
2. **A full-screen effect drawn more than once.** Each full-screen pass is one whole unit of
   overdraw by itself. Two blur passes and a tonemap is 3x before the scene is drawn at all.
3. **Opaque geometry drawn back to front.** The depth test can only reject a fragment if
   something nearer was already drawn. Drawn far-to-near, nothing is ever rejected.

Case 3 is what the depth rejection column below distinguishes from the other two.

### Fragments per primitive

**Frags/prim** is fragment invocations divided by primitives out of the clipper: the average
triangle's coverage in fragments. The rasterizer works in 2x2 quads, so a triangle covering fewer
than **4** fragments has shaded lanes it immediately throws away. Below 4 the report raises
`microtriangles`.

This is dense geometry drawn small: a high-poly mesh at a distance where it covers thirty pixels,
or a mesh authored for a close-up used everywhere. The fix is mesh level of detail, or culling the
objects that have become smaller than their own triangles. It is not a shader problem, and making
the shader cheaper will not help much.

### Depth rejection (Metal only)

**Depth reject** is the share of shaded fragments that the depth and stencil tests threw away. A
high number is *healthy*: it means the depth test is doing its job and rejecting work early.

The bad combination is **overdraw above 1.5 with depth rejection below 25%**, which the report
raises as `late-depth-rejection`. Fragments are being shaded and then replaced. Two fixes:

- **Sort opaque geometry front to back.** Nearer surfaces then populate the depth buffer first and
  reject the fragments behind them.
- **A depth prepass.** Draw the opaque geometry once writing depth only, then again shading with
  the depth test set to equal. Worth it when the fragment shader is expensive and the scene cannot
  be sorted usefully.

Neither helps transparent geometry, which does not write depth. If the overdraw is transparency,
go back to the overdraw section.

## Step 5: the rules that do not need counters

**Reports → Frame Stats** ends with **Frame Issues**: rules over the whole capture, each linked to
the command that raised it. The ones that bear on GPU cost, in the order they usually matter.
`late-depth-rejection` is Metal only; the rest apply to both APIs, sometimes under a slightly
different name:

| Rule | What it means |
|---|---|
| `high-overdraw` | A pass shades each pixel more than twice over. |
| `microtriangles` | Triangles too small for the rasterization quad. |
| `mergeable-passes` | A pass loads what the pass before it stored, with nothing between: one encoder would keep the target in tile memory instead of round-tripping it through DRAM. |
| `undefined-load` | An attachment loaded when nothing wrote it: the load is pure cost. |
| `msaa-store` | A multisampled attachment stored rather than only resolved, which writes every sample to memory. |
| `memoryless-candidate` | A target only ever cleared and discarded within a pass: it never has to exist in memory at all. |
| `late-depth-rejection` | Fragments shaded and then replaced. |
| `color-store` / `depth-store` | An attachment stored that nothing afterwards reads. |
| `unmipped-texture` | A megapixel-plus content texture sampled with no mip chain. Drawn smaller than itself it reads scattered texels and misses the cache on most of them; a mip chain costs a third more memory and reads one texel per sample. |
| `tiny-draws` | Many draws of a handful of vertices: candidates for instancing. |

The **Render Graph** report adds the rules that need the frame's dependencies rather than one
command: results nothing reads, targets replaced before use, and passes that could be one pass.

## Step 6: confirm the fix

Capture again and compare the same numbers. This matters more than it sounds: a change that halves
a shader's instruction count and moves nothing measurable means the pass was never limited by that
shader. The report's per-pass rows are the comparison — GPU ms, the stage bar, overdraw, frags per
primitive.

Save both captures (the capture bar's save button, `.gpucap`) and reopen them side by side in two
tabs if you want the before and after in front of you at once.

## What cannot be measured here

GPU Inspector reads what Metal exposes publicly. Three families of counter that Xcode shows have
no public API, and no amount of work on this tool will produce them:

- **Shader occupancy** against the theoretical maximum.
- **The limiters**: ALU, buffer read, texture read cache, texture filtering, texture write. These
  are what tell you *which unit inside the shader core* is saturated.
- **Per-line shader cost**, and the compiler statistics behind it.

For those, the capture bar's **Xcode Trace** button writes the next frame as a `.gputrace`
document. Open it in Xcode and you have the full Metal debugger, on the same frame you were just
looking at. The two tools are complementary: use this one to find the pass and the cause, and
Xcode when you need to know which unit inside a shader is the limit.

There is also a hardware limit to be aware of. Metal's statistic and stage-utilization counter
sets are exposed by some GPUs and not others; through public Metal, Apple Silicon exposes only
timestamps. On Vulkan the equivalent is a device that does not support `pipelineStatisticsQuery`,
or an application whose device the driver would not create with the feature added. When the
counters are missing, the GPU Bottlenecks report says so and the columns that need them are empty;
the pass durations still work, so steps 1 and 2 are unaffected. Both backends log what they found
when a capture is taken.

## Reference: the numbers and their thresholds

| Measurement | Where | Healthy | Flagged at |
|---|---|---|---|
| Frame time against the display interval | Frame Stats, session bar | under the interval | — |
| Pass GPU time | GPU Bottlenecks, pass headers | — | the slowest pass is named |
| Vertex versus fragment span | GPU Bottlenecks | — | whichever is 1.3x the other |
| Overdraw | GPU Bottlenecks | about 1.2 | above 2 |
| Fragments per primitive | GPU Bottlenecks | above 4 | below 4 |
| Depth rejection (Metal) | GPU Bottlenecks | high | below 25% with overdraw above 1.5 |
| Sampled texture without mips | Frame Issues | — | 1 megapixel and up |
| Draws of very few vertices | Frame Issues | — | 32 draws of 12 vertices or fewer |

Thresholds are starting points, not laws. A deferred renderer's g-buffer pass legitimately writes
several targets; a particle system legitimately overdraws. What the numbers are for is telling you
which pass to spend the afternoon on.
