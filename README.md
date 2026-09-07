# GPU Inspector

A cross-platform (Windows, Linux) graphics inspector for native applications, the native
counterpart of [WebGPU Inspector](https://github.com/brendan-duncan/webgpu_inspector) (the web
version). Vulkan is the first supported API: every Vulkan call is intercepted through a layer, so
any application works without instrumentation, and Unity Vulkan players are the primary target.
The UI and protocol are API-neutral so Metal and Direct3D capture libraries can follow.

* **Live object inspection** — every Vulkan object with its creation arguments, dependencies,
  labels, memory bindings and shader code (SPIR-V disassembly, GLSL, HLSL).
* **Frame capture** — the frame's command stream grouped by submit, command buffer, render pass
  and debug label, with reconstructed pipeline state per draw and read-back render targets.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design and the current state of the
project. Third-party code and licenses are listed in
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

## Prerequisites

Both platforms need the same things: a C++20 compiler, CMake 3.20 or newer, Python 3.8 or newer
(the layer's source is generated from `vk.xml`), Node.js 18 or newer with npm (the Electron UI),
the windowing-system headers Vulkan's surface extensions include, and the shader tools `glslc`,
`spirv-dis` and `spirv-cross`. Where they come from differs per platform.

### Linux

Everything is in the distribution's package repositories. On Debian/Ubuntu:

```
sudo apt install build-essential cmake ninja-build git python3 nodejs npm \
                 libvulkan-dev libxcb1-dev libx11-dev libwayland-dev \
                 glslc spirv-tools spirv-cross vulkan-tools
```

| Package | Needed for |
|---|---|
| `build-essential`, `cmake`, `ninja-build`, `git`, `python3` | building the Vulkan layer |
| `nodejs`, `npm` | building and running the Electron UI |
| `libxcb1-dev`, `libx11-dev`, `libwayland-dev` | serializing each windowing system's surface arguments; the layer builds without them but skips the ones that are missing |
| `libvulkan-dev`, `glslc` | the bundled test application (`vkinsp_triangle`) |
| `spirv-tools` (`spirv-dis`), `spirv-cross` | optional: shader text in the Inspect panel |
| `vulkan-tools` (`vulkaninfo`) | optional: checking that the Vulkan driver works |

You also need a Vulkan driver for your GPU: `mesa-vulkan-drivers` for AMD and Intel, NVIDIA's
proprietary driver (`nvidia-driver-<version>`) for NVIDIA. `vulkaninfo --summary` should list your
device. On other distributions the equivalent packages are `gcc-c++ cmake ninja-build python3
nodejs vulkan-loader-devel libxcb-devel libX11-devel wayland-devel glslc spirv-tools spirv-cross`
(Fedora) or `base-devel cmake ninja python nodejs npm vulkan-headers libxcb libx11 wayland shaderc
spirv-tools spirv-cross` (Arch).

`tools/setup.sh --check` reports which of these are missing and prints the install command for
your package manager, so you do not have to work the list out by hand.

The [LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home#linux) is *not* required — it is an
alternative source for the same shader tools if your distribution's are too old.

### Windows

| What | Where |
|---|---|
| Visual Studio 2022 or newer, with the **Desktop development with C++** workload | https://visualstudio.microsoft.com/downloads/ |
| CMake 3.20+ — the C++ workload above installs one; standalone: | https://cmake.org/download/ |
| Python 3.8+ (tick **Add python.exe to PATH**) | https://www.python.org/downloads/ |
| Node.js LTS (includes npm) | https://nodejs.org/en/download |
| Vulkan SDK — supplies `glslc`, `spirv-dis`, `spirv-cross` and the loader the test app links against | https://vulkan.lunarg.com/sdk/home#windows |
| Git | https://git-scm.com/download/win |

The Windows SDK that comes with Visual Studio provides the windowing headers, and your GPU's
Vulkan driver comes with its normal graphics driver, so nothing extra is needed for either.

## Build and run

### Linux

```
git clone --recurse-submodules https://github.com/brendan-duncan/gpu_inspector.git
cd gpu_inspector
tools/setup.sh
cd app && npm start
```

`tools/setup.sh` checks the prerequisites, initializes the `Vulkan-Headers` submodule, builds the
layer and the test application into `build/bin`, and installs the app's node modules. It changes
nothing outside the checkout unless you pass `--install-deps`, which installs the missing system
packages first (using `sudo`). `--check` only reports what is missing, `--debug` builds the native
side as Debug.

To do the same by hand:

```
git submodule update --init
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd app && npm install && npm start
```

### Windows

```
git submodule update --init
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
cd app && npm install && npm start
```

Use the generator name of the Visual Studio you installed (`"Visual Studio 18 2026"` for VS 2026).

### Using it

Point the launcher at a Vulkan executable — for example the bundled test application,
`build/bin/vkinsp_triangle` (`build\bin\Release\vkinsp_triangle.exe` on Windows) — and press
**Launch**, then **Capture** in the Capture tab. The inspector sets the layer environment
variables for the process it launches, so nothing is registered system-wide.

Other useful commands, from `app/`:

```
npm run typecheck    # tsc
npm run watch        # rebuild the UI on change
npm run icons        # re-render assets/icon.{ico,png} from assets/icon.svg
```

## Troubleshooting

**"layer not found" when launching.** The app looks for `VK_LAYER_INSPECTOR_capture.json` in
`build/bin`, `build/bin/{Release,RelWithDebInfo,Debug}` and next to a packaged app. If your build
directory is somewhere else, point `INSPECTOR_LAYER_DIR` at the directory holding the manifest and
the layer library.

**The target starts but never connects.** The layer only loads if the Vulkan loader can find it;
run the target with `VK_LOADER_DEBUG=layer` to see the loader's search, and turn on **Log** in the
launch dialog to see the layer's own output in the session's Log tab.

**`TypeError: Cannot read properties of undefined (reading 'handle')` at startup.** Electron
started as plain Node because the terminal exported `ELECTRON_RUN_AS_NODE=1` — VS Code's
integrated terminal does. `npm start` clears it; running `npx electron .` directly does not, so
unset the variable in that case.

**Electron fails to start on Linux with a sandbox or user-namespace error.** Recent distributions
(Ubuntu 23.10+) restrict unprivileged user namespaces, which Chromium's sandbox needs. Either
allow them for this binary, or start the app with `npx electron . --no-sandbox`.

**`vulkaninfo` reports no devices.** The Vulkan driver for your GPU is missing; see the driver
packages under Prerequisites. Nothing in the inspector will work until a driver is present.

**Shader text shows only SPIR-V bytes.** `spirv-dis` and `spirv-cross` were not found. Install
them, or point `INSPECTOR_TOOLS_DIR` at a directory containing them (`VULKAN_SDK` is searched too).
