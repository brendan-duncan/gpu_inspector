# Claude Code plugin

[Docs index](README.md) › Claude Code plugin

The `gpu-inspector` plugin gives Claude Code your frame captures, read with GPU Inspector's own
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
| `/gpu-inspector:analyze [capture]` | Correctness and performance review: validation, frame issues, bottlenecks, shaders |
| `/gpu-inspector:profile [capture]` | What limits the frame, pass by pass, following [PROFILING.md](PROFILING.md) |
| `/gpu-inspector:debug [capture] <symptom>` | Traces a rendering problem to the draw and the state that causes it |
| `/gpu-inspector:compare <before> <after>` | Whether a change moved the numbers it should have |
| `/gpu-inspector:live <exe> [args]` | Launches an application, captures it, and tries shader changes while it runs |

## What makes a capture worth asking about

Set before capturing, in GPU Inspector:

- **Profile passes** — GPU times and counters. Without it there is nothing to profile.
- **Validation layer**, in the launch dialog — validation messages.
- **Stack traces** — where each command was recorded.
- Larger **Max KB**, when whole buffers matter.

## Driving an application

With GPU Inspector's capture library available, the plugin can start an application itself and
work on it without the app open:

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
