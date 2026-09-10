---
description: Find what limits a capture's frame time, pass by pass, and what to change
argument-hint: "[capture path or id]"
---

Find what limits the frame in the GPU Inspector capture `$ARGUMENTS`, following the performance
method of the `gpu-capture-analysis` skill (GPU Inspector's docs/PROFILING.md).

1. **Open the capture.** Call `open_capture` with a path, or use the id. If `$ARGUMENTS` is empty,
   pick one from `list_captures` and say which. Then call `get_capture_summary`.
2. **Check it was profiled.** If `timing.profiled` is false, stop and explain:
   - The GPU times and counters are sampled while the frame is captured, so a capture taken without
     them cannot be profiled afterwards.
   - The user should capture again with **Profile passes** on in GPU Inspector's capture bar, while
     the application is doing the slow thing.

   A capture without timings can still get a structural review, and you can offer one.
3. **Is the GPU the limit?** State the Frame Bound verdict with its numbers.
   - CPU bound: say that the rest is not a GPU problem, and what the submit time suggests.
   - Vsync bound: say the frame has headroom.
4. **Call `get_bottlenecks`.** Take the slowest passes that together cover most of the GPU time.
   For each, report:
   - its share of the frame
   - what bounds it
   - overdraw, fragments per primitive and depth rejection where measured
   - each `problems` entry, with its cause
5. **Check the rules that bear on those passes.** Call `get_frame_issues` and `list_commands` with
   `pass` and `kind: "issue"` for the passes above. Look for:
   - load and store ops
   - transient targets
   - mergeable passes
   - redundant binds
   - tiny draws
   - one pass per eye
6. **Check the shaders those passes use** (Vulkan). Call `analyze_shaders`. For the costliest
   fragment shaders, call `get_shader` with `view: "analysis"`.
7. **Recommend changes**, ordered by the GPU time they can save. For each give:
   - the pass
   - the measured evidence
   - the change
   - the number that should move

   Suggest capturing again and running `/gpu-inspector:compare` to confirm.
