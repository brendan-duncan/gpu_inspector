# GPU Inspector TODO

Feature parity with [WebGPU Inspector](https://github.com/brendan-duncan/webgpu_inspector), and
what Vulkan needs beyond it. Ordered roughly by value per effort within each section. Items marked
**(RenderDoc)** have reference implementations in RenderDoc (MIT) that can be adapted: its
replay-driven analysis is the Vulkan counterpart of what WebGPU Inspector does with a WGSL
interpreter and re-created pipelines.

## Done

- Live object inspection: object lists with counts, history, search and per-type filters,
  "used in last capture", meters (frame time with submit time, object count), memory totals.
- Texture viewer (mip, layer, channels, exposure, auto range, zoom, hover/pinned values, copy).
- Descriptor set contents on demand. Shader stage labels. A Reflection section on every shader
  payload (entry points, interface, resources by set and binding, push constants).
- Leak report at device / instance destruction: the objects still alive, by type and by name,
  in the Inspect tab, the session bar and the log.
- Device sections: the physical device's properties, limits, memory heaps and types, queue
  families, features and extensions; the device's enabled extensions, queues and features
  (across the pNext chain); the instance's application info, layers and extensions.
- Frame capture: N frames, queued capture at a frame or after a delay (launch dialog and CLI),
  one tab per capture, command filter, debug-group coloring, render target read-back, a
  thumbnail strip of every pass's attachments, and the image viewer (zoom, channels, exposure,
  texel values) on captured render targets.
- Command inspection: pipeline state, per-stage reflection, descriptor sets with parsed uniform
  and storage buffers (Format editor, radix, array paging) and the contents of bound images
  (read back once per view, budgeted), vertex/index/indirect data, push constants, render
  targets, and "Affected by" on every buffer (the frame's earlier writers of it).
- Frame Stats (API activity, passes, pipelines, bindings, memory, geometry).
- Profile passes: GPU timestamps per render pass and per run of dispatches (compute passes),
  pass durations in headers, pass timeline, Frame Bound card and pass timings in Frame Stats.
- Shader editing: edit as GLSL / HLSL / SPIR-V assembly, compile with the SDK, live replacement
  pipelines, syntax highlighting, line numbers, find, compile errors marked on their lines.
- Shader source maps: embedded source (OpSource / NonSemantic DebugInfo) as a Source view, SPIR-V
  disassembly annotated and linked to source lines, editing from the embedded source.
- Static shader analysis on SPIR-V: modeled Shader Cost per entry point and function, a
  Performance Analysis findings list per shader, and "Analyze Shaders" over a capture's frame.
- Frame Issues: frame-level rules over a capture (attachment loads and stores, clears outside
  passes, transient depth, MSAA stores, stereo without multiview, redundant binds, tiny draws)
  in Frame Stats, linked to their commands; the slow XR test package exercises them.
- Validation messages: the layer's debug-utils messenger forwards validation layer output (with
  repeat counts and object links); "Validation layer" in the launch dialog enables the Khronos
  layer; Inspect lists the messages, marks the objects, the session bar counts them.
- Capture files (`.gpucap`): save from the capture bar or tab menu, open from the launch bar or by
  drag and drop into a session of their own with the object graph, shaders, buffers, render
  targets and timings; "Open in New Tab" copies a capture in memory.
- Metal (macOS): the capture library in `src/metal/`, hooking the driver's classes without wrapping
  objects, with object tracking and lifetime, frame capture with render targets, buffers in
  every storage mode, pass timings and pipeline reflection (see `src/metal/README.md`).
- Direct3D 12 (Windows): the capture library in `src/d3d12/`, injected at process start by
  `dxinsp_launch.exe` and hooking the D3D12 and DXGI entry points and vtables without wrapping
  objects, speaking the Vulkan layer's protocol: object tracking with descriptors, names and
  descriptor heap contents, frame capture with synthesized passes, render targets, bound buffers
  and textures, root constants, `ExecuteIndirect` arguments, pass timings and counters, the
  debug layer's messages, stack traces, DXBC/DXIL reflection and disassembly (`dxinsp_shader.exe`),
  shader editing through `dxc`, and record-always for pre-recorded lists (see `src/d3d12/README.md`).
  `test/d3d12_triangle` is its test application.
- Render graph suggestions: rules over the graph (unread stores per subresource and per write,
  results overwritten before anything reads them, transient-attachment candidates, mergeable
  passes, barriers that synchronize nothing), in the Render Graph view and Frame Issues, shared
  by both APIs; they replace the per-command rules that answered the same questions by proxy.
- Render Graph: the capture's passes and the resources connecting them, from attachments,
  descriptor sets and transfers, versioned per write and keyed per subresource; a resource
  lifetime chart with a node-link view of the selected pass' neighborhood, GPU times, the
  critical path, external inputs and passes whose output nothing reads.
- Claude Code plugin (`claude-plugin/`): an MCP server (`src/app/src/mcp/`) over saved `.gpucap` files,
  built on the renderer's own analysis modules (split out of the UI for it).
  - Tools: summary, Frame Issues, GPU Bottlenecks, render graph, command list and bound state,
    objects, validation, read-back images as PNG, buffers through GLSL layouts, vertices with
    bounds, shader reflection, source, cross-compiled text and analysis, the Shader Flame Graph
    with the hottest functions and lines, and before/after comparison. Shader sources and stack
    symbols come from this machine's source and build trees (`set_search_paths`). A Metal draw's
    argument buffers list the resources their members hold.
  - A capture analysis skill, and analyze / profile / debug / compare commands.
  - Live sessions without the app: launch or attach, frame statistics, captures saved as
    `.gpucap`, and shader replacement for an edit, capture, compare loop, with a `live` command.
    Between captures: live images, descriptor sets, and objects with their creation stacks.
    Android applications are launched over adb (`launch_android_app`).
    The capture libraries end a capture's stream with `CaptureComplete`.

## Next

### Claude Code
- [x] CI: the release workflow runs `npm test` on every platform, and fails when the committed MCP
      bundle differs from a fresh build of its sources.

### Captures
- [x] Pipeline statistics queries per pass on Vulkan (`src/vulkan/src/pipeline_stats.h`), carrying the
      same counters Metal's statistic set does, so the GPU Bottlenecks report and its rules work
      for Vulkan captures too.
- [x] Depth rejection on Vulkan: the layer runs a precise occlusion query around each render pass
      (`src/vulkan/src/capture.cpp`), counting the samples that passed its depth and stencil tests
      (`fragmentsPassed`, Metal's name for the same figure), so `late-depth-rejection` and the
      report's depth rejection column work for Vulkan captures. A pass whose command buffer has an
      application query open is skipped, and the query ends early (dropping that pass's count) when
      one begins or a secondary command buffer runs inside the pass.
- [x] Multisampled stencil read-back: the resolve carries both aspects of a depth-stencil image,
      with the same resolve mode so a device without `independentResolve` can still do it. Untested:
      no test application has a multisampled stencil attachment.
- [x] Dynamic rendering passes suspended and resumed across command buffers: the layer records
      nothing between the parts (the resumed part reads back for both). They are **timed**, across
      their parts rather than per part, which is what a suspended instance is: one pass. The begin
      timestamp goes before the first part's `vkCmdBeginRendering` and the end after the last part's
      `vkCmdEndRendering`, both outside the instance where recording is allowed, and the pair is
      reserved and reset in the first part's command buffer -- submission order puts that before the
      part that writes the end (`DeviceCapture::suspendedQuery`, the query counterpart of the
      copies the same chain already carried). Counters are still not taken: a `vkCmdBeginQuery` has
      to be ended in the command buffer that began it, and the pass ends in another one.
      `test/triangle --suspend` reads 2 of 2 passes timed where it read 1 before, with the
      validation layer silent; the `suspend` UI case covers it.
      The replay's own per-pass instrumentation (overdraw, pixel history: `PrepareOverdraw` /
      `PrepareHistory` before each `vkCmdBeginRendering`) still treats each part as a pass, so those
      analyses of a split-pass capture inject commands between the parts; a plain replay is fine.
- [x] Read back the stencil aspect of a depth-stencil image: a render target's stencil is a texture
      of its own beside its depth (through the resolve when multisampled), the frame-start contents
      take a loaded stencil apart from the depth, and the replay uploads and compares it.
      `test/triangle --stencil` (with or without `--msaa`) exercises it. D3D12 reads its stencil
      plane back too (below); Metal still reads depth only (its own item).
- [ ] Multisampled read-back on Vulkan 1.0 devices: the depth/stencil resolve needs dynamic
      rendering (core 1.3, `VK_KHR_dynamic_rendering` on 1.2), so a 1.0 device would need a
      shader-based resolve of sample zero instead.
- [x] Descriptor buffers (`VK_EXT_descriptor_buffer`, `src/vulkan/src/descriptor_buffer.h`): the
      layer keeps every descriptor it saw `vkGetDescriptorEXT` make, resolves a bound descriptor
      buffer's device address to a buffer, reads the set's memory through the application's own
      mapping and decodes it into an ordinary descriptor set snapshot, so a draw bound that way
      shows its bindings and their contents like any other. `test/triangle --descriptor-buffer`.
- [ ] Descriptor buffers the layer cannot read: memory with no host mapping (a device-local
      descriptor buffer filled by a staging copy) and descriptors made before it attached. Both are
      reported as unread rather than guessed at. Reading the first needs the buffer copied back the
      way a captured buffer range is, and then decoded once the copy has landed rather than at
      record time; the second cannot be recovered at all, since the bytes say nothing about what
      they name.
- [ ] Sampled images bound through shader objects.

### Inspect
- [x] GPU-assisted validation messages attached to the commands they name
      (`src/vulkan/src/validation.cpp`). A GPU-assisted message is about a shader invocation, so it
      arrives when the submission finishes — after the capture is over and its recorders are gone —
      and the command is read out of the text: the command buffer it names, the entry point in its
      header (`vkCmdDispatch(): ...`) and which draw or dispatch of that buffer it was ("Compute
      Dispatch Index 1"), which tells two dispatches of the same name apart. The capture's command
      lists outlive it for this (`CaptureManager::CapturedCommandsFor`, a reference count on the
      recorder's frozen snapshot). The same mistake is reported once per invocation that made it, so
      the invocation id and the out-of-bounds offset are folded out of the dedupe key: 200 messages
      became 12 on `test/triangle --oob`, both dispatches marked. The `oob` case in
      `tools/ui_tests.py` covers it. Submit-time synchronization validation messages are linked as
      before (the command buffer handle and the command name in their text; a name that occurs
      several times links to its first occurrence, unless an index says which).
      Still true, and not fixable here: the capture's own read-back barriers can resolve a hazard in
      the captured frame (a barrier before the layer's buffer copy orders the draw after an earlier
      unsynchronized write), so a hazard seen every other frame may be missing from the captured
      one; turning the Buffers and Render targets options off avoids that.
- [x] Stack traces: a symbol path for PDBs that are not next to the modules on Windows
      (`VKINSP_SYMBOL_PATH` / `DXINSP_SYMBOL_PATH` from the launch's symbol directories, set with
      `SymSetSearchPath` in front of what DbgHelp works out for itself), and inlined callers for the
      DbgHelp path (`SymAddrIncludeInlineTrace` / `SymQueryInlineTrace` / `SymFromInlineContext`):
      the frame takes the innermost function and the rest become its `inlinedInto`, the shape the
      host symbolizer already produced for Android/Linux. Checked with the triangle's PDB moved out
      of its build directory: 0 frames with lines without the symbol directory, 6 with it, and the
      CRT's `invoke_main` shown inlined into `__scrt_common_main_seh`.
- [x] Refresh rate on X11 without a driver timing extension: `MonitorRefreshMs` in
      `refresh_rate.cpp` reads the mode of the CRTC the process's largest window is on
      (`XRRGetScreenResourcesCurrent`, dotClock over hTotal*vTotal), falling back to the first
      active CRTC when the swapchain is made before the window is mapped. No typed surface field
      was needed after all: the window is found the way the Windows path finds its own, by asking
      the server for the process's windows (`_NET_CLIENT_LIST` filtered by `_NET_WM_PID`) on a
      connection of the layer's own. Both libraries are dlopened, so the layer still links against
      no windowing library; the headers are a build-time option (`VKINSP_HAVE_XRANDR`,
      libxrandr-dev, which `tools/setup.sh` and the release workflow now install). Checked against
      `xrandr` with the two driver sources forced off: 74.98 Hz, source `monitor`, matching the
      `3840x1600 74.98*` the monitor is actually running.
- [ ] Refresh rate on Wayland-native applications, which the above does not cover (XWayland does).
      The rate is in a `wl_output` mode event, and reaching it needs either the application's
      `wl_display` — which only its surface holds, so this is where the typed surface field comes
      in — or a second connection of our own with `wl_display_connect`, a registry and a
      `wl_output` listener on a queue of our own. The second is the smaller change and matches
      what the X11 path does; the open question is which output to believe on a multi-monitor
      desktop, since without the surface there is nothing saying which one the window is on.
- [ ] OpenGL ES plugin on Linux (`src/plugins/gles/src/platform_linux.cpp`, `hooks_glx.cpp`): written
      on the Windows machine and only syntax-checked there (the NDK's clang with glibc's dlfcn
      extras shimmed in). Now built and partly run on Linux: the plugin and `test/gles_linux` build
      (they need `libegl1`/`libgles2`, plus `libegl-dev`/`libgles-dev` and `libsdl2-dev` for the
      test, which `tools/setup.sh` checks for), and launching `build/bin/gles_linux` from the app
      inspects it and captures frames on an NVIDIA driver, offscreen both with EGL linked and
      through dlopen. `test/gles_linux` now also opens a window through SDL, which on X11 is a GLX
      OpenGL ES profile context, so `hooks_glx.cpp` has something that exercises it at last.
      Still to check: capturing that windowed run; that a capture holds the whole scene (two
      passes, the ETC2 texture, four buffers); a Vulkan application and a desktop OpenGL one
      launched the same way are unaffected; drivers other than NVIDIA's. `--window-egl` forces SDL
      onto EGL for eglCreateWindowSurface, but NVIDIA's X11 EGL rejects SDL's window surface (a raw
      eglCreateWindowSurface on the same display works), so that one needs Wayland or another
      driver. Unknown until then: whether the forwarded dlopen keeps every caller's library search
      working (OpenAsCaller).

### Shaders
- [x] Shader flame graph: fragment stages are weighted by the fragment invocations a pass's GPU
      counters measured, split between its draws by scissor area, instead of the scissor-area
      upper bound. A pass without counters still uses the estimate.
- [x] Shader flame graph: per-draw GPU timing and per-draw fragment counts from the replay
      (**Measure draws**, `get_shader_flame_graph`). A pass's measured time is split between its
      draws by what the replay timed each at, and fragment stages take their measured counts.
- [x] Shader editor `#include` resolution from the source roots: `-I` per root for glslang and dxc,
      with the Google include-directive preamble where a GLSL source needs it (glslang counts the
      preamble separately, so error lines stay the source's).
- [x] Debug-info variable names (DebugGlobalVariable, DebugTypeComposite, DebugTypeMember) in
      buffer layouts, for modules without OpName / OpMemberName.

## Replay-based features **(RenderDoc)**

- [x] Replay-based analyses of a live Vulkan capture seemed to hang in the app (the `overdraw`,
      `mesh`, `overlay`, `debug-pixel` and `debug-vertex` UI cases). Nothing was wrong with the
      replay: the renderer was frozen within seconds of launching. NVIDIA's Vulkan driver makes a
      D3D12 device to present through DXGI, the D3D12 library launched alongside the layer tracked
      it and logged every call, and the session log redrew itself for each of ~1,400 lines a second.
      The library now leaves a D3D12 device made while the layer has a Vulkan device alone, and the
      log draws its lines in batches. Two things hid it: the launch's `VK_LOADER_LAYERS_ENABLE` ran
      the *installed* GPU Inspector's implicit layer instead of the build's (the launch now adds its
      layer to `VK_ADD_IMPLICIT_LAYER_PATH`), and an occluded test window stopped `ResizeObserver`,
      so the overdraw tab's image stayed at 10% and the scripted click missed it.

These need the capture to be re-executed. WebGPU Inspector does it on the DevTools GPU device with
re-created pipelines plus a CPU WGSL interpreter; for Vulkan the equivalent is RenderDoc's replay
of a serialized frame. Two routes: (a) an in-app replay engine that re-creates the captured
resources and re-executes the frame on the inspector's own Vulkan device (what RenderDoc does,
`renderdoc/driver/vulkan/vk_replay.cpp`), or (b) lean on the existing layer and re-run the live
application with injected state. Route (a) is the general one and is the prerequisite for the rest.

- [x] Replay engine (`src/replay/`, docs/REPLAY.md): `vkinsp_replay` re-creates a capture's objects on
      this machine's GPU through decoders generated from vk.xml, re-executes its command buffers,
      and compares every read-back render target with its own copy. The triangle, hazard and
      Unity captures replay pixel-identical.
- [x] Swapchain images of a recreated swapchain: a driver hands the new swapchain its
      predecessor's image handles, and the tracker kept them under the old swapchain, so destroying
      it took them and the views over them away. A recycled handle now moves to its new owner
      (`src/vulkan/src/tracker.cpp`). A Unity frame used to lose its final image in the capture and drop
      two passes in the replay; it now replays with 0 problems and every target identical.
- [x] Frame-start contents, RenderDoc's "initial contents" (`vk_initstate.cpp`), the way a layer
      that sees every command can take them: the first read of an image subresource the capture has
      not written whole (a loaded attachment, a copy, blit or copy-to-buffer source) copies it before
      the command runs (texture kind `initial`, the command's `imageData`), and buffer copy sources
      are read whole into `bufferData`, which covers staging buffers the host writes between
      submits. The replay uploads them, starts every subresource in its own layout, and compares
      multisampled targets through the capture's resolve. `test/triangle --persistent` replays
      identical (its 3 persistent targets differed before), and a Unity frame takes no copies.
- [ ] Frame-start contents, the rest (docs/REPLAY.md, "What is left"): stencil and multisampled
      contents, memory no command names (buffer device addresses, descriptor buffers), storage
      resources a shader reads before writing inside a pass, command buffers recorded in another
      order than they run.
- [x] Per-draw timing and counters inside secondary command buffers: the replay instruments them
      there too, so a Unity frame (every draw in a secondary) measures all of them. Its per-draw
      fragment counts add up to the layer's own per-pass counters exactly.
- [x] Depth rejection for a pass that executes secondary command buffers: an occlusion query cannot
      stay active across `vkCmdExecuteCommands`, so the layer leaves those passes unmeasured. The
      replay now runs a precise occlusion query around each draw, inside the secondary, and the
      pass metrics sum them where the capture's own counter is missing. A Unity frame's 11 draws
      are all measured.
- [x] Record live shader replacements in captures: a bind recorded during `replace_shader` names
      the replacement pipeline (or shader objects), the original beside it as `replaced`, so the
      draw's state, `get_shader`, the shader debugger and the replay all use the edited code.
- [x] Pixel history (`vkinsp_replay --pixel`, docs/REPLAY.md): every pass start, draw and clear
      that touched a pixel, with what each draw's fragments met (outside the scissor, culled,
      discarded, depth, stencil), measured with occlusion queries on pipeline copies, and the
      pixel's value and depth after each event.
- [x] Pixel history, the rest (`src/replay/src/history.cpp`):
      **writes outside render passes** — a clear, a copy from an image or a buffer, a blit and a
      resolve are recognized from their own arguments (which image, which region, which layout, and
      whether it covers the pixel at that mip and layer) and the pixel is read straight out of the
      image after the command; a dispatch or a trace writes through a descriptor, so it is reported
      by what was bound (a descriptor set of that bind point holding the image as a storage image),
      which the replay now tracks as it walks the binds. Verified on `test/triangle --persistent`
      (a copy from an image, a copy from a buffer at an offset, and a blit into mip 1, each with the
      value it wrote) and `--ray-tracing` (the trace into its storage image).
      **Multisampled targets** — the pixel is resolved into a one-pixel image of the replay's own
      and read from there, since a multisampled image cannot be copied to a buffer; a note says the
      values are what the samples resolve to. `--msaa` checked. A multisampled *depth* target still
      cannot be read, and says so.
      **Early fragment tests** — a fragment shader declaring `EarlyFragmentTests` has the depth and
      stencil tests on in the variant that measures the shader, the way the hardware runs it, so a
      shader that discards what depth would have killed is no longer reported as discarding; the
      event carries the flag, since it changes what the shaded count means.
      **Per-fragment values** — the primitive of the fragment that won the pixel, from a pass of the
      replay's own: the draw again with its fragment shader replaced by one writing `gl_PrimitiveID`
      into an R32_UINT target, over the copy of the pass's depth, with the one-pixel scissor (needs
      the geometryShader feature, which the replay now enables). Checked on the cube: the center
      pixel is primitive 5, and other pixels 2, 3 and 4.
      The `pixel-history` case in `tools/ui_tests.py` covers it through the app.
- [x] Pixel history, every fragment of a draw with its own value and primitive: the frame is
      replayed a second time (`_historyFragmentRound`, `measureFragments` in
      `src/replay/src/history.cpp`) and each draw that rasterized more than one fragment at the
      pixel is run once per fragment, with the stencil as a counter -- every fragment increments it
      and the comparison lets through the one that finds its own index there, which is RenderDoc's
      per-fragment pass (`vk_pixelhistory.cpp`). Each fragment is run twice, into images of the
      replay's own: with the draw's own fragment shader for what it computed, and with the
      primitive-id shader for where it came from. The first replay has to count the fragments before
      the second can ask them anything, so the round only runs when a draw had more than one, and
      the first 16 of a draw are measured. The UI lists them under the draw and marks the one that
      won (`renderer/pixel_history_view.ts`); `--no-cull` in `test/triangle` keeps the cube's back
      faces so one draw puts two fragments on a pixel, which the `pixel-fragments` UI case checks.

      **The primitive that won, corrected.** The primitive-id pass ran with depth writes off against
      the pass's shared depth copy, so with several fragments in one draw it reported the last one
      that passed against the depth the *draw* started from, not the one that actually won. It now
      tests against a copy of its own (`PendingHistory::idDepthCopy`, refreshed before each draw)
      with the draw's own depth writes, so the primitive it leaves is the winner. Seen on
      `--no-cull`: the draw reported primitive 7 while the pixel held primitive 5's color; both say
      5 now.
- [ ] Pixel history in a layered pass past its first layer. What it needs, none of which is written:
      the shadow copies of the attachments as arrays rather than single-layer images (so
      `CreateTransientImage` takes a layer count and the views become 2D_ARRAY), the framebuffer
      given those layers, the initial copy and `CopyHistoryPixel` addressing the followed layer
      rather than layer 0, and the primitive-id and fragment targets layered too -- a draw writing
      through `gl_Layer` or a multiview mask misses a single-layer target of ours. A multiview pass
      also needs its view mask kept, since `gl_ViewIndex` is what its shaders index by. Not done
      because nothing here renders to a layered pass: neither `test/triangle` nor any capture on
      this machine has one, and this is not code to write without a frame to check it against.
      The pass is still found and the note says the layer was not followed.
- [x] Pixel history in the app and the MCP server: the pixel clicked in a capture's render target
      tab, beside the image, and `get_pixel_history`. Both replay the capture with
      `vkinsp_replay --pixel-data`.
- [x] Replays kept alive (`vkinsp_replay --serve`, `ReplayServerPool`): one process per capture,
      its device and objects made once, each analysis replaying only the frame (tens of
      milliseconds on a Unity frame), for the app and the MCP server.
- [ ] A frame restored between served analyses: buffers the frame writes outside their captured
      ranges keep what the previous frame left.
- [x] Overdraw heatmap (`vkinsp_replay --overdraw`, docs/REPLAY.md): each pass is replayed with a
      counting fragment shader. It gives two counts per pass (every rasterized fragment, and the
      fragments passing depth and stencil in draw order), with a heatmap and a histogram. The
      triangle's count matches its pipeline statistics exactly.
- [x] Vulkan overdraw in the app and the MCP server: "Measure Overdraw" (a pass's details, or the
      Reports menu) replays the capture with `vkinsp_replay --overdraw-data` and shows the result the
      way a Metal capture's is shown; `get_overdraw` replays a Vulkan capture on first use.
- [x] A capture's render target in a tab of its own (`renderer/capture_texture_view.ts`), after
      WebGPU Inspector's capture texture viewer: the image with the pass's overdraw over it when
      **Overdraw** is ticked (legend, counts under the pointer, how much color covers it), and the
      history of the pixel clicked beside the image.
- [ ] Overdraw of fragments a shader discards (alpha-tested geometry counts as opaque), and of every
      view of a multiview pass.
- [x] Draw-call overlays (`vkinsp_replay --overlay`, `src/replay/src/overlay.cpp`): highlight draw,
      depth test and wireframe in the render target tab, for any draw of the pass.
- [x] Draw overlays: the stencil test apart from the depth one, back-face culling, and
      `get_draw_overlay` in the MCP server.
      **Stencil Test** re-issues the draw with its depth test off and its stencil test as it is
      (`ReissueMode::StencilOnly`), so a fragment the stencil rejected is no longer indistinguishable
      from one the depth killed; only where the pass's format has a stencil aspect.
      **Backface Cull** re-issues it with nothing culled and a fragment shader that writes for back
      faces alone (`kBackFaceFragmentSpirv`), and marks the pixels where *only* back faces landed —
      the draw's culling emptied them. The bit is cleared wherever the draw also drew, because every
      pixel of a closed mesh has a back face behind it and the raw bit would light the whole mesh up;
      what is left is the answer to "it ran, the geometry is there, and nothing appeared".
      Both are measured by the Vulkan replay (`src/replay/src/overlay.cpp`, five runs now) and by the
      D3D12 capture library (`src/d3d12/src/draw_overlay.cpp`, five runs), and both land in the same
      mask: bit 3 passed the stencil alone, bit 4 culled away.
      `test/triangle --inside-out` draws half the cube wound the other way, which culls away to
      nothing; the `overlay-backface` and `overlay-stencil` UI cases cover them. The stencil case
      holds that the run is made and reports against the stencil: the sample's own stencil test
      compares ALWAYS, so nothing there is rejected by it.
- [x] The attach list on Metal (`src/metal/src/transport.mm`), which the library's own TODO had
      spelled out and my gap analysis missed: `gpuinsp::PortIsServed` to step off a port another
      inspected application already has (only when `MTLINSP_PORT` did not name it), and the
      handshake that answers a `Probe` with a `Target` and drops it rather than taking the
      connection for a client. `target_probe.h` needed the socket headers added to its `__APPLE__`
      branch — its POSIX `PortIsServed` had never been compiled on a Mac.
      Verified by hand: two `mtlinsp_triangle` started at once land on 47532 and 47533 and each
      answers with its own pid and port.
      **Not done, and said where it lives:** answering a probe *while* a client is attached. The
      Vulkan library runs a client's session on a thread of its own and returns straight to accept;
      the Metal one runs it on the accept thread, so while somebody is attached nothing is accepted
      and a probe of that port times out — an attached Metal application is missing from the list
      rather than listed as busy. Moving the session to its own thread needs the `client` handle
      made safe first, since the present shape serializes it by construction, and that is a change
      to the one path every session depends on for a refinement discovery does not need.
- [x] The macOS half of the CPU sampler (`src/vulkan/src/cpu_sampler.h`, the `#elif` branch): the
      same public surface, the same behavior, through Mach — `task_threads` for the list,
      `thread_suspend` / `thread_resume`, `thread_get_state` with `ARM_THREAD_STATE64`,
      `thread_info` with `THREAD_BASIC_INFO` for the thread's own CPU time *and* its run state,
      pthread for the name and the stack bounds — unwinding by walking frame pointers, which the
      arm64 ABI guarantees are there. Wired into the Metal timing capture and the **Sample stacks**
      checkbox, which was Windows-only in the capture bar.
      The rule that matters was kept: between suspend and resume nothing is touched that could want
      a lock. The stack copy is clamped to the bounds pthread reports, which is the whole of what
      keeps the `memcpy` from faulting — macOS has no structured exception handling to catch one,
      where the Windows branch wraps its walk in a `__try`.
      Verified: a standalone probe with a deliberately 24-deep recursion walks 29 frames and
      symbolizes to a real chain (`std::__thread_proxy` -> `_pthread_start` -> `thread_start`), and
      tells the busy thread from the idle one. A five-minute run gave 181,438 samples over 8,739
      stacks with the sampler holding its 250 ticks a second start to finish, and about *one* of
      the six threads suspended per tick — the rest counted under the wait they were already in,
      which is the rule that keeps this from causing hitches. Cost, measured back to back on an
      idle machine for two minutes each: 60.0 frames a second with sampling on against 58.6 with
      it off, which is noise. Then left running for several minutes at a time watching for the
      intermittent deadlock that is this code's failure mode.
      Worth remembering about the measurement rather than the code: an earlier five-minute run
      showed the frame rate halved, and the cause was this machine building and testing at the same
      time. A profiler's cost has to be measured on an idle machine or it is not measured at all.
      Linux stays at the stub, and `docs/METAL.md`'s old bullet claiming no CPU sampling on macOS
      is gone — it was a platform gap rather than a Metal one, and now it is neither.
- [x] Ray queries in the MSL shader debugger (`src/app/src/renderer/msl/raytracing.ts`): the
      traversal on the CPU over the geometry the capture read back, and *into* the shader's own
      intersection function for a procedural geometry. Much less than the ~1200 lines this was
      sized at, because the capture already holds the scene — `accelerationScene` resolves a top
      level's instances to their bottom levels and reads the builds' vertices and boxes back, which
      is exactly a traversal's input. What it needed beside the arithmetic:
      the types (`ray` and `intersection_result` as real structs, the handles as opaque with
      methods dispatched by name, and `intersector<T>::result_type` teaching the parser to accept a
      member type after template arguments and the declaration detector to skip past it);
      the enumerations (`intersection_type::triangle` is 1 and `geometry_type::triangle` is 0, and
      the parser kept only the last component of a qualified name, so both read as `triangle` and
      one was wrong — those enums now keep their qualifier);
      and `intersect` as its own IR op rather than a builtin, because it has to *call* the shader's
      intersection function and the interpreter is a stepping machine. The trick is that `rayQuery`
      does not advance the program counter: the callee's `return` writes into the caller's
      destination register and leaves it on the same instruction, so the op runs again, reads the
      answer, and asks about the next box. The debugger steps into the function while that happens.
      The capture learned one thing: `ReadBackTableBuffers` reads back the buffers an intersection
      function table binds for its functions, since those are set once at setup and no command of
      any captured frame binds them — without them the function steps with zero arguments, which
      for the path tracer is every sphere at the origin with radius zero.
      Verified against the hardware: for thread (19, 32) of `--ray-tracing` the interpreter computes
      `(1, 0.2999999, 0.3312500, 2)` where the GPU wrote `(1.0, 0.30000001, 0.33125004, 2.0)`.
      `metal-debug-ray-triangle` and `metal-debug-ray-boxes` cover both kinds, and
      `msl_raytracing.test.js` pins the transforms, the barycentric convention and the candidate
      ordering.
      **Two crashes found on the way, neither in ray tracing.** A hex literal brought the whole
      debugger down: the suffix regex `[uUlLfFhH]*$` matched the `FF` of `0xFF`, leaving
      `BigInt("0x")`, so every shader with a ray mask or a bit field in it threw before running an
      instruction. And an enum name a replay's table does not have decoded as *zero*, which for
      most Metal enums means "invalid" (see the replay entry above).
- [x] Metal ray tracing replay and **Export to C++** (`src/metal/replay/src/mtl_raytracing.mm`):
      the structures re-created at the size the capture recorded, the builds, refits and copies
      replayed with the descriptor rebuilt from the capture's JSON, the intersection function tables
      made from their pipeline and filled by function name, and the bindings. Cheaper than either
      other backend for the reason the plan predicted: no `RemapAddress` analog, because a Metal
      geometry descriptor holds the `id<MTLBuffer>`, and `Replayer::TraceRays`'s whole binding-table
      rewrite collapses into a name lookup among the pipeline's linked functions. Those linked
      functions had to be carried onto the pipeline descriptor — without them the table exists and
      cannot be filled, and every ray misses.
      Also: a storage texture a compute pass wrote is read back after the frame and compared, which
      a frame whose work is all in compute needs to compare anything at all; and a kernel that reads
      the texture it writes is reported as unreproducible rather than differing, from the pipeline's
      own reflection saying the slot is `read_write`.
      Verified: `test/metal_triangle --ray-tracing` replays with all three targets identical, the
      traced 64x64 storage texture included, and its exported project builds and reports the same;
      `test/path_tracer/metal` replays every command with bounding box geometry and an intersection
      function table. `metal-accel-replay`, `metal-accel-scene-replay` and `metal-export-cpp-accel`
      cover them, through a new `Case.then` hook that runs `mtlinsp_replay` on a capture the case
      saved — the replay's own report is where a replay is verified.
      **The bug worth remembering** was not in the ray tracing code. The first frame replayed with
      every ray missing and nothing said so: `ParseEnum` resolves an enum by name, falls back to
      reading it as a number, and `strtoll` on a name that is not a number returns *zero*, which for
      most Metal enums is "invalid". The capture writes a geometry's vertex format under its
      `MTLVertexFormat` name and the replay looked it up in `MTLAttributeFormat`, so the build got
      invalid vertices and produced a structure holding nothing. It now returns the caller's
      fallback, which is also right for an enumerator from a newer SDK than the replay was built
      against.
      Not done: curve and motion geometry in the *export* (the replay builds them), and an opaque
      triangle intersection function, which is Metal's own and named by signature.
- [x] Shader editing on Metal (`src/metal/src/shader_edit.mm`): a pipeline's stage recompiled from
      edited Metal Shading Language and bound in the running application, which needed no protocol
      change — `ReplaceShader {pipeline, stage, spirv}` already carries whatever bytecode a backend
      wants, and Metal puts source there. The cheapest of the three: the other two compile in the
      UI and need the Vulkan SDK on this machine, while Metal sends the text and the application's
      own device compiles it, so the compiler's diagnostics come back and mark the editor's lines
      (its compiler is clang, so `parseCompileErrors` needed no new case).
      Three things needed care, and none of them was the compiler. A pipeline state cannot be
      copied, so `RememberPipelineDescriptor` — already kept for the measurements' pipeline copies
      — is what a rebuild works from, and the compute descriptor forms now fill it too. A compute
      pipeline built from a bare function has no descriptor at all, so the function is remembered
      instead and the rebuild makes one; `metal-shader-edit-compute` exists because a render
      pipeline never touches that path. And function constants have to be carried across, which has
      no counterpart in either other backend: `MTLFunctionConstantValues` has no getters, so the
      values object is retained at function creation, and without it a variant-heavy library would
      recompile into a *different variant* that compiles, draws and looks like a clean edit.
      Verified by pixels rather than by the reply, since a replacement that builds and is never
      bound reads exactly like a successful apply: `--debug-view=shader-edit` edits the first
      draw's fragment function to return magenta and checks the next capture's target went from 0%
      to the draw's own coverage, then restores and checks it went back; `:bad` checks the
      compiler's diagnostics come back with line and column; `:compute` edits the kernel to write a
      constant and finds it in the captured buffers.
      Not done: the other two backends' **Compile & Replay**, which needs a replay that serves
      analyses (below); tile and mesh pipelines, whose descriptors are classes this does not copy.
- [x] Draw overlays on Metal (`src/metal/src/draw_overlay.mm`), all five kinds: the third consumer
      of the recorded-calls engine `overdraw.h` already exposed, after overdraw and the pixel
      history, and measured inside the application the way D3D12's are — one draw per capture, named
      by its pass and its ordinal within it. Five runs into `R8Unorm` targets folded into the same
      one-byte-per-pixel mask the Vulkan replay writes, so the app and `get_draw_overlay` read all
      three backends identically. Fill mode and cull mode being encoder state in Metal is what makes
      it cheap: one pipeline copy serves all five runs.
      A multisampled pass is drawn at one sample per pixel — which is what a mask means — so
      Highlight Draw, Wireframe and Backface Cull work on the passes most real frames are made of;
      only Depth Test and Stencil Test are skipped there, since the runs that test start from a copy
      of the pass's depth and a multisampled depth attachment is not copied.
      `test/metal_triangle --inside-out` reverses the index buffer's winding and culls back faces,
      so the draw leaves no pixel at all: the one case where covered 0 is the right answer, and only
      the cull-off run can say where the draw went. `metal-overlay-{highlight,depth,stencil,backface,
      wireframe}` cover the five; the depth case holds that every fragment of `--occluded`'s second
      draw was rejected, not merely that something was.
      Fixed while doing it: a pass with nothing to test against reported every covered pixel as
      *rejected* rather than as passed, which is the opposite of what Vulkan and D3D12 say about the
      same pass, and the overlay's own `depthTested` was missing from the render target tab's debug
      state, so no test could see it.
- [x] **Viewport / Scissor** (`renderer/viewport_overlay.ts`), the one of these that needs no replay:
      the rectangles come from the draw's own state, so it draws on a capture of any API and on a
      saved one. The four spellings are read in one place (`drawViewports` / `drawScissors` in
      `renderer/draw_state.ts`: `VkViewport`, `D3D12_VIEWPORT`, `MTLViewport`, and the GLES
      backend's), which the command details view now reads too, so what it prints and what the
      overlay paints cannot drift apart -- and a Metal draw's viewport prints properly for the first
      time. A negative height (Vulkan's flipped Y) keeps the rectangle it covers and says it is
      flipped. `test/triangle --half-scissor`, the `overlay-viewport` UI case and
      `src/app/test/viewport_overlay.test.js` cover it.
- [ ] Draw overlays, the rest of the rest: the three that need real work in the replay.
  - **The fragments a shader discards.** The overlays draw the pass again with a constant fragment
    shader, which does not discard, so alpha-tested geometry covers its whole quad. Two routes, both
    in `OverdrawPipeline` (`src/replay/src/overdraw.cpp`): keep the application's own fragment shader
    and count with a **stencil increment** instead of a color write (no shader edit, but the overlay
    render pass then needs a color attachment per output the shader declares, and the stencil read
    back), or edit the SPIR-V to keep the discard and replace the outputs with one constant (the
    edit "overdraw of fragments a shader discards" needs as well). A shader with side effects
    (storage writes) is re-run either way, which is what to decide first. `test/triangle` has no
    discarding shader yet.
  - **Triangle size**: a geometry shader passing the primitive's screen area through to the
    fragment stage, which means generating one (and the `geometryShader` feature), and a value per
    pixel rather than a mask bit.
  - **Quad overdraw**: quad-granular atomics into a storage image, from a patched fragment shader.
      Not on this list any more: **NaN/INF** and **clipping**, which the image viewer's
      [Highlight](docs/REPORTS.md#what-the-picture-cannot-show) already marks on any render target,
      and marks by default.
- [x] Per-draw GPU timing and counters via replay with timestamp and pipeline-statistics queries
      (`src/replay/src/draw_stats.cpp`, `vkinsp_replay --draws`, docs/REPLAY.md): every draw and
      dispatch timed and counted, kept in capture files, read by the Shader Flame Graph.
- [x] Mesh output view (`renderer/mesh_view.ts`, `vkinsp_replay --mesh`, `src/replay/src/mesh.cpp`):
      VS In from the captured buffers and VS Out through transform feedback, as a wireframe and a
      table, with `get_mesh_output` in the MCP server.
- [ ] Mesh output, the rest: tessellation and geometry stage outputs, every view of a multiview
      pass, GPUs without transform feedback (RenderDoc's compute-shader conversion), and a solid
      shaded preview. A Metal draw's VS Out could come from the interpreter the shader debugger
      already runs it in (`renderer/metal/shader_debug.ts`, `interpretedMeshOutput`).
- [x] Shader debugger (`renderer/spirv/`, `renderer/shader_debugger_view.ts`, `debug_shader`): a
      SPIR-V interpreter for vertex, pixel and compute invocations, with variables, stepping and
      breakpoints.
- [x] The debugger behind an API-neutral seam (`renderer/debug/program.ts`): the stepping, the tab
      and `debug_shader` are written against DebugProgram and DebugInvocation, and each language
      supplies one (`renderer/spirv/program.ts`, `renderer/msl/program.ts`).
- [x] Metal shaders (`renderer/msl/`): the Metal Shading Language a capture holds, lexed, parsed,
      lowered to a linear form and interpreted, for all three stages. A fragment's varyings come
      from running the draw's own vertex shader rather than from a replay, since Metal has none.
- [ ] Shader debugger, the rest: tessellation and geometry stages (Metal: object, mesh and tile),
      per-sample shading, watch expressions, and editing a value and running on.
- [x] Shader cost by ablation (`renderer/vulkan/spirv_ablate.ts`, `src/replay/src/ablation.cpp`,
      **Measure shader**, `measure_shader_cost`): a draw replayed with SPIR-V variants that leave out
      a function, a line or a texture, sizing the flame graph's measured stages.
- [ ] Ablation, the rest:
  - Vertex stages, which decide what is rasterized: time them with the fragment stage off.
  - Parts inside control flow: measure a branch's arms by forcing the condition, rather than
    skipping everything a branch depends on.
  - More than one draw of a pipeline: several targets per request exist, but nothing sends them.
  - Metal: `mtlinsp_replay` replays a capture now, but serves no analyses, so there is still
    nothing to time the variants in.
  - Engine shaders with no line information: steps through GLSL decompiled by spirv-cross, the
    way the shader debugger does.

## Vulkan-specific
- [x] Implicit layer: **Set for my account** for the environment variables (the account's
      environment on Windows, `~/.config/environment.d` on Linux), and the Windows installer
      registers the layer and the uninstaller removes it (`src/app/installer/installer.nsh`).
- [x] Implicit layer, the rest: the .deb registers the layer in
      `/usr/share/vulkan/implicit_layer.d` and removes it again
      (`src/app/installer/deb-postinst.sh`, `deb-postrm.sh`, wired up as electron-builder's
      `afterInstall`/`afterRemove`). The shipped manifest names its library relatively, so the
      postinst rewrites `library_path` to the installed absolute path. Checked by extracting the
      built .deb, running the scripts against that root, and pointing `XDG_DATA_DIRS` at it:
      `vkinsp_triangle` loads the layer with `VKINSP_ENABLE=1` and not without it, which is the
      Windows behavior. The Windows installer script has now been run on a machine too.
- [ ] Remote targets over TCP (the transport is already socket-based; Android devices are
      reached through `adb forward` today, see ARCHITECTURE.md).
- [ ] Android: verify `test/android_triangle` (the phone NativeActivity, built by
      `tools/build_android_triangle.py`) on a phone: on a Quest it runs as a 2D panel that the
      shell keeps in the background, so it never gets a window; a GLES layer for Unity's GLES
      player; lower default read-back limits for phones.
- [x] Read-back after submission (command buffers recorded before the capture) splits the
      submission after each such buffer, so a later buffer of the same submission cannot
      overwrite what it rendered before it is read (`PreHook_vkQueueSubmit`, triangle `--prerecord`).
- [ ] Read-back after submission, the rest: two passes of one prerecorded buffer that write the
      same image still read back after both. Such buffers have no pass timings: the timestamps
      go in at record time, though a split could time each buffer as a whole. Submissions
      extended with structures other than timeline semaphore values are not split. Stencil store
      ops are left alone (no stencil read-back yet).
- [x] Frame Issues rules: `oversized-attachment` (larger than every render area drawn into it),
      `subpass-candidate` (a pass reading only the previous pass's output, its shaders checked for
      filtering), `redundant-transition` (a transition nothing uses before the next, or a barrier
      that changes nothing).
- [ ] OpenXR: the XR frame period (72/90/120 Hz) has no source without a swapchain, so the
      meter relies on the interval estimate; the runtime's display period would need an
      OpenXR layer or the runtime's own properties.
- [x] Multiple devices and queues in one process: query pools, staging and resolve images per
      device (`CaptureManager::DeviceCapture`), captures started by presents rather than another
      device's fence waits, and the replay running every device's objects on its one device
      (triangle `--second-device`, `--second-queue`).
- [ ] Several devices, the rest: devices on different GPUs replay on one (their formats and
      features may not all be there), and a second device's own frame counter can disagree with the
      presenting device's, so its submissions are numbered from the frame it joined at.
- [x] Graphics pipeline libraries and shader objects (`VK_EXT_shader_object`) in the shader editor:
      linked pipelines show and edit their libraries' stages (the libraries made again from their
      records), shader objects are replaced at `vkCmdBindShadersEXT` (a linked set made again
      unlinked), and the replay makes both (triangle `--pipeline-library`, `--shader-object`).
- [x] Pipelines linked from libraries in the replay's copies and the app's state views: a copy is
      made whole from the libraries' records (`Replayer::MergeLibraries`), and a linked pipeline's
      descriptor fills in what its libraries hold (`withLibraries`).
- [x] Shader objects in captures: a draw bound with `vkCmdBindShadersEXT` carries its shader
      objects and the dynamic state standing in for a pipeline's (`DrawState.shaders`,
      `DrawState.dynamic`), and reports that group by pipeline group these by a program key
      (`ShaderProgram`): the command details, Analyze Shaders, the Shader Flame Graph, the shader
      debugger, frame statistics and `get_command` all see them. The replay's VS Out issues such a
      draw in dynamic rendering with a feedback copy of its vertex shader object (triangle
      `--shader-object`).
- [ ] Shader objects, the rest: the replay's overdraw, draw-call overlays, pixel history and
      per-stage ablation (**Measure shader**, `measure_shader_cost`) copy pipelines, so draws with
      shader objects are left out of them.
- [x] Push descriptors with templates in descriptor snapshots (`DescriptorTracker::FromTemplate`),
      replayed as plain pushes from the snapshot (`Replayer::IssueCommand`, triangle `--push-template`).
- [x] Ray tracing pipelines: every stage's code (payloads named with their index in pStages), shader
      groups in the Inspect panel, the command details and `get_command`, a trace command's shader
      binding table regions, acceleration structures with what their last build held, and
      acceleration structures in descriptor snapshots (triangle `--ray-tracing`).
- [x] What a build was built from (`src/vulkan/src/resources.h` `ResolveAddress`/`StructureAt`,
      `renderer/acceleration_structure.ts`): the layer records buffer and structure device
      addresses, resolves a build's addresses to the buffers behind them and captures the
      contents, and captures keep every acceleration structure. Verified on an RTX 4080 with
      `vkinsp_triangle --ray-tracing`: the instance decodes to the identity transform, mask 0xFF
      and TRIANGLE_FACING_CULL_DISABLE the application wrote, and its reference resolves to the
      bottom level's object — the link from a top level to what is under it, which did not exist.
- [x] Which group each binding table record holds (`renderer/binding_table.ts`): the layer keeps
      the pipeline's group handles and reads the table back at the trace, and each record is
      matched to its group, with records whose handle matches none called out. Verified on an RTX
      4080: raygen, miss and hit records resolve to groups 0, 1 and 2, none unresolved.
- [x] The replay makes ray tracing pipelines and acceleration structures, and replays builds
      (`Replayer::BuildAccelerationStructures`, `RemapAddress`): a build's addresses are turned
      into this process's through the buffer and offset the layer recorded for each, and scratch
      is the replay's own. Verified on an RTX 4080: six problems to one, no validation messages,
      and the triangle and Unity captures still replay identically.
- [x] The replay traces (`Replayer::TraceRays`): the shader binding table is rebuilt in the
      replay's own buffer with every record's handle replaced by this driver's handle for the
      group the captured handle named, matched through the handle blob the layer keeps on the
      pipeline. Verified on an RTX 4080: the ray tracing capture replays with no problems and no
      validation messages, and the triangle and Unity captures are unaffected.
- [x] The replay compares what a frame computed into an image, not only what it drew into a
      target (`Replayer::InjectStorageReadbacks`). It found a real gap at once: on the ray
      tracing capture exactly 12.5% of the traced image differs, the triangle's share of it,
      because the bottom level is unbuilt in the replay. Checked for false positives on the
      triangle and Unity captures, which have no storage read-backs and are unchanged.
- [ ] Ray tracing, the rest:
  - [x] **A replayed trace found no geometry** — solved, and the fault was not in the replay.
    `test/triangle --ray-tracing` never put a barrier between the top level's build and the
    trace that reads it. On this driver the race resolved in the application's favor often
    enough that the frame looked right, so nothing showed it until the replay ran the same
    commands with different timing and every ray missed. With the barrier the traced image is
    identical to the capture's. Synchronization validation does not report this hazard: with it
    on, the application names only a pre-existing depth-attachment transition and says nothing
    about the acceleration structure or the trace. Three real defects were fixed along the way
    (the build's geometry contents were never uploaded, instance references were left as the
    captured addresses, and the primitive count driving the rewrite was read after the loop that
    needed it), but none of them was the one that mattered.
  - [x] A replay's scratch used to start at offset 0 for every build and free the buffer when it
    grew, so a frame of several builds replayed them into each other and, once the buffer grew,
    into memory the driver had taken back. It is now handed out a stretch at a time within a
    submission, and an outgrown buffer is retired rather than freed (`Replayer::ReserveScratch`).
  - [x] A bottom level built before the capture is built by the replay from what the layer read
    back when the capture began (`Replayer::BuildEarlierStructures`). A structure the application
    built on the host, or whose input buffers it has rewritten since, still is not.
  - `vkCmdTraceRaysIndirect*`, the NV ray tracing commands and the acceleration structure copies
    (`vkCmdCopyAccelerationStructure*`) are still left out.
  - Editing a ray tracing stage.
  - Ray queries in the shader debugger. **Metal is the cheapest place to build this first**: the
    MSL interpreter is hand-written here (`renderer/msl/`, and `parser.ts` already reserves
    `intersector` as a template name and does nothing with it), where Vulkan and D3D12 would need
    `OpRayQuery*` in a SPIR-V interpreter and `TraceRay` in a DXIL one. The scene is already in the
    capture — the geometry, the instances and the transforms are all read back
    (`src/metal/src/raytracing.h`) — so `intersect` is a CPU traversal over captured buffers,
    brute force being fine for the one thread being stepped. The piece with no precedent is calling
    the intersection function from inside the traversal, which `test/path_tracer/metal` *requires*:
    its geometry is entirely procedural, so a traversal that cannot call `sphereIntersection`
    reports every ray as a miss.

## What Nsight Graphics has **(Nsight)**

Measured against Nsight Graphics, with the Nsight Systems and Aftermath pieces used beside it.
Ordered by value per effort. Everything here is reachable through public APIs; what needs the
vendor's driver is listed at the end so nobody spends time on it.

- [x] Hardware unit counters per pass and per draw (Nsight's GPU Trace and Range Profiler: SM
      throughput, L2 hit rate, VRAM bandwidth, texture unit load): `vkinsp_replay --counters`
      (`src/replay/src/hw_counters.cpp`, docs/REPLAY.md "Hardware counters") reads the GPU's own
      counters around each render pass, and around each draw with `--counter-draws`, replaying the
      frame once per collection pass (a `pct_of_peak` metric needs dozens). Two backends:
      NVIDIA's Nsight Perf SDK (`src/replay/src/nvperf.cpp`, headers vendored in `third_party/nvperf`,
      per pass and per draw) and `VK_KHR_performance_query` (per draw). `get_hw_counters` in the MCP
      server, `--list-counters` for what a GPU offers. docs/PROFILING.md's "the limiters" are now
      reachable on Vulkan. Verified on an RTX 4080 against the triangle and a 34-pass Unity frame;
      collection needs GPU counter access enabled (`ERR_NVGPUCTRPERM`; NVIDIA Control Panel >
      Developer > Manage GPU Performance Counters).
- [x] Hardware counters in the app: **Measure hardware counters** in the GPU Bottlenecks report
      (`renderer/bottleneck_report.ts`) replays the capture and shows a column per counter beside
      each pass, kept with the capture and saved into its file.
- [x] The counters decide each pass's verdict (`PassMetrics.limiter`, `passLimiter` in
      `renderer/hw_counters.ts`): shader, bandwidth, cache or latency bound, naming the unit at its
      limit, shown in GPU Bottlenecks' Verdict column and its advice and reported by
      `get_bottlenecks`. On Vulkan it is the only per-pass verdict there has been, the inferred one
      needing Metal's stage spans. Checked against three real frames on an RTX 4080: a Unity frame
      at 800x600 (20 passes latency bound, 5 shader, 4 cache, 5 saturating nothing),
      `vkinsp_triangle --heavy --width 2560 --height 1440` (shader bound, SM at 90% of peak) and
      `vkinsp_triangle --msaa --width 3840 --height 2160` (cache bound, L2 at 48% against SM's 13%,
      a pass the stage verdict would have called fragment bound). The bandwidth verdict has only
      synthetic coverage: nothing here drives DRAM above L2.
- [ ] Hardware counters, the rest: a portable default counter set for the
      `VK_KHR_performance_query` path (it takes the first command-scoped counters now), which has
      only ever run as far as its precondition check, never against a driver that offers the
      extension; per-draw counters in the Shader Flame Graph beside the ablation costs; the
      thresholds the verdict uses (60% saturated, 30% busy, 30% occupancy) are starting points,
      judged against the three frames above.
- [x] Device-lost diagnostics on Vulkan (Nsight Aftermath's answer to "what was the GPU running when
      it hung"): `src/vulkan/src/device_lost.h`, breadcrumbs through `VK_AMD_buffer_marker` written
      before and after every draw and dispatch, read back when any call reports
      `VK_ERROR_DEVICE_LOST`. The session log names the command and says whether it also finished,
      which separates a hang inside it from a hang in what came next. **Device-lost breadcrumbs** in
      the launch dialog, `breadcrumbs` for `launch_app`, `VKINSP_BREADCRUMBS=1`; off by default (two
      GPU writes per action). Verified on an RTX 4080 with `VKINSP_SIMULATE_DEVICE_LOST=<n>[:hung]`,
      which reports a loss without hanging the GPU.
- [x] Device-removed diagnostics on D3D12 (`src/d3d12/src/device_removed.h`): Device Removed Extended
      Data turned on before the device is created, so a removal reports which operation each command
      list stopped on, what the removal code means, and for a page fault the faulting address with
      the objects allocated nearest it. On by default — the runtime keeps the breadcrumbs itself, so
      unlike the Vulkan side there is no per-draw cost; `DXINSP_NO_DRED=1` turns it off. Verified on
      an RTX 4080 with `DXINSP_SIMULATE_DEVICE_REMOVED=<n>`, with DRED on and off.
- [ ] Device-lost diagnostics, the rest: `VK_NV_device_diagnostic_checkpoints`, which reports every
      checkpoint still in flight rather than the last two markers; the faulting address through
      `VK_EXT_device_fault`; a dialog rather than only a log line; and a real hang, which has not
      been tried because it trips a TDR reset on the machine running it (`test/triangle --hang`
      would be the way). The D3D12 breadcrumbs in particular have only been reached by simulation,
      which by design cannot carry data: the runtime hands DRED over only after a real removal, so
      the code that names the stopped operation has never run against one.
- [x] Where a frame's CPU time went (`src/vulkan/src/cpu_timeline.h`, `renderer/cpu_timeline.ts`,
      **Where the CPU went** in Frame Stats): the layer times submit, present, fence waits, acquire
      and wait-idle during a capture, with the thread of each, and the capture carries them with a
      `VK_KHR_calibrated_timestamps` relation between the GPU and CPU clocks. The verdict separates
      waiting for the GPU from being paced by the display from paying for submission, which
      docs/PROFILING.md's first step could previously only infer. Checked on an RTX 4080: a vsynced
      triangle reads as display-paced (65% in present and acquire), an offscreen one as spending its
      time outside timed calls. The fence-wait and submission verdicts have tests but no real
      capture that reaches them — nothing here is GPU-bound or submission-bound enough.
- [x] The timeline as a drawing (**Timeline** in Frame Stats, `renderer/timeline_tracks.ts`): one
      lane per thread with each timed call as its own span, and a GPU lane with the passes, on the
      shared axis the calibration provides. The layer now sends `originTicks`, the device tick the
      pass starts are measured from, without which the passes cannot be placed on the host clock.
      The verdict names the longest gap between passes, attributes it to what the CPU lanes were
      doing across it (display-paced, waiting on submission, or the application's own untimed work),
      and reports the wait from a submission to the pass it queued. `get_capture_summary` carries
      it. Checked on an RTX 4080: a 1-frame triangle capture places its passes 2.69 ms after the
      submission (the swapchain image, not a stall) and reports its passes back to back; a 4-frame
      one finds the 1.99 ms interframe gap and correctly calls it display pacing rather than a
      stall. Two things the real capture caught: the tick values are past 2^53 and so arrive as
      JSON strings, and counting the axis either side of the GPU lane as idle reported a 96% idle
      GPU on a frame whose GPU was simply not the limit.
- [x] Frame Bound no longer names a bottleneck from numbers measured under different conditions
      (`renderer/capture_statistics.ts`, `CAPTURE_DISTORTION`): the GPU figure comes from a capture
      and the budget from the application running without one. Checked against the real Unity
      player, which runs at 1475 fps (0.68 ms a frame) and whose captured passes span 9.2 ms — 12x
      the whole frame — and was being called "GPU bound".
- [ ] What a capture costs, measured rather than only flagged: the captured frame's own wall-clock
      interval would let the card compare like with like instead of refusing, and would say how
      much of the captured frame was the library's own work (queries, read-backs). The Unity player
      is the target that shows it — 12x on a 7-pass frame.
- [x] The timeline zooms, pans and answers a click (`renderer/timeline_tracks.ts`,
      `renderFrameStats`): the lanes draw a view of the range rather than all of it, so a frame
      whose spans are sub-pixel at frame scale can be opened up. The model keeps every span — the
      old cap dropped the tail of a busy track, and with it those passes' idle gaps — and the view
      decides what it can draw, merging spans that would share a pixel into one box that says how
      many it stands for. Ctrl and the wheel zoom where the pointer is, dragging pans, the bar under
      the axis shows and moves the window, and the keys (arrows, `+`, `-`, `0`) do the same; a
      pass's span selects it in the command list, and a merged box zooms in on what it holds.
      Checked on a Unity frame and a live triangle capture.
- [ ] The timeline as a drawing, the rest: the CPU timeline live in the session bar rather than only
      in a capture; and per-queue GPU lanes rather than one, which needs the layer to report the
      queue each pass ran on.

- [x] Compiler statistics per pipeline (Nsight: register count, occupancy, spills per shader):
      `src/vulkan/src/shader_statistics.h`, through `VK_KHR_pipeline_executable_properties`. The
      layer adds the capture flag to every pipeline and asks the driver what it made of each stage;
      the Inspect tab shows it on the pipeline. **Compiler statistics** in the launch dialog,
      `shaderStatistics` for `launch_app`, `VKINSP_SHADER_STATISTICS=1`; off by default, since the
      driver has to keep the information. On an RTX 4080 the driver reports register count, binary
      size, stack, local and shared memory, and input/output counts per stage. The names are the
      driver's own, so they are passed through rather than mapped.
- [x] The register count explains the occupancy verdict (`limiterAdvice` in
      `renderer/hw_counters.ts`, `PassLimiter.registers`): a latency-bound pass names the stage whose
      registers are holding occupancy down, or rules register pressure out when the shader is light.
      `pass_metrics.ts` tracks the pipelines each pass's draws bind and reads the driver's register
      count off them. Checked on an RTX 4080: the heavy shader at 96x96 is 8.6% occupancy with 40
      registers (blamed), the plain one 9.5% with 16 (ruled out).
- [ ] Compiler statistics, the rest: the internal representations
      (`VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR`, the driver's own disassembly)
      beside the SPIR-V in the shader viewer; statistics for shader objects
      (`VK_EXT_shader_object`), which have no pipeline to query; and the register count in the flame
      graph's modeled cost.

- [x] Acceleration structure viewer (`renderer/acceleration_structure.ts`,
      `renderer/acceleration_scene.ts`, **Instances** on a top level): the instances with their
      transforms, masks, custom indices, hit group offsets and per-instance flags, each linked to
      the bottom level it names, and the scene drawn in the mesh preview — the geometry where the
      bottom level's build is in the capture, a box where it is not. Verified on an RTX 4080 with
      `vkinsp_triangle --ray-tracing`.
- [x] Mesh and acceleration structure views against Nsight Graphics' Geometry Viewer and Ray
      Tracing Inspector (docs.nvidia.com/nsight-graphics/UserGuide/graphics-capture-ui.html):
  - [x] **A bottom level built before the capture.** Each structure keeps the ranges its last build
        read, and the first submission of a capture reads them back (`ReadBackEarlierStructures` in
        `src/d3d12/src/raytracing.h` and `src/vulkan/src/capture.h`), posted on the structure as
        `captureInputs`. The views draw it with a note that it is what those buffers hold now, and
        both replays build it before the frame (`BuildEarlierStructures`). Verified on an RTX 4080
        with `dxinsp_triangle --ray-tracing`, `dxinsp_path_tracer` (4 structures) and
        `vkinsp_triangle --ray-tracing --static-blas`: every target identical, 0 problems.
  - [x] **Instance overlap** (`renderer/acceleration_tree.ts`, **Overlaps** in the structure tab):
        the instances' world boxes swept for overlaps, the pairs most overlapped first, and an
        **Overlap heat** coloring of the scene and of the instance boxes.
  - [x] **The structure tree** (**Tree** in the structure tab): top level, instances, bottom levels
        and geometries with primitives, world-space surface area and memory rolled up (the driver's
        size for the build, recorded by both capture libraries as `resultSize`), a checkbox per
        row, search by name, **Boxes** for the instance bounding boxes.
  - [x] **Mesh view parity** (`renderer/mesh_preview.ts`, `renderer/mesh_controls.ts`): Points,
        Wireframe + Solid and Smooth shading, flat and smooth shading from a normal attribute or the
        geometry's own, any attribute as the color, a Position picker on VS In, normals drawn as
        lines, hover and click picking that selects the vertex's row, Zoom to Selected (F), and
        camera bookmarks (Ctrl+1-9, 1-9).
  - Not planned: Nsight's traversal-cycles-per-ray and intersections-per-pixel heatmaps. They read
    what the RT cores did per ray, which only NVIDIA's driver can see; no API exposes it.
- [x] Memory per heap (`renderer/memory_heaps.ts`, **Memory Use** on the physical device): what the
      application allocated from each heap and type, its share of the heap, and the driver's own
      residency and budget through `VK_EXT_memory_budget` (added at device creation, sampled with
      the frame report, attached to the physical device as `memoryBudget`). Heaps near their limit
      are flagged, including ones another process is filling. Checked on an RTX 4080: the triangle
      holds 15 MB of a 16 GB device-local heap while the driver reports 156 MB resident, the
      difference being its own overhead and other processes.
- [x] Memory over time (`renderer/memory_timeline.ts`, **Over time** in Memory Use): both capture
      libraries keep a running total per heap and send a `MemorySample` with each frame report, and
      the app reads the series as growing, sawtoothing, flat or shrinking. The shape comes from how
      far the series moves each way, not its endpoints: a pool can stop anywhere in its cycle, so
      endpoints alone call a refill a leak. Verified on an RTX 4080 on both backends by agreeing
      the library's C++ running counter with the app's independent walk of the object graph
      (Vulkan 1.257 MB in 6 allocations, D3D12 1.625 MB in 7, both exact) — a flat series alone
      would look the same whether or not the counter worked.
- [ ] Memory, the rest: the growing, sawtooth and shrinking branches have only been read from
      synthetic series here, since the test applications allocate once and hold; fragmentation,
      which Vulkan does not expose and which would have to be approximated from the allocation size
      distribution; what each allocation is bound to (images and buffers already name their memory,
      so the reverse mapping is derivable); the series in a saved capture, which today holds one
      instant; and per-heap lines rather than one total.

- [x] Export to C++ (`src/replay/src/exporter.h`, `tools/vkgen/emit_source.py`, **Export to C++** in
      the capture bar, `vkinsp_replay --export`, `export_cpp`): a frame written as a standalone CMake
      project, mainly for driver bug reports. The source is emitted from the replay's own walk — the
      exporter watches it create, upload, record, submit and read back, and spells each step with
      emitters generated from vk.xml — so the program does what the replay did, and compares its
      render targets with the capture's. Verified by building and running the exported program with
      the validation layer on an RTX 4080: the triangle, `--hazard` and `--msaa` identical, a Unity
      frame's replayed passes identical, and an XR frame captured on an Adreno differing in exactly
      the texels the replay differs in. Worth keeping: `EmitPNext` as one function of several hundred
      cases had a stack frame of hundreds of kilobytes, and it recurses along the chain, so the first
      engine capture (a dozen structs on its device create info) overflowed the stack with no output
      at all; each case is now a small function of its own.
- [ ] Export to C++, the rest: ray tracing (builds and traces name what they read by device address
      and shader group handle, which the replay finds at run time; the source needs a spelling for
      `address of buffer N + offset` and a binding table built at start-up); a frame that crashes
      the driver, which is the bug report that most wants a repro and the one case the export cannot
      write, since it needs the replay to finish (a checkpoint before each pipeline creation and
      each submit would do).
- [x] D3D12 replay and Export to C++ (`src/d3d12/replay/`, `dxinsp_replay`, docs/REPLAY.md
      "Direct3D 12"): a capture re-executed and compared, and written as a CMake project. One
      `Reflect` per struct (`dx_reflect.h`) serves the decoder and the source emitter, since the
      D3D12 arguments are serialized by hand and there is no registry to generate from. Worth
      keeping: the first engine frame differed in every pass that sampled anything, and the fault
      was the capture's, which snapshot a descriptor table when it was bound while Unity writes
      the descriptors after the bind; tables are now snapshot at the next draw.
- [x] Metal replay and Export to C++ (`src/metal/replay/`, `mtlinsp_replay`, docs/REPLAY.md
      "Metal"): a capture re-executed and compared, and written as a CMake project of
      Objective-C++. The D3D12 shape, with one difference that decided the design: a Metal
      descriptor is an object, not a C struct, so a visitor cannot bind to its members. A property
      reaches the two visitors of `mtl_reflect.h` as the value it holds, the value a freshly
      allocated descriptor of the same class holds, and a block that sets it — which also gives
      the emitter, for free, the thing that makes the source readable: only the properties the
      application actually set, out of the dozens Metal defaults. Verified on an M1 Max by
      building and running what it wrote: the triangle, `--occluded` (depth) and
      `--present-direct` all replay and re-run with every target identical. Worth keeping: the
      first frame with a depth attachment differed in **every texel of its color target**, and
      the fault was the capture's — a depth attachment is announced under attachment index 0 like
      color attachment 0, and `CaptureTextureData` carried no aspect, so the depth read-back
      landed on the color entry. The Vulkan layer had always sent the aspect for exactly this
      reason. Nothing in the UI had shown it, because a depth image and a color image of the same
      pass both render as an image.
- [ ] Metal replay, the rest: acceleration structures and ray tracing (the capture side is done —
      `src/metal/src/raytracing.h` — so the replay has descriptors that name their buffers
      outright, with no `RemapAddress` analog to write and no binding-table handle substitution:
      `Replayer::TraceRays`'s whole rewrite collapses into re-filling an
      `MTLIntersectionFunctionTable` by function name. `CreateObject` in `mtl_replayer.mm` names
      acceleration structures as an explicit gap, and `OpenEncoder` in `mtl_commands.mm` bails out
      of the encoder with `LeftOut`), indirect command buffers,
      argument encoders, mesh shader draws, and the analyses `vkinsp_replay` serves (overdraw, pixel
      history and draw overlays are measured while capturing on Metal already, the mesh view's VS
      Out is interpreted, and a shader edit goes to the running application; per-draw timing,
      ablation and **Compile & Replay** are not). Ray tracing *is* replayed now
      (`mtl_raytracing.mm`), so what is left of that entry is the rest. Tile shading is recorded but is
      not on `MTLRenderCommandEncoder` in the macOS SDK. `test/path_tracer/metal` is the sample that needs the first of these.
- [ ] D3D12 replay, the rest: mesh shader pipelines from a stream, the analyses
      `vkinsp_replay` serves (overlays, mesh output, per-draw timing), descriptors indexed out of
      the heap (shader model 6.6), which no table snapshot covers, and a slot rewritten within one
      submission, which needs descriptors staged per draw rather than written at record time.
- [x] In-app HUD and live pause, both on all three backends. The HUD draws the application's frame
      time over its own window (`src/vulkan/src/hud_text.h` holds the font and the layout, with no
      graphics API in it, so the three libraries only differ in how they put flat rectangles on the
      screen). Live pause (`src/vulkan/src/frame_pause.h`) holds the application at its frame
      boundary, after the present, so the frozen frame is the complete one; a step lets exactly one
      more frame through. Two things worth keeping: pausing grants one step on purpose, because most
      of a frame's wall time is inside the present call and a pause request otherwise arrives after
      the HUD has drawn the frame in flight, freezing on one with no PAUSED badge; and a frame
      interval measured across a pause is the pause's length, which read as a 4985 ms frame until
      the pause counted its own generations. Checked on an RTX 4080: the triangle's HUD reads
      6.95 ms / 143.9 FPS against a 144 Hz display, pausing freezes it byte-for-byte across seconds,
      a step advances exactly one frame number, and the synchronization validation layer reports no
      hazard the application did not already have. The D3D12 half is checked the same way with the
      debug layer on; **the Metal half is written but not compiled or run** — it needs a Mac.
      The HUD also arms a capture hotkey (`src/vulkan/src/hud_hotkey.h`): F11 in the application's
      own window sends the same `AppCaptureRequest` `gpu_inspector_capture` does, so the capture is
      the one the Capture button takes — which is the only way to capture a frame you have to be in
      the application to reach, since clicking away from it changes the frame. Windows polls
      `GetAsyncKeyState` and tests the foreground window's process, Linux polls `XQueryKeymap` and
      asks the window manager whose window is active, macOS uses an `NSEvent` local monitor (no
      accessibility permission, and focus for free). `--debug-hud` and `tools/press_key.py` make it
      testable: `hotkey-capture` and `d3d12-hotkey-capture` in `tools/ui_tests.py` press the key in
      the sample's window for real. **The Metal and Linux halves are written but not run.**
- [ ] The paused frame scrubbed in the target's own window, which is the half of Nsight's live pause
      still missing: while paused, re-issue the frame's commands up to draw N and present that, so
      the application's window shows the frame building up. The blocker is that the layer keeps a
      frame's commands only as serialized JSON (`command_recorder.h`: `std::string args`), which
      cannot be re-issued; the replay tool's decoder can, but it is 63k generated lines bound to its
      own object map and a separate process, and it could not present to the application's swapchain
      anyway. What this wants is a native record: a generator pass beside `tools/gen_vulkan.py`
      emitting a tagged union of each `vkCmd*`'s arguments, deep-copied into an arena the same way
      `vk_serialize.gen.cpp` already walks them, plus a dispatcher that re-issues one against the
      live handles — which need no re-creation, since the application's objects are all still there.
      Re-submitting the application's own command buffers is not an alternative: truncating at a
      draw needs re-recording, and many engines record with `ONE_TIME_SUBMIT`.
- [x] Capture the frame you are looking at, rather than resuming. A paused application renders no
      frames to capture, so the pause is held open for the capture instead of lifted
      (`HoldForCapture` / `ReleaseCaptureHold` in `src/vulkan/src/frame_pause.h`): the frames the
      capture needs are let through -- one to arm it, one per captured frame -- and it is released
      at the frame boundary the last captured frame ends on, which each library reaches before
      that frame's own `Wait()`. So the application blocks again on the frame the capture holds
      and the window still shows it. Released in `CaptureManager::Finish` on Vulkan and D3D12,
      where the finish runs inside the present; on Metal where the last frame is counted, since
      its finish may be a completion handler a frame or two later. The hold carries a frame budget
      as well, so a capture that never runs cannot leave a paused application running for good,
      and a capture queued for a later frame still resumes rather than holding the application
      open until it gets there. `--debug-pause` and the `pause-capture` / `d3d12-pause-capture` UI
      cases cover it; the Metal third is written but needs a Mac to run.

Out of reach without the vendor's driver, so ablation stays the honest substitute and the docs
should say so:
- Instruction-level shader profiling (Nsight's Shader Profiler samples the program counter to
  rank SASS lines). No public API on any vendor exposes PC sampling.
- Warp-state and occupancy stall reasons, for the same reason.

Nsight Graphics has no shader debugger and no LLM-facing surface, both of which this project has.

## What PIX has **(PIX)**

Measured against PIX on Windows. What PIX has that is already done or listed elsewhere is left out:
the DXIL shader debugger, the acceleration structure viewer, export to C++, remote targets, pixel
history, the dependency view, DRED, and PIX's event markers (decoded in
`src/d3d12/src/hooks_command_list.cpp`). Ordered by value per effort.

- [x] Timing captures, the CPU half (**Timing Capture** in the capture bar, `renderer/frame_timing.ts`,
      `renderer/timing_view.ts`). Every frame's wall time and its CPU time per category, kept in a
      ring in the layer (about twenty minutes) and batched to the UI on the frame report's interval,
      with a frame-time graph, the distribution, and each hitch named with what caused it. A hitch is
      a frame over twice the median *and* at least 4 ms over it — the multiple alone calls ordinary
      jitter a hitch on an application running at 300 fps — and a category is only blamed when it
      accounts for half the frame's time over the median, so submission that costs the same in every
      frame is not blamed for the one that stalled. It is a mode rather than always on: recording
      needs a clock read in every timed call, and the layer's cost when idle is one relaxed atomic
      read. Vulkan only so far.
- [ ] Timing captures, the rest:
  - [x] **D3D12 and Metal**: both have the ring and the message now (v0.21.0 for Metal; the D3D12
    library answers `TimingCapture` in `src/d3d12/src/ui_messages.cpp`).
  - **The GPU half**: a pass's GPU time every frame, not only while capturing. Unlike the CPU side
    this is not free — it needs timestamp queries around every pass in every frame — so it wants to
    be its own option rather than part of the same switch.
  - [x] **Statistics over a selected range**: a range dragged out on the frame-time graph
    (`renderer/timing_view.ts`), which every figure and hitch below it is then of. Held by frame
    number rather than by position (`rangeIndices`), since both rings drop the oldest frames out of
    the front and a selection held by position would slide backwards through the run as it
    recorded; a range that ages out entirely goes back to the whole run. The graph keeps drawing the
    whole run with the rest veiled, and keeps its median and threshold, so the picture does not
    move as a range is dragged across it. `--debug-drag=x,y;x,y[;...]` was added to drive a drag
    from the command line, since a click cannot stand in for one.
  - [x] **Capture on hitch** (`renderer/capture_panel.ts`, `_captureOnHitch`): a checkbox on the
    capture bar; while a timing capture runs with it ticked, the first frame over the report's
    own threshold (`hitchThresholdMs` over the run's median so far, after 30 frames of warm-up)
    takes a frame capture with the bar's options, named *hitch N ms at frame F*, and unticks the
    box. UI-side only, so it works on every backend that streams `TimingFrames`. The samples'
    `--hitch-every N` stalls one frame in N; `capture-on-hitch` and `d3d12-capture-on-hitch` in
    `tools/ui_tests.py` (`--debug-capture-on-hitch`). What it captures is the frame *after* the
    hitch, which the docs say plainly; capturing the hitch itself would need the layer to record
    every frame in case (`VKINSP_RECORD_ALWAYS` is the half of that which exists).
- [x] Pipeline and shader creation on the CPU timeline, on all three backends, as a category of its
      own with its own verdict (`renderer/cpu_timeline.ts`, "Creating pipelines"). Metal's
      completion-handler forms are left untimed on purpose: they do not block.
      `test/triangle --compile-hitch`. Still open from this item: **D3D12 pipeline library hits and
      misses** as a count rather than only as time — a `Load*Pipeline` that misses returns fast and
      the application then compiles, so the two are already distinguishable by eye on the timeline,
      but a "N of M pipelines came from the library" figure would say it outright.
- [ ] A compile that a *capture* cannot see: an engine building its pipelines at load compiles
      nothing during a captured frame, so the category above is usually absent — which is correct,
      and also means the finding only lands when the hitch happens to fall inside the capture.
      Timing captures (below) are what make it reliable: a ring buffer over minutes would catch the
      compile wherever it happened.
- [ ] Which resources a shader actually used: PIX instruments shaders to report which entries of
      a bindless descriptor array a draw read. A draw's descriptor sets list everything bound,
      thousands of entries for a bindless heap. A replay with SPIR-V rewritten to record the
      indices it reads fits beside the ablation variants (`renderer/vulkan/spirv_ablate.ts`), and
      would limit sampled-image read-back to what was sampled.
- [x] App-triggered captures: a capture from a failed test, an assert or a debug key.
      `include/gpu_inspector.h`: `gpu_inspector_capture(frames)` and
      `gpu_inspector_capture_named(frames, label)`, answered by every capture library (the Vulkan
      layer, D3D12, Metal, and the D3D11 and OpenGL ES plugins on Windows, Linux and Android)
      through three exports found by the library's file name (`GpuInspectorConnected`,
      `GpuInspectorCapture`, `GpuInspectorCaptureNamed`). The library sends `AppCaptureRequest` to
      the inspector, which takes the capture with the bar's options; the label names the tab
      (*shadow test failed (Frame 212)*), the saved file and the file's manifest, so it survives a
      save. Every sample's `--capture-at N` exercises it, and `tools/ui_tests.py` runs it on all four
      Windows backends (`app-capture`, `d3d12-app-capture`, `d3d11-app-capture`,
      `gles-app-capture`). Metal and Linux are written but untested from here.
      An MCP session takes the application's capture too (`LiveSession._appCapture` in
      `src/app/src/mcp/live_session.ts`): capture_frames' defaults, saved under the label, opened in
      the store; `get_session_status` lists them under `appCaptures`, `list_captures` and
      `get_capture_summary` carry the label. A request during a capture is recorded as dropped,
      as the capture library ignores it. `src/app/test/live.test.js` covers it with the fake library.

      **Not by answering RenderDoc's API.** Unity, Unreal and the test harnesses that already call
      it find it with `GetModuleHandle("renderdoc.dll")`, so being found at all would mean shipping
      a library named `renderdoc.dll` — which shadows a real RenderDoc on the same machine and
      breaks it for anyone who has one installed. The same goes for PIX's
      `WinPixGpuCapturer.dll`. Interfering with another tool on someone's computer is not worth a
      convenience, so this is settled: no impersonation, whatever the API.

      What is left is the inspector's own in-app API — a small header, and entry points exported
      from the capture library, reached through the library's own name — which costs the
      application a few lines it has to add on purpose. That is the honest version and the one to
      build.
- [x] GPU-based validation from the launch dialog (**GPU validation**): Vulkan's GPU-assisted
      validation and D3D12's `SetEnableGPUBasedValidation`, which catch the out-of-bounds descriptor
      and buffer access no CPU-side check can see. `test/triangle --oob` writes past its storage
      buffer from the shader and is reported as `VUID-vkCmdDispatch-storageBuffers-06936`.
      Running a capture again under validation after the fact: the **Validate** report
      (`renderer/replay_validation.ts`, `_view.ts`; `showValidate` in `capture_panel.ts`) replays the
      file with `vkinsp_replay --validate --validate-data`, which now records the captured command
      each message fired on and the phase (setup / frame / submit: `ValidationRecord`,
      `ReplayReport::currentCommand`), so the rows open the command and the command list marks it.
      Synchronization validation is a second run with the layer's setting in the environment; the
      MCP's `get_validation` takes `replay: true` and `sync`. The `validate-report` UI case captures
      `--bad-scissor` *without* the layer and finds `VUID-vkCmdSetScissor-x-00595` at the
      vkCmdSetScissor command by replay. GPU-assisted validation is not offered on the replay yet
      (`VK_LAYER_VALIDATE_GPU_BASED`, the same environment path; the `--oob` case would check it).
- [ ] Replay on another device to tell a driver bug from an application bug (PIX replays on WARP):
      replay on lavapipe or SwiftShader and compare the render targets with the hardware result,
      which the replay's own comparison mostly does already.
- [x] Memory events beside the totals (D3D12): `Evict`, `MakeResident` and `EnqueueMakeResident`
      are hooked (`hooks_device.cpp`, `NoteResidency` in `cpu_timeline.cpp`), sized from what the
      library noted at creation, and go out two ways: as `evicted` / `madeResident` bytes on each
      `MemorySample` heap, which the Inspect memory series marks (red down, green up) with a
      Residency row, and as `kind: evict | resident` events in a memory capture, which the report
      marks on its graph, counts in a row and names in the verdict. The budget is not watched
      through `RegisterVideoMemoryBudgetChangeNotificationEvent` after all: the sample already
      asks the adapter for the budget every report, so a change between two samples is the
      notification (`budgetChanged` on the heap, a `budget` event in a capture, a dashed mark on
      both graphs). `test/d3d12_triangle --evict`; `d3d12-memory-residency` in `tools/ui_tests.py`.
      Not seen here: a real budget change, which needs another process to take GPU memory.
- [x] Dropped frames on D3D12, measured rather than estimated (`UpdatePresentStatistics`,
      `src/d3d12/src/device_info.cpp`): the swap chain's `DXGI_FRAME_STATISTICS` counts the
      refreshes that showed the previous frame again, where the library used to send a hard-coded
      zero. The Vulkan layer still works its count out from the frame interval and the refresh
      period, so the number says **(estimated)** there and not on D3D12 (`droppedMeasured`).
      `test/d3d12_triangle --stall <ms>` misses refreshes on purpose.
- [ ] Present statistics, the rest:
  - [x] Dropped frames on Vulkan, measured (`src/vulkan/src/present_timing.h`): every present the
    application did not time itself gets a `VkPresentTimingsInfoEXT` asking for the first-pixel-out
    stage (or the nearest the surface reports), the results are drained before each present, and
    two frames shown n refreshes apart are n-1 dropped. No present ids were needed after all:
    results come back in present order. A full results queue refuses the present, which is made
    again untagged. `test/triangle --stall <ms>`, and `dropped-frames` / `d3d12-dropped-frames` in
    `tools/ui_tests.py`. Checked against a 144 Hz display: a 41 ms frame counts five drops a frame.
    Not checked under validation: the SDK's validation layer here (1.4.304) predates the extension,
    so the layer keeps present timing off whenever it is enabled (the existing guard).
  - [x] Present latency (`presentLatencyMs` in FrameStats, the median over the report's frames, on
    the meter after the dropped count). D3D12: `SyncQPCTime` against the QPC taken before the
    `Present` call, matched by `GetLastPresentCount` (`device_info.cpp`). Vulkan: the first-pixel-out
    stage against the call, with the present-stage-local domain calibrated against QPC through
    `VkSwapchainCalibratedTimestampInfoEXT` every 120 presents (`present_timing.cpp`); NVIDIA here
    offers only that domain, and the calibration works. A composed window reads several
    refreshes (34 ms at 144 Hz for the sample), a stalled one two.
  - Metal's `presentedTime` / the drawable's presented handler.
  - **Presentation mode (composed or independent flip) is not reachable from DXGI at all.** PIX and
    PresentMon read it from ETW, which needs a trace session and administrator rights. Worth
    recording as out of scope rather than leaving on the list as if it were a small thing.
- [x] Texture viewer extras: NaN and infinity marking (**Highlight**, on by default, since an image
      that has them is already wrong and nobody goes looking), optional clipping marks, the NaN and
      infinity counts beside the format, and a per-channel histogram. Min and max are over the
      finite values only, which also fixes **Auto Range** blacking out an image holding one `+Inf`.
      Works on any read-back image, so on Metal and D3D12 captures too. The NaN/INF *draw overlay*
      above is separate and still needs the replay.
- [ ] App-reported counters (`PIXReportCounter`) plotted on the timeline, e.g. an engine's visible
      object count beside frame time.

Not worth the effort, since other tools already do them well:
- ETW-based CPU sampling, context switches, file I/O and CPU heap allocations: Windows-only,
  needs administrator rights, and Tracy and Superluminal cover it.
- DirectML / NPU captures and the GDK / Xbox-specific features.

## Metal

- [x] **Run the CPU timeline and the memory breakdown on a Mac** (2026-09-18), which the three
      items below had only ever been written for, on Windows, where `src/metal/` does not compile.
      All three work; each is now a case in `tools/ui_tests.py` rather than something to look at
      once, since what they check is invisible to a screenshot:
      1. **Pipeline creation timing** (`hooks_device.mm`): `mtlinsp_triangle --compile-hitch`
         compiles a library and a pipeline inside every frame — a fresh source each time, since
         Metal's compiler cache would answer an identical one instantly — and the frame stops for
         it under *Creating pipelines* (`metal-compile-hitch`). The other side of the guard is
         `metal-self-compile`: the same application compiling nothing, captured with **Overdraw**
         on so the library compiles counting pipelines of its own (`overdraw.mm`), and no
         *Creating pipelines* row appears. `reentry.outermost()` holds because those compiles are
         issued from inside a hook; a compile made off one — from the transport thread, say —
         would be timed as the application's, which is what that case is watching for.
      2. **Memory Use on an `MTLDevice`** draws (`metal-memory`), and the heap rows found a real
         bug: the breakdown read the heap's live usage from `updates.usage`, but an `ObjectUpdate`
         carries its fields flat beside the id, so `usedSize` was always the creation-time zero and
         every heap read as entirely empty. `test/metal_memory.test.js` had encoded the same wrong
         shape, which is why it passed. The sample now reserves a heap and takes two resources out
         of it, so the "In heaps" and "mostly empty" rows have something to report.
      3. **The CPU timeline itself**: `nextDrawable` shows as *Waiting for a swapchain image* and
         `commit` as *Submitting*, the clocks are related, and the GPU lane lands 0.5 ms after the
         commit that issued it rather than a frame away (`metal-cpu-timeline` checks
         `submitToFirstPassMs`, which is where a wrong calibration would show).
- [x] The CPU timeline and memory over time (`src/metal/src/cpu_timeline.h`), so **Where the CPU
      went**, the **Timeline** card and memory as a shape work on a Metal capture. Submit is
      `commit`, waiting for the GPU is `waitUntilCompleted`/`waitUntilScheduled`, waiting for the
      display is `nextDrawable`; there is no present span, because `presentDrawable:` does not
      block. `sampleTimestamps:gpuTimestamp:` gives the clock relation and the pass timings now
      carry `originTicks`.
- [x] Metal memory by what is holding it (`renderer/metal/metal_memory.ts`): there is no heap
      table to enumerate and no residency figure separate from `currentAllocatedSize`, so the
      breakdown totals each resource's `allocatedSize` by object kind — buffers, textures, heaps —
      instead. A resource made from a heap is kept out of the total (its bytes are the heap's), and
      a heap using much less than it reserved is called out. Computed in the renderer from the
      object graph the library already records, so there was no Metal code to write; the sizes it
      reads (`allocatedSize`, the heap's `usage` update) were already being sent.

The Metal capture library (`src/metal/`) reaches the Inspect and Capture panels through the same
protocol as the Vulkan layer. What it lacks falls into two groups: what the UI already does for
Vulkan and only needs the library to send, and what Xcode's Metal Debugger has that neither
backend does. Ordered by value per effort.

### Parity with the Vulkan side of the UI
- [x] Typed buffer views through reflection: every pipeline creation asks Metal for argument
      and buffer-type reflection, the pipeline's descriptor carries it per stage, and a draw's
      stage buffers and inline bytes render as named fields with the Format editor; pipeline
      objects get a Reflection section per stage.
- [x] `FrameStats` (frame time, min, max, submit time, refresh rate) from the frame boundary,
      so the frame-time meter and the Frame Bound card fill in; the refresh period from the
      display, or the layer's interval estimate, and none with display sync off.
- [x] Capture options: `atFrame`, `maxBufferSize`, `maxBufferTotal`, `maxTextureSize`,
      `captureTextures`, `captureBuffers`, `profilePasses`, `captureSampledTextures`,
      `maxSampledTextureTotal`.
- [x] Sampled texture read-back (`QueueTextureCapture` in `src/metal/src/capture.mm`): every texture a
      draw or dispatch bound, whole (all mips, all slices), read once per capture and blitted at
      the end of the pass the bind was in, carried on the binding command as `textureData`. It is
      what lets a debugged fragment sample what the GPU sampled.
- [x] The shader debugger on a Metal capture: the Metal Shading Language interpreter in
      `src/app/src/renderer/msl/`, with the sessions in `src/app/src/renderer/metal/shader_debug.ts`.
- [x] Validation messages: a command buffer's error with the encoder that faulted (encoder
      execution status is on while a client is connected), Metal's validation layer in its
      logging mode through an NSLog interpose (the launch dialog's "Validation layer" sets
      `MTL_DEBUG_LAYER` and shader validation), and shader logs, as `ValidationMessage` with
      repeat counts.
- [x] Leak report at process exit from the tracker.
- [x] Creation stack traces (`backtrace` + `dladdr`) for objects and captured commands,
      answering `RequestStacktraces` and `RequestSymbols`; the launch dialog's "Stack traces"
      and the capture bar's switch reach the library.
- [x] Argument buffers decoded: the reflection marks pointer, texture and sampler members,
      objects report their GPU address or resource id, and a draw's argument buffer lists each
      member resolved to its buffer (with offset), texture or sampler.

### What Xcode's frame capture has
- [x] "Xcode Trace" in the capture bar: `MTLCaptureManager` writing the next frame as a
      `.gputrace` from inside the process, so Xcode's shader debugger and per-line profiler
      open the same frame.
- [x] Per-pass GPU counters beyond timestamps: the statistic set (vertex, fragment and kernel
      invocations, clipper counts) and the stage-utilization set (cycles per stage), each in its
      own counter sample buffer on the pass descriptor, in the pass header's tooltip.
- [x] Vertex-versus-fragment split timing from all four stage-boundary samples of a render
      pass, in the pass header.
- [x] Insights as Frame Issues rules over Metal captures (`metal/frame_analysis.ts`): undefined
      loads, first-use loads, unread stores, multisample stores, memoryless candidates,
      mergeable back-to-back passes, redundant binds, tiny draws, single-threadgroup dispatches.
- [x] Memory viewer: `allocatedSize`, heap and purgeable state per resource, the device's
      `currentAllocatedSize`, into the memory totals meter.
- [x] Bottleneck analysis over the pass counters: overdraw, fragments per primitive, depth
      rejection and the bound stage per pass, as a report and as Frame Issues rules
      (`metal/pass_metrics.ts`, `metal/bottleneck_report.ts`, `docs/PROFILING.md`).
- [x] Overdraw measured while capturing (`src/metal/src/overdraw.mm`): every render pass drawn again
      right after the application's encoder ends, with counting copies of its pipelines, with and
      without its depth and stencil tests. Heatmaps in the pass's details, the measured figure in
      the pass header and GPU Bottlenecks, kept in capture files, `get_overdraw` in the MCP server.
- [x] Run Metal overdraw on a Mac: the heatmaps show in the pass details (2026-09-11).
- [x] Metal overdraw on a Unity player (2026-09-18): a frame of `fps_microgame` measures, every
      measurement carries its counts, and the figures are sane — one pass 2,116,655 fragments over
      518,400 pixels, max 7. `tools/ui_tests.py --unity <player.app>` keeps it (`unity-overdraw`).
      Running it found the report opening on the *first* measured pass, which in a real frame is a
      prepass that drew nothing into the target being viewed and whose heatmap is an empty image;
      it opens on the pass with the most overdraw now. The depth-tested and untested counts come
      out equal on that frame, which is correct and not the bug it looks like:
      `mtlinsp_triangle --occluded` draws the triangles twice, the second set behind the first, and
      there they are 183,366 against 707,881, max 1 against 6.
- [ ] Metal overdraw against the pass's `fragmentsPassed` counter. **Needs an Intel Mac or an AMD
      eGPU**: the statistic counter set that `fragmentsPassed` comes from is not supported on Apple
      Silicon, which the layer reports as `statistics 0` in its `pass timings:` log line, so there
      is nothing on this machine to compare the measurement against.
- [x] Pixel history for Metal, the same way (`src/metal/src/pixel_history.mm`): a pixel picked in a
      capture captures the next frame with every pass that renders to the texture drawn again one
      draw at a time at the pixel, into copies of its attachments, with a one-pixel scissor,
      visibility results in counting mode, and cull mode and depth-stencil state varied on the
      encoder. The same JSON as `vkinsp_replay --pixel-data`, the same tab, `get_pixel_history`.
- [x] Run Metal pixel history on a Mac (2026-09-12).
- [x] Metal pixel history follows a **multisampled** pass (2026-09-18), which was the first thing
      both the sample and a Unity frame hit — the sample's own triangle pass is multisampled, so
      `--debug-view=pixel-history` had never produced a single event on it. The shadows are made
      with the attachment's sample count, since the draws re-issued into them are the
      application's and a pipeline's sample count has to match its attachment; nothing can be
      blitted out of a multisampled texture, so the two attachments the pixel is read from resolve
      into a single-sample copy first (color the way the hardware would, depth sample 0). The
      sample now reports "wrote the pixel (12 samples passed)" — three instances over four
      samples. `metal-pixel-history` in `tools/ui_tests.py`.
- [x] Metal pixel history on a Unity player (2026-09-23): a real frame's draws are attributed now —
      18 events over two passes of one `fps_microgame` frame, four of them "wrote the pixel".

      The library was right all along. The three passes that reported only their start make **no
      recorded calls at all** — not a pipeline bind, not a viewport — so they are Unity's
      clear-only passes, one at the head of each command buffer, and "pass 0 begins (Clear)" was
      the honest and complete answer for them. What was wrong was the *pixel being followed*.

      Two things had to exist before that could be said rather than guessed:
      - **`MTLINSP_LOG_FILE`** (`swizzle.mm`), the Metal counterpart of `VKINSP_LOG_FILE` and
        `DXINSP_LOG_FILE`. A Unity player's stderr goes nowhere anyone can read, so `MTLINSP_LOG=1`
        alone produced no lines at all — the session's **Log** tab was empty for the whole run.
        `--debug-log=<file>` now sets it for a Metal launch too, beside the layer's and the D3D12
        library's.
      - **Log lines the question needs**: which texture each pass's attachments are and why one is
        declined, and per followed pass "1 encoder(s), 0 recorded call(s), 0 draw(s), 1 event(s)",
        which is the line that settled it.

      What was actually wrong: `--debug-view=pixel-history` fell back to "the center of the first
      color render target", and a real frame's first render target is one of those clear-only
      passes'. It picks the color target of the pass with the **most draws** now
      (`debugHistoryPixel` in `capture_panel.ts`) — and the two callers that had each copied the
      old rule, `showView` and `debugCaptureHistory`, share it, which is a second thing this found:
      they disagreed, so the tab opened on one texture while the library was asked to follow
      another.

      A history where no pass makes a draw says so rather than leaving a list of pass starts with
      nothing to explain it. `unity-pixel-history` now asserts a draw wrote the pixel.
- [ ] A pixel history of a **ping-ponged** render target, which is what `unity-pixel-history` runs
      into half the time: `fps_microgame` alternates its camera color target between two textures
      of its own, so the frame captured to follow the pixel renders to the one the *earlier*
      capture showed only every other frame. The other frames report "No render pass of the capture
      rendered to the texture at that level and slice", which is true and useless. The case accepts
      both outcomes for now and fails on anything else, so it still catches a regression in the
      path — but only when the parity is right.

      Ruled out on the way: those two textures are not drawables the library failed to register.
      Logging the texture of every drawable presented says the pool is three textures, all three in
      `g_drawableOfTexture`, and neither of the ping-ponged pair is among them — so `anyDrawable`,
      which exists for exactly this shape of problem, does not apply and should not be stretched
      to.

      Two candidate fixes, neither obviously right:
      - **Anchor on the pass, not the texture.** A draw overlay already names its draw by pass and
        ordinal rather than by command index, because the next frame numbers its commands afresh
        (`DrawOverlayRequest`). The pixel history could resolve its texture the same way: take the
        color attachment of the pass the request names, then follow *that* texture through the rest
        of the frame, so all the passes writing it are still reported. The weakness is the same one
        the overlay has — `passIndex` is per command buffer, so "pass 3" names one pass per command
        buffer, not one per frame.
      - **Follow two frames instead of one.** Covers any ping-pong of any period 2, costs the
        application a second measured frame, and needs the tab to say which frame each event is
        from (the events already carry one).
- [x] Metal pixel history, the rest (2026-09-23): **layered** passes, indirect command buffers'
      draws, and writes from outside a render pass.
      - **Layered passes.** The shadows take the pass's `renderTargetArrayLength` as well as its
        sample count, so a draw that picks a layer with `render_target_array_index` lands in the
        same one, and the pixel is read from the layer the request named rather than from slice 0.
        `mtlinsp_triangle --layered` exercises it: one pass, a two-layer array target, a draw per
        layer in different colors, layer 1 drawn *first*. Following layer 0's center reports both
        draws — a visibility result counts either's samples whichever layer they went to — and the
        pixel ends up green. Red would mean the shadows were not arrays and both draws landed in
        the same layer, which is why `metal-pixel-layered` checks the value and not just the
        events; the tab's debug state carries the last event's value for it.
      - **Indirect command buffers.** `executeCommandsInBuffer:withRange:` is executed one command
        at a time, each in an encoder of its own under a visibility result. Each command carries
        its own pipeline, which the library never saw created and so cannot copy, so only the last
        of the six counts is measurable: the event says whether the command wrote the pixel, not
        where its fragments stopped. That needed a `DrawOutcome` of its own — with only the final
        count measured, zero means "wrote nothing at the pixel", not "failed the depth and stencil
        tests", which is what the existing reading of that count would have claimed. The form whose
        range comes from a buffer the CPU cannot read is still skipped, with a note.
        `mtlinsp_triangle --indirect`, `metal-pixel-indirect`.
      - **Writes from outside a render pass.** A blit command per command, read back on the
        application's own blit encoder right behind the write (a blit encoder cannot be interrupted
        by one of the library's, and a texture-to-buffer copy is all this needs); a compute encoder
        per *encoder*, read once the application closes it, because a compute encoder cannot be
        interrupted to read a texture at all; and a pass's multisample **resolve** into the followed
        texture, which writes it at the pass's store and so matches no attachment —
        `MatchPixelHistoryResolve` finds those. The renderer already had the kinds and the
        "Outside a render pass" label. `mtlinsp_triangle --texture-writes` writes the resolve
        target all three ways in one frame and then draws over it twice, and
        `metal-pixel-writes` asserts the history holds the resolve, the dispatch, the copy and the
        draws, in that order.

      Not covered, and honest about it: a texture written through an argument buffer or made
      resident with `useResource:` rather than bound with `setTexture:` is not noticed on a compute
      encoder, and a layered *multisampled* pass's resolve is written but untested — nothing here
      has one.
- [x] Ray tracing (`src/metal/src/raytracing.h`, `renderer/metal/raytracing.ts`), the third
      backend's. The smallest of the three, because Metal names things with objects where the other
      two name them with numbers: an `MTLAccelerationStructure` is a real object with a `dealloc`
      the tracker already watches (no `StructureRegistry` to mint one per address), a geometry
      descriptor holds `id<MTLBuffer>` and an offset (no `AddressMap` to resolve), and a top level
      names its bottom levels by *index* into `instancedAccelerationStructures` (no address map to
      turn a reference into an object). The structure even carries its own `size`, which is the
      `resultSize` the other two ask the driver for.

      What is *not* shared, and where copying `d3d12/raytracing.ts` would have been wrong: its
      "nothing to translate" note about instance descriptors does not carry over. A
      `D3D12_RAYTRACING_INSTANCE_DESC` is byte for byte a `VkAccelerationStructureInstanceKHR`; a
      Metal one is not, in four ways — an `MTLPackedFloat4x3` transform (the transpose of the
      row-major 3x4 the views use), four unpacked `uint32`s instead of two 24/8 words, an index or
      an `MTLResourceID` instead of an address, and five layouts instead of one. So
      `parseMetalInstances` is a parser of its own; only the four option bits line up, under
      Metal's names. Every layout is pinned in `test/metal_raytracing.test.js`, because a wrong
      stride reads an instance out of the middle of its neighbor and still produces plausible
      numbers.

      Verified on an M1 Max: `mtlinsp_triangle --ray-tracing` (two instances of one triangle
      bottom level, at non-identity transforms — the scene's center is only right if the transform
      was transposed), `--static-blas` (the bottom level built once at start-up, so its geometry
      can only come from the capture-start read-back of a *private* buffer), and
      `mtlinsp_path_tracer --rebuild` (three bounding-box bottom levels, three instances, the
      intersection function table, and two timed build passes). `metal-accel-triangle`,
      `metal-accel-static-blas` and `metal-accel-scene` in `tools/ui_tests.py`.

      Worth keeping: two defects found by testing rather than by reading. `UpdateObject` spreads
      its argument's *fields* into the message and uses the key only to decide which update a later
      one replaces — so a `build` update has to nest itself under `"build"` explicitly, and until it
      did, the structure tree showed no memory. And a build was only recorded while a capture was
      recording, which is exactly backwards: an engine builds its bottom levels at load, so the one
      case the feature exists for recorded nothing. The Vulkan layer had always recorded builds
      unconditionally, for the same reason.
- [x] Metal build cost: an acceleration structure encoder is timed like any other pass. It had no
      timing slot at all — `CreateOtherEncoder` passed a default-constructed `PassTimingSlot()` —
      and the form almost every application uses (`accelerationStructureCommandEncoder`, no
      descriptor) has nowhere to attach a sample buffer, so the call is re-issued as
      `accelerationStructureCommandEncoderWithDescriptor:` with a descriptor of the library's own,
      the way `CreateBlitEncoder` already did. `MTLAccelerationStructurePassDescriptor` is macOS
      13; below that the encoder-boundary path (gated on `MTLCounterSamplingPointAtBlitBoundary`,
      there being no acceleration-structure sampling point) is the only one. No protocol change: an
      acceleration structure pass is timed under the render key, as a blit pass already is.
- [ ] Metal ray tracing, the rest:
  - Curve geometry (`MTLAccelerationStructureCurveGeometryDescriptor`, macOS 15) is recorded but
    not drawn — neither Vulkan nor D3D12 has it in core, so there is no shared path to reuse and
    the mesh preview would need a curve tessellation of its own.
  - An indirect instance descriptor's `MTLResourceID` is shown rather than resolved. The driver
    hands out small ids that collide across objects (every structure in the path tracer reports
    `0x1`), so a lookup would answer confidently and wrongly; resolving it needs something the
    capture does not have.
  - Ray queries in the shader debugger, and the replay of builds and traces: both listed in their
    own sections above.
- [x] The mesh view's **VS Out** on a Metal capture (`mesh_view.ts`, `interpretedVertexOutputs` in
      `shader_debug_setup.ts`). This item had been written as needing a replay, and it did not: the
      MSL interpreter already ran a draw's vertex function to rasterize a pixel's inputs for the
      shader debugger (`metal/shader_debug.ts`, `interpretedMeshOutput`), and the mesh view simply
      never called it. About eighty lines, most of it the branch and the note about the 20,000
      vertex cut-off. Verified on `mtlinsp_triangle`: nine vertices of the three instances, stride
      28, three primitives, nothing behind or degenerate (`metal-mesh-out`).
- [x] Stencil read-back (`StencilReadbackDetails`, `PassAspect` in `src/metal/src/capture.h`). Two
      read-backs of the one texture, because `MTLBlitOptionDepthFromDepthStencil` and
      `MTLBlitOptionStencilFromDepthStencil` may not both be set on one copy. Two defects found by
      testing rather than reading: `ForceStore` covered color and depth but not stencil, so the
      read-back returned whatever the tile memory held — uniformly 0xFF, which looks like data — and
      the render target tab picked a target by image id alone, so the two aspects of one image were
      indistinguishable (which affects Vulkan's `--stencil` equally, and is now fixed for all
      three). `test/metal_triangle --stencil` and `metal-stencil` in `tools/ui_tests.py`; the
      stencil reads 0 and 1 over 14.9% of the frame, the inverse of the depth image.
- [x] The rest of Metal's pixel formats. The TODO and the docs both said ASTC, ETC and PVRTC were
      unsupported; 135 of Metal's 152 formats were already mapped, those among them. What was
      actually missing: `RG8Unorm_sRGB`, the three-component formats macOS 27 added, and the
      stencil-only views. PVRTC is mapped but has no decoder in the UI, and is iOS-only.
- [ ] Per-draw counter sampling (`MTLCounterSamplingPointAtDrawBoundary`, already probed in
      `capture.mm`) so the microtriangle and overdraw findings can name the draws inside a pass
      rather than the pass, the way Xcode's GPU Commands tab sorts by fragments per primitive.
      **Needs an Intel Mac or an AMD eGPU**: Apple Silicon supports stage-boundary sampling only,
      which the layer reports as `draw 0, dispatch 0, blit 0` in its `pass timings:` log line, so
      none of it can be exercised here. When it is written, it should fill `DrawStat`
      (`renderer/draw_stats.ts`) — the shape `vkinsp_replay --draws` already produces and the
      Shader Flame Graph and `frame_cost_tree.ts` already consume — and arrive as a
      `CaptureDrawStats` message the way `CapturePixelHistory` does, rather than a shape of its
      own. `capture_panel.ts` refuses per-draw measurements on Metal today ("Metal captures do not
      replay yet"), which is what would lift.
- [x] Pass dependency graph from the recorded attachments and bound textures: already built, and
      it works on a Metal capture — `frame_graph.ts` picks `MetalResourceSource`
      (`renderer/metal/frame_resources.ts`) and a capture of the sample makes 3 nodes, 7
      resources, 1 edge.
- [ ] The "Affected by" section the Vulkan side shows on buffers, on a Metal capture. Its table
      (`BUFFER_WRITE_METHODS`, `capture_command_info.ts`) holds `vkCmd*` selectors only and its
      walk reads `c.descriptors`, the Vulkan shape, so the section finds nothing on Metal. Not
      derivable from the graph above, which is per pass where this lists individual commands, but
      the tables it needs are written: `BLITS` in `metal/frame_resources.ts` maps each blit
      selector to its destination, and which *bound* resources a draw writes comes from the
      pipeline reflection Metal captures already carry (`access` in `src/metal/src/reflection.mm`,
      mapped in `metal/reflection.ts`).

### Robustness
- [ ] Intel and AMD class trees and the encoder-boundary timing path (only Apple Silicon and
      stage-boundary sampling are verified).
- [ ] Metal 4 command buffers and encoders: a separate class tree with different selectors.
- [x] Test app coverage for the read-back paths: a private-storage vertex buffer, an MSAA pass
      with resolve through a parallel render encoder, a sampled texture and inline constants.
- [x] ASTC, ETC2 / EAC, PVRTC and the extended-range and packed 4:2:2 pixel formats, in the
      read-back table and the UI's decoder, with reference vectors.
- [ ] Stencil attachment read-back; the multi-planar YUV formats.
- [x] Function constants in the capture (`src/metal/src/function_constants.mm`): MTLFunctionConstantValues
      has no getters, so its setters are hooked and the values ride along on the tracked MTLFunction.
      The shader debugger specializes the invocation with them, so a `[[function_constant]]`-guarded
      variant steps the branches the draw used. `is_function_constant_defined` answers from them.
- [ ] Function constants on an entry point's *arguments* (`[[function_constant(isEnabled)]]` on a
      parameter), which decide whether the argument exists at all: the debugger binds it regardless.
- [ ] The shader debugger on a Unity player's Metal shaders, which are generated MSL rather than
      hand-written: the parser's coverage is what to watch (`src/app/test/vectors/msl/`).

## Direct3D 12

The D3D12 capture library (`src/d3d12/`) reaches the Inspect and Capture panels through the same
protocol as the Vulkan layer. What it lacks is what the replay does for Vulkan, and what the
library does not read back yet.

- [x] Shader debugger (`renderer/d3d12/shader_debug.ts`, `compileHlslForDebugging`): the stage's
      HLSL (embedded by `-Zi`, or in a PDB under the symbol directories) compiled to SPIR-V with
      `dxc -spirv` and stepped in the SPIR-V interpreter. Registers survive as shifted bindings
      (`shared/hlsl_debug.ts`) and are found in the root tables, root views, root constants and
      static samplers; stage variables keep their semantics (`-fspv-reflect`), which pair a vertex
      input with the input layout and a pixel input with the vertex shader's output; a pixel's
      inputs come from the vertex shader run in the interpreter, as on Metal. `dxinsp_shader
      --sources` now reports how dxc was run (main file, defines, arguments) so the compile
      matches the build's.
- [ ] Shader debugger on D3D12, the rest: a DXIL interpreter (or DXIL run on the GPU) to check the
      translation against, the way the Vulkan decompile route runs the original; a shipped shader
      with no HLSL anywhere (the stub of `hlsl_stub.ts` compiles but computes nothing); shader
      model 6.6 dynamic resources (`ResourceDescriptorHeap[]`), which `dxc -spirv` cannot compile;
      the mesh view's VS Out from the same interpreted run.
- [x] Overdraw and pixel history, by the Metal route: measured while capturing, inside the
      application (`src/d3d12/src/overdraw.*`, `pixel_history.cpp`, `pass_record.h`). The pass is
      issued again on the application's own command list with a counting pixel shader, and one
      pixel is followed draw by draw under occlusion queries.
- [x] The CPU timeline and memory per heap (`src/d3d12/src/cpu_timeline.h`), so **Where the CPU
      went**, the **Timeline** card and **Memory Use** work on a D3D12 capture. Submit and present
      are timed in their hooks; the two waits are not D3D12 calls at all, so the library notes the
      event a fence was given (`SetEventOnCompletion`) and the swapchain's frame-latency object and
      hooks `WaitForSingleObject(Ex)` / `WaitForMultipleObjectsEx` to time a wait on either — which
      is the only way a D3D12 frame can be called GPU-bound. `GetClockCalibration` (core, unlike
      Vulkan's extension) and a new `originTicks` on the pass timings put the passes on the CPU
      axis. Memory comes from the adapter's two segments plus `QueryVideoMemoryInfo`, with committed
      resources sized by `GetResourceAllocationInfo` and placed ones deliberately not counted.
      Checked on an RTX 4080: a 1-frame triangle capture reads submit 0.09 ms, present 11.97 ms,
      a 16.9 ms fence wait, and the pass 28.6 ms after its submission; memory reads 1.44 MB
      device-local against the driver's 21.9 MB resident.
- [ ] Compiler statistics per pipeline on D3D12. Vulkan asks the driver through
      `VK_KHR_pipeline_executable_properties`; D3D12 has no portable equivalent, so register
      counts, spills and occupancy are unavailable and the occupancy verdict stays Vulkan-only.
      `D3DReflect`'s `InstructionCount` is DXBC (SM 5) only and reports 0 for DXIL; the real numbers
      need a vendor API (NVAPI, AMD GPUOpen) or the driver's own cached blob, neither portable.
- [ ] The analyses a D3D12 capture still has no answer for: draw overlays, mesh output, per-draw
      timings and counters (**Measure draws**), shader cost by ablation (**Measure shader**) and
      hardware counters (**Measure hardware counters**). Overdraw and pixel history are done, by
      measuring inside the application while it captures (above); these five are what is left, and
      each can go either way — in the application through `pass_record.h`'s recorded pass ops, the
      way overdraw does, or in `dxinsp_replay`, the way `vkinsp_replay` does for Vulkan. The
      in-application route needs the application running and costs time in the captured frame; the
      replay route works on a saved capture, needs a `--serve` mode the D3D12 replay does not have,
      and measures only what the capture holds, which for an engine that records its lists ahead
      (Unity, below) is not the whole frame. In rough order of effort:
  - [x] **Per-draw timings and counters.** `vkinsp_replay` puts a timestamp pair and a pipeline
        statistics query around every action (`src/replay/src/draw_stats.cpp`, 237 lines).
        D3D12 has both: `D3D12_QUERY_TYPE_TIMESTAMP` and `D3D12_QUERY_TYPE_PIPELINE_STATISTICS`,
        whose `D3D12_QUERY_DATA_PIPELINE_STATISTICS` carries the same seven counters in the same
        order the Vulkan query reports them, and `dxinsp_replay` already creates query heaps and
        resolves query data (`dx_replayer.cpp`, `ID3D12QueryHeap` and `ResolveQueryData`). What
        consumes it is API-neutral: the Shader Flame Graph splits a pass's measured time between
        its draws by these, and weights fragment stages by the measured invocation counts.
        Done in the application rather than in the replay: the queries go into the application's own
        list as it records (`CaptureManager::BeginDrawQueries`, `src/d3d12/src/capture.cpp`), so they
        time the application's own draws rather than a second execution of the pass. The capture
        bar's **Measure draws** asks for it; the first 16,384 draws of a frame are measured, and a
        list recorded before the capture began carries no queries. `d3d12-draw-timings` covers it.
  - [x] **Hardware counters.** `src/replay/src/nvperf.cpp` drives NvPerf's
        `RangeProfilerVulkan` around each render pass. NvPerf ships `NvPerfRangeProfilerD3D12.h`
        with the same shape of API, so this is a port rather than a design. NVIDIA only: there is
        no D3D12 equivalent of `VK_KHR_performance_query`, so the vendor-neutral half of the
        Vulkan path has no counterpart. The report, its rules and its verdict column
        (`renderer/counter_rules.ts`, `bottleneck_report.ts`) are already API-neutral.

        This is the one of the five that does not fit the in-application route the other four took.
        A range profiler collects a large metric set over *several passes* of the same GPU work,
        synchronizing the queue between them. In the application that means re-issuing each render
        pass N times inside the frame, with a flush between each, and what the counters would then
        describe is the re-issued pass drawing into copies of the targets rather than the
        application's own draws — at the cost of stalling the frame N times over. In a replay it is
        what `vkinsp_replay --counters` already does: the frame is re-executed as many times as the
        counters need, with nothing else running, which is what the measurement wants.

        So this one went into `dxinsp_replay`, next to the export it already does, rather than into
        the capture library: `--counters` / `--list-counters`, a range per render pass, writing the
        same `gpu-inspector-hw-counters` file the Vulkan replay writes
        (`src/d3d12/replay/src/dx_counters.cpp`). The SDK's utility layer ships a Vulkan range
        profiler but no D3D12 one, so its state machine's `IProfilerApi` is implemented over the
        `NVPW_D3D12_*` entry points in `dx_nvperf.cpp`.

        Three things the port had to get right, each found by it failing:
        - `NVPW_D3D12_LoadDriver` has to run *before the device is created*, the way the Vulkan path
          adds the SDK's extensions before creating its instance and device.
        - The session belongs on the queue the frame is submitted on — the captured queue the replay
          re-created, not the replay's own upload queue — or the pass waits for work it never sees.
        - A profiled submission cannot be waited for inside the pass: the profiler holds it until
          `EndPass`, so the round's submissions go in back to back and are waited for once, after it.

        What could not be checked here: the values. This machine leaves NVIDIA's counters
        administrator-only (`RmProfilingAdminOnly` unset) and the session then *accepts* the
        configuration and simply never finishes the first profiled submission — so the replay now
        checks the permission before it starts, and bounds the wait at 30 seconds rather than
        hanging. Verified as far as that goes: the SDK loads, the chip is identified, and
        `--list-counters` enumerates 1,411 metrics on this GPU.
  - [x] **Draw overlays** (Highlight Draw, Depth Test, Wireframe). The closest thing to what the
        library already does: `src/d3d12/src/overdraw.cpp` re-issues a pass with every pipeline
        replaced by a counting copy, and an overlay is the same machinery issuing *one* draw with a
        flat-color pixel shader. Depth Test is the two runs overdraw already makes (with the
        pass's depth-stencil state, and without); Wireframe is `D3D12_FILL_MODE_WIREFRAME` on the
        PSO copy. Done in `src/d3d12/src/draw_overlay.cpp`: three runs into count targets of the
        pass's size, folded into the one byte per pixel the UI draws. Asking for an overlay captures
        the application's next frame and opens that capture, the way a Metal pixel history does, and
        the draw is named by its pass and its ordinal within it. `d3d12-draw-overlay` covers it.
  - [x] **Mesh output (VS Out).** This needs no shader edit and no HLSL: D3D12 streams a vertex
        shader's declared outputs out of the unmodified bytecode. The PSO is rebuilt with a
        `D3D12_STREAM_OUTPUT_DESC` whose `D3D12_SO_DECLARATION_ENTRY` list comes from the VS output
        signature (which `dx_reflect.h` already reads), `RasterizedStream` set to
        `D3D12_SO_NO_RASTERIZED_STREAM`, and a root signature carrying
        `D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT`; RenderDoc does exactly this in
        `driver/d3d12/d3d12_postvs.cpp`. The Vulkan side instead patches the SPIR-V for transform
        feedback (`src/replay/src/xfb_patch.cpp`), which has no DXIL counterpart and is not needed
        here. Exception: a mesh or amplification shader has no VS stage to stream out of, and would
        need its own route. Done in `src/d3d12/src/mesh_output.cpp`, with the root signature
        deserialized from the blob `RootSignatureInfo` now keeps, the stream-output flag added and
        the signature created again; the copy is layout-compatible, so the application's root
        arguments still apply. `d3d12-mesh-output` covers it.
  - [ ] **Shader cost by ablation.** The one that genuinely needs source. Ablation means removing a
        function's calls, a line's values or a texture's reads from the shader and timing what that
        saves (`src/replay/src/ablation.cpp`), and there is no DXIL editor here to do it with — so
        on D3D12 it falls under the rule the shader debugger already states: it needs the HLSL, from
        a `-Zi` build or a `-Zs` build's PDB under the symbol directories, compiled the way the
        build compiled it. Two experiments need no source at all and are worth having for the
        shaders that have none, as long as the report says what each one does and does not measure:
        replacing a whole stage with a trivial shader (what that stage costs, not which line of it),
        and binding a 1x1 texture in place of one SRV (that texture's bandwidth, not its ALU).
- [x] Stencil read-back: plane 1 of a depth-stencil target, beside its depth (`--stencil` in
      `test/d3d12_triangle`, the `d3d12-stencil` UI case). A multisampled stencil is not resolved.
- [x] Ray tracing (`src/d3d12/src/raytracing.h`), the DXR half of what the Vulkan layer does. Three
      numbers hide everything, and each needed its own answer:
      * **Shader identifiers.** A trace names four regions of memory, and each record begins with
        the 32 opaque bytes the runtime gave for an export. `ID3D12StateObjectProperties::GetShaderIdentifier`
        is hooked (a new `ID3D12StateObjectProperties1` in `gen_d3d12.py`, patched through a plain
        `HookVtable` because the interface shares the state object's reference count), and the
        library also asks for the identifier of every export the description names. Both are needed:
        a DXIL library with `NumExports` 0 exports everything in it under its own names, which
        nothing but the container lists, and the hook is what catches those.
      * **Build inputs.** Resolved through `AddressMap` and read back, the way the Vulkan layer
        reads a build's vertices and instances.
      * **The structures.** This is where D3D12 differs in kind rather than in spelling: a
        `VkAccelerationStructureKHR` is a handle, and a DXR structure is a range in a UAV buffer
        named only by the address a build wrote it to. So the library mints one tracked
        `ID3D12RaytracingAccelerationStructure` per destination address, keyed by a sentinel of its
        own so it can never collide with an interface pointer, and every later build, copy,
        descriptor and instance reference resolves to it.
      Verified on an RTX 4080 with `dxinsp_triangle --ray-tracing`: both levels in the capture with
      their builds, the instances resolving to the bottom level they name, and all four binding
      table records resolving to their exports. Two defects found doing it: the structures were
      minted only while a recorder existed, so a bottom level built at start-up had no object at
      all; and `capture_file.ts` dropped them when saving, because nothing in a command references
      one (the Vulkan structures were already kept for the same reason).
- [x] DXR replay (`src/d3d12/replay/src/dx_raytracing.cpp`). The addresses decode like any other,
      so what the replay had to do itself is the two kinds of number written *inside* buffers: an
      instance's bottom level reference, remapped through the address each structure object carries,
      and a binding table record's identifier, remapped through the export name it resolves to. A
      structure's buffer is **created** in `D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE`
      rather than moved into it, which a barrier cannot do. Verified byte for byte on an RTX 4080
      with `--rebuild-blas`, under the debug layer. The defect it found was the replay's own: a
      build's inputs are read back under their field names rather than in the flat `bufferData`
      list, and nothing uploaded them, so the replay built a bottom level out of uninitialized
      memory — which is indistinguishable from a correct replay of an empty scene.
- [x] A capture of `test/path_tracer/d3d12` shut the application down, which is what a real DXR
      application found that the triangle could not. Two faults in the library, one in the sample:
      * The library read back whatever buffer a **root view** names, and that application binds its
        top level as a root SRV. A resource in `D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE`
        may never be transitioned out of it, so the read-back's barrier closed the list with
        E_INVALIDARG -- and an application has no reason to expect `Close` to fail. The read-back is
        now refused for a buffer a build wrote a structure into (its contents are the driver's
        anyway), with a state check behind it for a structure built before the library attached.
        This was not new: nothing before had any way to tell such a buffer apart.
      * `GetShaderStackSize` takes a shader, and a hit group is not one, so asking for a hit group's
        stack raised validation errors *in the application's log*, where they read as its own. The
        hit groups are known from the description and are no longer asked.
      * The sample laid its shader tables out back to back at the record stride, so the miss and hit
        tables were not 64-byte aligned and the runtime dropped every trace. Nothing noticed until
        the capture was taken with the debug layer on, which is why the rule below now exists.
- [x] `binding-table-alignment` and `empty-binding-table` frame rules
      (`renderer/d3d12/frame_analysis.ts`): DXR's alignment requirements checked against a trace's
      own arguments, which the capture already holds, so a table the runtime would drop is named
      with nothing turned on. Verified against a real capture taken with the fault present.
- [ ] Ray tracing, the rest:
  - [x] A bottom level built before the capture is filled by the replay: the capture library reads
    back what its last build read when the capture starts, and `dxinsp_replay` builds it before
    the frame (`DxReplayer::BuildEarlierStructures`). The Vulkan side does the same.
  - A binding table record's local root arguments are copied as they were, so a descriptor handle or
    a GPU address among them points at the captured process's memory. Nothing in the capture says
    which of a record's bytes are which.
  - An opacity micromap array build, `ExecuteIndirect` over a trace, and exporting any of it to C++.
- [ ] The contents of sampler feedback, video and work graph objects; enhanced
      barriers (`Barrier`) beyond the layouts that map to legacy states.
- [ ] A descriptor table set in a bundle before the bundle set its own root signature is recorded
      without contents (bundles inherit the caller's root signature).
- [x] A capture of a Unity D3D12 player took the application's GPU with it
      (`D:\Unity\urp_sample\build\urp_sample.exe`, URP, 800x600). Two faults, both fixed: a
      descriptor naming a released resource, which the library asked for its description (a crash
      while merely recording), and the queries and read-back copies the capture added to a render
      pass the application suspends across command lists. Direct3D allows no GPU-work-generating
      call between a suspension and its resume; `Close` then returns E_FAIL, and Unity treats that
      as a lost device ("Unrecoverable GPU device error"). Only `-force-d3d12-debug` named it:
      `ResolveQueryData ... called while a Render Pass is suspended`. A capture now completes and
      the player runs on.
- [x] A Unity frame is measured almost not at all: a capture holds over a million commands but
      only ~18 pass timings, ~30 textures and 21 buffers (and the buffers all fail with "command
      list was not executed during the capture"). The queries and the read-back go in while a
      list is *recorded*, and Unity records its lists several frames ahead, so the lists submitted
      in the captured frame were built before the capture began; 58 more pass segments are
      suspended across lists and take nothing by construction (README.md, "Passes"). Measuring at
      record time cannot reach either. What RenderDoc does instead (`D:\src\ref\renderdoc`): it
      records every command list from injection (`CaptureState::BackgroundCapturing`, so
      `IsCaptureMode` is true outside captures too) and adds nothing to the application's lists
      during the captured frame. At `StartFrameCapture` it waits for the device to be idle, copies
      the *initial contents* of dirty resources once per resource into **its own** lists and submits
      those itself (`D3D12Device::StartFrameCapture` -> `PrepareInitialContents`, which also
      postpones and skips the large ones); the frame then runs untouched, and what each pass
      rendered, the overdraw, the pixel history and the timings all come from replaying the capture.
      The Vulkan side already replays for them (`vkinsp_replay`); D3D12 has no replay yet, and that
      is what this needs.
      Since measured again on the same player (below): the commands are no longer the problem -- the
      warm-up frame of recording reaches every list the captured frame submits -- and the queries now
      go in during that frame too. What is left of this entry is the suspended passes and a lead of
      more than one frame, each its own item below.
      Measured a third time, after the suspended passes were timed (URP player, 800x600, 19
      captures): 58 of 58 passes timed in most, but 32, 29, 28, 27 or 11 in about one capture of
      three, and a third of the buffers reported as failed read-backs in every one. Two causes, both
      fixed. (1) Unity pools its lists and resets each as soon as it has run, frames before it
      records into it again, so whichever were reset before the capture was armed come back
      **adopted** (`CaptureManager::Adopt`) -- 40 to 60 of the captured frame's lists in the bad
      captures -- and an adopted list took no queries at all. It now takes the passes' timestamps,
      which Direct3D allows in whatever state the list is in (statistics and occlusion stay off: the
      application may have a query of its own open); the end timestamp of an `OMSetRenderTargets`
      pass in such a list goes where the pass ends (`EndPass`). Every capture since times 58 of 58,
      those with 60 adopted lists too. `test/d3d12_triangle --pool` reproduces it (a pool of four
      lists, each reset right after it runs) and the `d3d12-pool` UI case covers it under the debug
      layer: 0 timings with the library before, 2 of 2 after. (2) The "failed" buffers were
      read-backs queued by lists no captured list ran -- the lists Unity records in the captured
      frame for the next one, and pooled lists recorded again after running -- which were dropped
      only when the frame before had queued them. Every entry no captured list ran is now dropped
      (`SendTextures`, `SendBuffers`): 0 failed read-backs in 8 captures of 8, where each had had
      260 to 1086.
- [x] An adopted command list (above) had no render targets read back, as a suspended pass has
      none. Unlike a suspended pass, a pass the capture sees begin in an adopted list is not inside
      a region the capture missed, so "adopted" no longer makes a pass split: it is read back in
      place like any other. What made that safe is that the per-list barrier log is kept whether or
      not anything records (`ResourceTracker::OnListReset` / `OnBarriers` run for every list), so
      the adopted list's own transitions are known. The target is copied from the list's own last
      transition of it, else from the state the pass needs it in (`RENDER_TARGET`, `DEPTH_WRITE`),
      no longer from the tracker's global state, which is only as recent as the last submission
      (`StateIn(..., inList)`); read-only depth still takes the global state. Still no statistics,
      occlusion or measurements (overdraw, pixel history, overlays) in an adopted list.
      Checked: `d3d12_triangle --pool` with and without `--render-pass`, `--msaa` and `--stencil`
      under the debug layer, every target read back and the colour target the frame on the screen;
      on the URP player, captures with 60 adopted lists read back 14 targets where they had 0, and
      the final image is right. The entry's "107 textures against ~166" was the *sampled* count, and
      a capture without adopted lists has had 107 as well, so that is not adoption's doing.
- [ ] Every capture of the URP player under the debug layer adds 6,500 to 8,000 errors from the
      capture's own barriers, with the library before the work above as with it:
      `Before state (0xE0: DEPTH_READ|NON_PIXEL_SHADER_RESOURCE|PIXEL_SHADER_RESOURCE) of resource
      'Depth-BackBuffer-800-600' ... does not match ... DEPTH_WRITE`, and a few on the bloom mips
      (`PIXEL_SHADER_RESOURCE` against `RENDER_TARGET`). None arrive between captures. They look
      like the copies of what draws read taken after a submission (`RecorderSlot::afterSubmit`),
      which transition from the tracker's global state, and that state is wrong for the depth
      buffer at that point.
- [x] What an engine that records ahead leaves unmeasured. Measured on the URP player
      (`D:\Unity\urp_sample`, 800x600, `-force-d3d12`), where a captured frame submits 64 command
      lists and resets only 3 of them: 61 were recorded in the frame before, which the capture's
      one warm-up frame of recording already covers -- no list came out unrecorded in any run. What
      it did not cover was everything that goes into a list *as it records*: a pass's timestamps and
      statistics were only taken once the capture had started, so a frame whose lists were built in
      the warm-up frame reported **no pass timings at all**. The queries now go in during the warm-up
      frame too, their entries marked `warmup` and kept only if their list runs in the capture, the
      way the read-backs already were (`BeginPass`, `BeginDrawQueries`, `TimingEntry::warmup`); the
      query counters start over when the capture is armed rather than when it starts, or the
      captured frame's passes would be handed the slots the warm-up frame already used. A/B on the
      player: without it, a capture landing on such a frame read 0 timings of its 29 timeable
      passes; with it, every timeable pass is timed, 10 of the 11 in one run coming from lists
      recorded before the frame (the log says how many).
- [x] A render pass suspended across command lists is timed. 46 to 74 of a Unity frame's ~58 pass
      segments are suspended, and a suspended pass took no queries at all, so most of such a frame
      had no GPU time. What actually forbade them was the *resolve*: a `ResolveQueryData` between a
      suspension and its resume closes the list with E_FAIL, which is how the player was lost once.
      A timestamp itself is a single `EndQuery`, which Direct3D allows inside a pass region, so each
      segment now takes one at each end -- the begin where the pass begins, the end before
      `EndRenderPass` is forwarded (`CaptureManager::EndSplitPassTimestamp`), since after that the
      pass is suspended -- and nothing of the capture's is resolved in the application's lists any
      more: `Impl::ResolveQueries` resolves every pass's queries from a list of the capture's own,
      on a direct queue of the device, once the frame's work has been waited for at the finish. Only
      the passes whose list actually ran are resolved, since resolving a query that was never
      written is undefined where an unresolved slot reads as the zeros that mean "never ran". A list
      recorded again drops the measurements of the recording it replaced (`DropUnrunMeasurements`),
      which a warm-up frame's entries would otherwise keep beside the new ones.
      `test/d3d12_triangle --suspend` splits its pass across two lists and the `d3d12-suspend` UI
      case covers it, with the debug layer on. On the URP player: 11 of 58 passes timed before, 21
      to 58 after, and the Frame Stats page reads *"the GPU ran 58 passes over 2.40 ms"* on a frame
      that had no GPU time at all before. Checked on an RTX 4080 with the debug layer silent, and
      against a heavier second half to prove the resumed segment is really measured (3.07 us for the
      first half, 5.12 us for a second half of 2000 instances).
      Still not taken on a split pass: the render targets (a copy there is what Direct3D forbids)
      and pipeline statistics, which no pass of the render-pass API carries -- a `BeginQuery` /
      `EndQuery` pair is not allowed inside a pass region, only timestamps are.
- [ ] Lists recorded more than one frame ahead, which the warm-up frame does not reach: recording
      from two frames ahead, or keeping the last frame's recordings and using them when a list is
      submitted unchanged, would cover them. Not seen on the URP player, whose captured frames were
      all recorded in the frame before, but an engine with a deeper lead would need it.
- [ ] 32-bit targets: only x64 processes are injected.
- [x] Catching an application started elsewhere: `dxinsp_launch.exe --watch <image>` polls for the
      process and injects it while it is held suspended, which is what D3D12 has in place of an
      implicit layer (the "wait for an application" launch target, and `wait_for_app`).
- [ ] Attaching to a process that already has a device: injection still has to happen at process
      start, because the hooks go on the entry points and D3D12 cannot enumerate an existing device.
- [x] DXIL reflection and disassembly without `dxcompiler.dll` on the machine: the Windows package
      ships one beside the library. Not downloaded or built: Electron already bundles a full DXC
      (1.9.2607 with Electron 44, for Dawn), which `after_pack.cjs` used to delete and now moves
      into `resources/layer`, where the library and `dxinsp_shader.exe` look first. Checked that
      Electron's build reflects, disassembles, assembles and validates (the `d3d12-plain` and
      `d3d12-shader-edit` cases pass against it), so `dxil.dll` stays out. RenderDoc ships its
      copy the same way (`plugins/d3d12/dxcompiler.dll`, from its prebuilt plugins bundle).
- [x] Automated test: `tools/ui_tests.py` runs five D3D12 cases over `dxinsp_triangle` (a plain
      capture, a render pass, a bundle, an offscreen frame with no swap chain, and opening a saved
      capture), the way the Vulkan cases run.

## iOS devices

Inspecting a Unity iOS player on a device with the full inspector UI, without changing the Unity
project or the Xcode project it generates. The macOS design carries over unchanged in principle:
dyld honors `DYLD_INSERT_LIBRARIES` on iOS for a process signed with `get-task-allow` (every
development-profile build), which is how Xcode itself inserts `libMTLCapture.dylib` for GPU Frame
Capture; `__DATA,__interpose` and the class hooks work the same; and the transport already binds
`127.0.0.1` and listens (`src/metal/src/transport.mm`), which is exactly what a USB port forward
(usbmux, the iOS `adb forward`) connects to, with no Local Network permission prompt.

What is unknown until measured on a device is where the library may live and whether iOS accepts
its signature. iOS refuses a library signed by a different team ("mapping process and mapped file
(non-platform) have different Team IDs"), so the library must be signed with the **same team** as
the application. Everything below is done on the Mac; this Windows machine cannot build any of it.

Limits known up front: App Store, TestFlight, Ad Hoc and Enterprise builds lack `get-task-allow`
and would need re-signing with a development profile first; the device needs Developer Mode on
(Settings > Privacy & Security); a Mac is the host (codesign, devicectl, the signing identity).

### Setup (once)
- [ ] Tools: Xcode 16 or later (`xcrun devicectl`), CMake, and a USB port forwarder, either
      `brew install libimobiledevice` (`iproxy`) or `pipx install pymobiledevice3`. pymobiledevice3
      also answers "where is this app installed" (`apps query`), which step 0 needs.
- [ ] The device plugged in over USB, trusted, Developer Mode on, Auto-Lock off (a locked device
      suspends the app and its socket).
- [ ] The device's identifier: `xcrun devicectl list devices` (the Identifier column), and its
      UDID for iproxy / pymobiledevice3: `xcrun xctrace list devices` or Finder.
- [ ] The signing identity and team:
      ```sh
      security find-identity -v -p codesigning     # "Apple Development: Name (XXXXXXXXXX)"
      ```
      The team the library must match is the one the app is signed with (step below), which is not
      necessarily the ID in parentheses above.
- [ ] A Unity iOS player built the normal way: Unity > Build for iOS, open `Unity-iPhone.xcodeproj`,
      Signing & Capabilities with *Automatically manage signing* and your team, build and run it
      once from Xcode so it is installed and known to work. Then take the built bundle from
      DerivedData (Xcode > Product > Show Build Folder in Finder, `Products/<config>-iphoneos/`),
      or build from the command line:
      ```sh
      xcodebuild -project Unity-iPhone.xcodeproj -scheme Unity-iPhone -configuration Release \
        -destination 'generic/platform=iOS' -derivedDataPath build/ios -allowProvisioningUpdates \
        DEVELOPMENT_TEAM=<team>
      # -> build/ios/Build/Products/Release-iphoneos/<product>.app
      ```
- [ ] Record what the app is signed with:
      ```sh
      APP=path/to/<product>.app
      codesign -dv --verbose=4 "$APP" 2>&1 | grep -E 'TeamIdentifier|Authority'
      codesign -d --entitlements - --xml "$APP" > app-entitlements.plist
      plutil -p app-entitlements.plist             # must contain "get-task-allow" => true
      /usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$APP/Info.plist"
      ```
      No `get-task-allow`: the profile is not a development one, and nothing below can work.

### Step 0: will iOS load an inserted library at all, and from where
A probe library that only logs, so the answer does not depend on porting `src/metal/`. The same
experiment `src/metal/README.md` records for macOS signing; put the resulting table beside it.
- [ ] Build and sign the probe:
      ```c
      // probe.c
      #include <os/log.h>
      #include <stdio.h>
      #include <unistd.h>
      __attribute__((constructor)) static void probe(void) {
          os_log(OS_LOG_DEFAULT, "[mtlinsp-probe] loaded into pid %d", getpid());
          fprintf(stderr, "[mtlinsp-probe] loaded into pid %d\n", getpid());
      }
      ```
      ```sh
      xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=15.0 -dynamiclib \
        -install_name @rpath/libprobe.dylib probe.c -o libprobe.dylib
      codesign --force --timestamp=none --sign "Apple Development: <you>" libprobe.dylib
      codesign -dv libprobe.dylib 2>&1 | grep TeamIdentifier    # must equal the app's
      ```
- [ ] How to see the result: the constructor's line in Console.app (pick the device in the sidebar,
      filter `mtlinsp-probe`, start streaming before launching), or on stdout with `--console` on the
      launch below. **A library dyld cannot load is fatal**: the app dies at launch, and the reason
      (`could not load inserted library`, `code signature invalid`, `different Team IDs`) is in the
      `--console` output, in Console.app under the `kernel` / `amfid` processes, or in the crash
      log (Xcode > Window > Devices and Simulators > Open Recent Logs). A launch that runs normally
      with no probe line means dyld dropped the variable.
- [ ] The launch command, used for every case (check `xcrun devicectl device process launch
      --help` if a flag has been renamed):
      ```sh
      xcrun devicectl device process launch --device <identifier> --terminate-existing --console \
        --environment-variables '{"DYLD_INSERT_LIBRARIES":"<path>"}' <bundle-id>
      ```
      Add `"DYLD_PRINT_LIBRARIES":"1"` to see on the console whether dyld honors `DYLD_*`
      variables for the process at all.
- [ ] Case A, in the bundle (re-signs a copy of the app; the Unity and Xcode projects stay as they
      are):
      ```sh
      cp -R "$APP" Probe.app
      cp libprobe.dylib Probe.app/Frameworks/
      codesign --force --timestamp=none --sign "Apple Development: <you>" \
        --entitlements app-entitlements.plist Probe.app
      codesign --verify --deep --strict Probe.app
      xcrun devicectl device install app --device <identifier> Probe.app
      ```
      - [ ] A1: `DYLD_INSERT_LIBRARIES=@executable_path/Frameworks/libprobe.dylib`
      - [ ] A2: the absolute path: the install location from `pymobiledevice3 apps query
            <bundle-id>` (its `Path`), then `<Path>/Frameworks/libprobe.dylib`. The UUID in it
            changes with every install.
- [ ] Case B, in the app's data container (no re-signing, the app exactly as Xcode built it;
      reinstall the original `$APP` first):
      ```sh
      xcrun devicectl device copy to --device <identifier> --domain-type appDataContainer \
        --domain-identifier <bundle-id> --source libprobe.dylib --destination Documents/libprobe.dylib
      ```
      - [ ] B1: `DYLD_INSERT_LIBRARIES=<Container>/Documents/libprobe.dylib`, `Container` from
            `pymobiledevice3 apps query <bundle-id>`. If this loads, the inspector never needs to
            touch the application bundle, which is the better product.
- [ ] Controls, so a failure is understood rather than guessed at:
      - [ ] the probe ad-hoc signed (`codesign --force --sign - libprobe.dylib`) in the winning
            placement: expected to be refused, and shows what a signature refusal looks like;
      - [ ] if devicectl turns out not to pass the variables, the same case launched from Xcode
            instead: in the Unity Xcode project, Product > Scheme > Edit Scheme > Run > Arguments >
            Environment Variables, with Diagnostics > *Metal API Validation* and Options > *GPU Frame
            Capture* set to Disabled so Xcode inserts nothing of its own. The scheme only affects
            the launch, not the build, so this still leaves the build uninstrumented.
- [ ] Write the results down (placement × path form × launcher → loaded / refused and the message)
      in `src/metal/README.md` next to the macOS table. **No case loads: stop here**; Route A is not
      possible and the remaining option is Xcode's `.gputrace` via LLDB.

### Step 1: build the capture library for iOS
- [ ] Configure and build with the iOS SDK:
      ```sh
      cmake -S metal -B build/ios -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
        -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo
      cmake --build build/ios
      # -> build/ios/bin/libmtlinsp_capture.dylib
      ```
- [ ] Fix what the iOS SDK marks unavailable, each behind `#if TARGET_OS_OSX` (or `@available` /
      `respondsToSelector:` where iOS has the API from some version on). Found by reading, so the
      compiler may find more:
      - `interpose.mm`: `MTLCopyAllDevices`, `MTLCopyAllDevicesWithObserver`,
        `CGDirectDisplayCopyCurrentMetalDevice` and the `CGDirectDisplayMetal.h` import are
        macOS-only; iOS keeps `MTLCreateSystemDefaultDevice` and the `nextDrawable` fallback.
      - `hooks_descriptors.mm:521`: `lowPower`, `headless`, `removable` are macOS-only;
        `registryID` depends on the iOS version.
      - `capture.mm:895` and `capture.mm:1329`, `formats.mm:318`: `MTLStorageModeManaged` and the blit
        encoder's `synchronizeResource:` do not exist on iOS (no managed storage; shared buffers
        read directly). Hooking the `synchronizeResource:` selector in `hooks_encoders.mm` is
        harmless, since the class simply has no such method.
      - `frame_stats.mm` `QueryDisplayRefreshMs`: no CoreGraphics display modes on iOS; ask
        `UIScreen.mainScreen.maximumFramesPerSecond` through the runtime, the way `NSScreen` is
        asked now (60 or 120 with ProMotion).
      - `gpu_trace.mm` `DefaultPath`: no Desktop; write under `NSTemporaryDirectory()` and fetch with
        `xcrun devicectl device copy from --domain-type appDataContainer ... --source tmp/<file>`.
      - `CMakeLists.txt`: guard the CoreGraphics link if nothing else needs it.
- [ ] Sign it with the same identity as the probe, and check its TeamIdentifier.

### Step 2: inject it into the Unity player and connect
- [ ] Put `libmtlinsp_capture.dylib` where step 0 said a library loads from (Frameworks + re-sign,
      or the data container), and launch:
      ```sh
      xcrun devicectl device process launch --device <identifier> --terminate-existing --console \
        --environment-variables '{"DYLD_INSERT_LIBRARIES":"<path>","MTLINSP_PORT":"47531","MTLINSP_LOG":"1","METAL_CAPTURE_ENABLED":"1"}' \
        <bundle-id>
      ```
      Expect `[mtlinsp] loaded into pid …` then `[mtlinsp] listening on 127.0.0.1:47531` on the
      console.
- [ ] Forward the port over USB, in a second terminal, and leave it running:
      ```sh
      iproxy 47531:47531 -u <udid>            # libimobiledevice 1.3+; older: iproxy 47531 47531 <udid>
      # or: pymobiledevice3 usbmux forward 47531 47531
      ```
- [ ] Connect the UI: **Connect** in the launch bar with port 47531, or from `src/app/`:
      `npm start -- --connect=47531`. The MCP server's attach should work the same way.
- [ ] What to check, in order, noting anything that differs from a Mac:
      - [ ] Inspect: the snapshot arrives; device, queues, buffers, textures, pipelines listed.
            Record the concrete class names (the `AGX…Device` family for the device's GPU, and the
            `MTLDebug*` ones with `MTL_DEBUG_LAYER=1`) in the class-tree table in `src/metal/README.md`.
      - [ ] Frame capture of one frame: commands per command buffer and pass, render targets read
            back, vertex and index buffers, pass timings.
      - [ ] Pixel formats a phone uses that a Mac player does not: ASTC textures, `BGRA8_sRGB`
            drawables, memoryless depth (cannot be read back, must not crash).
      - [ ] Frame Stats: frame time and the refresh rate (60 / 120).
      - [ ] Validation layer: add `"MTL_DEBUG_LAYER":"1","MTL_DEBUG_LAYER_ERROR_MODE":"nslog",
            "MTL_DEBUG_LAYER_WARNING_MODE":"nslog"` to the environment (what `captureEnvironment`
            in `src/app/src/main/metal.ts` sets on a Mac).
      - [ ] Stack traces (`MTLINSP_STACKTRACES=1`): addresses symbolize against the dSYMs Xcode
            wrote for `UnityFramework`, which the host needs via `set_search_paths`.
      - [ ] Overdraw, pixel history and Xcode Trace (then `devicectl device copy from` the
            `.gputrace` and open it in Xcode).
      - [ ] Memory: a full capture's read-back on a phone near jetsam's limit; watch for the app
            being killed and lower `maxBufferTotal` / `maxTextureSize` as the Android note suggests.
      - [ ] Backgrounding the app and returning: the listener survives, and the UI can reconnect.

### Step 3: make it a launch target (only once step 2 works)
- [ ] `src/app/src/main/ios.ts`, the counterpart of `metal.ts` and the Android launcher: list devices
      (`xcrun devicectl list devices --json-output`), check the app's `get-task-allow` and team,
      sign and place the library the way step 0 found works, install, launch with the environment,
      forward the port, connect. The usbmux forward can be spoken directly (a plist protocol over
      `/var/run/usbmuxd`) instead of depending on iproxy.
- [ ] The launch dialog's *Run On* lists iOS devices beside Android ones; a `launch_ios_app` MCP
      tool beside `launch_android_app`.
- [ ] The iOS library built by CI and staged into the macOS app's resources
      (`src/app/tools/stage_layer.mjs`), signed at launch time with the user's identity rather than
      the project's (a Developer ID signature is a different team from the app's).
- [ ] `docs/IOS.md`: requirements (development-signed build, Developer Mode, same team), and a
      Troubleshooting section built from step 0's failure messages.

## Distribution
- [ ] Code-sign the Windows installer and the layer DLL (SmartScreen warns on unsigned installers).
- [ ] An rpm target next to the .deb and the AppImage (electron-updater supports it). The AppImage
      has shipped since the Linux packaging work (`src/app/electron-builder.yml`, docs/RELEASING.md).
- [x] macOS build of the UI, signed with the project's Developer ID and notarized.

## Tooling
- [ ] UI tests: cases for the Unity player and Android devices when attached (the saved-capture
      mode covers their captures), image comparison of the screenshots against references.
- [ ] Help links to docs from the panels.
- [ ] A screenshot of **Memory Use / Over time**, the one v0.14.0 feature the documentation still
      describes without a picture. It needs a session whose memory actually moves: the shot has to
      come from a live run rather than a capture file, because the series is the session's, and the
      test application's is flat, so a chart of it would illustrate none of the four shapes the
      text names. A player that streams, or `test/triangle` given something that allocates and
      frees per frame, would.
- [ ] `tools/doc_screenshots.py` skips any shot whose capture is missing, and two of its captures
      (`unity-ui.gpucap` for the flame graph and the draw overlay) are no longer anywhere on this
      machine, so those two images cannot be regenerated. Worth keeping the capture set the
      screenshots are taken from somewhere durable rather than in a temp directory.
