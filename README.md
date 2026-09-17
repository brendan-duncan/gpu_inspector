<p align="center"><img src="docs/images/title.png" alt="GPU Inspector" width="800"></p>

**GPU Inspector** captures and inspects frames from native graphics applications. It is the
native counterpart of [WebGPU Inspector](https://github.com/brendan-duncan/webgpu_inspector).

Supports **Vulkan**, **Direct3D 12**, **Metal**, **Android** and **Quest**, on **Windows**, **macOS** and **Linux**.

* **Object inspection**: every GPU object, with how it was created and what it uses.
* **Frame capture**: a frame's commands, with each draw's state, buffers, textures and render targets.
* **Saved captures**: reopen them later, anywhere, without the application.
* **Validation**: validation layer errors, linked to the objects and commands they name.
* **Shader editing**: change a shader and see the running application use it.
* **Shader debugger**: step through a vertex, pixel or compute shader line by line.
* **Profiling**: GPU time for each pass, and what bounds the frame.
* **GPU bottlenecks**: overdraw, triangle size and wasted work, with likely causes.
* **Pixel history**: every draw that touched a pixel, and what happened to it.
* **Render graph**: the frame's passes and the resources between them.
* **Claude Code**: a plugin that lets Claude analyze captures and drive applications.

## Documentation

The user documentation is in [docs/](docs/README.md):

| | |
|---|---|
| [Install](docs/INSTALL.md) · [Getting started](docs/GETTING_STARTED.md) | the first session, start to a saved capture |
| [Vulkan](docs/VULKAN.md) · [Direct3D 12](docs/D3D12.md) · [Metal](docs/METAL.md) · [Android and Quest](docs/ANDROID.md) | the workflow for each platform |
| [Inspect](docs/INSPECT.md) · [Capture](docs/CAPTURE.md) · [Reports](docs/REPORTS.md) | using the inspector |
| [Finding GPU bottlenecks](docs/PROFILING.md) | working out what limits a frame |
| [Claude Code plugin](docs/MCP.md) | asking Claude about a capture |
| [Troubleshooting](docs/TROUBLESHOOTING.md) · [Building from source](docs/BUILDING.md) | when something does not work, and building it yourself |

Third-party code and licenses are listed in
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

## Install

Installers for each release are on the [releases page](https://github.com/brendan-duncan/gpu_inspector/releases):
`GPU-Inspector-Setup-<version>.exe` for Windows, `gpu-inspector_<version>_amd64.deb` for
Debian and Ubuntu (`sudo apt install ./gpu-inspector_<version>_amd64.deb`), and
`GPU-Inspector-<version>-arm64.dmg` or `-x64.dmg` for macOS (see [macOS](#macos) for what that
build does and does not do). Installed builds check
for updates at startup and offer to download them; the version label at the right of the launch
bar checks on demand. The installers contain the layer, so nothing below is needed unless you
want to build from source. What changed in each release is in [CHANGELOG.md](CHANGELOG.md), and
how releases are made in [docs/RELEASING.md](docs/RELEASING.md).

## Prerequisites

Linux and Windows need the same things: a C++20 compiler, CMake 3.20 or newer, Python 3.8 or newer
(the layer's source is generated from `vk.xml`), Node.js 18 or newer with npm (the Electron UI),
the windowing-system headers Vulkan's surface extensions include, and the shader tools `glslc`,
`spirv-dis` and `spirv-cross`. Where they come from differs per platform. Windows also builds the
Direct3D 12 capture library (`src/d3d12/`), which needs a recent Windows SDK and the MinHook
submodule, and its test application, which needs `dxc`. macOS builds the Metal capture library
instead of the Vulkan layer and needs a shorter list.

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
| Vulkan SDK — supplies `glslc`, `spirv-dis`, `spirv-cross`, `dxc` (the D3D12 test application's shaders, and `dxcompiler.dll` for DXIL reflection) and the loader the Vulkan test app links against | https://vulkan.lunarg.com/sdk/home#windows |
| Windows SDK 10.0.26100 or newer — the Direct3D 12 headers the capture library is built against | The Visual Studio installer's *Individual components*, or https://developer.microsoft.com/windows/downloads/windows-sdk/ |
| Git | https://git-scm.com/download/win |

The Windows SDK provides the windowing and Direct3D headers, and your GPU's Vulkan and D3D12
drivers come with its normal graphics driver, so nothing extra is needed for either.
`git submodule update --init` brings `third_party/minhook` (the D3D12 library's entry-point
hooks) beside `Vulkan-Headers`. The D3D12 library's generated enum and vtable tables
(`src/d3d12/gen/`) are committed, so Python regenerates them only when `tools/gen_d3d12.py` changes.

### macOS

| What | Where |
|---|---|
| Xcode command-line tools — the Objective-C++ compiler and the Metal framework headers | `xcode-select --install` |
| CMake 3.20+ | `brew install cmake`, or https://cmake.org/download/ |
| Node.js LTS (includes npm) | `brew install node`, or https://nodejs.org/en/download |

Nothing else: `src/vulkan/` does not build for Apple targets, so neither `vk.xml`'s Python generator
nor `glslc` is part of the build. `spirv-dis` and `spirv-cross` (`brew install spirv-tools
spirv-cross`) are still worth having, for the shader text of Vulkan captures taken on another
machine or on an Android device.

## Build and run

### Linux

```
git clone --recurse-submodules https://github.com/brendan-duncan/gpu_inspector.git
cd gpu_inspector
tools/setup.sh
cd src/app && npm start
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
cd src/app && npm install && npm start
```

### Windows

```
git submodule update --init
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
cd src/app && npm install && npm start
```

Use the generator name of the Visual Studio you installed (`"Visual Studio 18 2026"` for VS 2026).
`build\bin\Release` then holds the Vulkan layer, the D3D12 capture library with its launcher
(`dxinsp_launch.exe`) and shader tool (`dxinsp_shader.exe`), and both test applications.

### macOS

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd src/app && npm install && npm start
```

No submodule, and a different CMake build: `src/vulkan/` has no Apple target, so the top-level
`CMakeLists.txt` builds `src/metal/` — the Metal capture library, `build/bin/libmtlinsp_capture.dylib`
— and the `mtlinsp_triangle` test application in its place. `tools/setup.sh` is Linux-only. See
[macOS](#macos) below for what the Metal side can and cannot do.

### Using it

Point the launcher at a Vulkan executable — for example the bundled test application,
`build/bin/vkinsp_triangle` (`build\bin\Release\vkinsp_triangle.exe` on Windows) — and press
**Launch**, then **Capture** in the Capture tab. On Windows the target can also be a Direct3D 12
application (`build\bin\Release\dxinsp_triangle.exe`); there is nothing to choose, since every
local target is started with both the layer and the D3D12 library and whichever API it uses
connects (see [Direct3D 12](#direct3d-12)). On macOS the target is a Metal application
instead — `build/bin/mtlinsp_triangle`, or an `.app` bundle — and the rest is the same. The
inspector sets the capture environment variables for the process it launches, so nothing is
registered system-wide. The save button of
the capture bar writes the capture to a `.gpucap` file; **Open Capture...** (or dropping the file
on the window) reopens it later, on any machine, without the application.

Other useful commands, from `src/app/`:

```
npm run typecheck    # tsc
npm run watch        # rebuild the UI on change
npm run icons        # re-render assets/icon.{ico,png} from assets/icon.svg
npm run pack         # unpacked packaged app in src/app/release (needs the Release layer build)
npm run dist         # installer for this platform in src/app/release (see docs/RELEASING.md)
```

## Claude Code

The `gpu-inspector` plugin ([claude-plugin/](claude-plugin/README.md)) is an MCP server over saved
`.gpucap` files. It uses the same frame rules, bottleneck measurements, render graph, draw state
reconstruction, SPIR-V reflection and texture decoding as the app. Install it with:

```
claude plugin marketplace add brendan-duncan/gpu_inspector
claude plugin install gpu-inspector@gpu-inspector-plugins
```

Save a capture, then ask Claude about it, or use one of the plugin's commands:
- `/gpu-inspector:analyze`: a correctness and performance review.
- `/gpu-inspector:profile`: what limits the frame, following [docs/PROFILING.md](docs/PROFILING.md).
- `/gpu-inspector:debug <symptom>`: traces a rendering problem to the draw that causes it.
- `/gpu-inspector:compare <before> <after>`: whether a change moved the numbers.

Without a path, Claude picks from the captures GPU Inspector saved or opened recently. The plugin
needs Node.js 18 or newer, and neither the app nor the application being inspected has to be
running.

The plugin can also drive an application itself, with the capture library from this checkout's
build or from an installed GPU Inspector:
- launch it, and watch its frame rate
- capture frames
- replace a pipeline's shader while it runs, then capture again to measure the change

`/gpu-inspector:live <exe>` walks through it.

## Shader sources

Shaders compiled with source-level debug information (`-g` for glslc and glslangValidator,
`-fspv-debug=vulkan-with-source` for dxc) carry their text, and the Source view, the cost per
line and the findings' line links use it. A shader that carries line information only (dxc
`-Zi`, a build that strips the text) names its file instead: **Source roots** in the launch
dialog (directories, separated by `;`) tells the inspector where those files are on this
machine, and it reads them from there.

## Applications started elsewhere

An application the inspector cannot launch itself (an editor, a game behind its launcher) can
still be inspected: pick **An application started elsewhere (implicit layer)** under *Run On*
in the launch dialog and press **Register**. The layer is then registered for your user account
as an implicit layer (nothing is loaded anywhere until asked), and any application you start
with `VKINSP_ENABLE=1` and `VKINSP_PORT=<port>` in its environment loads it. Press **Wait** and
start the application; the session connects when it does (`VKINSP_LOG_FILE=<path>` writes the
layer's log to a file, since the inspector cannot read the output of a process it did not
start). For an editor started from a launcher, set the variables for your account (`setx` on
Windows) and restart the launcher. **Unregister** removes the registration. From the command
line: `npm start -- --wait-for-app --port=<port>`, and `--implicit-layer=on|off` switches the
registration.

Direct3D 12 has no implicit layer to register — its capture library has to be inside the process
before it creates a device — so the inspector waits for the process instead: pick **An application
started elsewhere (Direct3D 12)**, type the executable's name (`TestVulkan.exe`), press **Wait**
and *then* start the application. The library goes in as the process starts, ahead of its first
D3D12 call. An application that is already running cannot be caught. See
[Direct3D 12](docs/D3D12.md#waiting-for-an-application-to-start).

## Direct3D 12

On Windows the inspector captures Direct3D 12 applications too. D3D12 has no loader layers, so
`src/d3d12/` is a library that `dxinsp_launch.exe` injects into the target before its first
instruction runs; it hooks `D3D12CreateDevice` and `CreateDXGIFactory*` and patches the vtables
of the objects they hand out, and speaks the Vulkan layer's protocol byte for byte. The launch
dialog needs no API field: a Windows target is started with the layer and the library both, and
whichever the application uses connects.

What works: object inspection with descriptors, names and descriptor heap contents; frame
capture with passes synthesized from `OMSetRenderTargets` (or the application's own
`BeginRenderPass`), render targets, bound buffers and textures, root constants, pass timings and
counters; the D3D12 debug layer's messages; stack traces; DXBC/DXIL disassembly, embedded HLSL
and reflection; shader editing through `dxc`; and the shader debugger, which steps a stage as its
HLSL compiled to SPIR-V by `dxc` (there is no DXIL interpreter, so it needs the source: `-Zi`, or
a PDB under the symbol directories). Not yet: the replay-based analyses (draw overlays, mesh output,
per-draw measurements), 32-bit targets, and attaching to an application already running.
[docs/D3D12.md](docs/D3D12.md) is the user's page and `src/d3d12/README.md` the design.

`build\bin\Release\dxinsp_triangle.exe` is the D3D12 test application, the counterpart of
`vkinsp_triangle` (`--msaa`, `--bundle`, `--indirect`, `--render-pass`, `--compute`, `--leak`).

`test/path_tracer/` holds a path tracer for each API — `vkinsp_path_tracer`,
`dxinsp_path_tracer` and `mtlinsp_path_tracer` — that renders the final scene of
[Ray Tracing in One Weekend](https://raytracing.github.io/books/RayTracingInOneWeekend.html)
from one shared scene, to exercise ray tracing capture: bounding-box acceleration structures,
intersection shaders, per-material hit groups and a running mean carried from frame to frame
(`--spp N`, `--depth N`, `--no-accumulate`, `--rebuild`).

## macOS

Applications on macOS render with Metal, and the Vulkan layer would see only the few that run on
MoltenVK — so a port of it was never the answer, and `src/vulkan/` does not build for Apple targets at
all. `src/metal/` is a Metal capture library that takes its place, loaded into the target with
`DYLD_INSERT_LIBRARIES`. It is newer than the Vulkan layer and does less, but the Inspect and
Capture panels both work against a Metal application today, over the same protocol and with no
separate UI of their own.

What works:

* **Object inspection** — the device, command queues, buffers, textures, libraries and render and
  compute pipeline states, each with the call that created it, its arguments and its label.
  Clicking a texture reads its pixels back live. A library lists its function names, and shows
  the Metal Shading Language it was compiled from when it was compiled here rather than loaded as
  a precompiled `metallib`.
* **Frame capture** — the frame's commands grouped by command buffer and pass, each draw with the
  pipeline bound at it, its decoded vertex and index buffers, the textures it sampled and the
  pass's read-back colour attachments. Captures save to the same `.gpucap` files and reopen on any
  platform.
* **Shader debugging** — a draw's vertex or fragment shader, or a dispatch's compute shader,
  stepped line by line through its Metal Shading Language, on the buffers, textures and samplers
  the encoder had bound. There is no replay on this path: a fragment's inputs come from running
  the draw's own vertex shader in the same interpreter and rasterizing the result, so it needs
  nothing built. A library the application loaded as a precompiled `metallib` carries no source,
  and the debugger says so rather than guessing.
* **Android and saved captures** — unchanged from the Windows and Linux builds: Vulkan
  applications on Android devices over adb (see [Android](#android)), and `.gpucap` files taken
  anywhere.

What is not there yet: only the pixel formats `src/metal/src/formats.h` maps are read back (no ASTC,
ETC or PVRTC); stencil attachments are not; and shader editing does not apply — it is built around
SPIR-V and its compilers, and Metal's shaders are already source. Only Apple Silicon has been
verified. `src/metal/README.md` is the detailed account, including what the
interception itself cost to get right, and it keeps the current list.

### Injecting into an application

Metal has no loader and no layer mechanism — no manifests, no dispatch chaining, no supported
extension point. The library is inserted by dyld and interposes the C functions that hand out a
device, hooking the Objective-C classes it reaches from there. Choose **This computer** under
*Run On* in the launch dialog and pick the application's `.app` bundle: the inspector finds the
executable inside it (`CFBundleExecutable` — a directory cannot be spawned) and sets
`DYLD_INSERT_LIBRARIES` for that process alone, so nothing is registered system-wide.

**Code signing decides whether this is possible.** dyld silently drops `DYLD_*` for a process
with the hardened runtime unless it carries
`com.apple.security.cs.allow-dyld-environment-variables` and
`com.apple.security.cs.disable-library-validation`, which no notarized application does. The
inspector checks the signature with `codesign` *before* launching and reports it, rather than
leaving a session waiting for a connection that can never arrive; the message includes the
`codesign --force --sign - --entitlements ...` command that adds the two keys. A locally built
player — a Unity development build, the primary target here — is normally ad-hoc signed without
the hardened runtime and needs none of that.

Re-signing is deliberately not done for you: it rewrites the application bundle and invalidates
its signature and notarization, so it is something to do to a development build, not to a shipped
copy. This is the same shape as Android's requirement that the application be debuggable, and it
is the one place where "works with any uninstrumented application" does not carry over from
Windows and Linux.

An application the inspector cannot launch itself can still be started by hand with
`DYLD_INSERT_LIBRARIES=<path>/libmtlinsp_capture.dylib` and `MTLINSP_PORT=<port>` in its
environment (`MTLINSP_LOG=1` logs the intercepted calls to the session's Log tab), and attached
to with **Connect** or `npm start -- --connect=<port>`.

### Distribution

Releases are signed with the project's Apple Developer ID and notarized by Apple, so the `.dmg`
opens and the app runs without a Gatekeeper warning or any `xattr` incantation. Both
architectures are published: `-arm64` for Apple Silicon and `-x64` for Intel. The capture library
is packaged with the app, so an installed build needs no CMake step of its own.

A build you make yourself is a different matter: without a Developer ID certificate in the
keychain `npm run dist:mac` produces an ad-hoc signed app, which runs on the machine that built
it but is not something to hand to anyone else. It also needs the library built first —
`npm run pack` and `npm run dist` stage `build/bin/libmtlinsp_capture.dylib` into the app, or
`INSPECTOR_METAL_LIB` names it elsewhere.

Shader text for Vulkan captures — Android sessions, and `.gpucap` files from other machines —
still needs `spirv-dis` and `spirv-cross` (`brew install spirv-tools spirv-cross`, or the macOS
[Vulkan SDK](https://vulkan.lunarg.com/sdk/home#mac)). Nothing on the Metal path uses them.

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

`python tools/build_android_triangle.py` builds `build/android/android_triangle.apk`, a debuggable
test application for phones (a NativeActivity rendering a ring of triangles into a swapchain),
to launch the same way. For a headset, `python tools/build_xr_triangle.py` builds two debuggable OpenXR test
applications to launch the same way: `build/android/xr_triangle.apk` renders a ring of
triangles in one multiview pass, and `xr_triangle_slow.apk` ("XR Triangle (Slow)") renders the
same scene with deliberate inefficiencies, so the capture's **Frame Stats** (Frame Issues) and
**Analyze Shaders** reports have something to flag. The build needs the Vulkan SDK's `glslc`
and downloads the Khronos OpenXR loader on the first run. The headset must be worn (or its
proximity sensor covered) for the OpenXR session to start. "Symbol directories" in the launch
dialog names where the application's unstripped libraries are (its build tree, such as
`build/android/xr_triangle`): stack traces then show functions, files and lines, resolved with
the NDK's `llvm-symbolizer`.

Shader editing works as on the desktop: the shaders are compiled on this machine with the Vulkan
SDK's compilers and sent to the device. Expect the captured frame to take noticeably longer on a
tiled mobile GPU, since every render target is read back at the end of its pass.

## Testing

`python tools/ui_tests.py` runs the UI end to end against the built triangle application: a
plain capture, MSAA, frames without a present, a validation error and a synchronization hazard
linked to their commands, and stack traces with source lines. Each case starts the app through
the inspector, captures a frame, and checks what the renderer reports (`--debug-dump`) and the
layer's log. `--captures <dir>` also opens every `.gpucap` in a directory, checking a
`<name>.expect.json` next to it (`{"findings": {"rule": count}}`) when there is one. The
renderer's and the MCP server's unit tests run with `npm test` in `src/app/`. The triangle cases drive the Vulkan layer,
so on macOS only `--captures <dir>` applies; the Metal library has no automated test yet, and
`build/bin/mtlinsp_triangle` is run by hand (see `src/metal/README.md`).

## Troubleshooting

**"layer not found" when launching.** The app looks for `VK_LAYER_INSPECTOR_capture.json` in
`build/bin`, `build/bin/{Release,RelWithDebInfo,Debug}` and next to a packaged app. If your build
directory is somewhere else, point `INSPECTOR_LAYER_DIR` at the directory holding the manifest and
the layer library. On macOS it is the Metal capture library that is looked for in those same
places, and `INSPECTOR_METAL_LIB` points at the `.dylib` itself.

**macOS: the target starts but never connects.** Almost always the hardened runtime: dyld
dropped `DYLD_INSERT_LIBRARIES`, so the capture library was never loaded. The launch dialog
checks for this and refuses, so a target that got past it and still went quiet is worth
confirming with `codesign -d -v --entitlements - <the .app>`. Launch with **Log** on to see the
library's own output in the session's Log tab.

**The target starts but never connects.** The layer only loads if the Vulkan loader can find it;
run the target with `VK_LOADER_DEBUG=layer` to see the loader's search, and turn on **Log** in the
launch dialog to see the layer's own output in the session's Log tab.

**Direct3D 12: the target starts but never connects.** The Log tab has the launcher's `dxinsp:`
line when it could not inject the library (a 32-bit executable, a protected process, the DLL not
found), and the target then runs without it. `INSPECTOR_D3D12_DIR` points at a build of
`dxinsp_capture.dll` and `dxinsp_launch.exe` that is somewhere other than `build/bin`. See
[docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md#direct3d-12).

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
