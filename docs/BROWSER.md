# Web pages and WebGPU (Windows)

[Docs index](README.md) › Web pages and WebGPU

A WebGPU page is a graphics application like any other, and GPU Inspector captures it the same
way — by putting its Direct3D 12 capture library into the process that does the rendering. For a
browser that is not the process you start: the page's WebGPU work, and the browser's compositing,
happen in a **GPU process** the browser spawns itself. The launch dialog's **A web page in a
browser (WebGPU, WebGL)** target starts the browser, follows it into that child, and gives you an
ordinary session with the same Inspect and Capture tabs as a native application.

What you get is the **Direct3D 12 underneath WebGPU**: the pipelines, descriptor heaps, command
lists and barriers that Chrome's [Dawn](https://dawn.googlesource.com/dawn) or Firefox's
[wgpu](https://github.com/gfx-rs/wgpu) produced from the page's WebGPU calls. That is a lower
question than the page-level one — *which WebGPU call did this, and with what arguments* — which
[WebGPU Inspector](https://github.com/brendan-duncan/webgpu_inspector) answers in the browser's
DevTools. The two go together: WebGPU Inspector to find the call, this to see what the driver was
actually given.

**Windows and Linux.** The two get into the GPU process differently. On Windows the D3D12
launcher follows the browser into it and what a capture holds is the Direct3D 12 underneath
WebGPU. On Linux nothing has to follow anything: the Vulkan layer is enabled by environment
variables and the GPU process inherits them from the browser it is started by, so what a capture
holds is the Vulkan underneath WebGPU — Dawn's backend in a Chromium browser, wgpu's in Firefox.

On Linux the launch also passes `--use-webgpu-adapter=vulkan`. Without it Chrome answers
`requestAdapter()` with SwiftShader, its software renderer, which draws WebGPU on the CPU and
creates no Vulkan device through the loader: the page renders, and there is nothing to capture.

The launch dialog hides the target on macOS, which has no capture layer for what a browser
renders with.

**WebGL** goes through the same launch with **Page API** set to WebGL: Chromium browsers are then
started with ANGLE on Vulkan, which the Vulkan layer captures, and Firefox with the OpenGL ES plugin
in place of the Direct3D 12 library. [A WebGL page](HOWTO_WEBGL.md) covers it.

## Launching a page

Press **Launch...** and set *Run On* to **A web page in a browser (WebGPU, WebGL)**.

| Field | What it takes |
|---|---|
| **Browser** | the browsers found on this machine, each with its version. **Refresh** looks again — after installing one, or after a Canary updated itself. **Other...** adds a **Browser Path** field for a browser that was not found, or a build of your own |
| **Page URL** | the page to open: an `http(s)://` address, or a `file:///` path to a local page. Left empty, the browser opens on its own start page and you navigate yourself |

Then press **Launch**. The browser starts, the library goes into its GPU process as that process
appears, and the session connects as soon as the browser's WebGPU implementation creates its
device — which for most pages is when the page first asks for an adapter. Until then the session
sits on *connecting*, and the **Log** tab carries the browser's command line, the launcher's
injection lines and the library's own output.

The browsers looked for under `Program Files`, `Program Files (x86)` and `%LOCALAPPDATA%` are
Chrome (stable, Beta, Dev and Canary), Microsoft Edge and Edge Canary, Brave, and Firefox, Firefox
ESR, Firefox Developer Edition and Firefox Nightly. Anything else — Chromium, Vivaldi, Opera, a
local build — works through **Other...**: what decides how it is launched is the executable's name,
`firefox.exe` being the Firefox family and everything else treated as Chromium.

### From the command line

```
npm start -- --launch-browser=https://example.com/webgpu-page --browser="Chrome Canary"
```

(the installed application takes the same options: `GPUInspector.exe --launch-browser=<url>`).
`--browser` takes a name (matched as a substring, so `Canary` or `Firefox` is enough) or a full
path to an executable; without it the first installed browser is used. The other launch options
apply as they do to a native launch — `--capture-frame=N` and `--capture-after=SECONDS` queue a
capture, `--validation` turns the D3D12 debug layer on, `--port=N` picks the port.

### A profile of its own

The browser is always started on a profile the inspector keeps for it, under the app's user data
directory in `browser-profiles\<browser>`. This is not a detail you can turn off, and there are two
reasons for it:

- **The browser you already have open keeps its windows, tabs and session.** The capture launch
  cannot share a profile with a running browser.
- **A running browser would otherwise swallow the launch.** Handed a URL, an already-running Chrome
  or Firefox opens a tab in *itself* and the process you started exits — leaving nothing to follow
  and nothing to capture. A separate profile (plus `-no-remote` on Firefox) is what prevents that.

The practical cost is that the profile is a fresh one: no extensions, no logins, no bookmarks, and
no saved permissions. A page that needs a sign-in needs it again in this profile — it persists
between launches, so once is enough. It is a normal browser profile, so DevTools (and the WebGPU
Inspector extension, if you install it there) work in it as usual.

## What the launch changes, and why

The two browser families agree on almost nothing here, so the launch composes a different command
line and a different follow pattern for each (`src/app/src/main/browsers.ts`).

| | Chromium (Chrome, Edge, Brave) | Firefox |
|---|---|---|
| WebGPU implementation | Dawn | wgpu (its Windows backend is D3D12 as well) |
| Process followed | `--type=gpu-process`, excluding `--use-gl=disabled` | `" gpu"` — the child's type, at the end of its command line |
| GPU sandbox off | `--disable-gpu-sandbox` | no switch exists: `security.sandbox.gpu.level = 0` in the profile |
| Kept out of a running browser | `--user-data-dir=<profile>` | `-no-remote -profile <profile>` |
| Also | `--disable-gpu-watchdog`, `--no-first-run`, `--no-default-browser-check` | `dom.webgpu.enabled = true`, written to `user.js` so it holds however the profile was left |

Three of those are load-bearing, and each fails in its own way when it is missing:

- **The GPU sandbox has to be off.** The capture library in a sandboxed GPU process cannot open its
  socket, so nothing ever connects and the library's log never appears. This is the usual reason a
  hand-rolled browser launch sits on *connecting* forever.
- **The watchdog has to be off** on Chromium, or the browser kills its own GPU process while a
  capture holds it and the session dies mid-frame.
- **The second GPU process has to be excluded.** Chrome starts an extra `--type=gpu-process` to
  collect GPU information; it creates a device of its own and exits again, and if the library goes
  into it first it takes the session's port from the process that actually renders (the library
  refuses a port another process already serves). `!--use-gl=disabled` is what leaves it alone.

A browser launch also leaves the [plugins](PLUGINS.md) out. The browser composites through
Direct3D 11, and the Direct3D 11 plugin, which goes into every other Windows launch, would connect
from the compositor before Dawn made its device and take the session: the capture was of the
browser drawing the page into its window, not of the page.

Following tracks the target's whole process tree rather than the one process the launcher started,
which is what makes Firefox work at all: Firefox starts its browser from a first process that exits
straight away, so the GPU process is a grandchild.

### Doing it by hand

For a browser that needs switches of its own — a flag to force a backend, `--enable-unsafe-webgpu`
on an older Chrome, a page that needs `--allow-file-access-from-files` — use *Run On* **This
computer** instead, point it at the browser executable, and fill the fields in yourself:
`--type=gpu-process !--use-gl=disabled` in **Follow child processes**, and the switches above in
the arguments. From a shell it is the launcher directly:

```
dxinsp_launch.exe --dll <path>\dxinsp_capture.dll --follow --type=gpu-process ^
                  --follow !--use-gl=disabled ^
                  -- chrome.exe --disable-gpu-sandbox --disable-gpu-watchdog ^
                     --user-data-dir=%TEMP%\gpuinsp-profile <url>
```

and from Claude Code the MCP server's `launch_app` with its `follow` argument
([Claude Code plugin](MCP.md)):

```json
{ "exe": "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
  "args": "--disable-gpu-sandbox --disable-gpu-watchdog --user-data-dir=C:\\Temp\\p https://example.com",
  "follow": "--type=gpu-process !--use-gl=disabled" }
```

Chrome's own `--gpu-launcher` hook, which starts the GPU process through a wrapper of your
choosing, does **not** work on current Chrome: a GPU process started that way exits within a
fraction of a second however it is wrapped — with no library involved at all — and the browser
respawns it in a loop. Follow mode is the way in.

## Capturing a frame

Capturing is the [ordinary capture workflow](CAPTURE.md), with one difference that shows up
everywhere in the result: **a browser's renderer does not present.**

Dawn and wgpu render into textures that the browser's *compositor* presents, so the captured
device never calls `Present`, and frames cannot be delimited by it. The inspector falls back — as
it does for an OpenXR application — to ending a frame at `ExecuteCommandLists` once the device has
gone a while submitting without a present. The Capture panel then shows **`frameBoundary: submit`**,
and:

- There is no display refresh, no vsync rate and no dropped-frame count: those come from the swap
  chain, and this device has none.
- A "frame" is the browser's submission rhythm, which usually is one page frame, but a page that
  submits several times per `requestAnimationFrame` may capture as more than one.
- If the compositor's own presents are visible on a hooked device and steal the framing, set
  `DXINSP_FRAME_BOUNDARY=submit` for the launch so presents are ignored outright.

The capture holds **everything the GPU process did**, which in a fresh single-tab profile is the
page plus the browser's own compositing — a handful of extra passes at the end, drawing the page
into the window. Keeping that profile to one tab is the easiest way to keep a capture readable.

**Capture several frames.** Dawn submits one page frame as more than one `ExecuteCommandLists` --
the page's command buffers, then a blit of the result -- and each is a "frame" here. A one-frame
capture of the shadow-mapping sample holds only that blit, with the scene arriving as a sampled
texture; **Frames** 4 holds the shadow pass, the scene and the blit, twice.

**Raise Max KB for a replay.** A buffer read-back is cut at Max KB (128 by default), and a WebGPU
simulation keeps its state in buffers much larger than that -- the particles sample's is 2.4 MB, the
A-buffer's 34 MB. The replay starts the rest as its own buffer holds it and says so, naming the
buffer and the size it needed.

Replaying the [WebGPU samples](https://webgpu.github.io/webgpu-samples/) this way, 4 frames and
Max KB 8192, on an RTX 4080: rotatingCube, texturedCube, fractalCube, renderBundles (20,019
commands), shadowMapping, deferredRendering, computeBoids, particles, imageBlur, gameOfLife,
skinnedMesh, cubemap and occlusionQuery replay with every target identical, with and without the
debug layer, which reports nothing. The A-buffer sample differs in 2 texels in some replays and
none in others: it resolves fragments its shaders appended with atomics, whose order varies from
run to run.

## Reading a WebGPU capture

Everything in the capture is under its D3D12 name, so the mapping back to the page is worth
knowing.

- **Passes.** A WebGPU render pass arrives as the render targets being set; D3D12 has no render
  pass object unless the implementation uses `BeginRenderPass`, so the capture groups commands into
  passes itself and marks the end with a synthetic `EndRenderTargets` command — the one command in
  the list the application did not make. A run of dispatches is a compute pass.
- **Bindings.** Bind groups arrive as descriptor tables and root parameters on a root signature
  that the implementation generated; a uniform buffer is a constant buffer, and its contents are
  decoded through the shader's reflection at each draw. Buffer and texture names from the page
  (`label:` in WebGPU) survive when the implementation passes them to `SetName`.
- **Shaders.** The page's WGSL is gone by this point: it was translated to HLSL and compiled to
  DXIL inside the browser, with no source in the container, so a stage shows its **disassembly**
  plus the HLSL GPU Inspector generates from its reflection. Entry point names are the tell of which
  browser you are looking at — naga keeps the page's WGSL names (`vertexMain`), while Dawn mangles
  them (`dawn_entry_point_66696c…`). A Firefox capture is the easier one to read for that reason
  alone.
- **Validation.** **Validation layer** on this target turns on the *D3D12 debug layer inside the
  browser's GPU process*. It reports what Dawn or wgpu did wrong with D3D12, which is a browser bug
  when it happens, not a page bug. Page-level WebGPU validation errors are the browser's own and
  belong in DevTools or WebGPU Inspector.

## Debugging a page

Where this earns its place over the page-level tools is anything that depends on what the GPU
actually did.

**Is it even using the GPU?** `chrome://gpu` (`about:support` in Firefox) says whether WebGPU is
hardware-backed, and on a laptop with two GPUs which adapter the browser picked. If the session
never connects and the page renders, that page is not creating a WebGPU device at all.

**Why is the frame slow?** The [Frame Stats](REPORTS.md) report gives GPU time per pass, pipeline
statistics and the samples that passed the depth test, all measured by the library in the GPU
process; **GPU Bottlenecks** turns those into what is limiting the frame, with **Overdraw** measured
in the browser during the capture rather than replayed. [Finding GPU bottlenecks](PROFILING.md) is
the step-by-step method, and it applies unchanged here. This is the part no page-level tool can do:
`timestamp-query` gives a page its own numbers, but not what the driver did between them.

**Why is this pixel wrong?** **Pixel history** follows one pixel of a render target through the
frame and lists every draw that touched it and what happened to it, including the clears, copies
and resolves. Take it from the capture's render target tab.

**Does this shader change fix it?** **Edit** on a pipeline stage compiles replacement HLSL with
`dxc` and the running browser draws with it from the next frame, **Restore Original** puts the
browser's own pipeline back. Because the HLSL is generated from reflection, the replacement is
binding-compatible by construction — so a shader can be bisected, or a suspect term knocked out,
without touching the page or reloading it.

**What did the browser make of my pipeline?** The pipeline state object in the Inspect tab has the
blend, depth, rasterizer and input layout state the implementation derived from the page's
`GPURenderPipelineDescriptor`, and the root signature it built for the bind group layouts — which
is where a surprising `@group`/`@binding` cost or an unexpected dynamic offset shows itself.

### What is not available here

Beyond the [D3D12 limits](D3D12.md#what-is-not-there-yet), which all apply:

- **Measure shader** needs the stage's HLSL for the flame graph's frames, and browser-generated
  shaders have none, so their stages stay unweighted and are not measured. **Measure draws** and
  hardware counters replay the capture as they do any D3D12 one.
- **The shader debugger needs HLSL source**, and browser-generated shaders have none. Stepping a
  WebGPU shader is not possible this way.
- Dropped frames, vsync rate and the present-based figures need a swap chain (above).

## If it does not work

| What you see | What it usually is |
|---|---|
| The browser opens the page, the session stays on *connecting* | The page has not created a WebGPU device yet (the session connects when it does), or the page does not use WebGPU at all. `chrome://gpu` / `about:support` says whether WebGPU is available in this build |
| Nothing connects, and the **Log** has no line from the library | The GPU sandbox is on. With a hand-made launch, add `--disable-gpu-sandbox`; for Firefox the preference has to be in the profile before it starts |
| The session connects and dies a few seconds in | Chromium's GPU watchdog killed the process during a capture — `--disable-gpu-watchdog` |
| It connects, then the port is taken by something else | Chrome's GPU-information process was injected into. Keep the `!--use-gl=disabled` exclusion |
| The browser opens a tab in the browser you already had open, and the session never starts | The launch reached a running instance. The dialog's browser target avoids this; by hand, use a `--user-data-dir` of your own, or `-no-remote -profile` on Firefox |
| Frames never end, or one "frame" holds everything | Presents are being seen from elsewhere on the device, or the submit heuristic has not tripped. `DXINSP_FRAME_BOUNDARY=submit` |
| Firefox starts but the page says WebGPU is unsupported | The launch writes `dom.webgpu.enabled` into its profile, but a build without WebGPU compiled in cannot be switched on — try Firefox Nightly |
| The browser is not in the list | It installs somewhere the search does not look. **Other...** and point at the executable |

[Troubleshooting](TROUBLESHOOTING.md) has the general Direct3D 12 entries, and
[Direct3D 12](D3D12.md#renderers-that-do-not-present-webgpu--dawn) the mechanism underneath this
page.

---

[Docs index](README.md) · [Direct3D 12](D3D12.md) · [Capture](CAPTURE.md) ·
[Finding GPU bottlenecks](PROFILING.md)
