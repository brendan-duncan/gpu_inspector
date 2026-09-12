# Install

[Docs index](README.md) › Install

Installers for every release are on the
[releases page](https://github.com/brendan-duncan/gpu_inspector/releases).

| Platform | File | Install |
|---|---|---|
| Windows | `GPU-Inspector-Setup-<version>.exe` | Run it |
| Debian / Ubuntu | `gpu-inspector_<version>_amd64.deb` | `sudo apt install ./gpu-inspector_<version>_amd64.deb` |
| macOS (Apple Silicon) | `GPU-Inspector-<version>-arm64.dmg` | Open it and drag the app to Applications |
| macOS (Intel) | `GPU-Inspector-<version>-x64.dmg` | Open it and drag the app to Applications |

The installer contains the capture library — the Vulkan layer on Windows and Linux, the Metal
capture library on macOS — so nothing else has to be built or registered. macOS builds are signed
and notarized, so they open without a Gatekeeper warning.

## What you may still want

These are optional, and only affect what the inspector can show you.

| Tool | Needed for |
|---|---|
| `spirv-dis`, `spirv-cross` | GLSL, HLSL, MSL and disassembly views of Vulkan shaders. From the [Vulkan SDK](https://vulkan.lunarg.com/sdk/home), or `brew install spirv-tools spirv-cross`, or your distribution's packages |
| Vulkan SDK (`glslangValidator`, `dxc`, `glslc`) | [Shader editing](INSPECT.md#editing-a-shader) — compiling a replacement shader |
| Android SDK and NDK | [Inspecting Android devices](ANDROID.md) |

A Vulkan application also needs a Vulkan driver for your GPU, which normally comes with the
graphics driver. `vulkaninfo --summary` should list your device.

## Updates

An installed build checks for updates when it starts and offers to download one. The version label
at the right of the launch bar checks on demand. **Restart and Install** stops the applications
being inspected, installs the update and restarts.

## Building it yourself

Everything above also works from a source build. See [Building from source](BUILDING.md).

---

[Docs index](README.md) · Next: [Getting started](GETTING_STARTED.md)
