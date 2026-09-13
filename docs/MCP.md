# Claude Code plugin

[Docs index](README.md) › Claude Code plugin

The `gpu-inspector` plugin lets Claude Code read your frame captures with GPU Inspector's own
analyses — the same frame rules, bottleneck measurements, render graph, draw state and shader
reflection the app shows. It can also launch an application and drive it: capture frames, watch
the frame rate, and replace a shader while it runs.

This page is the short version. The full tool list and every configuration variable are in
[claude-plugin/README.md](../claude-plugin/README.md).

## Install

From a terminal:

```sh
claude plugin marketplace add brendan-duncan/gpu_inspector
claude plugin install gpu-inspector@gpu-inspector-plugins
```

In the Claude Code terminal CLI the same commands are `/plugin marketplace add ...` and
`/plugin install ...`. In the VS Code and JetBrains extensions, open `/plugins` and add the
marketplace there.

Reading saved captures needs only Node.js 18 or newer. Neither GPU Inspector nor the application
has to be running.

## Use it

Save a capture from the capture bar, then ask:

> Why is the frame in `C:\captures\battle.gpucap` slow?

> The character is missing in my last capture. Find out why.

> Launch `build\bin\Release\my_game.exe`, capture a frame, and make the most expensive fragment
> shader cheaper. Show me the before and after.

Without a path, Claude picks from the captures GPU Inspector saved or opened recently.

Or use a command:

| Command | What it does |
|---|---|
| `/gpu-inspector:analyze [capture]` | Reviews correctness and performance: validation, frame issues, bottlenecks, shaders |
| `/gpu-inspector:profile [capture]` | Works out what limits the frame, pass by pass, following [PROFILING.md](PROFILING.md) |
| `/gpu-inspector:debug [capture] <symptom>` | Traces a rendering problem to the draw and the state that causes it |
| `/gpu-inspector:compare <before> <after>` | Says whether a change moved the numbers it should have |
| `/gpu-inspector:live <exe> [args]` | Launches an application, captures it, and tries shader changes while it runs |

## Replaying a capture

A few tools replay a Vulkan capture on this machine's GPU, the way the app does, and take seconds
rather than milliseconds:

- `get_overdraw` — fragments per pixel, for every pass or as one pass's heatmap
- `get_pixel_history` — every clear and draw that touched a pixel, and what became of each draw's
  fragments
- `get_mesh_output` — what a draw's vertex shader wrote: vertices behind the eye, primitives
  outside the view volume, triangles with no area
- `get_shader_flame_graph` — measures every draw the first time it is asked

They need `vkinsp_replay`: GPU Inspector's Windows and Linux installers put it beside the layer, and a checkout
builds it (`cmake --build build --target vkinsp_replay`). See [Capture replay](REPLAY.md).

## What to turn on before capturing

Claude can only read what the capture recorded, so in GPU Inspector, before you capture:

- **Profile passes** — GPU times and counters. Without them there is nothing to profile.
- **Validation layer**, in the launch dialog — so the capture carries the validation messages.
- **Stack traces** — where each command was recorded.
- A larger **Max KB**, when whole buffers matter.

## Driving an application

With GPU Inspector's capture library available, the plugin can start an application itself and
work on it without GPU Inspector running:

- launch it, and watch its frame rate
- capture frames into `.gpucap` files
- replace a pipeline's shader while it runs, then capture again to measure the change
- launch a debuggable package on an [Android device](ANDROID.md) over adb

The library comes from an installed GPU Inspector, from a checkout named by `GPU_INSPECTOR_ROOT`,
or from `INSPECTOR_LAYER_DIR` (`INSPECTOR_METAL_LIB` on macOS). Replacing shaders also needs the
Vulkan SDK's compilers. `/gpu-inspector:live <exe>` walks through it.

## Configuration

The variables worth knowing, set in the plugin's `.mcp.json`:

| Variable | What it is for |
|---|---|
| `VULKAN_SDK` or `INSPECTOR_TOOLS_DIR` | Where the shader tools are, for GLSL/HLSL/MSL views and for compiling replacement shaders |
| `GPU_INSPECTOR_CAPTURES_DIR` | Where captures taken by the plugin are saved |
| `GPU_INSPECTOR_ROOT` | A checkout whose build holds the capture library |
| `GPU_INSPECTOR_SOURCE_ROOTS`, `GPU_INSPECTOR_SYMBOL_DIRS` | Where shader sources and unstripped libraries are |

The rest, including the full tool list, is in
[claude-plugin/README.md](../claude-plugin/README.md).

---

Previous: [Finding GPU bottlenecks](PROFILING.md) · [Docs index](README.md) · Next: [Troubleshooting](TROUBLESHOOTING.md)
