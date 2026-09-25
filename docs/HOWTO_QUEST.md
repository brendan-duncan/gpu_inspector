# How to inspect a Quest application

A Quest headset is an Android device that runs OpenXR applications, and GPU Inspector reaches it
the way it reaches any Android device: over adb, with the capture layer installed on the device and
turned on for one package. [Android and Quest](ANDROID.md) is the setup and the reference; this is
the order to do it in, and the three things about a headset that will otherwise cost you an
afternoon.

A Unity build, a native OpenXR application and the test package below all work the same way, as
long as they render with **Vulkan** (or OpenGL ES on Android 10 and newer).

![The launch dialog set to an Android device, with the device, package, activity and symbol directory fields](images/launch-android.png)

## Steps

1. Put the headset on, or cover its proximity sensor. Nothing below works while it is idle.
2. Connect it over USB and accept the debugging prompt **in the headset**.
3. Check the device is there: `adb devices`. A Quest shows as an ordinary device.
4. In the inspector, press **Launch...** and set *Run On* to **Android device (adb)**.
5. Pick the device, then the package. **Refresh** looks again.
6. Set *Graphics API* to **Vulkan** (what almost every headset application renders with).
7. Leave *Activity* empty for the package's launcher activity.
8. Press **Launch**. The inspector installs the layer APK if the device does not have this version,
   turns Android's GPU debug layer settings on for that package, starts it and connects.

From a shell:

```
GPUInspector.exe --launch-android=com.example.game --device=<serial>
```

Put the headset on and let the application render; then capture from the **Capture** tab.

## Start with the test package

Before pointing this at a real application, run the one built for it — it takes a minute and tells
you whether the setup works:

```
python tools/build_xr_triangle.py
```

That builds `xr_triangle.apk` (a ring of triangles in one multiview pass) and
`xr_triangle_slow.apk` (the same scene rendered badly on purpose: a pass per eye, a stored depth
buffer, a draw and a bind per triangle, a wasteful fragment shader). Install one with
`adb install -r build/android/xr_triangle_slow.apk` and launch it from the dialog like any other
package. The slow one is the better first capture: **Frame Stats** and **Analyze Shaders** have
something to say about it, so you can tell the reports are working.

## The three things about a headset

- **The headset must be awake and worn.** An OpenXR session stays idle until the headset is on a
  head, so the application renders nothing until then — and a capture of nothing waits forever.
  Covering the proximity sensor does just as well as wearing it.
- **Controllers.** The Quest shell refuses to launch an application until controllers or tracked
  hands are on, and the launch-check dialog it leaves behind blocks later launches until the shell
  restarts. If launches start failing for no clear reason, take the headset off and put it back on.
- **The frames have no present.** An OpenXR application never presents: the runtime composites its
  layers. The layer notices after sixty submissions without a present and ends frames at the
  application's fence wait after a submission instead, so **Capture** still captures one frame.
  Nothing is needed from you, but it is why the frame boundary in the log is a submit rather than a
  present.

After a launch the inspector reads `dumpsys power` and `dumpsys window` and logs whichever of these
is in the way, so the **Log** tab usually names the problem.

## What an XR frame looks like

A well-built XR frame renders both eyes in **one multiview pass**, with the view index chosen in
the shader (`gl_ViewIndex`). The capture reads back every layer in the pass's view mask, so both
eyes are in the thumbnail strip and the image viewer's layer control switches between them.

The frame rules flag what costs a headset most: a pass per eye where one multiview pass would do,
a depth buffer stored when nothing reads it, a clear that a load op would have done, and draws
small enough that the bind around them costs more than the draw. **Frame Stats** lists them against
the commands they are about.

A Quest's GPU is a tiled one, so **Reports → Tile-Based GPUs** is the report to read next: the
bytes each pass moves between tile memory and DRAM, and which of them the frame could keep on chip
([Tile-Based GPUs](REPORTS.md#tile-based-gpus)). The eye buffers show there as stored and read by
nothing in the capture — they go to the XR compositor, which the capture does not see — and are
not counted as avoidable; eye depth stored for nothing is.

## What capturing costs

Expect the captured frame to take noticeably longer than the frames around it: every render target
is read back at the end of its pass, and a tiled mobile GPU pays for that in bandwidth. A Quest
stereo pass measured 5.1 ms captured against 2.9 ms running free. This affects the frame being
captured, not the frames before or after it, and the pass timings of a capture include it — so read
a capture's pass times as "what this pass costs plus what reading it back costs", and use a
[timing capture](PROFILING.md) when the question is what the application's own frames cost.

Turning **Render targets** off in the capture bar removes most of that cost when the frame's
commands, not its pictures, are what you are after.

## If it does not work

| What you see | What it means |
|---|---|
| The application never starts, or the shell shows a dialog | Controllers or hands are not on. Take the headset off and put it back on, then try again. |
| The session sits on *connecting* | The headset is idle: put it on, or cover the sensor. |
| `adb devices` shows nothing | The debugging prompt was not accepted in the headset, or the cable is charge-only. |
| Nothing is captured, and the log has no frame boundary | The application renders through OpenXR without presenting; wait for the sixty-submission fallback, or capture again once it has kicked in. |

The rest is in [Troubleshooting](TROUBLESHOOTING.md#android).

## See also

- [Android and Quest](ANDROID.md) — the requirements, building the layer, symbol directories and
  what works the same as on the desktop
- [How to inspect a Unity player](HOWTO_UNITY.md) — for a Unity build, including which backend to
  prefer
- [Finding GPU bottlenecks](PROFILING.md) — reading an XR frame's timings and counters
