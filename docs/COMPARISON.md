# GPU Inspector compared with RenderDoc, PIX and Nsight Graphics

[Docs index](README.md) › Comparison

A frame debugger is not a thing you pick once. Most people who work on a renderer end up with
several installed, because each was built around a different question and each one's answer to
the others' questions is a weaker version of it. This page is what GPU Inspector does and does not
do, next to the three tools it is most often compared with.

| Tool | Version this page describes | Who makes it | License |
|---|---|---|---|
| **GPU Inspector** | 0.18.0 | this project | MIT, free |
| **RenderDoc** | 1.47 | Baldur Karlsson and contributors | MIT, free |
| **PIX on Windows** | 2026 releases | Microsoft | proprietary, free |
| **Nsight Graphics** | 2025.x / 2026.x | NVIDIA | proprietary, free |

As of September 2026. The other three are actively developed and this page will fall behind them;
where a row is wrong or out of date, please say so in
[Discussions](https://github.com/brendan-duncan/gpu_inspector/discussions) and it will be fixed.
Their entries are taken from their own documentation, linked at the [end](#sources); GPU
Inspector's are what the rest of these docs describe, and each links to the page that covers it.

**Legend:** ● supported · ◐ supported with a real limit, named in the cell · ○ not supported

---

## 1. What each one is for

**GPU Inspector** inspects a *live* application as much as it captures frames from it: the object
list fills as the application creates objects, a shader edited in the app is used by the next frame
the application draws, and a capture saved from it is a self-contained file that opens on any
machine without the application or a compatible GPU. It leans on analysis over browsing — the frame
rules, the render graph, the bottleneck report, the shader flame graph — and it is the only one of
the four an AI agent can drive, through its [Claude Code plugin](MCP.md).

**RenderDoc** is the broadest and the most mature frame debugger: more APIs than anything else
here, no vendor lock, a Python API for everything its UI can do, and a decade and more of every
awkward case in it. If a frame can be captured at all, RenderDoc is the most likely to capture it.

**PIX on Windows** is Direct3D 12 in depth, and the only one of the four whose timing and memory
captures are first-class tools in their own right rather than a panel inside the frame debugger.
On D3D12, its replay-driven timing data and Dr. PIX experiments answer performance questions
nothing else on Windows answers as directly.

**Nsight Graphics** is the hardware. Its GPU Trace and shader profiler read NVIDIA GPUs at a level
no vendor-neutral tool can reach — unit throughputs, warp stall reasons, shader hot spots
correlated to source — and its ray tracing support, including the acceleration structure viewer,
is the best there is. The price is that it is NVIDIA-only.

---

## 2. APIs and platforms

| | GPU Inspector | RenderDoc | PIX | Nsight Graphics |
|---|---|---|---|---|
| Vulkan | ● [Windows, Linux, Android](VULKAN.md) | ● | ○ | ● |
| Direct3D 12 | ● [Windows](D3D12.md) | ● | ● | ● |
| Direct3D 11 | ○ | ● | ○ | ● |
| OpenGL / OpenGL ES | ○ | ● | ○ | ◐ OpenGL 4.2–4.6 |
| Metal | ● [macOS](METAL.md) | ◐ in development, not in releases | ○ | ○ |
| WebGPU in a browser | ◐ [as the D3D12 under it](BROWSER.md) | ○ | ○ | ○ |
| Ray tracing (DXR, VK\_KHR\_ray\_tracing) | ◐ Vulkan: [structures, instances, shader binding tables](INSPECT.md#acceleration-structures) | ● | ● | ● best in class |
| Host OS | ● Windows, macOS, Linux | ● Windows, Linux | ◐ Windows | ◐ Windows, Linux |
| Android / Quest | ● [Vulkan, over adb](ANDROID.md) | ● Vulkan and GLES | ○ | ○ |
| Consoles | ○ | ◐ Switch, with a devkit | ● Xbox (PIX for Xbox) | ○ |
| GPU vendors | ● any | ● any | ● any, with vendor plugins for counters | ○ NVIDIA only |

The shape of this table is most of the decision. Anything that is not Vulkan, D3D12 or Metal is
not a GPU Inspector question; anything on a Mac is not a RenderDoc, PIX or Nsight question.

---

## 3. Inspecting an application

| | GPU Inspector | RenderDoc | PIX | Nsight Graphics |
|---|---|---|---|---|
| Live object list, filling as objects are created | ● [Inspect](INSPECT.md) | ○ within a capture only | ○ within a capture only | ○ within a capture only |
| Creation arguments, `pNext` chains decoded | ● | ● | ● | ● |
| Dependencies between objects, both directions | ● | ● Resource Inspector | ● | ● |
| Read a texture or a descriptor set back from the *running* application | ● [live, with refresh](INSPECT.md#textures) | ○ | ○ | ○ |
| Leaked objects at device destruction | ● [with creation stacks](INSPECT.md#leaks) | ○ | ◐ memory captures | ○ |
| Creation call stacks | ● | ● | ● | ● |
| Device properties, limits, heaps, extensions | ● | ● | ● | ● |
| Frame time graph and in-application HUD | ● [HUD and live pause](INSPECT.md#the-in-app-hud) | ◐ overlay with frame time | ○ | ◐ HUD |
| Freeze the application on a frame and step frames | ● [live pause](INSPECT.md#live-pause) | ○ | ○ | ○ |

This column is where GPU Inspector differs most in kind rather than in degree. The other three are
frame debuggers: you capture, and then you study the capture. Inspect is a window onto the process
while it runs.

---

## 4. Capturing and reading a frame

| | GPU Inspector | RenderDoc | PIX | Nsight Graphics |
|---|---|---|---|---|
| Capture N consecutive frames | ● | ● | ● | ● |
| Capture a numbered frame, or after a delay, unattended | ● [queued capture, CLI](CAPTURE.md#taking-a-capture) | ● | ● | ● |
| Commands grouped by submit, command buffer, pass, debug label | ● | ● | ● | ● |
| Full pipeline state at any command | ● | ● | ● | ● |
| Descriptor sets / heaps, buffers decoded into shader types | ● [with format and radix overrides](CAPTURE.md#reading-the-frame) | ● | ● | ● |
| Vertex, index and indirect data | ● | ● | ● | ● |
| Render targets read back per pass | ● | ● | ● | ● |
| Texture viewer: mips, layers, channels, exposure, texel values | ● | ● | ● | ● |
| NaN / Inf / out-of-range texels marked by default | ● [Highlight](REPORTS.md#what-the-picture-cannot-show) | ◐ available, off by default | ◐ | ◐ |
| Histogram per channel | ● | ● | ● | ● |
| Per-command call stacks | ● | ● | ● | ● |
| Custom visualization shaders over a target | ○ | ● | ● | ● |
| Programmatic capture from the application | ● [`gpu_inspector.h`: Vulkan, D3D12](CAPTURE.md#capturing-from-the-application) | ● in-application API | ● `PIXBeginCapture` | ● |

---

## 5. Debugging what a frame drew

| | GPU Inspector | RenderDoc | PIX | Nsight Graphics |
|---|---|---|---|---|
| Pixel history | ● [Vulkan by replay; Metal and D3D12 measured while capturing](REPORTS.md#pixel-history) | ● Vulkan, D3D11, D3D12, GL | ● | ● |
| Overdraw heatmap | ● [Vulkan by replay; Metal and D3D12 measured while capturing](REPORTS.md#overdraw) | ● quad overdraw | ● overdraw and depth complexity | ● |
| Draw highlight / depth test / wireframe overlays | ● [Vulkan by replay; D3D12 measured while capturing](REPORTS.md#draw-call-overlays) | ● all APIs | ● | ● |
| Mesh view: vertex inputs, 3D preview | ● [all backends](REPORTS.md#mesh-view) | ● the reference implementation | ● | ● |
| Vertex shader output in clip space, with why a mesh is invisible | ● [Vulkan by replay; D3D12 by stream output](REPORTS.md#mesh-view) | ● VS/GS/DS out | ● | ● |
| Shader debugger, stepping by source line | ● [Vulkan, Metal, D3D12](REPORTS.md#shader-debugger) | ● Vulkan, D3D11, D3D12 | ● incl. geometry and work graph shaders | ◐ Vulkan only, beta, on-hardware |
| Debug a pixel from its history | ● | ● | ● | ● |
| Debug a compute invocation | ● | ● | ● | ● |
| Step SPIR-V with no debug information, by line | ● [via decompiled GLSL, checked against the original](REPORTS.md#shader-debugger) | ◐ by instruction | n/a | ○ |
| Shader edit and re-run inside the capture | ● [Compile & Replay, with the changed targets side by side: Vulkan, D3D12](INSPECT.md#editing-a-shader) | ● | ● Edit & Continue, with a diff | ● dynamic shader editing |
| Shader edit applied to the **running application** | ● [Vulkan, D3D12, Android](INSPECT.md#editing-a-shader) | ○ | ○ | ◐ live editing during replay |
| Acceleration structure contents and instances, drawn | ◐ [Vulkan, from the build](INSPECT.md#acceleration-structures) | ◐ | ◐ | ● the AS viewer, with overlap analysis |
| Shader binding table records matched to their groups | ● [Vulkan](CAPTURE.md#reading-the-frame) | ◐ | ● | ● |

---

## 6. Performance

| | GPU Inspector | RenderDoc | PIX | Nsight Graphics |
|---|---|---|---|---|
| GPU time per pass | ● [Profile passes](CAPTURE.md#taking-a-capture) | ◐ event timings | ● replay-based timing data | ● |
| GPU time per draw | ● [Measure draws: Vulkan and D3D12 by replay, D3D12 also while capturing](REPORTS.md#shader-flame-graph) | ◐ | ● | ● |
| Pipeline statistics (invocations, primitives, fragments) | ● Vulkan, Metal | ● | ● | ● |
| Hardware counters (throughput, cache, occupancy, stall reasons) | ◐ [Vulkan and D3D12 by replay: NvPerf, or `VK_KHR_performance_query` on Vulkan](REPORTS.md#gpu-bottlenecks) | ◐ counter viewer, vendor APIs | ◐ via IHV plugins, occupancy on NVIDIA | ● GPU Trace, the deepest here |
| Shader profiler: hot spots correlated to source | ◐ [modelled, then measured per line](REPORTS.md#shader-flame-graph) | ○ | ◐ | ● hardware sampling |
| Measured cost of one function, source line or texture in a shader | ● [Measure shader, by ablation: Vulkan, D3D12](REPORTS.md#shader-flame-graph) | ○ | ◐ Dr. PIX experiments | ◐ |
| Flame graph of the frame's GPU work | ● [pass → pipeline → stage → function → line](REPORTS.md#shader-flame-graph) | ○ | ○ | ○ |
| Per-pass bottleneck verdict with what usually causes it | ● [GPU Bottlenecks](REPORTS.md#gpu-bottlenecks) | ○ | ● Dr. PIX | ● |
| CPU timeline: where the frame's CPU time went | ◐ [the calls the library times; no other threads](REPORTS.md#frame-stats) | ○ | ● timing captures, ETW and callstacks | ● (Nsight Systems) |
| Is the frame CPU-bound, GPU-bound or display-bound | ● [Frame Bound card](REPORTS.md#frame-stats) | ○ | ● | ● |
| Recording every frame's time to find a hitch | ◐ [Timing Capture: Vulkan, D3D12](PROFILING.md#step-1c-a-hitch-rather-than-a-slow-frame) | ○ | ● | ● |
| What each thread was doing in the hitch | ◐ [call stacks sampled in the process, running or blocked: Windows](PROFILING.md#step-1c-a-hitch-rather-than-a-slow-frame) | ○ | ● ETW: context switches, every process | ● (Nsight Systems) |
| Memory allocation analysis | ◐ [Memory Capture, every allocation and free: Vulkan, D3D12](PROFILING.md#what-is-allocating); no residency per resource | ◐ | ● memory captures | ● |
| Render graph: passes, the resources between them, the critical path | ● [Render Graph](REPORTS.md#render-graph) | ○ | ○ | ○ |
| Frame-level rules flagging waste, each linked to its command | ● [Frame Issues](REPORTS.md#frame-stats) | ○ | ● Warnings | ◐ |
| Static shader analysis with no source needed | ● [Analyze Shaders](REPORTS.md#analyze-shaders) | ○ | ○ | ◐ |
| Driver compiler statistics (registers, spills, binary size) | ● [Vulkan](INSPECT.md#compiler-statistics) | ○ | ◐ occupancy | ● |

A frame profiler and a hardware profiler are different tools. GPU Inspector's answer to "which
unit is saturated" is NVIDIA's own counter SDK on a Vulkan replay, which is Nsight's data arriving
by a longer road; if you are on NVIDIA hardware and that is your question, use Nsight. What GPU
Inspector has that the others do not is the layer above it — the render graph, the frame rules and
the flame graph, which say *what in the frame* to change rather than what the silicon was doing.

---

## 7. Correctness

| | GPU Inspector | RenderDoc | PIX | Nsight Graphics |
|---|---|---|---|---|
| Validation layer / debug layer messages in the UI | ● [linked to the objects they name](INSPECT.md#validation-messages) | ◐ in the log | ● | ● |
| Synchronization validation | ● Vulkan sync validation | ○ | ◐ barrier warnings | ◐ |
| GPU-assisted validation, attached to the draw that failed | ● [Vulkan and D3D12](INSPECT.md#validation-messages) | ○ | ● GPU validation | ● |
| GPU crash analysis after a device removal | ● [Vulkan breadcrumbs](TROUBLESHOOTING.md#vulkan), [D3D12 DRED](TROUBLESHOOTING.md#direct3d-12) | ○ | ● DRED integration | ● Aftermath |

A lost device is named rather than dumped: the command the GPU was running, and on D3D12 a page
fault's address with the resources around it. What is not here is a crash dump to open later, or
Aftermath's shader-level detail on NVIDIA.

---

## 8. Sharing, automation and files

| | GPU Inspector | RenderDoc | PIX | Nsight Graphics |
|---|---|---|---|---|
| Capture file that reopens without the application | ● [`.gpucap`](CAPTURE.md#capture-files) | ● `.rdc` | ● `.pix3` | ● |
| …on a different OS, or without a GPU that can run the frame | ● everything shown is in the file | ◐ replay needs a compatible device, locally or remotely | ◐ | ◐ |
| Export the frame as a standalone C++ project | ● [Vulkan, D3D12, Metal](CAPTURE.md#export-to-c) | ○ | ○ | ● C++ Capture |
| Export a report as a standalone HTML file | ● [every report](REPORTS.md) | ○ | ○ | ○ |
| Scripting API | ○ | ● Python, all of the UI's functionality | ◐ `pixtool` command line | ◐ |
| Command line: launch, capture, save, unattended | ● | ● | ● | ● |
| AI / MCP integration | ● [Claude Code plugin over saved captures and live sessions](MCP.md) | ○ | ○ | ○ |
| Numerical before/after comparison of two captures | ◐ [through the plugin; by eye in two windows](MCP.md) | ○ | ○ | ○ |
| Remote capture from another machine | ◐ Android over adb | ● remote replay and capture | ○ | ● |
| Analytics or telemetry collected | ○ none | ○ none | ◐ | ◐ |
| Source code available | ● MIT | ● MIT | ○ | ○ |

RenderDoc's Python API is the biggest single thing GPU Inspector has no answer to for anyone who
automates their tooling. The MCP plugin covers a different case — asking questions in English, and
letting an agent run the edit, capture, compare loop — and is not a substitute for a scriptable
API.

---

## 9. Where GPU Inspector is not the right tool

- **You are on OpenGL or Direct3D 11.** Use RenderDoc.
- **You need the last 10% on NVIDIA hardware** — warp stalls, unit throughput, the shader
  profiler, ray tracing in depth. Use Nsight Graphics.
- **You need a kernel's view of a D3D12 title** — context switches, the other processes, the GPU's
  hardware queues, residency per resource. Use PIX. The timing and memory captures here see inside
  the process only.
- **Your renderer crashed the GPU and the command it stopped on is not enough.** Use Aftermath.
- **Your pipeline scripts a frame debugger.** Use RenderDoc's Python API.
- **You need a tool with a decade of edge cases in it.** RenderDoc captures applications that
  GPU Inspector, at 0.x, will not.

And where it is:

- **Metal on macOS**, where the alternative is Xcode's own tools and nothing cross-platform.
- **A frame you want to hand to someone else** — a capture file that opens anywhere, an HTML
  report, or a C++ project a driver team can build.
- **A question you would rather ask than click through**, with the Claude Code plugin reading the
  capture.
- **Changing a shader and watching the running application**, rather than the capture.
- **Knowing what to change**: the render graph, the frame rules, the bottleneck report and the
  flame graph are opinions about your frame, which the other three deliberately do not offer.

---

## Sources

- [RenderDoc documentation](https://renderdoc.org/docs/) and its [source](https://github.com/baldurk/renderdoc) (1.47)
- [PIX on Windows: GPU captures](https://devblogs.microsoft.com/pix/gpu-captures/) and [Analyze frames with GPU captures](https://learn.microsoft.com/en-us/windows/win32/direct3dtools/pix/articles/gpu-captures/pix-gpu-captures)
- [Nsight Graphics feature list](https://developer.nvidia.com/nsight-graphics-features) and [documentation](https://docs.nvidia.com/nsight-graphics/)
- GPU Inspector: this documentation, and the [changelog](../CHANGELOG.md)

---

[Docs index](README.md) · [Project README](../README.md)
