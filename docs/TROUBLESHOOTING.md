# Troubleshooting

[Docs index](README.md) › Troubleshooting

## Vulkan

**The target starts but never connects.** The layer only loads if the Vulkan loader can find it.
Run the target with `VK_LOADER_DEBUG=layer` to see the loader's search, and tick **Layer log** in
the launch dialog to see the layer's own output in the session's **Log** tab.

**"layer not found" when launching.** The inspector looks for `VK_LAYER_INSPECTOR_capture.json` in
`build/bin`, `build/bin/{Release,RelWithDebInfo,Debug}` and next to a packaged app. If your build
directory is somewhere else, point `INSPECTOR_LAYER_DIR` at the directory holding the manifest and
the layer library.

**`vulkaninfo` reports no devices.** The Vulkan driver for your GPU is missing. Install it
(`mesa-vulkan-drivers` for AMD and Intel on Linux, NVIDIA's proprietary driver for NVIDIA; the
normal graphics driver on Windows). Nothing in the inspector works until a driver is present.

## Direct3D 12

**The target starts but never connects.** Look at the **Log** tab first. The launcher writes a
line starting with `dxinsp:` when it could not inject the library — a 32-bit executable, a
protected process, or `dxinsp_capture.dll` not found — and the target then runs without it. If
the Log tab says the library was injected and nothing connects, the application is probably not
a D3D12 one: a Vulkan application connects through the layer, a D3D11 or OpenGL one through
nothing. Tick **Layer log** to see the library's own output.

**"D3D12 capture library not found" in the Log tab.** `dxinsp_capture.dll` and
`dxinsp_launch.exe` are looked for next to a packaged app and in `build/bin` and
`build/bin/{Release,RelWithDebInfo,Debug}`. Build them ([Building from source](BUILDING.md)), or
point `INSPECTOR_D3D12_DIR` at the directory holding them. A Vulkan application launched in the
meantime still works.

**Injection fails on a running application, or from an editor's launcher.** There is no way to
attach to a process that already exists: the library must be inside the process before its first
D3D12 call. Start the application through `dxinsp_launch.exe` (see
[Direct3D 12](D3D12.md#starting-an-application-by-hand)) and press **Connect**.

**Shaders show no text, and constant buffers are bytes.** The shaders are DXIL and
`dxcompiler.dll` was not found. The library looks beside itself, in `%VULKAN_SDK%\Bin`, in the
Windows SDK's `bin\<version>\x64` and on `PATH`; install the Vulkan SDK or copy the DLL next to
`dxinsp_capture.dll`. Text with no **Source** view means the shader carries no embedded source:
compile it with `dxc -Zi -Qembed_debug`.

**Validation layer ticked, but no messages.** The D3D12 debug layer is part of Windows'
optional *Graphics Tools* feature (Settings › System › Optional features). Without it
`DXINSP_DEBUG_LAYER` has nothing to enable.

**The application is slow while captured.** Every render target and every bound buffer and
texture is read back at the end of the pass it was used in, and the debug layer, when on, checks
every call. Untick **Validation layer**, and lower the capture's buffer and texture limits.

## macOS

**The target starts but never connects.** Almost always the hardened runtime: dyld dropped
`DYLD_INSERT_LIBRARIES`, so the capture library was never loaded. The launch dialog checks for
this and refuses to launch, so a target that got past the check and still went quiet is worth
confirming by hand:

```sh
codesign -d -v --entitlements - <the .app>
```

See [Metal](METAL.md#code-signing-decides-whether-this-works). Launch with **Layer log** on to see
the library's output in the session's **Log** tab.

**"capture library not found".** Point `INSPECTOR_METAL_LIB` at `libmtlinsp_capture.dylib`
directly, when it is not in the build tree or the packaged app.

## Android

**The application starts but never connects.** Check the **Log** tab:

- `run-as` failing with "not debuggable" means the build is not debuggable. Rebuild it as a
  development build.
- Nothing at all from `[vkinsp]` means the loader did not pick the layer up. `adb logcat -s
  vulkan` shows its search, and `adb shell settings list global | grep gpu_debug` shows the
  settings the inspector wrote.

**"adb not found".** Set `ANDROID_HOME` to the SDK, put `platform-tools` on `PATH`, or point
`INSPECTOR_ADB` at the adb executable.

**"The Android layer is not built".** Run `python tools/build_android.py` in a checkout, or set
`INSPECTOR_ANDROID_LAYER_DIR` to the directory holding `lib/<abi>/` and the layer APK. Released
installers do not include it.

**A headset launches nothing.** The Quest shell refuses to launch until controllers or tracked
hands are on, and leaves a dialog behind that blocks later launches until the shell restarts. An
OpenXR session also stays idle until the headset is worn. See
[Android and Quest](ANDROID.md#headsets-quest-and-other-openxr-devices).

## Shaders

**Shader text shows only SPIR-V bytes.** `spirv-dis` and `spirv-cross` were not found. Install
them, or point `INSPECTOR_TOOLS_DIR` at a directory containing them (`VULKAN_SDK` is searched
too).

**No Source view.** The shader carries no embedded source. Compile it with `glslc -g`,
`glslangValidator -g` or `dxc -fspv-debug=vulkan-with-source` (for Direct3D 12: `dxc -Zi
-Qembed_debug`), or set **Source roots** in the launch dialog so the inspector can read the files
the debug information names. See [shader sources](VULKAN.md#shader-sources).

## The app itself

**`TypeError: Cannot read properties of undefined (reading 'handle')` at startup.** Electron
started as plain Node because the terminal exported `ELECTRON_RUN_AS_NODE=1` — VS Code's
integrated terminal does. `npm start` clears it; `npx electron .` does not, so unset the variable
in that case.

**Electron fails to start on Linux with a sandbox or user-namespace error.** Recent distributions
(Ubuntu 23.10 and newer) restrict unprivileged user namespaces, which Chromium's sandbox needs.
Either allow them for this binary, or start the app with `npx electron . --no-sandbox`.

**The dock or task bar shows a generic icon (a gear on GNOME).** GNOME takes the icon from the
`.desktop` file matching the window's `WM_CLASS` (`gpu-inspector`), ignoring the icon the window
advertises. Run `tools/install_desktop_entry.sh`.

---

Previous: [Claude Code plugin](MCP.md) · [Docs index](README.md) · Next: [Building from source](BUILDING.md)
