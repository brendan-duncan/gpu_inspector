## 0.4.0

### Added
- Device sections in the Inspect tab: the physical device's properties, a filterable limits
  table, memory heaps and types, queue families, supported features and extensions; the device's
  enabled extensions, queues and features (flattened across the pNext chain); the instance's
  application info, layers and extensions. The layer attaches the physical device's
  capabilities at `vkEnumeratePhysicalDevices`, so capture files carry them too.
- Physical devices show their name and type in the object list.
- "Affected by" on a captured buffer (descriptor bindings, vertex, index and indirect buffers):
  the earlier commands of the frame that wrote it, transfers naming it as their destination and
  draws or dispatches that had it bound as a storage buffer; clicking one selects it.
- Leak report: when a device or instance is destroyed with objects still alive under it, the
  layer reports them (type counts and the objects with their names); the Inspect tab lists them
  in a Leaked Objects group, the session bar counts them, and the Log tab records the summary.
  The triangle test application's `--leak` option leaves a sampler and a buffer alive.

### Changed
- Shader Reflection sections are collapsed by default.

### Fixed
- Compile & Apply from the Source view failed with `'#version' : must occur first in shader`:
  the editor passed glslang's embedded prefix (`// OpModuleProcessed` comments and a `#line`
  directive) to the compiler. The editor now opens the source without it.
- Compile & Apply on a pipeline re-rendered the details panel and closed the editor; the edit
  result now updates the object list and the section label in place, and the editor stays open.
- Shader payloads in loaded capture files could start at an unaligned byte, which made the
  debug-info parser throw and left the shader section at "Loading..." with no Source view.

## 0.3.0

### Added
- Render pass thumbnail strip beside the capture's command list; clicking a tile selects the
  pass. Captured render targets open in the full image viewer (zoom, channels, exposure, auto
  range, texel values) in place.
- Sampled and storage images bound by descriptor sets are read back at capture time, once per
  image view under a per-capture budget (the "Images" capture option), and shown on the
  binding with the image viewer a click away. Capture files carry them, and the Inspect tab's
  image viewer falls back to captured contents when no application is connected.
- A Reflection section on every shader payload in the Inspect tab: entry points, interface,
  resources by set and binding with struct members, push constants.
- The mouse test aid accepts a sequence of points.

## 0.2.0

### Added
- Capture files (`.gpucap`): save a capture from the capture bar or the tab menu, reopen it
  from the launch bar or by dropping it on the window. A loaded file is a session of its own
  with the object graph, shaders, buffers, render targets, timings and validation messages;
  "Open in New Tab" copies a capture in memory.
- Validation messages: the layer registers a debug-utils messenger and forwards the validation
  layer's output with repeat counts and object links; a "Validation layer" option in the launch
  dialog enables `VK_LAYER_KHRONOS_validation`; the Inspect tab lists the messages, marks the
  objects they name, and the session bar counts errors and warnings.
- Compute pass timings: runs of dispatches outside a render pass are bracketed with timestamps
  and grouped as "Compute N" blocks in the command list, the timeline and Frame Stats.
- Android targets through adb (layer APK, GPU debug layer settings, port forwarding).
- The triangle test application has a compute stage and a `--bad-scissor` option that raises a
  validation error.

### Fixed
- The launcher's `VK_LAYER_PATH` hid the loader's registry search for explicit layers, so the
  SDK's validation layer was never found; the validation layer's directory is added to the path.

## 0.1.0

### Added
- Vulkan capture layer (`VK_LAYER_INSPECTOR_capture`) generated from `vk.xml`, with a TCP
  transport to the Electron UI; launch or connect to any Vulkan application, Unity players first.
- Live object inspection: every object with its creation arguments, dependencies, labels and
  memory bindings; object list filters; frame time, submit time and object count meters;
  memory totals; descriptor set contents on demand; the image viewer for live images.
- Frame capture: the command stream grouped by submit, command buffer, render pass and debug
  label; per-draw pipeline state, per-stage reflection, descriptor sets with parsed uniform and
  storage buffers (Format editor, radix, array paging), vertex/index/indirect data, push
  constants, and read-back render targets. Multiple frames, queued captures at a frame or after
  a delay, one tab per capture, a command filter, Frame Stats.
- Profile passes: GPU timestamps per render pass, pass durations, a pass timeline, and a Frame
  Bound card comparing GPU time and CPU submit time with the frame interval.
- Shader editing as GLSL, HLSL or SPIR-V assembly with the Vulkan SDK's compilers and live
  replacement pipelines; syntax highlighting; shader source maps from embedded debug information
  (OpSource / OpLine and NonSemantic.Shader.DebugInfo.100) with a Source view linked to the
  disassembly.
- Dark and light themes; Windows and Linux builds; installers with self-update.
