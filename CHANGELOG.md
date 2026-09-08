## 0.5.0

### Added
- Shader cost per source line: modules with line information get their modeled cost split by
  the line each instruction came from. The Shader Cost section lists the costliest lines with
  their share, clickable to the line in the Source view (Inspect tab and captured draws), and
  the Shader Flame Graph shows a function's own cost as line frames under it.
- Sampled image read-back covers every mip level of the bound view (the base mip only before):
  the image viewer on a captured binding offers the mips, and the binding says which are held
  ("mips 0-3"). The triangle test application's checker texture has a mip chain.

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
- Static shader analysis on SPIR-V: every shader payload in the Inspect tab gets a Shader Cost
  section (modeled ALU, special-function, texture and memory cost per entry point and function,
  loops weighted) and a Performance Analysis section (texture samples, expensive builtins,
  non-constant division, atomics, barriers and storage access inside loops; derivatives in
  branches; discard; integer division), with findings linked to source lines when the module
  has debug information. "Analyze Shaders" in a capture reports the same for every shader the
  frame's draws and dispatches used, worst first, with per-shader cost and use counts.

- Shader editor: a line-number gutter, a find bar (Ctrl+F; Enter and Shift+Enter step through
  matches, Escape closes), and compile errors marked on their lines with the message as a
  tooltip; the compiler log's error lines jump to the line, and the editor goes to the first
  error when a compile fails. glslangValidator, dxc and spirv-as messages are understood.

- "At frame" field in the capture bar: capture a given frame of the application instead of the
  next one (0 is the first frame; the launch dialog's queued capture offered this already).
- Recent capture files in the Recent menu: files saved or opened are listed under the recent
  launches and reopen with a click.
- Multisampled depth read-back: depth attachments and images are resolved by a render pass
  (dynamic rendering with a depth resolve attachment, sample zero) into the temporary image
  before the copy, in captures and in the live image viewer. The layer enables dynamic
  rendering for the application at device creation when the driver offers it (core on 1.3,
  `VK_KHR_dynamic_rendering` and its dependencies on 1.1 and 1.2 applications).
- Captures in windows of their own: "Open in New Window" on a capture file's tab moves the file
  to a new window (and "Move to Main Window" brings it back), and on a capture tab of a live
  session opens a copy of the capture in a new window (through a temporary file removed when
  the application quits).
- Display refresh rate from the display instead of an estimate: the layer enables
  `VK_EXT_present_timing` (with its dependencies and feature) or `VK_GOOGLE_display_timing` on
  the device when the driver offers one, asks the swapchain for its refresh period, and on
  Windows otherwise reads the current mode of the monitor showing the application. The frame
  interval estimate remains the last resort. Dropped frames now count against the real refresh
  period (a 30 fps application on a 60 Hz display shows them). The meter says which it is
  ("60 Hz display" or "estimated", the tooltip names the source), and the Frame Bound card's
  note does too. `VK_EXT_present_timing` stays off when an enabled Khronos validation layer is
  older than the extension (it would not understand it); `VKINSP_NO_REFRESH_EXTENSIONS=1`
  leaves the device untouched.
- Source view in captures: a draw's or dispatch's shader sections in the command details show
  the embedded source (with the file bar for includes), the modeled Shader Cost and the
  Performance Analysis findings, whose line links open the source at that line, next to the
  reflection. The code is fetched when the section is opened, from the layer or the capture
  file.
- Shader Flame Graph ("Flame Graph" in the capture bar, next to Analyze Shaders): the frame's
  GPU work by pass, pipeline (or draw), shader stage and function, as a zoomable flame graph.
  Pass widths are the measured GPU durations when the capture profiled its passes, the split
  inside a pass comes from the static cost model times the invocation counts: vertex and
  compute counts from the draw and dispatch arguments (indirect ones from the captured
  argument buffers), fragment counts estimated from the scissor area (switchable). Frames are
  colored by the dominant kind of work (ALU, SFU, texture, memory); a draw frame selects the
  draw, a shader frame reveals the shader.
- Stack traces: the layer records the call stack of every object creation ("Stack traces" in
  the launch dialog, on by default) and, with "Stack traces" in the capture bar, of every
  command of the captured frame. Object details and command details have a Stack trace section
  that fetches the symbolized frames (DbgHelp on Windows: functions, files and lines from PDBs
  next to the modules; dladdr elsewhere), with the frames inside the Vulkan loader, layers and
  driver folded away. Capture files keep the symbolized stacks of their commands and objects.
  The triangle test application is built with debug information so its frames resolve.
- Validation messages on captured commands: a message raised while a command was being recorded
  is attached to that command. The capture's command list marks it (the severity glyph after
  the call number, the message as tooltip), and the command's details open with a Validation
  section whose entries jump to the message in the Inspect tab. Capture files keep the link.
  The launcher lifts the validation layer's duplicate-message limit (10 by default), since the
  inspector's layer counts repeats itself and the link needs the occurrence of the captured
  frame.
- Multisampled render targets and images are read back: the layer resolves them into a temporary
  single-sampled image before the copy, in captures (attachments and images bound by descriptor
  sets) and in the live image viewer. The capture labels them "4x MSAA". Dynamic rendering's
  resolve targets are captured as well, marked "(resolve)". Multisampled depth still fails with
  a note (`vkCmdResolveImage` resolves color only). The triangle test application has a
  `--msaa` option (4x, resolved into the swapchain).
- Refresh rate and dropped frames: with vsync on (a FIFO present mode) the layer estimates the
  display refresh period from the frame intervals and counts the refreshes that repeated the
  previous frame.
  The frame time meter shows the vsync period and the dropped frames, the session bar counts
  them, and the pass timeline's budget marker and the Frame Bound card use the refresh period,
  with "vsync bound" and "missing the refresh" verdicts. Without vsync the present mode is shown
  and the frame interval stays the budget.

### Changed
- Shader Reflection sections are collapsed by default.
- The GPU pass timeline follows the theme (light and dark) instead of fixed dark colors.

### Fixed
- Compile & Apply from the Source view failed with `'#version' : must occur first in shader`:
  the editor passed glslang's embedded prefix (`// OpModuleProcessed` comments and a `#line`
  directive) to the compiler. The editor now opens the source without it.
- Compile & Apply on a pipeline re-rendered the details panel and closed the editor; the edit
  result now updates the object list and the section label in place, and the editor stays open.
- The live image viewer crashed the application when the validation layer was enabled: the
  layer's read-back command buffer skipped the loader's dispatch-pointer setup that layers below
  rely on. The layer now sets it itself.
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
