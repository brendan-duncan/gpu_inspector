# How to inspect a WebGPU page

A WebGPU page renders in a process the browser starts for itself, so the inspector launches the
browser, follows it into that **GPU process**, and gives you an ordinary session. What a capture
then holds is the **Direct3D 12 (or Vulkan) underneath WebGPU** — the pipelines, command lists and
barriers that Chrome's Dawn or Firefox's wgpu made out of the page's WebGPU calls.

[Web pages and WebGPU](BROWSER.md) is the reference: every field, every browser, what the launch
changes and why. This is the short path, checked against Chrome on Windows with the
[WebGPU samples](https://webgpu.github.io/webgpu-samples/).

## Steps

1. Press **Launch...** and set *Run On* to **A web page in a browser (WebGPU)**.
2. Pick a **Browser** from the list of the ones found on this machine.
3. Put the page in **Page URL** — an `http(s)://` address, or a `file:///` path to a local page.
   Leave it empty to start on the browser's own start page and navigate yourself.
4. Press **Launch**.

From a shell:

```
GPUInspector.exe --launch-browser=https://webgpu.github.io/webgpu-samples/ --browser=Chrome
```

`--browser` matches as a substring, so `Canary` or `Firefox` is enough.

The browser starts on a profile the inspector keeps for it, the capture library goes into the GPU
process as that process appears, and the session connects when the page's WebGPU implementation
creates its device — for most pages, when the page first calls `requestAdapter()`. Until then the
session sits on *connecting*: it is waiting for the page to use WebGPU, not for the browser.

Then capture from the **Capture** tab as usual.

## What a WebGPU capture looks like

Smaller than you expect, and that is the point of it. A capture of the samples' rotating-cube page
is 15 commands: one `ExecuteCommandLists`, one command list holding a `Reset`, a
`SetDescriptorHeaps`, a `BeginRenderPass`, the viewport and scissor, the pipeline, topology, root
signature and root constants, one `DrawInstanced`, `EndRenderPass` and `Close`. That is the whole
frame the page draws — Dawn produced it from a handful of WebGPU calls.

Things worth recognising in it:

- **Shaders are named `dawn_entry_point_<hash>`.** WGSL entry point names do not survive Dawn's
  translation into HLSL and DXIL, so the capture shows the translated stage. The disassembly and
  reflection are the real thing.
- **Dawn passes its uniforms as root constants** where it can, which is why a draw's bound
  resources can look emptier than the page's bind groups suggest.
- **The browser's own compositing is in the process too.** A capture may hold the page's frame, the
  compositor's, or both, depending on when it lands. The passes' render targets say which is which.

For the question one level up — *which WebGPU call did this, with what arguments* —
[WebGPU Inspector](https://github.com/brendan-duncan/webgpu_inspector) answers that in the
browser's own DevTools. The two go together: WebGPU Inspector to find the call, this to see what
the driver was given for it.

## What is different from a native application

- **Windows and Linux only.** The launch target is hidden on macOS, which has no capture layer for
  what a browser renders with.
- **On Linux the capture is Vulkan**, not Direct3D 12: the layer is enabled by environment
  variables the GPU process inherits. The launch also passes `--use-webgpu-adapter=vulkan`, without
  which Chrome answers with SwiftShader and renders WebGPU on the CPU — the page draws and there is
  nothing to capture.
- **The profile is the inspector's own**, not your everyday one: your extensions, logins and tabs
  are not there, and neither is anything you do in it afterwards.
- **A page that renders once** (a still image, a chart) may finish before you press Capture. Use
  **At frame** in the capture bar, or a queued capture in the launch dialog, and reload the page.

## If it does not work

Read the session's **Log** first. The two lines that matter are the browser process being loaded
into and then the GPU process:

```
loaded into pid 41660: "...\chrome.exe" ... https://webgpu.github.io/webgpu-samples/
loaded into pid 2092:  "...\chrome.exe" --type=gpu-process ...
```

The second one is the one that captures. If it never appears, nothing was captured because nothing
was followed.

| What you see | What it means |
|---|---|
| The session stays on *connecting* | The page has not asked for a WebGPU adapter. Check it actually uses WebGPU in this browser — `chrome://gpu` says whether WebGPU is available. |
| The browser opens a tab in a window you already had, and no session starts | The launch reached a running instance of that browser. The launch target's own profile normally avoids this; close the running browser and launch again. |
| A capture holds only compositing | The page's own frame was not in flight when the capture landed. Capture more frames, or use **At frame**. |
| The page renders on the CPU (Linux) | The adapter is SwiftShader; the launch's `--use-webgpu-adapter=vulkan` did not take. |

## See also

- [Web pages and WebGPU](BROWSER.md) — the full reference, including doing the launch by hand and
  what the browser's command line is changed to
- [Direct3D 12](D3D12.md) — the backend a capture holds on Windows
- [Capture](CAPTURE.md) — the capture bar and reading a frame
