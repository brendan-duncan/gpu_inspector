<p align="center"><img src="docs/images/title.png" alt="GPU Inspector" width="800"></p>

**GPU Inspector** is a cross-platform (Windows, Linux) graphics inspector for native applications, the native counterpart of [WebGPU Inspector](https://github.com/brendan-duncan/webgpu_inspector) (the web
version). Vulkan is the first supported API: every Vulkan call is intercepted through a layer, so
any application works without instrumentation, and Unity Vulkan players are the primary target.
The UI and protocol are API-neutral so Metal and Direct3D capture libraries can follow.

* **Live object inspection** — every Vulkan object with its creation arguments, dependencies,
  labels, memory bindings and shader code (SPIR-V disassembly, GLSL, HLSL).
* **Validation messages** — enable the Khronos validation layer from the launch dialog and the
  errors and warnings it reports are listed in the Inspect tab, linked to the objects they name.
* **Shader editing** — edit a pipeline's shader as GLSL, HLSL or SPIR-V assembly, compile it with
  the Vulkan SDK's compilers and see the running application use it; restore the original at any
  time. Shaders compiled with debug information (`-g`, `-fspv-debug=vulkan-with-source`) show
  their embedded source, linked line by line to the SPIR-V, and are edited as that source.
* **Profiling** — GPU timestamps around every render pass and every run of compute dispatches
  of a capture: pass durations, a pass timeline, and a Frame Bound card comparing GPU and CPU
  submit time with the frame interval.
* **Frame capture** — the frame's command stream grouped by submit, command buffer, render pass
  and debug label. Each draw shows its pipeline state and shaders, every bound descriptor set
  with the parsed contents of its uniform and storage buffers and the images it sampled, the
  decoded vertex and index buffers, push constants and the pass's read-back render targets. Captures save to `.gpucap`
  files that reopen anywhere without the application, for bug reports and comparisons.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design and the current state of the
project, and [TODO.md](TODO.md) for what is planned. Third-party code and licenses are listed in
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

## Install

Installers for each release are on the [releases page](https://github.com/brendan-duncan/gpu_inspector/releases):
`GPU-Inspector-Setup-<version>.exe` for Windows and `gpu-inspector_<version>_amd64.deb` for
Debian and Ubuntu (`sudo apt install ./gpu-inspector_<version>_amd64.deb`). Installed builds check
for updates at startup and offer to download them; the version label at the right of the launch
bar checks on demand. The installers contain the layer, so nothing below is needed unless you
want to build from source. What changed in each release is in [CHANGELOG.md](CHANGELOG.md), and
how releases are made in [docs/RELEASING.md](docs/RELEASING.md).

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

Optionally, `tools/install_desktop_entry.sh` adds GPU Inspector to the desktop's application
list and dock (with its own icon rather than a placeholder), launching this checkout through
`tools/gpu-inspector`; `--uninstall` removes it.

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
variables for the process it launches, so nothing is registered system-wide. The save button of
the capture bar writes the capture to a `.gpucap` file; **Open Capture...** (or dropping the file
on the window) reopens it later, on any machine, without the application.

Other useful commands, from `app/`:

```
npm run typecheck    # tsc
npm run watch        # rebuild the UI on change
npm run icons        # re-render assets/icon.{ico,png} from assets/icon.svg
npm run pack         # unpacked packaged app in app/release (needs the Release layer build)
npm run dist         # installer for this platform in app/release (see docs/RELEASING.md)
```

## Android

Vulkan applications on Android devices (a Unity player built as a **Development Build**, for
example) can be inspected through adb. The same layer runs on the device; the inspector installs
it, starts the application with it enabled, and talks to it over an `adb forward` port.

What is needed, beyond the desktop prerequisites:

| What | Where |
|---|---|
| Android SDK with `platform-tools` (adb), `build-tools` and a `platforms/android-*` | Android Studio's SDK Manager, or `sdkmanager` from the command-line tools |
| Android NDK (r26 or newer) | SDK Manager, or `sdkmanager "ndk;26.3.11579264"` |
| A Java runtime, for signing the layer APK | `JAVA_HOME`, `java` on `PATH`, or the JDK bundled with Android Studio |
| A device running Android 9 or newer with USB debugging on, and a **debuggable** build of the application | Android only loads layers into debuggable applications (or on rooted devices) |

Build the Android layer once (it finds the SDK and NDK in `ANDROID_HOME`, `ANDROID_NDK_HOME` or
the default install location):

```
python tools/build_android.py            # arm64-v8a; add --abi arm64-v8a,x86_64 for an emulator
```

This produces `build/android/lib/<abi>/libVkLayer_inspector_capture.so` and
`build/android/gpu_inspector_layer.apk`, a layer package with no code of its own. Then choose
**Android device (adb)** under *Run On* in the launch dialog, pick the device and the package,
and press **Launch**. The inspector installs the layer APK when the device does not have this
version yet (Android 10+; on Android 9 it copies the library into the application's data
directory instead), enables Android's GPU debug layer settings for the package, starts it, and
connects. The Log tab shows the layer's logcat output. Closing the session turns the debug layer
settings off again. From the command line: `npm start -- --launch-android=<package>
--device=<serial>`.

Shader editing works as on the desktop: the shaders are compiled on this machine with the Vulkan
SDK's compilers and sent to the device. Expect the captured frame to take noticeably longer on a
tiled mobile GPU, since every render target is read back at the end of its pass.

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

**The dock or task bar shows a generic icon (a gear on GNOME).** GNOME identifies a window by
its `WM_CLASS` (`gpu-inspector`) and takes the icon from the matching `.desktop` file, ignoring
the icon the window itself advertises. Run `tools/install_desktop_entry.sh` to install one.

**Electron fails to start on Linux with a sandbox or user-namespace error.** Recent distributions
(Ubuntu 23.10+) restrict unprivileged user namespaces, which Chromium's sandbox needs. Either
allow them for this binary, or start the app with `npx electron . --no-sandbox`.

**Android: the application starts but never connects.** Check the Log tab: `run-as` failing with
"not debuggable" means the build is not debuggable; nothing at all from `[vkinsp]` means the
loader did not pick the layer up (`adb logcat -s vulkan` shows its search). `adb shell settings
list global | grep gpu_debug` shows the settings the inspector wrote.

**Android: "adb not found".** Set `ANDROID_HOME` to the SDK, put `platform-tools` on `PATH`, or
point `INSPECTOR_ADB` at the adb executable. `INSPECTOR_ANDROID_LAYER_DIR` likewise overrides
where the Android layer is looked for (the directory holding `lib/<abi>/` and the APK).

**`vulkaninfo` reports no devices.** The Vulkan driver for your GPU is missing; see the driver
packages under Prerequisites. Nothing in the inspector will work until a driver is present.

**Shader text shows only SPIR-V bytes.** `spirv-dis` and `spirv-cross` were not found. Install
them, or point `INSPECTOR_TOOLS_DIR` at a directory containing them (`VULKAN_SDK` is searched too).
