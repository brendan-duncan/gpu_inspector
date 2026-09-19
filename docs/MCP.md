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

A few tools replay a Vulkan capture on this machine's GPU, the way the app does. The first question
about a capture takes a moment while its replay starts; the replay is kept for the ones after it:

- `get_overdraw` — fragments per pixel, for every pass or as one pass's heatmap
- `get_pixel_history` — every clear and draw that touched a pixel, and what became of each draw's
  fragments
- `get_mesh_output` — what a draw's vertex shader wrote: vertices behind the eye, primitives
  outside the view volume, triangles with no area
- `debug_shader` — one vertex, pixel or compute invocation run in the shader debugger: its outputs,
  the values every line computed in order, and the first NaN or infinity. A Vulkan capture's SPIR-V
  or a Metal capture's Metal Shading Language. A vertex or compute invocation needs no replay; a
  Vulkan pixel does, and a Metal one does not. `decompiled` steps SPIR-V without debug information
  by line, through GLSL decompiled from it, and says whether that agrees with the original.
- `get_shader_flame_graph` — measures every draw the first time it is asked
- `measure_shader_cost` — what a draw's fragment or compute shader spends in each function, source
  line and texture, timed with each taken out; the flame graph then sizes that stage by it
- `get_hw_counters` — the GPU's own hardware counters per pass and per draw (the limiters: SM
  throughput, memory bandwidth, cache, occupancy), what Nsight Graphics shows. Needs NVIDIA's
  Nsight Perf SDK or `VK_KHR_performance_query`, and GPU counter access enabled; `list: true` names
  what the GPU offers

- `export_cpp` — the frame written as a standalone C++ project that re-creates its objects and runs
  it again, for a driver bug report ([Export to C++](REPLAY.md#export-to-c))

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

On Windows a launch carries both capture libraries: the Vulkan layer's environment and the
Direct3D 12 library, injected by its launcher (`dxinsp_launch.exe`, from the same directory or
`INSPECTOR_D3D12_DIR`), and whichever API the application uses connects. For a Direct3D 12
application the plugin does not start itself, `wait_for_app` watches for the executable's name and
injects the library as the process starts; call it before the application is launched, since one
that already has a device cannot be caught. A D3D12 session's
`replace_shader` takes HLSL, compiled to DXIL with `dxc`, and `get_shader` shows a D3D12 pipeline's
reflection, embedded source and disassembly (through `dxinsp_shader.exe`), and `debug_shader`
steps a stage as its HLSL compiled to SPIR-V by `dxc`, so it needs the source (`-Zi`, or a PDB
under `symbolDirs`). The tools that replay a capture — overdraw, pixel history, mesh output, the
flame graph's measured draws, shader cost by ablation — are Vulkan-only (Metal has its own paths
for some of them) and say so for a D3D12 capture.

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
