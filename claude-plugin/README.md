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

```
GPU Inspector ── Save capture ──► frame.gpucap ◄── MCP (stdio) ── Claude Code
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

The server is one JavaScript file with no dependencies ([server/](server/)). The only requirement
is Node.js 18 or newer on `PATH`. The GPU Inspector app does not have to be installed or running.

## Use it

Save a capture from GPU Inspector's capture bar. Then either ask in plain language:

> Why is the frame in `C:\captures\battle.gpucap` slow?
>
> The character is missing in my last capture. Find out why.

or use one of the commands:

| Command | What it does |
|---|---|
| `/gpu-inspector:analyze [capture]` | Correctness and performance review: validation, Frame Issues, bottlenecks, shaders |
| `/gpu-inspector:profile [capture]` | What limits the frame, pass by pass, and what to change (GPU Inspector's docs/PROFILING.md method) |
| `/gpu-inspector:debug [capture] <symptom>` | Traces a rendering problem to the draw and the state that causes it |
| `/gpu-inspector:compare <before> <after>` | Did a change move the numbers it should have |

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
| `get_render_graph` | Passes and the resources between them, critical path, unread outputs; one node in full |
| `compare_captures` | Timing, statistics, issues and per-pass changes between two captures |
| `list_commands`, `get_command` | The command stream; one command with the state bound at it |
| `list_objects`, `get_object` | The object graph with creation arguments |
| `get_validation` | Validation messages, linked to commands |
| `list_textures`, `read_texture` | Read-back images, as PNG plus statistics and texel values |
| `read_buffer`, `read_vertices` | Buffer ranges as scalars or GLSL structs; a draw's vertices with bounds |
| `get_shader`, `analyze_shaders` | Reflection, embedded source, GLSL/HLSL/MSL, disassembly, static cost analysis |

Answers are compact JSON: object references read `VkImage#12 "name"`, lists page, and long values
are cut with a note.

## Configuration

Environment variables, which can be set in [.mcp.json](.mcp.json):

- `VULKAN_SDK` or `INSPECTOR_TOOLS_DIR`: where `spirv-cross` and `spirv-dis` are. They are only
  needed for `get_shader`'s `glsl`, `hlsl`, `msl` and `disassembly` views; the rest reads SPIR-V
  directly.
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
