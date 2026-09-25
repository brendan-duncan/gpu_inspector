# Building from source

[Docs index](README.md) › Building from source

Only needed if you want to build GPU Inspector yourself, or to build the
[Android layer](ANDROID.md), which the installers do not include. To use the released builds, see
[Install](INSTALL.md).

## Prerequisites

Linux and Windows need the same things: a C++20 compiler, CMake 3.20 or newer, Python 3.8 or newer
(the layer's source is generated from `vk.xml`), Node.js 18 or newer with npm, the windowing-system
headers Vulkan's surface extensions include, and the shader tools `glslc`, `spirv-dis` and
`spirv-cross`. Windows also builds the Direct3D 12 capture library, which needs a recent Windows
SDK and the MinHook submodule, and its test application, which needs `dxc`. A macOS build makes
the Metal capture library instead of the Vulkan layer, and needs less.

### Linux

```sh
sudo apt install build-essential cmake ninja-build git python3 nodejs npm \
                 libvulkan-dev libxcb1-dev libx11-dev libwayland-dev libxrandr-dev \
                 libegl-dev libgles-dev libsdl2-dev \
                 glslc spirv-tools spirv-cross vulkan-tools
```

| Package | Needed for |
|---|---|
| `build-essential`, `cmake`, `ninja-build`, `git`, `python3` | building the Vulkan layer |
| `nodejs`, `npm` | building and running the Electron UI |
| `libxcb1-dev`, `libx11-dev`, `libwayland-dev` | serializing each windowing system's surface arguments; the layer builds without them but skips the ones that are missing |
| `libxrandr-dev` | reading the monitor's refresh rate on X11, for a driver that reports none itself ([Profiling](PROFILING.md)); without it such a capture falls back to the frame-interval estimate |
| `libvulkan-dev`, `glslc` | the bundled test application |
| `libegl-dev`, `libgles-dev` | optional: the OpenGL ES test application (`test/gles_linux`); the [OpenGL ES capture library](GLES.md) itself builds without them |
| `libsdl2-dev` | optional: a window for that test application, and its EGL-through-`dlopen` path; without it the application is offscreen-only |
| `spirv-tools`, `spirv-cross` | optional: shader text in the Inspect panel |
| `vulkan-tools` | optional: `vulkaninfo`, for checking the driver |

The equivalents elsewhere are `gcc-c++ cmake ninja-build python3 nodejs vulkan-loader-devel
libxcb-devel libX11-devel wayland-devel libXrandr-devel mesa-libEGL-devel mesa-libGLES-devel
SDL2-devel glslc spirv-tools spirv-cross` (Fedora) and `base-devel cmake ninja python nodejs npm
vulkan-headers libxcb libx11 wayland libxrandr mesa sdl2 shaderc spirv-tools spirv-cross` (Arch).

`tools/setup.sh --check` reports what is missing and prints the install command for your package
manager. The [LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home#linux) is *not* required — it
is an alternative source of the same shader tools.

### Windows

| What | Where |
|---|---|
| Visual Studio 2022 or newer, with the **Desktop development with C++** workload | https://visualstudio.microsoft.com/downloads/ |
| CMake 3.20+ (the C++ workload installs one) | https://cmake.org/download/ |
| Python 3.8+ (tick **Add python.exe to PATH**) | https://www.python.org/downloads/ |
| Node.js LTS | https://nodejs.org/en/download |
| Vulkan SDK — `glslc`, `spirv-dis`, `spirv-cross`, `dxc` (the D3D12 test application's shaders, and `dxcompiler.dll` for DXIL reflection), and the loader the Vulkan test app links against | https://vulkan.lunarg.com/sdk/home#windows |
| Windows SDK 10.0.26100 or newer — the D3D12 headers the capture library needs | The Visual Studio installer's *Individual components*, or https://developer.microsoft.com/windows/downloads/windows-sdk/ |
| Git | https://git-scm.com/download/win |

The Windows SDK provides the windowing and Direct3D headers, and the Vulkan driver comes with
your graphics driver. `git submodule update --init` brings `third_party/minhook`, which the D3D12
library hooks the entry points with, beside `Vulkan-Headers`. The D3D12 library's generated enum
and vtable tables (`src/d3d12/gen/`) are committed, so Python regenerates them only when
`tools/gen_d3d12.py` changes.

### macOS

| What | Where |
|---|---|
| Xcode command-line tools | `xcode-select --install` |
| CMake 3.20+ | `brew install cmake` |
| Node.js LTS | `brew install node` |

`src/vulkan/` does not build for Apple targets, so neither `vk.xml`'s Python generator nor `glslc` is
part of the build. `spirv-dis` and `spirv-cross` (`brew install spirv-tools spirv-cross`) are
still worth having, for the shader text of Vulkan captures taken elsewhere.

## Build and run

### Linux

```sh
git clone --recurse-submodules https://github.com/brendan-duncan/gpu_inspector.git
cd gpu_inspector
tools/setup.sh
cd src/app && npm start
```

`tools/setup.sh` checks the prerequisites, initializes the `Vulkan-Headers` submodule, builds the
layer and the test application into `build/bin`, and installs the app's node modules. It changes
nothing outside the checkout unless you pass `--install-deps`. `--check` only reports what is
missing; `--debug` builds the native side as Debug.

`tools/install_desktop_entry.sh` adds GPU Inspector to the desktop's application list and dock;
`--uninstall` removes it.

By hand:

```sh
git submodule update --init
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd src/app && npm install && npm start
```

### Windows

```sh
git submodule update --init
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
cd src/app && npm install && npm start
```

Use the generator name of the Visual Studio you installed. The build puts the Vulkan layer, the
D3D12 capture library with `dxinsp_launch.exe`, `dxinsp_shader.exe` and `dxinsp_replay.exe`, and the test
applications (`vkinsp_triangle.exe`, `dxinsp_triangle.exe`, and the ray tracing
`vkinsp_path_tracer.exe` and `dxinsp_path_tracer.exe`) in `build\bin\Release`.

### macOS

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd src/app && npm install && npm start
```

No submodule: the top-level `CMakeLists.txt` builds `src/metal/` — `build/bin/libmtlinsp_capture.dylib`
— and the `mtlinsp_triangle` and `mtlinsp_path_tracer` test applications instead of the Vulkan layer. `tools/setup.sh` is
Linux-only.

### Android layer

```sh
python tools/build_android.py            # arm64-v8a; add --abi arm64-v8a,x86_64 for an emulator
```

See [Android and Quest](ANDROID.md#build-the-android-layer).

## What the test applications can be asked to do

Each feature of the inspector has a mode of the test application that exercises it, which is how
they are developed and how a regression is reproduced. `--frames N`, `--width` and `--height` apply
to both, and every sample (Metal's and the plugins' too) takes `--capture-at N`, which asks the
inspector for a capture at frame N through `include/gpu_inspector.h`; the rest draw the cube
differently or misbehave on purpose.

`vkinsp_triangle`:

| Mode | What it does |
|---|---|
| `--ray-tracing` | Builds both acceleration structures every frame and traces into a storage image |
| `--shader-record` | Ray tracing with the hit record holding a tint buffer's device address, read through a buffer reference (implies `--ray-tracing`) |
| `--descriptor-buffer` | Binds its set through `VK_EXT_descriptor_buffer` instead of a descriptor set |
| `--device-local-descriptors` | `--descriptor-buffer`, with the descriptor buffer in device-local memory the host never maps, filled by a copy |
| `--compile-hitch` | Builds a pipeline inside every frame, so the CPU timeline has a compile in it |
| `--hitch-every <n>` | Stalls 100 ms inside every nth frame, in its own code, for **Capture on hitch** |
| `--stall <ms>` | Sleeps each frame so vsynced presents miss refreshes, for the dropped-frame count |
| `--oob` | Writes past its storage buffer from the shader: found only by **GPU validation** |
| `--hazard` | Writes the vertex buffer with no barrier, for synchronization validation |
| `--leak` | Never destroys what it creates, for the leak report |
| `--bad-scissor` | A negative scissor offset, a validation error |
| `--occluded` | Draws the cube twice in the same place, so every fragment is overdrawn |
| `--heavy` | A costly fragment shader with known per-function costs, for shader analysis |
| `--churn` | A buffer made and freed every frame and one kept every 30th, for a Memory Capture to find |
| `--capture-at <frame>` | Asks the inspector for a capture at that frame itself (`include/gpu_inspector.h`) |
| `--msaa`, `--stencil` | Multisampled and stencil attachments, for the read-back paths |
| `--push-template` | Pushes its descriptors through an update template |
| `--pipeline-library` | Links the pipeline from graphics pipeline libraries |
| `--shader-object` | Draws with `VK_EXT_shader_object` instead of a pipeline |
| `--suspend` | A dynamic rendering pass suspended and resumed across command buffers |
| `--prerecord` | Records command buffers once and resubmits them, for **Record all command buffers** |
| `--persistent` | Each frame reads what the last left behind, which a replay must restore |
| `--second-device`, `--second-queue` | A second stream of work each frame |
| `--offscreen` | Renders without ever presenting, like an OpenXR application |

`mtlinsp_triangle`:

| Mode | What it does |
|---|---|
| `--present-direct` | Presents through `[drawable present]` from a scheduled handler, the way Unity's macOS player does |
| `--compile-hitch` | Compiles a library and a pipeline inside every frame, so the CPU timeline has a stall to attribute |
| `--hitch-every <n>` | Stalls 100 ms inside every nth frame, in its own code, for **Capture on hitch** |
| `--occluded` | Draws the triangles twice, the second set behind the first with a depth test, so overdraw has fragments to reject |
| `--half-scissor` | A scissor that keeps the left half of the target, for the **Viewport / Scissor** overlay |
| `--layered` | One pass into a two-layer array target, a draw per layer through `[[render_target_array_index]]`, for a pixel history of a layered pass |
| `--indirect` | The triangle drawn through an `MTLIndirectCommandBuffer` of two commands instead of by calls on the encoder |
| `--texture-writes` | Writes the resolve target from a compute kernel and a blit as well as from a pass, for the pixel history's "resolve", "compute" and "copy" events |

`dxinsp_triangle`:

| Mode | What it does |
|---|---|
| `--stall <ms>` | Sleeps each frame so vsynced presents miss refreshes, for the dropped-frame count |
| `--hitch-every <n>` | Stalls 100 ms inside every nth frame, for **Capture on hitch** |
| `--compute`, `--bundle`, `--indirect` | A dispatch, a bundle, and an indirect draw |
| `--render-pass` | Uses `ID3D12GraphicsCommandList4` render passes |
| `--suspend` | A render pass suspended across two command lists, submitted together (implies `--render-pass`) |
| `--pool` | Records each frame into one of a pool of lists reset as soon as they run, so the captured frame's list is adopted |
| `--async-compute` | The compute dispatch on a compute queue of its own, beside the render pass, for the Timeline's lane per queue (implies `--compute`) |
| `--keep-depth` | The depth buffer is cleared only when it is new; each frame loads what earlier frames left |
| `--late-descriptor` | The cubes' volatile SRV slot is rewritten between `Close` and `ExecuteCommandLists` |
| `--bindless` | The cubes' pixel shader reads a texture through `ResourceDescriptorHeap` (shader model 6.6), from a slot no root table covers |
| `--local-root` | Ray tracing with a local root signature on one hit group, whose arguments the binding table record holds (implies `--ray-tracing`) |
| `--debug-layer` | Turns the D3D12 debug layer on from the application |
| `--msaa`, `--stencil`, `--leak`, `--offscreen`, `--heavy`, `--churn`, `--capture-at <frame>` | As above |
| `--evict` | A 32 MB buffer evicted every 120th frame and made resident 60 frames later, for the residency marks |

## Other commands

From `src/app/`:

```sh
npm run typecheck    # tsc
npm run watch        # rebuild the UI on change
npm run icons        # re-render assets/icon.{ico,png} from assets/icon.svg
npm run pack         # unpacked packaged app in src/app/release (needs the Release layer build)
npm run dist         # installer for this platform in src/app/release
npm test             # renderer and MCP server unit tests
```

`python tools/ui_tests.py` runs the UI end to end against the built test application.
`--unity <player.app>` adds two cases against a real Unity player — overdraw measured over its
frame, and a pixel followed through it — which has passes the samples do not. Opt-in: the player
is not part of the repository.

`python tools/doc_screenshots.py` regenerates the screenshots in `docs/images` from the built UI:
each is a run of the app with its testing aids, quitting once the shot is written.
Shots taken from capture files need those files — `--captures <dir>`, or
`GPU_INSPECTOR_DOC_CAPTURES` — and are skipped when they are not there; the rest come from the
built test application. `--list` names them, `--only <name>` takes one.

A macOS build made without an Apple Developer ID in the keychain is ad-hoc signed: it runs on the
machine that built it, but is not something to hand to anyone else. See
[Releasing](RELEASING.md).

---

Previous: [Troubleshooting](TROUBLESHOOTING.md) · [Docs index](README.md) · Next: [Capture replay](REPLAY.md)
