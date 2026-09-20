# The launch window

[Docs index](README.md) › The launch window

**Launch...** in the bar along the top of the main window opens the *Launch Application* window.
It is where every session begins except the ones that start from a saved capture file: it decides
what is started, how the capture library gets into it, and which of the library's options are on
while it runs.

![The launch window, set to run a Vulkan executable on this computer](images/launch-dialog.png)

This page is the reference for every field in it. The platform pages — [Vulkan](VULKAN.md),
[Direct3D 12](D3D12.md), [Metal](METAL.md), [Android and Quest](ANDROID.md),
[Web pages and WebGPU](BROWSER.md) — cover what each target does once it is running.

## Opening it, and the Recent list

| How | What happens |
|---|---|
| **Launch...** | Opens the window, filled in with the last launch of this session, or the most recent saved configuration |
| **Recent** | Relaunches a saved configuration directly, without opening the window. **×** removes one, **Clear recents** removes them all |
| **Recent** dropdown *inside* the window | Fills the fields in from a saved configuration, to change something before launching |

The last twelve configurations are kept, in the app's `settings.json` under its user data
directory. An entry is identified by its target, its executable (or package and device) and its
arguments, so relaunching the same application with different options updates that entry instead
of adding another.

## Run On: what is being started

*Run On* is the first choice, and it decides which fields the rest of the window shows. Only the
targets that make sense on this machine are listed.

| Target | What it does | Available on | Button |
|---|---|---|---|
| **This computer** | Starts an application here with the capture library enabled for that process alone. Vulkan or Direct3D 12 on Windows, Vulkan on Linux, Metal on macOS | all | **Launch** |
| **A web page in a browser (WebGPU)** | Starts a browser on a page, capturing the Direct3D 12 its GPU process does underneath WebGPU | Windows | **Launch** |
| **Android device (adb)** | Starts a package on a connected device or headset | all | **Launch** |
| **An application started elsewhere (implicit layer)** | Starts nothing: registers the Vulkan layer for your account and waits for an application you start yourself | Windows, Linux | **Wait** |
| **An application started elsewhere (Direct3D 12)** | Starts nothing: watches for a process by name and injects the Direct3D 12 library as it starts | Windows | **Wait** |

Nothing here is registered system-wide except the implicit layer, which is registered for your
user account and only when you press **Register**.

## The fields for each target

### This computer

| Field | What it takes |
|---|---|
| **Executable Path** | The application to start. **...** browses for it. On macOS this is usually an `.app` bundle, and the inspector finds the executable inside it |
| **Working Directory** | The directory it starts in. Empty means the executable's own folder; some applications need one of their own |
| **Command-line Arguments** | Passed to the application |
| **Environment Variables** | `KEY=VALUE`, one per line, added to the application's environment |

On Windows there is nothing to say about which API the application uses: every native launch gets
the Vulkan layer *and* the Direct3D 12 library, and whichever one the application uses connects.
On macOS the library is injected with `DYLD_INSERT_LIBRARIES`, which a target signed with the
hardened runtime ignores — the launch says so rather than waiting forever; see
[Metal](METAL.md#code-signing-decides-whether-this-works).

### A web page in a browser (WebGPU)

| Field | What it takes |
|---|---|
| **Browser** | The browsers found on this machine, with their versions. **Refresh** looks again. **Other...** adds a **Browser Path** field for a browser that was not found, or a build of your own |
| **Page URL** | The page to open: an `http(s)://` address or a `file:///` path. Empty opens the browser's start page, and you navigate yourself |

The browser is started on a profile the inspector keeps for it, so the browser you already have
open keeps its windows and its session. See [Web pages and WebGPU](BROWSER.md).

### Android device (adb)

| Field | What it takes |
|---|---|
| **Device** | The devices `adb` can see, with their model, API level and ABI. **Refresh** looks again. A device that is `unauthorized` or `offline` is listed with its state, and the hint below says what to do |
| **Package** | The application's package name. Typing filters the device's third-party packages, and the count beside it says how many match. It must be debuggable — a Unity Development Build — for Android to load the layer |
| **Activity** | The activity to start, as `com.example.Activity` or `.Activity`. Empty starts the package's launcher activity |

See [Android and Quest](ANDROID.md).

### An application started elsewhere (implicit layer)

Nothing is launched. The Vulkan loader is told to load the layer into any application that asks
for it, and the session waits for one to connect.

| Row | What it is |
|---|---|
| **Registration** | Whether the layer is registered for your account, and the manifest it was registered from. **Register** / **Unregister** switches it. The Windows installer registers it for you |
| **Environment** | Whether `VKINSP_ENABLE` and `VKINSP_PORT` are set for your account, and for which port. **Set for my account** sets them so an application started by a launcher inherits them; **Clear** removes them |

Press **Wait**, then start the application with `VKINSP_ENABLE=1` and `VKINSP_PORT` set to the
port below (`VKINSP_LOG_FILE=<path>` writes the layer's log to a file, since the inspector cannot
read the output of a process it did not start). While the account variables are set, *every*
Vulkan application you start loads the layer and tries to connect — clear them when you are done.
See [Vulkan](VULKAN.md#applications-the-inspector-cannot-start).

### An application started elsewhere (Direct3D 12)

| Field | What it takes |
|---|---|
| **Executable Name** | The process to watch for, such as `TestVulkan.exe`, matched without regard to case. A full path matches only that build |

Press **Wait** first and start the application afterwards, however it is normally started. The
library has to be inside the process before it creates its Direct3D 12 device, so an application
that is *already running* cannot be caught. Only x64 processes are injected, and one running
elevated or as another user needs the inspector to run elevated too. See
[Direct3D 12](D3D12.md#waiting-for-an-application-to-start).

## Inspector options

These apply to every target, and are saved with the configuration.

| Option | What it does | Cost |
|---|---|---|
| **Symbol directories** | Directories holding the application's debug files, separated by `;`: unstripped libraries, so stack traces show functions, files and lines, and the PDBs of Direct3D 12 shaders, so a shader built with `dxc -Zs -Fd` still gets its Source view. Searched five levels deep | none |
| **Source roots** | Directories holding the shader sources, separated by `;`, for a shader compiled with line information but no embedded text | none |
| **Follow child processes** | For an application that renders in a process it starts itself: the library also goes into children whose command line contains this text. Several patterns separated by spaces; `!text` excludes one. Windows and Direct3D 12 only | none |
| **Record all command buffers** | Records every command buffer as it is built, so buffers recorded once and reused every frame still appear in captures | CPU time in the application |
| **Layer log** | Writes the library's own output to the session's **Log** tab (logcat on Android). On by default | small |
| **Validation layer** | The Khronos validation layer for Vulkan, the debug layer for Direct3D 12, Metal's API and shader validation on macOS. Messages are listed in the Inspect tab | slows the application |
| **Sync validation** | With the validation layer: synchronization hazards between commands and between submissions | slow |
| **GPU validation** | With the validation layer: rewrites the shaders to check descriptor indices, buffer addresses and indirect parameters on the GPU. The only thing here that catches an out-of-bounds index into a bindless heap | very slow |
| **Stack traces** | Records the call stack of every object creation, shown in the object's details. On by default | a few microseconds per object |
| **Device-lost breadcrumbs** | Markers written around every draw and dispatch, so a `VK_ERROR_DEVICE_LOST` names the command the GPU was running. Vulkan only, and needs `VK_AMD_buffer_marker` | two GPU writes per draw |
| **Compiler statistics** | What the driver's shader compiler made of each pipeline stage: registers, code size, spilled memory. Vulkan only, and needs `VK_KHR_pipeline_executable_properties` | compile time and driver memory |
| **Port** | The port the capture library and the inspector talk over. Change it only if something else uses it, or to run two sessions at once |

**Sync validation** and **GPU validation** do nothing unless **Validation layer** is ticked. None
of the validation options apply to an Android launch. On macOS the options with no Metal
counterpart — record all, sync and GPU validation, breadcrumbs, compiler statistics — are hidden.

## Queued capture

**Queued Capture** takes a capture as soon as the application connects, without you pressing
anything in the Capture tab:

| Mode | What it does |
|---|---|
| **No queued capture** | Nothing is captured until you ask for it |
| **Capture frame** | Captures frame `N`, where 0 is the first frame the inspector sees |
| **Capture after seconds** | Waits that many seconds after connecting, then captures the next frame |

Use it for a frame that has already gone by before you can reach the Capture tab — a loading
screen, or the first frame of a scene.

## After pressing Launch

The session opens as a tab of its own, with **Inspect**, **Capture** and **Log** tabs inside it,
and connects when the application's graphics device is created. A launch that could not start —
a path that does not exist, a device that went away, a missing layer build — leaves the window
open and says why.

For **Wait**, the session appears the same way and sits waiting; **Stop** ends the wait, not the
application, which the inspector did not start.

To pick up an application that is already running with a capture library enabled, use **Port** and
**Connect** on the main bar instead of this window.

## From the command line

Every field here has a command-line form, for scripting and for the test runs. A source build
takes them after `npm start --`; an installed build takes them directly
(`"GPU Inspector.exe" --launch=...`).

| Option | Field |
|---|---|
| `--launch=<exe>` `--args="..."` | This computer: executable and arguments |
| `--launch-android=<package>` `--device=<serial>` `--activity=<name>` | Android device |
| `--launch-browser=<url>` `--browser=<name or path>` | A web page in a browser |
| `--wait-for-app` | Implicit layer, waiting |
| `--wait-for-d3d12=<image>` | Direct3D 12, waiting for that process |
| `--implicit-layer=on\|off` | Registers or unregisters the layer, then quits |
| `--connect=<port>` | The main bar's **Connect** |
| `--list-targets` | Names the applications a capture library is serving, then quits |
| `--port=N` | Port |
| `--record-always` `--validation` `--sync-validation` `--gpu-validation` | The matching options |
| `--symbol-dirs=<dirs>` `--source-roots=<dirs>` `--follow=<text>` | The matching fields |
| `--capture-frame=N` `--capture-after=SECONDS` | Queued capture |

## If the launch does not work

See [Troubleshooting](TROUBLESHOOTING.md), which has a section per platform — in particular "The
target starts but never connects".

---

Previous: [Getting started](GETTING_STARTED.md) · [Docs index](README.md) · Next: [Vulkan](VULKAN.md)
