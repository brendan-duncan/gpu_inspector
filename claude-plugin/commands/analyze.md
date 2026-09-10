---
description: Analyze a GPU Inspector capture for correctness and performance problems
argument-hint: "[capture path or id]"
---

Analyze the GPU Inspector capture `$ARGUMENTS` in depth with the gpu-inspector MCP tools, following
the `gpu-capture-analysis` skill.

1. **Find the capture.**
   - A path in `$ARGUMENTS`: call `open_capture` with it.
   - An id (`cap-2`): use it.
   - Empty: call `list_captures`. Use the open capture, or else the most recent file GPU Inspector
     saved that still exists, and say which one you picked.
   - Nothing found: ask for a path and stop.
2. **Summary.** Call `get_capture_summary`. Note what its `notes` say the capture lacks.
3. **Correctness.** If there are validation messages, call `get_validation`. Explain each error,
   using `get_command` on the command it fired on.
4. **Frame Issues.** Call `get_frame_issues`. For each high and medium finding:
   - Confirm it against the command it names with `get_command`, or with `get_render_graph` and a
     `node` for the render graph rules.
   - Say whether it is real in this frame or a pattern that may be intentional.
5. **Bottlenecks.** If the passes were profiled, call `get_bottlenecks`. Name the slowest passes and
   what limits each.
6. **Shaders** (Vulkan). Call `analyze_shaders`, then look at the costliest with `get_shader`
   `view: "analysis"`.
7. **Report** a prioritized list. For each item give:
   - severity
   - what it is
   - the evidence: command indices, object ids, pass labels, measured numbers
   - why it matters in this frame
   - the concrete fix

   Keep confirmed problems apart from heuristics and modeled costs. End with what the capture could
   not show, and the capture option that would show it.
