# Vulkan Inspector

A cross-platform (Windows, Linux) graphics inspector for Vulkan applications, modelled after
[WebGPU Inspector](https://github.com/brendan-duncan/webgpu_inspector). It intercepts every
Vulkan call through a layer, so any application works without instrumentation. Unity Vulkan
players are the primary target.

* **Live object inspection** — every Vulkan object with its creation arguments, dependencies,
  labels, memory bindings and shader code (SPIR-V disassembly, GLSL, HLSL).
* **Frame capture** — the frame's command stream grouped by submit, command buffer, render pass
  and debug label, with reconstructed pipeline state per draw and read-back render targets.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design, build instructions and the
current state of the project. Third-party code and licenses are listed in
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

## Quick start (Windows)

```
git submodule update --init
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
cd app && npm install && npm start
```

Then point the launcher at a Vulkan executable (for example `build/bin/Release/vkinsp_triangle.exe`)
and press **Launch**, then **Capture** in the Capture tab.
