# Plugins

[Docs index](README.md) › Plugins

A plugin adds a graphics API to GPU Inspector without changing the inspector: a console's API, a
mobile API, an engine's own abstraction. It brings the two things that are specific to an API:

- a **capture library**, which runs inside the application, records what it does and streams it
  to the inspector over the protocol every capture library speaks;
- a **backend module**, JavaScript that tells the inspector how to read what the library sends:
  which commands are draws and passes, what a draw's state is, how an object is described.

Everything else is the inspector's: the object list, the command tree, the render target strip,
the image viewer, buffer views, the mesh view, the render graph, frame issues, capture files and
the Claude Code tools all work on a plugin's captures unchanged.

The OpenGL ES plugin in `src/plugins/gles` is the worked example. Everything below says how it does
what it does, and [its README](../src/plugins/gles/README.md) covers the rest.

## What a plugin is

A directory holding a `plugin.json`:

```
my-plugin/
  plugin.json
  ui/backend.js            the backend module
  bin/mycapture.dll        the capture library, per platform
```

```json
{
  "id": "gles",
  "name": "OpenGL ES",
  "version": "0.1.0",
  "sdk": 1,
  "api": "gles",
  "backend": "ui/backend.js",
  "capture": {
    "win32": {
      "inject": ["bin/glesinsp_capture.dll"],
      "env": { "GLESINSP_PORT": "${port}", "GLESINSP_LOG": "${log}" }
    }
  }
}
```

| Field | Meaning |
|---|---|
| `id` | Unique, lower case letters, digits, `-` and `_`. |
| `name`, `version`, `description` | Shown in logs. |
| `sdk` | The plugin contract the plugin was written for: `1`. A plugin for a later contract than the app implements is listed with an error and not loaded. |
| `api` | The name the plugin's captures carry (`CaptureFrameResults.api`, a `.gpucap`'s manifest). Defaults to `id`. |
| `backend` | The backend module, relative to the plugin's directory. |
| `capture.<platform>` | How the capture library gets into an application the inspector launches on that platform (`win32`, `linux`, `darwin`). |
| `capture.win32.inject` | Libraries the inspector's launcher injects into the target before its first instruction. |
| `capture.linux.preload`, `capture.darwin.preload` | Libraries preloaded into the target (`LD_PRELOAD`). |
| `capture.<platform>.env` | Environment variables for the target. `${port}` is the session's port, `${log}` `1` or `0` for the launch dialog's Layer log, `${recordAlways}` and `${stacktraces}` the other options, `${pluginDir}` the plugin's directory. |

### Where plugins are found

In this order, the first plugin of each id winning:

1. the directories in `GPU_INSPECTOR_PLUGINS` (a path list; each entry a plugin or a directory of them);
2. the user's plugins directory: `%APPDATA%\gpu-inspector\plugins` on Windows,
   `~/Library/Application Support/gpu-inspector/plugins` on macOS, `~/.config/gpu-inspector/plugins`
   on Linux, or `$GPU_INSPECTOR_HOME/plugins`;
3. `build/plugins` in a checkout, which is where this repository's plugins are built;
4. `resources/plugins` in an installed app.

A plugin in the user's directory therefore overrides one shipped with the app. The Claude Code
plugin's MCP server looks in the same places and loads the same backends.

## The backend module

An ES module exporting `activate`, which is given the host and returns the API's `Backend`:

```ts
import type { Backend, PluginHost } from "../../../sdk/ts/index.js";

export function activate(host: PluginHost): Backend {
  const { isObject, num, str } = host.util;
  return {
    id: "gles",
    displayName: "OpenGL ES",
    objectTypePrefixes: ["GL"],
    sets: { ...host.emptySets, DRAW: new Set(["glDrawArrays", "glDrawElements"]), /* ... */ },
    replay: { draws: false, /* ... */ },
    live: { overdraw: false, pixelHistory: false, drawOverlay: false },
  };
}
```

The module is loaded in two places: the app's window, and the MCP server, which has no DOM. So a
backend module describes rather than draws. It imports nothing from the app at run time. Everything
it runs with comes from the host: `host.util` has the helpers for reading serialized arguments, and
`host.emptySets` is a classification with nothing in it, to spread its own over. The types it is
written against are the app's own, re-exported by `src/sdk/ts/index.ts`. Type-only imports vanish
when the module is bundled, so it stands alone. A plugin in this repository is bundled by
`src/app/build.mjs`, which builds `src/plugins/<id>/ui/backend.ts` into
`build/plugins/<id>/ui/backend.js`; an out-of-tree plugin bundles its own with any tool that writes
an ES module.

### What a backend says

The interface is in `src/app/src/renderer/backend.ts`. The required fields:

| Field | Meaning |
|---|---|
| `id`, `displayName` | The `api` its captures carry, and the API's name in messages ("OpenGL ES"). |
| `objectTypePrefixes` | The prefixes of its object types (`["GL"]`). They say whose an object is, and the object list drops them ("GLTexture" lists under Textures). |
| `sets` | The command classification (`CommandSets` in `renderer/command_sets.ts`): which methods are draws, dispatches, pass begins and ends, debug group begins and ends, submits, pipeline binds. The command tree, the pass list, frame statistics and every report are built from it. |
| `replay` | Which of the replay tool's analyses exist for the API: all `false` for a plugin, which has no replay tool. |
| `live` | Which measurements the capture library takes while it captures (overdraw, pixel history, draw overlays). |

The optional ones fill in what the command sets alone cannot say:

| Hook | What it provides |
|---|---|
| `sets.summarize(cmd, nameOf)` | The short text beside a command in the tree ("36 idx", a texture's name). |
| `sets.labelOf(cmd)` | The name a debug group opens with. |
| `sets.passLabel(cmd, index, nameOf)` | What a pass is called in the tree and the render target strip. |
| `sets.drawArgsOf(cmd)` | A draw's counts in Vulkan's terms (indexed or not, vertex or index count, first, offset), for the mesh view. |
| `drawState(data, db, cmd)` | The state bound at a draw, when a walk back through the command stream cannot find it. Vertex layouts go in `vertexInput` in the shape of `vkCmdSetVertexInputEXT`'s arguments, with `VK_FORMAT_*` names, which is what the mesh view and the buffer views read. |
| `vertexInputNames(cmd)` | A draw's vertex inputs by location, for the mesh view's columns. |
| `commandDetails(cmd, ctx)` | Sections to show for a command, described as data (`DetailSection`): label/value rows, tables, source text. A value can be an object reference, a captured texture (shown as a thumbnail opening the image viewer) or a captured buffer range, typed by the member offsets given. The app draws them, and the MCP server's `get_command` reports them. |
| `objectSummary(obj)`, `objectBytes(obj)` | An object's line in the object list, and the memory it holds. |
| `resourceSource(db)` | What each pass writes and each draw reads, for the render graph and its rules. Without it the graph is empty. |
| `analyzeFrame(data, db)` | The API's own frame analysis rules, beside the API-neutral ones that run for every capture. |
| `advice` | The API's spelling of the render graph's suggestions: how to discard a target, how to keep one in tile memory. |
| `submitCall` | The call FrameStats' submit time is measured inside, for the Frame Bound card. |

## The capture library

A capture library is the server side of the protocol every built-in capture library speaks. The
inspector connects to it, asks it for things, and reads what it streams. The message vocabulary is
`src/app/src/shared/protocol.ts`, and the framing is the Vulkan layer's (`src/vulkan/src/transport.h`):

```
u32 payloadLength (little endian), u8 kind, payload
kind 0: UTF-8 JSON
kind 1: u32 headerLength, JSON header, raw bytes
```

### What the SDK gives it

`src/sdk/include/gpu_inspector/sdk/` is header-only C++20 with no dependencies:

| Header | What it has |
|---|---|
| `transport.h` | `Server`: the listener, the framing, a sender thread so an API call never waits on the network, and the probe handshake the attach list relies on. It listens on 127.0.0.1 on the port the inspector gave, or on the first free port of the range every capture library shares, or on Android on an abstract Unix socket. It calls back on connect (send the object snapshot there), on each message and on disconnect. |
| `json.h` | `JsonWriter`, with the conventions the inspector reads arguments by: `Ref(id, className)` for an object reference, `Bytes()` for raw bytes. It also has `ParseJson` for the inspector's requests. |
| `config.h` | `Config`: the library's settings from the environment, from the launcher's settings block (a process the launcher did not start), or from Android system properties (`GLESINSP_PORT` is read from `debug.glesinsp.port`). |

### Getting in

On Windows the inspector's launcher (`dxinsp_launch.exe`) injects every library in
`capture.win32.inject` into a target it launches, beside the Direct3D 12 library. It loads the
library while the target is still suspended, then calls its initializer:

```cpp
extern "C" __declspec(dllexport) DWORD WINAPI GpuInspectorInitialize(LPVOID settings);
```

`settings` is null for a launch, whose target inherited the plugin's environment. For a process the
launcher did not start (the **Wait for application** target), it is the plugin's variables as an
environment block, which `Config::ApplySettingsBlock` takes. The initializer returns 0 when the
library is in. It runs before the application's own code, so it installs hooks and returns: the
connection should wait until the API is actually used. The OpenGL ES library starts its server at
the first `eglCreateContext`, so a process that never uses OpenGL ES leaves the session's port to
the Vulkan layer or the Direct3D 12 library. On Linux the library is preloaded instead, and on
consoles and phones getting in is the platform's business: a layer mechanism, a tool, a build flag.

### What it must send

On connect, the objects that exist: `Snapshot` with their count, then an `AddObject` for each
(parents before children). While connected: `AddObject`, `DeleteObjects`, `ObjectUpdate` (a changed
field of an object's description), `ObjectSetLabel`, and `FrameStats` every 100 ms or so.

It must answer `Ping` with `Pong`, `RequestSnapshot` with the snapshot again, and `Capture` with a
capture. A request it does not handle can be ignored. `RequestStacktraces` is best answered with
`Stacktraces` saying `available: false`, since a capture being saved waits for it.

A capture is, in order:

1. `CaptureFrameResults`, with `api` set to the plugin's api;
2. `CaptureFrameCommands`: the commands, in batches. Each has `index`, `frame` (0-based within the
   capture), `method`, `object` (a reference to the stream the command belongs to: a command buffer,
   a queue, a context), `args`, and any fields of the plugin's own, which the backend reads;
3. `CaptureTextureFrames` then a `CaptureTextureData` per texture: render targets keyed by `frame`,
   `commandBuffer` (the stream's object id), `passIndex` and `attachment`, and textures a draw read
   keyed by `capture` (`kind: "sampled"`);
4. `CaptureBuffers` then a `CaptureBufferData` per range;
5. optionally `CapturePassTimings`;
6. `CaptureComplete`.

Two things have to agree with the backend. **Pass indices**: the inspector finds a pass's render
targets by counting the `PASS_BEGIN` commands of the same stream since the stream's last `SUBMIT`
command (and its last `RECORD_BEGIN`, for an API that has one), so the library numbers its passes
the same way. **Formats**:
texture formats are `VK_FORMAT_*` names, the vocabulary the inspector's texture decoder reads for
every API (the Direct3D 12 library maps DXGI formats to them too), and texels are laid out as
Vulkan lays that format out. Block-compressed formats (BC, ETC2, EAC, ASTC) are decoded by the
inspector, so a library can send compressed data as it is.

## Building and shipping a plugin

In this repository a plugin lives in `src/plugins/<id>`, with its `plugin.json`, a `CMakeLists.txt`
the root one adds, and `ui/backend.ts`. CMake builds the capture library into
`build/plugins/<id>/bin`, and `npm run build` in `src/app` writes the backend module and copies
`plugin.json` into `build/plugins/<id>`. The app finds it there from a checkout.
`tools/stage_layer.mjs` copies `build/plugins` into the installer, which the app finds as
`resources/plugins`.

A plugin built elsewhere, such as a console plugin that cannot live in a public repository, is the
same directory, dropped into the user's plugins directory or named by `GPU_INSPECTOR_PLUGINS`. Its
backend module is compiled against a checkout's `src/sdk/ts`, and its capture library includes a
checkout's `src/sdk/include`. Nothing of the plugin has to be in this repository.

## What a plugin does not get

Things that are specific to an API and live in the app, not behind the plugin interface:

- **Replay**: overdraw, draw overlays, pixel history, per-draw timings, hardware counters and Export
  to C++ replay a capture with an API's own replay tool. A plugin's library can measure the first
  three while it captures and send them (`live` says which), as the Direct3D 12 and Metal libraries do.
- **The shader debugger and shader analysis**, which interpret SPIR-V and MSL.
- **Shader editing**, which needs the API's own compiler on this machine.
- **Launch targets beyond local processes**: an API on a console or a device needs a way to start an
  application there and forward the port, which the app does for Android over adb but not yet for a plugin.
