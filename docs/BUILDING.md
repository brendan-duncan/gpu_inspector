# Building from source

[Docs index](README.md) › Building from source

Only needed if you want to build GPU Inspector yourself, or to build the
[Android layer](ANDROID.md), which the installers do not include. To use the released builds, see
[Install](INSTALL.md).

## Prerequisites

Linux and Windows need the same things: a C++20 compiler, CMake 3.20 or newer, Python 3.8 or newer
(the layer's source is generated from `vk.xml`), Node.js 18 or newer with npm, the windowing-system
headers Vulkan's surface extensions include, and the shader tools `glslc`, `spirv-dis` and
`spirv-cross`. macOS builds the Metal capture library instead of the Vulkan layer, and needs less.

### Linux

```sh
sudo apt install build-essential cmake ninja-build git python3 nodejs npm \
                 libvulkan-dev libxcb1-dev libx11-dev libwayland-dev \
                 glslc spirv-tools spirv-cross vulkan-tools
```

| Package | Needed for |
|---|---|
| `build-essential`, `cmake`, `ninja-build`, `git`, `python3` | building the Vulkan layer |
| `nodejs`, `npm` | building and running the Electron UI |
| `libxcb1-dev`, `libx11-dev`, `libwayland-dev` | serializing each windowing system's surface arguments; the layer builds without them but skips the ones that are missing |
| `libvulkan-dev`, `glslc` | the bundled test application |
| `spirv-tools`, `spirv-cross` | optional: shader text in the Inspect panel |
| `vulkan-tools` | optional: `vulkaninfo`, for checking the driver |

The equivalents elsewhere are `gcc-c++ cmake ninja-build python3 nodejs vulkan-loader-devel
libxcb-devel libX11-devel wayland-devel glslc spirv-tools spirv-cross` (Fedora) and `base-devel
cmake ninja python nodejs npm vulkan-headers libxcb libx11 wayland shaderc spirv-tools
spirv-cross` (Arch).

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
| Vulkan SDK — `glslc`, `spirv-dis`, `spirv-cross`, and the loader the test app links against | https://vulkan.lunarg.com/sdk/home#windows |
| Git | https://git-scm.com/download/win |

The Windows SDK from Visual Studio provides the windowing headers, and the Vulkan driver comes
with your graphics driver.

### macOS

| What | Where |
|---|---|
| Xcode command-line tools | `xcode-select --install` |
| CMake 3.20+ | `brew install cmake` |
| Node.js LTS | `brew install node` |

`layer/` does not build for Apple targets, so neither `vk.xml`'s Python generator nor `glslc` is
part of the build. `spirv-dis` and `spirv-cross` (`brew install spirv-tools spirv-cross`) are
still worth having, for the shader text of Vulkan captures taken elsewhere.

## Build and run

### Linux

```sh
git clone --recurse-submodules https://github.com/brendan-duncan/gpu_inspector.git
cd gpu_inspector
tools/setup.sh
cd app && npm start
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
cd app && npm install && npm start
```

### Windows

```sh
git submodule update --init
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
cd app && npm install && npm start
```

Use the generator name of the Visual Studio you installed.

### macOS

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd app && npm install && npm start
```

No submodule: the top-level `CMakeLists.txt` builds `metal/` — `build/bin/libmtlinsp_capture.dylib`
— and the `mtlinsp_triangle` test application instead of the Vulkan layer. `tools/setup.sh` is
Linux-only.

### Android layer

```sh
python tools/build_android.py            # arm64-v8a; add --abi arm64-v8a,x86_64 for an emulator
```

See [Android and Quest](ANDROID.md#build-the-android-layer).

## Other commands

From `app/`:

```sh
npm run typecheck    # tsc
npm run watch        # rebuild the UI on change
npm run icons        # re-render assets/icon.{ico,png} from assets/icon.svg
npm run pack         # unpacked packaged app in app/release (needs the Release layer build)
npm run dist         # installer for this platform in app/release
npm test             # renderer and MCP server unit tests
```

`python tools/ui_tests.py` runs the UI end to end against the built test application.

`python tools/doc_screenshots.py` regenerates the screenshots in `docs/images` from the built UI:
each one is a run of the app with its testing aids, quitting itself once the shot is written.
Shots taken from capture files need those files — `--captures <dir>`, or
`GPU_INSPECTOR_DOC_CAPTURES` — and are skipped when they are not there; the rest come from the
built test application. `--list` names them, `--only <name>` takes one.

A macOS build made without an Apple Developer ID in the keychain is ad-hoc signed: it runs on the
machine that built it, but is not something to hand to anyone else. See
[Releasing](RELEASING.md).

---

Previous: [Troubleshooting](TROUBLESHOOTING.md) · [Docs index](README.md)
