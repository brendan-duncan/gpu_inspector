# GPU Inspector — Claude Code plugin

Debug and profile Vulkan, Metal and Direct3D 12 frames with Claude. The plugin gives Claude Code the frame
captures GPU Inspector saves (`.gpucap`), read with GPU Inspector's own analyses:
- the frame issues rules
- the GPU bottleneck measurements
- the render graph
- the state bound at every draw, with uniform values decoded by shader reflection
- vertex and index data
- read-back render targets, returned as images
- shaders

What Claude reports matches what the Capture tab shows.

It can also drive a running application itself. The plugin launches an application with GPU
Inspector's capture library in it — the Vulkan layer, on macOS the Metal library, and on Windows
the Vulkan layer and the Direct3D 12 library both, whichever the application turns out to use —
watches its frame rate, captures its frames, and swaps a pipeline's shader while it runs, so a fix
can be tried and measured on the spot.

```
GPU Inspector ── Save capture ──► frame.gpucap ◄──┐
                                                  MCP (stdio) ── Claude Code
Application + capture library ◄── socket ─────────┘
```

## Install

The repository is a Claude Code plugin marketplace
([../.claude-plugin/marketplace.json](../.claude-plugin/marketplace.json)). From a terminal:

```sh
claude plugin marketplace add brendan-duncan/gpu_inspector
claude plugin install gpu-inspector@gpu-inspector-plugins
```

Inside the terminal CLI the same commands are `/plugin marketplace add ...` and `/plugin install ...`.
In the VS Code and JetBrains extensions, open `/plugins` and add the marketplace there.

The server is one JavaScript file with no dependencies ([server/](server/)). Reading saved captures
needs only Node.js 18 or newer on `PATH`; the GPU Inspector app does not have to be installed or
running.

Launching applications also needs GPU Inspector's capture library. The server looks for it in three
places:
- **A checkout's build:** when the plugin runs from a checkout (`claude --plugin-dir`), or when
  `GPU_INSPECTOR_ROOT` names one.
- **An installed GPU Inspector.**
- **`INSPECTOR_LAYER_DIR`** (on macOS, `INSPECTOR_METAL_LIB`; on Windows, `INSPECTOR_D3D12_DIR`
  for the D3D12 library, its launcher and its shader tool).

Replacing shaders needs the Vulkan SDK's compilers for a Vulkan pipeline, and `dxc` for a D3D12
one. A D3D12 capture's shader text and reflection come from `dxinsp_shader.exe`, built beside the
D3D12 library; DXIL needs `dxcompiler.dll`, which it finds in the Vulkan SDK or the Windows SDK.

Android applications have more requirements:
- adb (the Android SDK platform-tools).
- The Android layer, built with `tools/build_android.py` or from an installed GPU Inspector.
  `INSPECTOR_ANDROID_LAYER_DIR` names another location.
- A debuggable application.

## Use it

Save a capture from GPU Inspector's capture bar. Then either ask in plain language:

> Why is the frame in `C:\captures\battle.gpucap` slow?
>
> The character is missing in my last capture. Find out why.
>
> Launch `build\bin\Release\my_game.exe`, capture a frame, and make the most expensive fragment
> shader cheaper. Show me the before and after.

or use one of the commands:

| Command | What it does |
|---|---|
| `/gpu-inspector:analyze [capture]` | Correctness and performance review: validation, Frame Issues, bottlenecks, shaders |
| `/gpu-inspector:profile [capture]` | What limits the frame, pass by pass, and what to change (GPU Inspector's docs/PROFILING.md method) |
| `/gpu-inspector:debug [capture] <symptom>` | Traces a rendering problem to the draw and the state that causes it |
| `/gpu-inspector:compare <before> <after>` | Did a change move the numbers it should have |
| `/gpu-inspector:live <exe> [args] [what to look at]` | Launches an application, captures it, and tries shader changes while it runs |

Without a path, Claude picks from the captures GPU Inspector saved or opened recently (its
settings file keeps the list). The bundled `gpu-capture-analysis` skill tells Claude how to read a
capture.

**What makes a capture useful** is set in GPU Inspector before capturing:
- **Profile passes**, for GPU times and counters.
- The launch dialog's **Validation layer**, for validation messages.
- **Stack traces**, for where commands were recorded.
- Larger buffer limits, when whole buffers matter.

## Tools

| Tool | Purpose |
|---|---|
| `open_capture`, `list_captures`, `close_capture` | Open `.gpucap` files; list open and recent captures |
| `get_capture_summary` | Counts, frame timing and Frame Bound verdict, slowest passes, issues, validation, notes |
| `get_frame_issues` | Frame Issues rules, each naming its command |
| `get_bottlenecks` | Per-pass GPU time, overdraw, fragments per primitive, depth rejection, bound stage, problems |
| `get_hw_counters` | The GPU's own hardware counters per pass and per draw (the limiters: SM throughput, VRAM bandwidth, cache, occupancy, ALU/FMA), what Nsight Graphics shows — a Vulkan capture replayed with `vkinsp_replay`; needs NVIDIA's Nsight Perf SDK or `VK_KHR_performance_query` and GPU counter access; not Metal or D3D12 |
| `export_cpp` | Export to C++: a Vulkan, Direct3D 12 or Metal capture's frame written as a standalone CMake project (every object, the contents the frame read, every command) whose program runs the frame and compares its render targets with the capture's — for a driver bug report; replayed with `vkinsp_replay` (`dxinsp_replay` for D3D12, `mtlinsp_replay` for Metal) to write it |
| `get_overdraw` | Overdraw measured per pixel (Metal captures taken with `overdraw`; Vulkan captures replayed with `vkinsp_replay`; not D3D12): every pass's figures, or one pass's heatmap as PNG |
| `get_draw_overlay` | Where one draw landed: the pixels it covered, what its depth and stencil tests did with them, and how many its own back-face culling emptied (a Vulkan capture replayed; a D3D12 capture measures overlays in the application while capturing) |
| `get_pixel_history` | Every clear and draw that touched one pixel, what each draw's fragments met (culled, discarded, depth, stencil, written) and the value after each; a draw that put several fragments on the pixel lists each one's primitive and shader output (a Vulkan capture replayed; a Metal capture taken with `capture_frames` `pixelHistory`; not D3D12) |
| `get_mesh_output` | What a draw's vertex shader wrote (VS Out): every output, vertices behind the eye, primitives outside the view volume, triangles with no area, NaN positions (a Vulkan capture replayed; not Metal or D3D12) |
| `debug_shader` | Runs one vertex, pixel or compute invocation in GPU Inspector's interpreter, SPIR-V or Metal Shading Language: outputs, every source line's values in order, the first NaN or infinity, and the GPU's result to compare with (a Vulkan pixel needs the replay, a Metal one does not); `decompiled` steps SPIR-V without debug information by line, through GLSL decompiled from it and checked against the original; a D3D12 shader is stepped as its HLSL compiled to SPIR-V by dxc (there is no DXIL interpreter), so it needs the source: `-Zi`, or a PDB under `symbolDirs` |
| `get_render_graph` | Passes and the resources between them, critical path, unread outputs; one node in full |
| `compare_captures` | Timing, statistics, issues and per-pass changes between two captures |
| `list_commands`, `get_command` | The command stream; one command with the state bound at it |
| `list_objects`, `get_object` | The object graph with creation arguments |
| `get_validation` | Validation messages, linked to commands |
| `list_textures`, `read_texture` | Read-back images, as PNG plus statistics and texel values |
| `read_buffer`, `read_vertices` | Buffer ranges as scalars or GLSL structs; a draw's vertices with bounds |
| `get_shader`, `analyze_shaders` | Reflection, embedded source, GLSL/HLSL/MSL, disassembly, static cost analysis. A D3D12 pipeline gives DXBC/DXIL reflection, disassembly and its HLSL (embedded by `dxc -Zi`, or out of the PDB `dxc -Zs` wrote, found under `set_search_paths`' `symbolDirs`); the SPIR-V cost analysis is Vulkan only |
| `get_shader_flame_graph` | The frame's shading work by pass, pipeline or draw, stage, function and source line, and its hottest functions and lines (Vulkan; a capture's draws are measured by replay on first use) |
| `measure_shader_cost` | Vulkan: a draw's shader stage replayed with each function, source line and texture taken out, giving what each costs on this GPU; the flame graph then sizes the stage by it |
| `set_search_paths` | Where shader sources and unstripped libraries are, for shaders without embedded text and stack frames without symbols |
| `launch_app`, `attach_app`, `stop_app` | Start an application with the capture library (on Windows the Vulkan and D3D12 libraries both, whichever it uses), or connect to one listening; end it |
| `wait_for_app` | Windows: wait for a Direct3D 12 application someone else starts (a launcher, an editor) and put the capture library into it as it starts, then connect — D3D12's answer to the Vulkan implicit layer. Call it before the application is launched; one already running cannot be caught |
| `list_android_devices`, `launch_android_app` | Android devices and packages over adb; start a debuggable package with the Vulkan layer |
| `list_sessions`, `get_session_status`, `get_session_log` | Live sessions: state, device, frame reports, objects, validation, output |
| `get_live_frame_stats` | Frame time, rate, submit time, refresh and dropped frames over a few seconds, with a verdict |
| `capture_frames` | Capture frames of a running application into a `.gpucap` and open it |
| `replace_shader`, `restore_shader` | Compile a stage's new source and swap it into the running pipeline (linked from libraries or not) or shader object; undo it |
| `read_live_image`, `get_live_descriptor_set` | An image's current pixels, or what a descriptor set binds now, without capturing |
| `list_live_objects`, `get_live_object` | The live objects, and one in full with its creation stack |

Answers are compact JSON: object references read `VkImage#12 "name"`, lists page, and long values
are cut with a note.

## Configuration

Environment variables, which can be set in [.mcp.json](.mcp.json):

- `VULKAN_SDK` or `INSPECTOR_TOOLS_DIR`: where the SDK's shader tools are. `spirv-cross` and
  `spirv-dis` are only needed for `get_shader`'s `glsl`, `hlsl`, `msl` and `disassembly` views; the
  rest reads SPIR-V directly. `glslangValidator`, `dxc` and `spirv-as` are needed for
  `replace_shader`.
- `GPU_INSPECTOR_CAPTURES_DIR`: where `capture_frames` saves captures. The default is
  `gpu-inspector-captures` in the system's temporary directory.
- `GPU_INSPECTOR_ROOT`: a GPU Inspector checkout whose build holds the capture library.
- `GPU_INSPECTOR_SOURCE_ROOTS` and `GPU_INSPECTOR_SYMBOL_DIRS` (`;`-separated): where the shader
  sources and the application's unstripped libraries are.
  - Source roots serve shaders compiled with line information but no embedded text.
  - Symbol directories serve stack frames named only by module and offset (Android, Linux).
  - Without these variables, the server uses the Source roots and Symbol directories that GPU
    Inspector's launch dialog used last. The `set_search_paths` tool changes them while the server
    runs.
- `INSPECTOR_LAYER_DIR` (the directory holding `VK_LAYER_INSPECTOR_capture.json`) and
  `INSPECTOR_METAL_LIB` (macOS): the capture library itself.
- `GPU_INSPECTOR_SETTINGS`: GPU Inspector's settings file, for the recent captures list. Defaults
  to the app's user data directory: `%APPDATA%\gpu-inspector`, `~/Library/Application
  Support/gpu-inspector`, or `~/.config/gpu-inspector`.

## Development

The server's sources are [src/app/src/mcp/](../src/app/src/mcp). It reuses the renderer's analysis modules
(capture format, object database, frame rules, pass metrics, render graph, SPIR-V reflection and
analysis, texture decoding) rather than copies of them.

- `npm run build` in `src/app/` rebuilds [server/gpu-inspector-mcp.mjs](server/gpu-inspector-mcp.mjs)
  along with the app. The build fails if the server would bundle a UI module.
- The bundle is committed, since a plugin installs from the repository as it is. Commit it with the
  source changes that produced it.
- `npm test` covers the tools against a generated capture, and the bundled server over stdio.
- `claude --plugin-dir <checkout>/claude-plugin` loads the working copy for one session (terminal
  CLI). `/reload-plugins` picks up a rebuilt server.

**Releasing:** users receive an update only when `version` in
[.claude-plugin/plugin.json](.claude-plugin/plugin.json) changes. Bump it with any change to the
server, the skill or the commands.
