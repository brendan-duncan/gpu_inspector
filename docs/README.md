# GPU Inspector documentation

GPU Inspector captures and inspects frames from native graphics applications: Vulkan on Windows,
Linux and Android, Direct3D 12 on Windows, and Metal on macOS — and, on Windows, a
[WebGPU page in a browser](BROWSER.md) as the Direct3D 12 underneath it. Start with
[Install](INSTALL.md), then [Getting started](GETTING_STARTED.md).

![A captured frame: the command list on the left, and the state bound at the selected draw on the right](images/capture-draw.png)

## Features

Supports **Vulkan**, **Direct3D 12**, **Direct3D 11**, **Metal**, **OpenGL ES**, on **Windows**, **macOS**, **Linux**, **Android** and **Quest**.

* **Object inspection**: every GPU object, with how it was created and what it uses.
* **Frame capture**: a frame's commands, with each draw's state, buffers, textures and render targets.
* **Saved captures**: reopen them later, anywhere, without the application.
* **Export to C++**: a Vulkan, Direct3D 12 or Metal frame as a standalone project that runs it again, for driver bug reports.
* **Validation**: validation layer errors, linked to the objects and commands they name.
* **Shader editing**: change a shader and see the running application use it.
* **Shader debugger**: step through a vertex, pixel or compute shader line by line.
* **Profiling**: GPU time for each pass, and what bounds the frame.
* **GPU bottlenecks**: overdraw, triangle size and wasted work, with likely causes.
* **Pixel history**: every draw that touched a pixel, and what happened to it.
* **Mesh view**: a draw's vertices before and after its vertex shader, in 3D and as a table.
* **Acceleration structures**: a ray tracing scene in 3D, with its instances, their costs and where they overlap.
* **Render graph**: the frame's passes and the resources between them.
* **WebGPU pages**: debug the D3D12 backend of WebGPU running in Chrome, Edge, Brave or Firefox.
* **Claude Code**: a plugin that lets Claude analyze captures and drive applications.
* **Custom plugins**: add custom plug-ins for console or NDA platforms.

## Getting started

| Page | What it covers |
|---|---|
| [Install](INSTALL.md) | Installers for Windows, Linux and macOS, and updates |
| [Getting started](GETTING_STARTED.md) | Your first session: launch an application, capture a frame, save it |
| [The launch window](LAUNCH.md) | Every field of the launch window: the targets, the inspector options, queued captures |

## Platform workflows

Pick the one that matches the application you want to inspect.

| Page | For |
|---|---|
| [Vulkan](VULKAN.md) | Vulkan applications on Windows and Linux |
| [Direct3D 12](D3D12.md) | Direct3D 12 applications on Windows |
| [Metal](METAL.md) | Metal applications on macOS |
| [Android and Quest](ANDROID.md) | Vulkan applications on Android phones and headsets, over adb |
| [Web pages and WebGPU](BROWSER.md) | A WebGPU page in Chrome, Edge, Brave or Firefox, captured through the browser's GPU process |
| [OpenGL ES](GLES.md) | OpenGL ES applications on Windows (the desktop driver's ES contexts, and ANGLE), Linux and Android, through the OpenGL ES plugin |
| [Direct3D 11](D3D11.md) | Direct3D 11 applications on Windows, through the Direct3D 11 plugin |

## How-to guides

Whole workflows for particular applications, where getting in is the hard part.

| Page | What it covers |
|---|---|
| [Minecraft Bedrock](HOWTO_MINECRAFT.md) | Inspecting and capturing Minecraft for Windows, which the inspector has to wait for rather than launch |

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
| [Capture replay](REPLAY.md) | `vkinsp_replay`, the tool behind overdraw, draw overlays, mesh output, pixel history and Export to C++, and `dxinsp_replay` and `mtlinsp_replay`, which replay and export Direct3D 12 and Metal captures |
| [Architecture](ARCHITECTURE.md) | How the layer, the protocol and the UI are put together (the Metal and Direct3D 12 libraries have their own accounts in `src/metal/README.md` and `src/d3d12/README.md`) |
| [Plugins](PLUGINS.md) | Adding a graphics API as a plugin: the manifest, the backend module, the capture library and the SDK |
| [Releasing](RELEASING.md) | How releases are built and published |
| [Comparison](COMPARISON.md) | GPU Inspector's features next to RenderDoc, PIX and Nsight Graphics |

---

[Project README](../README.md) · [Changelog](../CHANGELOG.md) · [Planned work](../TODO.md)
