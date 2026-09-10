---
description: Launch an application with GPU Inspector's capture library and investigate it while it runs
argument-hint: "<executable> [arguments] [what to look at]"
---

Launch and investigate a running application with the gpu-inspector MCP tools, following the
`gpu-capture-analysis` skill. `$ARGUMENTS` holds the executable, then its arguments if any, then what
the user wants looked at: a slow scene, something drawn wrong, a shader to make cheaper. Split them
sensibly; if the executable is missing, ask for it.

1. **Launch.** Call `launch_app` with the executable and arguments.
   - If it does not connect, read `recentLog` (and `get_session_log`) and explain why.
   - Common causes: the layer was not found (build it, install GPU Inspector, or pass `layerDir`),
     the application is not Vulkan, or the application crashed.
2. **Watch.** Call `get_live_frame_stats`: the frame rate, and whether the frame meets the display
   refresh or is bound by submission.
3. **Capture.** Call `capture_frames` with `profilePasses` on.
   - If what the user wants to see happens at a particular moment (a level, a menu, an effect), ask
     them to bring the application there first, or use `delaySeconds` or `atFrame`.
   - If the capture has no commands, the application reuses command buffers recorded earlier:
     capture again with `recordAlways: true`.
4. **Investigate** the capture as the request calls for, with the analyze, profile or debug method of
   the skill: `get_capture_summary`, `get_bottlenecks`, `get_frame_issues`, `get_command`,
   `read_texture`, `get_shader`.
5. **Try a shader change**, when one is called for:
   - Get the stage's source with `get_shader` (view `source` when it is embedded, `glsl`
     otherwise) and edit it.
   - Apply it with `replace_shader`, then `capture_frames` again.
   - Show the effect with `compare_captures` and `read_texture`, before and after.
   - Call `restore_shader` if the change is not wanted.

   Say plainly that the edit lives only in the running application: the fix itself belongs in the
   application's shader source, and the report should give the change to make there.
6. **Report** the findings with their evidence: command indices, object ids, pass labels, and the
   measured numbers before and after.
7. **Stop.** Call `stop_app` when done, unless the user wants the application left running.
