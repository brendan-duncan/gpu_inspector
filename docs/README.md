# GPU Inspector documentation

GPU Inspector captures and inspects frames from native graphics applications: Vulkan on Windows,
Linux and Android, Direct3D 12 on Windows, and Metal on macOS — and, on Windows, a
[WebGPU page in a browser](BROWSER.md) as the Direct3D 12 underneath it. Start with
[Install](INSTALL.md), then [Getting started](GETTING_STARTED.md).

![A captured frame: the command list on the left, and the state bound at the selected draw on the right](images/capture-draw.png)

## Getting started

| Page | What it covers |
|---|---|
| [Install](INSTALL.md) | Installers for Windows, Linux and macOS, and updates |
| [Getting started](GETTING_STARTED.md) | Your first session: launch an application, capture a frame, save it |

## Platform workflows

Pick the one that matches the application you want to inspect.

| Page | For |
|---|---|
| [Vulkan](VULKAN.md) | Vulkan applications on Windows and Linux |
| [Direct3D 12](D3D12.md) | Direct3D 12 applications on Windows |
| [Metal](METAL.md) | Metal applications on macOS |
| [Android and Quest](ANDROID.md) | Vulkan applications on Android phones and headsets, over adb |
| [Web pages and WebGPU](BROWSER.md) | A WebGPU page in Chrome, Edge, Brave or Firefox, captured through the browser's GPU process |

## Using the inspector

| Page | What it covers |
|---|---|
| [Inspect](INSPECT.md) | The live object list: creation arguments, textures, buffers, shaders, validation messages |
| [Capture](CAPTURE.md) | Capturing a frame, reading the command list, and capture files |
| [Reports](REPORTS.md) | Frame Stats, shader analysis, bottlenecks, render graph, overdraw, draw overlays, mesh view, pixel history, shader debugger |
| [Finding GPU bottlenecks](PROFILING.md) | A step-by-step method for working out what limits a frame |

## Claude Code

| Page | What it covers |
|---|---|
| [Claude Code plugin](MCP.md) | Giving Claude your captures, and letting it drive an application |

## Reference

| Page | What it covers |
|---|---|
| [Troubleshooting](TROUBLESHOOTING.md) | What to do when something does not work |
| [Building from source](BUILDING.md) | Prerequisites and build steps for each platform |
| [Capture replay](REPLAY.md) | `vkinsp_replay`, the tool behind overdraw, draw overlays, mesh output, pixel history and Export to C++, and `dxinsp_replay`, which replays and exports Direct3D 12 captures |
| [Architecture](ARCHITECTURE.md) | How the layer, the protocol and the UI are put together (the Metal and Direct3D 12 libraries have their own accounts in `src/metal/README.md` and `src/d3d12/README.md`) |
| [Releasing](RELEASING.md) | How releases are built and published |

---

[Project README](../README.md) · [Changelog](../CHANGELOG.md) · [Planned work](../TODO.md)
