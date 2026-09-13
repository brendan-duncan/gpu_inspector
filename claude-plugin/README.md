# GPU Inspector — Claude Code plugin

Debug and profile Vulkan and Metal frames with Claude. The plugin gives Claude Code the frame
captures GPU Inspector saves (`.gpucap`), read with GPU Inspector's own analyses:
- the frame issues rules
- the GPU bottleneck measurements
- the render graph
- the state bound at every draw, with uniform values decoded by shader reflection
- vertex and index data
- read-back render targets, returned as images
- shaders

What Claude reports matches what the Capture tab shows.

It can also drive a running application itself. The plugin launches a Vulkan application (on macOS a
Metal one) with GPU Inspector's capture library in it, watches its frame rate, captures its frames,
and swaps a pipeline's shader while it runs, so a fix can be tried and measured on the spot.

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
- **`INSPECTOR_LAYER_DIR`** (on macOS, `INSPECTOR_METAL_LIB`).

Replacing shaders also needs the Vulkan SDK's compilers.

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
| `get_overdraw` | Overdraw measured per pixel (Metal captures taken with `overdraw`; Vulkan captures replayed with `vkinsp_replay`): every pass's figures, or one pass's heatmap as PNG |
| `get_pixel_history` | Every clear and draw that touched one pixel, what each draw's fragments met (culled, discarded, depth, stencil, written) and the value after each (a Vulkan capture replayed; a Metal capture taken with `capture_frames` `pixelHistory`) |
| `get_mesh_output` | What a draw's vertex shader wrote (VS Out): every output, vertices behind the eye, primitives outside the view volume, triangles with no area, NaN positions (a Vulkan capture replayed) |
| `get_render_graph` | Passes and the resources between them, critical path, unread outputs; one node in full |
| `compare_captures` | Timing, statistics, issues and per-pass changes between two captures |
| `list_commands`, `get_command` | The command stream; one command with the state bound at it |
| `list_objects`, `get_object` | The object graph with creation arguments |
| `get_validation` | Validation messages, linked to commands |
| `list_textures`, `read_texture` | Read-back images, as PNG plus statistics and texel values |
| `read_buffer`, `read_vertices` | Buffer ranges as scalars or GLSL structs; a draw's vertices with bounds |
| `get_shader`, `analyze_shaders` | Reflection, embedded source, GLSL/HLSL/MSL, disassembly, static cost analysis |
| `get_shader_flame_graph` | The frame's shading work by pass, pipeline or draw, stage, function and source line, and its hottest functions and lines (a Vulkan capture's draws are measured by replay on first use) |
| `set_search_paths` | Where shader sources and unstripped libraries are, for shaders without embedded text and stack frames without symbols |
| `launch_app`, `attach_app`, `stop_app` | Start an application with the capture library (or connect to one listening), end it |
| `list_android_devices`, `launch_android_app` | Android devices and packages over adb; start a debuggable package with the Vulkan layer |
| `list_sessions`, `get_session_status`, `get_session_log` | Live sessions: state, device, frame reports, objects, validation, output |
| `get_live_frame_stats` | Frame time, rate, submit time, refresh and dropped frames over a few seconds, with a verdict |
| `capture_frames` | Capture frames of a running application into a `.gpucap` and open it |
| `replace_shader`, `restore_shader` | Compile a stage's new source and swap it into the running pipeline; undo it |
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

The server's sources are [app/src/mcp/](../app/src/mcp). It reuses the renderer's analysis modules
(capture format, object database, frame rules, pass metrics, render graph, SPIR-V reflection and
analysis, texture decoding) rather than copies of them.

- `npm run build` in `app/` rebuilds [server/gpu-inspector-mcp.mjs](server/gpu-inspector-mcp.mjs)
  along with the app. The build fails if the server would bundle a UI module.
- The bundle is committed, since a plugin installs from the repository as it is. Commit it with the
  source changes that produced it.
- `npm test` covers the tools against a generated capture, and the bundled server over stdio.
- `claude --plugin-dir <checkout>/claude-plugin` loads the working copy for one session (terminal
  CLI). `/reload-plugins` picks up a rebuilt server.

**Releasing:** users receive an update only when `version` in
[.claude-plugin/plugin.json](.claude-plugin/plugin.json) changes. Bump it with any change to the
server, the skill or the commands.
