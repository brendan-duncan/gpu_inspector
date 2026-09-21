<p align="center"><img src="docs/images/title.png" alt="GPU Inspector" width="800"></p>

**GPU Inspector** captures and inspects frames from native graphics applications. It is the
native counterpart of [WebGPU Inspector](https://github.com/brendan-duncan/webgpu_inspector).

Supports **Vulkan**, **Direct3D 12**, **Metal**, **Android** and **Quest**, on **Windows**, **macOS** and **Linux**.

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
* **Render graph**: the frame's passes and the resources between them.
* **WebGPU pages**: debug the D3D12 backend of WebGPU running in Chrome, Edge, Brave or Firefox.
* **Claude Code**: a plugin that lets Claude analyze captures and drive applications.

## Documentation

The user documentation is in [docs](docs/README.md):

| | |
|---|---|
| [Install](docs/INSTALL.md) · [Getting started](docs/GETTING_STARTED.md) | the first session, start to a saved capture |
| [The launch window](docs/LAUNCH.md) | launch apps for inspection and capture |
| [Vulkan](docs/VULKAN.md) · [Direct3D 12](docs/D3D12.md) · [Metal](docs/METAL.md) · [Android and Quest](docs/ANDROID.md) · [Web pages and WebGPU](docs/BROWSER.md) | the workflow for each platform |
| [Minecraft Bedrock](docs/HOWTO_MINECRAFT.md) | how-to guides for particular applications |
| [Inspect](docs/INSPECT.md) · [Capture](docs/CAPTURE.md) · [Reports](docs/REPORTS.md) | using the inspector |
| [Finding GPU bottlenecks](docs/PROFILING.md) | working out what limits a frame |
| [Claude Code plugin](docs/MCP.md) | asking Claude about a capture |
| [Troubleshooting](docs/TROUBLESHOOTING.md) · [Building from source](docs/BUILDING.md) | when something does not work, and building it yourself |

---

## License and Usage

GPU Inspector is provided under the [MIT](LICENSE) license, meaning there are no restrictions on how you use it, commercial or otherwise.

GPU Inspector includes **no analytics** on use or installation. I have no idea who is using it or for what purpose, and neither does any third party. Use on proprietary or NDA projects will stay private. 

I would love to hear if you are using it and find it useful, so feel free to reach out to me either in the [Discussions](https://github.com/brendan-duncan/gpu_inspector/discussions) or via [email](<mailto:brendandduncan@gmail.com?subject=GPU%20Inspector%20feedback>).

GPU Inspector is built on the inspiration of others. The libraries used, and projects that code has been adapted from, are listed in the [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES.md).
