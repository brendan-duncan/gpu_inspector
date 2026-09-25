# How to inspect a Unity player

A built Unity player is an ordinary Vulkan or Direct3D 12 application, and the inspector launches
it like any other. What is worth knowing before you do is which backend to point it at and how
Unity builds its frames, because the same scene reads very differently depending on both.

Everything here was checked against a URP sample player on Windows, on both backends. A player
built for Android or a Quest is a different route: see [Android and Quest](ANDROID.md) and
[How to inspect a Quest application](HOWTO_QUEST.md).

![A captured frame of a Unity player, with a draw selected and its pipeline state and descriptor sets shown](images/capture-draw.png)

## Steps

1. Press **Launch...** and leave *Run On* at **This computer**.
2. Put the player's executable in **Executable** — the `.exe` beside its `_Data` folder.
3. Put the backend and a window size in **Arguments**:

   ```
   -force-vulkan -screen-width 1280 -screen-height 720 -screen-fullscreen 0
   ```

   or `-force-d3d12` for the Direct3D 12 backend. These are Unity's own player arguments, not the
   inspector's. A windowed player is much easier to work with than a fullscreen one, which is what
   `-screen-fullscreen 0` is for.
4. Press **Launch**. The session connects as the player creates its device, before the splash.

From a shell:

```
GPUInspector.exe --launch=D:\project\build\game.exe --args="-force-vulkan -screen-fullscreen 0"
```

Capture from the **Capture** tab once the player is rendering the scene you care about. A Unity
player spends its first seconds on a splash and a loading screen, and a capture taken then is a
capture of those.

## Which backend to inspect

If the choice is yours, **inspect the Vulkan build**. The same URP scene, captured on the same
machine, at 800x600:

| | Vulkan | Direct3D 12 |
|---|---|---|
| Passes in the capture | 25 | 58 pass segments |
| Passes with a GPU time | 25 | 58 |
| Render targets read back | all of them | the passes that are not suspended |
| Commands | ~1,100 | ~1,900–3,400 |

The difference is not the renderer: it is that Unity's Direct3D 12 backend builds a render pass
across several command lists (`D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS` / `_RESUMING_PASS`), and
Direct3D 12 forbids a copy between a suspension and its resume. Such a pass is recorded and timed,
and its **render targets are not read back** — so the thumbnail strip and the image viewer have
nothing for most of the frame's passes. On Vulkan nothing is split that way and the frame comes
back whole.

Both are worth capturing when the question is about the backend itself — a state the D3D12 path
sets and the Vulkan path does not, say. For reading a frame, Vulkan.

## Why a Direct3D 12 frame can look thin

**Unity records its command lists a frame or more ahead**, on worker threads. Two consequences:

- A capture starts recording when you press **Capture** and lets one frame pass before capturing,
  so lists built during that frame are recorded whole. Lists built *earlier* than that are not, and
  the frame shows `<unrecorded command list>` where they ran, with an `unrecorded-list` finding in
  Frame Stats. The answer is **Record all command buffers** in the capture bar: it records every
  list from the moment it is ticked, whenever the engine builds it, at the cost of CPU time in the
  player for as long as it is on.
- Everything measured at record time — pass timings, per-draw timings — goes into the list as the
  engine builds it. That is handled (the frame of recording before the capture takes the queries
  too), but it is why a Unity capture is the frame where this shows up at all.

## Shaders

**On Vulkan you can step a Unity shader.** The debugger interprets the captured SPIR-V, so it needs
no source: select a draw and press **Debug Pixel** or **Debug Vertex**. Unity ships no shader source
with a player, so pick **Decompiled GLSL** rather than **Original SPIR-V** to step by — the
decompiled form reads like the shader, where the SPIR-V reads like the module. Editing works from
the SPIR-V disassembly in the same way.

**On Direct3D 12 you cannot.** The debugger steps a shader as its HLSL compiled to SPIR-V by `dxc`,
and a Unity player ships no HLSL, so the tab says so ([Direct3D 12](D3D12.md#what-is-not-there-yet)).
Reflection, the disassembly and the flame graph's static analysis still work.

**A Vulkan capture carries Unity's own shader names** — a draw's stages read as
`Shader Graphs/ScreenBlockerDissolve_Graph main` rather than as a hash — so the command list says
which material a draw is without going through its bindings. A Direct3D 12 capture shows what the
DXBC/DXIL carries: the reflection, the bindings and the disassembly.

## Stack traces

A Unity player ships no PDBs, so **Stack traces** in the capture bar gives frames of
`UnityPlayer.dll+0x...` rather than functions. If you control the build and your Unity version offers it,
turn on *Copy PDB files* in the player settings and point **Symbol directories** in the launch
dialog at the player's folder; traces then resolve as they do for any other application.

## What a frame looks like

The URP sample above, on Vulkan: 1,073 commands, 148 draws, 25 passes and 40 render targets. The
passes are Unity's render graph, so they are all called `ExecuteRenderGraph...` and what tells them
apart is what they render into — the thumbnail strip names each one's attachments, and a 2048x2048
`D16_UNORM` first pass is the shadow map whatever the label says. Unity's own debug labels
(`WaitForRenderJobs`, `FrameTime.GPU`, `CustomRenderTextures.Update`) group the commands under them,
and those come from the player rather than from us.

**Unity records its draws into secondary command buffers** on Vulkan, which the capture says under
each draw. That matters for one thing only: a pass that executes secondaries can carry pipeline
statistics counters only where the device has the `inheritedQueries` feature, so a pass may be timed
and uncounted ([Finding GPU bottlenecks](PROFILING.md)).

The same scene on Direct3D 12 is ~2,900 commands over 58 pass segments. Unity splits a render pass
across command lists (a suspended pass, resumed in the next list), so one of its passes is several
segments in the command list; the render graph and the Tile-Based GPUs report count them as the one
pass they are. The lists Unity builds inside the captured frame run after it, so they are not in
the capture; what they asked to have read back is dropped with them, rather than reported as failed.

**A Direct3D 12 Unity frame replays identical** ([Capture replay](REPLAY.md#direct3d-12)): the URP
sample's frames do, every target. Replay it without the debug layer to judge that. Under the debug
layer the replay reports some thirty errors, and they are Unity's own — its G-buffers attached and
sampled in the same pass, and back-buffer depth barriers that contradict each other — which the
player raises by the hundred when it runs under the debug layer itself, and which leave its depth
buffer undefined there too.

Two things about the numbers, both inherent to capturing rather than to Unity:

- The captured frame takes much longer than the frames around it, because every render target is
  read back at the end of its pass: 2.76 ms of GPU work spread over a 9.36 ms span in the Vulkan
  capture above. **Frame Bound** in Frame Stats compares the captured frame against its own budget
  and says when the capture's own cost dominates — it reported 766% of the budget for that frame.
- The pass timings are the GPU's, measured with timestamps around each pass; the frame time in the
  session bar is the player's own wall clock.

## If it does not work

| What you see | What it means |
|---|---|
| The session connects, then the player exits | Capture the frame again with the debug layer on (**Validation layer** in the launch dialog) and read the **Log**: a player that treats a failed `Close` as a lost device says so there. |
| `<unrecorded command list>` in the command list | Lists were built before the capture began: tick **Record all command buffers** and capture again (above). |
| Most passes have no render targets | Those passes are suspended across command lists, which is the Direct3D 12 backend's normal shape; capture the Vulkan build to see them. |
| Nothing is captured while the player shows its splash | It is capturing the splash. Wait for the scene and capture again. |

## See also

- [Capture](CAPTURE.md) — the capture bar's options and reading a frame
- [Finding GPU bottlenecks](PROFILING.md) — what to do with a Unity frame once you have it
- [Direct3D 12](D3D12.md) and [Vulkan](VULKAN.md) — the backends, including what each measures
- [How to inspect a Quest application](HOWTO_QUEST.md) — a Unity player on a headset
