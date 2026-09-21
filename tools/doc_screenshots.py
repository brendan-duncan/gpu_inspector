#!/usr/bin/env python3
"""Regenerates the screenshots in docs/images.

Each shot is one run of the app with its testing aids (--debug-open, --debug-view,
--debug-select, --debug-launch-dialog, --screenshot), quitting itself when the shot is written.
The UI is the one in src/app/dist, so build it first (`npm run build` in src/app/, or `npm start` once).

    python tools/doc_screenshots.py --captures <dir>      # all of them
    python tools/doc_screenshots.py --only render-graph   # one

Shots taken from capture files need those files: name a directory holding them with --captures
(or GPU_INSPECTOR_DOC_CAPTURES). Shots whose capture is missing are skipped, so check what the run
reports: a skipped shot leaves the committed image as it was, which is how one goes stale. The captures used
are listed in SHOTS below. Most work from any .gpucap of the same shape, since they depend on
nothing beyond looking like a real frame; the ones that name a draw by its command index
(draw-overlay, mesh-view) need the capture they were taken from, or a draw index changed to suit.
"""

import argparse
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "src", "app")
IS_WIN = sys.platform == "win32"
# The launch bar's height in the window, for the shot of the bar and for the dialog crop that has
# to start below it (the bar is bright all the way across, so it would bound every dialog).
BAR_HEIGHT = 46


class Shot:
    def __init__(self, name, args, delay_ms=9000, capture=None, launch=False, crop=None, d3d12=False):
        self.name = name
        self.args = args
        self.delay_ms = delay_ms
        self.capture = capture    # a file name in --captures, substituted into args as {capture}
        self.launch = launch      # needs the built Vulkan test application
        self.d3d12 = d3d12        # needs the built Direct3D 12 one (Windows)
        self.crop = crop          # "dialog": trim the shot to the modal dialog over the dimmed page;
                                  # "bar": trim it to the launch bar across the top


def crop_dialog(path, pad=10):
    """Trims a shot of a modal dialog to the dialog itself.

    The dialog is the one solid block brighter than the dimmed page behind it, so the columns
    and rows holding many bright pixels bound it (a line of placeholder text on the page holds
    only a few). Needs Pillow; without it the shot is left whole.
    """
    try:
        from PIL import Image
    except ImportError:
        return "Pillow is not installed: left uncropped"
    im = Image.open(path)
    gray = im.convert("L")
    w, h = gray.size
    px = gray.load()
    top = BAR_HEIGHT - 1          # below the launch bar, which is bright everywhere
    cols = [sum(1 for y in range(top, h) if px[x, y] > 55) for x in range(w)]
    rows = [sum(1 for x in range(w) if px[x, y] > 55) for y in range(h)]
    xs = [x for x, c in enumerate(cols) if c > (h - top) * 0.10]
    ys = [y for y, c in enumerate(rows) if y >= top and c > w * 0.10]
    if not xs or not ys:
        return "the dialog was not found: left uncropped"
    box = (max(0, min(xs) - pad), max(0, min(ys) - pad), min(w, max(xs) + pad), min(h, max(ys) + pad))
    im.crop(box).save(path)
    return None


def crop_bar(path):
    """Trims a shot of the whole window to the launch bar across the top of it."""
    try:
        from PIL import Image
    except ImportError:
        return "Pillow is not installed: left uncropped"
    im = Image.open(path)
    im.crop((0, 0, im.width, BAR_HEIGHT)).save(path)
    return None


SHOTS = [
    # The launch bar itself, with nothing open: the controls the getting-started guide names.
    Shot("launch-bar", [], delay_ms=4000, crop="bar"),
    # The main window: a real frame, with the state of the draw it opens on.
    Shot("capture-draw", ["--debug-open={capture}"], capture="unity.gpucap"),
    # The live object list, with an image read back from the running application.
    Shot("inspect", ["--launch={triangle}", "--debug-select=VkImage"], delay_ms=12000, launch=True),
    # The reports, over a frame that has something to say.
    Shot("frame-stats", ["--debug-open={capture}", "--debug-view=stats"], capture="slow.gpucap"),
    # Bottlenecks needs a capture carrying counters, which a live capture on a GPU with
    # pipelineStatisticsQuery has and the saved ones here do not.
    Shot("bottlenecks", ["--launch={triangle}", "--debug-capture", "--debug-view=bottlenecks"],
         delay_ms=15000, launch=True),
    Shot("render-graph", ["--debug-open={capture}", "--debug-view=graph"], capture="unity.gpucap"),
    # The Timeline card. Wants a real frame: the test application's GPU work is a tenth of a
    # millisecond, which is sub-pixel against an axis wide enough to hold the CPU calls. It also
    # needs a capture taken by a layer new enough to send the CPU timeline, which the older saved
    # captures are not — unity-live.gpucap is a Unity player captured with the current one.
    Shot("timeline", ["--debug-open={capture}", "--debug-view=stats:Timeline"], delay_ms=12000,
         capture="unity-live.gpucap"),
    # Frame Bound reporting the capture's own cost rather than a bottleneck, which wants a frame
    # short enough that capturing it dominates — a player running far above its display's rate.
    Shot("frame-bound-capture-cost", ["--debug-open={capture}", "--debug-view=stats:Frame Bound"],
         delay_ms=12000, capture="unity-live.gpucap"),
    # Overdraw and pixel history replay the capture on this machine's GPU: slower. Both open on
    # the first color target, and pixel history follows its center pixel, so they want a frame
    # whose first target is small enough to see whole and has something at the middle of it —
    # which the test application's frame is, and a real one usually is not.
    Shot("overdraw", ["--launch={triangle}", "--debug-capture", "--debug-view=overdraw"],
         delay_ms=25000, launch=True),
    Shot("pixel-history", ["--launch={triangle}", "--debug-capture", "--debug-view=pixel-history"],
         delay_ms=30000, launch=True),
    # A draw that put more than one fragment on the pixel, which the history then breaks into them:
    # the cube with its culling off (test/triangle --no-cull), so the far face rasterizes at the
    # center pixel behind the near one.
    Shot("pixel-history-fragments", ["--launch={triangle}", "--args=--no-cull", "--debug-capture",
                                     "--debug-view=pixel-history"], delay_ms=30000, launch=True),
    # The Shader Flame Graph of a real frame, whose passes each run different shaders.
    Shot("flame-graph", ["--debug-open={capture}", "--debug-view=flame"], delay_ms=15000,
         capture="unity-ui.gpucap"),
    # A draw overlay over a real frame: a Unity frame whose last pass draws a menu, which is its
    # last draw -- named that way rather than by index, so it survives being taken from another
    # frame of the same player. Replayed, so slower.
    Shot("draw-overlay", ["--debug-open={capture}", "--debug-view=overlay:highlight:last"],
         delay_ms=25000, capture="unity-ui.gpucap"),
    # The mesh tab: VS In of a real mesh, and VS Out of the test application's cube inside the view
    # volume (replayed).
    #
    # The last draw rather than an index: a named command needs the very capture it was chosen in,
    # and pointed at another frame it opens on whatever that index happens to be there -- a bind or
    # a fullscreen triangle, which makes a shot saying "0 vertices" rather than failing. The menu
    # pass's own draw is the last one, and it carries a real vertex layout to show.
    Shot("mesh-view", ["--debug-open={capture}", "--debug-view=mesh:in:last"], delay_ms=12000,
         capture="unity-ui.gpucap"),
    Shot("mesh-output", ["--launch={triangle}", "--debug-capture", "--debug-view=mesh:out"],
         delay_ms=22000, launch=True),
    # The Backface Cull overlay, over a cube wound inside out so its own culling takes most of it
    # (test/triangle --inside-out); the Highlight one above has nothing to show on a solid cube.
    Shot("backface-overlay", ["--launch={triangle}", "--args=--inside-out", "--debug-capture",
                              "--debug-view=overlay:backface"], delay_ms=25000, launch=True),
    # The Direct3D 12 forms of the two, from that backend's own test application: what the D3D12
    # page shows to say the overlays and the mesh view work there as well.
    Shot("d3d12-draw-overlay", ["--launch={d3d12}", "--debug-capture", "--debug-view=overlay:depth"],
         delay_ms=25000, d3d12=True),
    Shot("d3d12-mesh-output", ["--launch={d3d12}", "--debug-capture", "--debug-view=mesh:out"],
         delay_ms=22000, d3d12=True),
    # The shader debugger, paused one line into the cube's fragment shader at a pixel the cube covers
    # (its inputs rasterized from the replayed vertex outputs).
    Shot("shader-debugger", ["--launch={triangle}", "--debug-capture", "--debug-view=debugger:pixel::1"],
         delay_ms=22000, launch=True),
    # A shader with its embedded source, in a captured draw.
    Shot("shader-source", ["--debug-open={capture}", "--debug-command=4", "--debug-expand=Fragment Shader"],
         delay_ms=12000, capture="xrstack.gpucap"),
    # The acceleration structure viewer: a top level's instances and the scene they make. Wants a
    # capture whose top level was built while watching, which rt.gpucap is (test/triangle
    # --ray-tracing rebuilds both levels every frame).
    # Named rather than taking the first structure: Instances is a top level's view, and the bottom
    # level sorts first.
    Shot("acceleration-structures", ["--debug-open={capture}", "--debug-select=VkAccelerationStructureKHR:TLAS"],
         delay_ms=12000, capture="rt.gpucap"),
    # The shader binding table of a trace (rt.gpucap, command 14 is the vkCmdTraceRaysKHR).
    Shot("binding-table", ["--debug-open={capture}", "--debug-command=14", "--debug-expand=Shader Binding Table"],
         delay_ms=12000, capture="rt.gpucap"),
    # The launch dialog, in its desktop and Android forms.
    # Named rather than left to whatever was launched last, so the shot is the same everywhere:
    # the caption calls it a Vulkan executable, and the recents of whoever runs this may not be.
    Shot("launch-dialog", ["--debug-launch-dialog=native:{triangle}"], delay_ms=5000, launch=True, crop="dialog"),
    Shot("launch-android", ["--debug-launch-dialog=android"], delay_ms=7000, crop="dialog"),
]


def electron():
    exe = os.path.join(APP, "node_modules", ".bin", "electron.cmd" if IS_WIN else "electron")
    return exe if os.path.isfile(exe) else None


def d3d12_triangle():
    """The built Direct3D 12 test application, for the shots of what only that backend does."""
    for sub_dir in ("Release", "RelWithDebInfo", "Debug", ""):
        p = os.path.join(ROOT, "build", "bin", sub_dir, "dxinsp_triangle.exe")
        if os.path.isfile(p):
            return p
    return None


def triangle():
    """The built test application, for the shots that need a running one."""
    names = ["vkinsp_triangle.exe"] if IS_WIN else ["vkinsp_triangle"]
    if sys.platform == "darwin":
        names = ["mtlinsp_triangle"]
    for sub in ("Release", "RelWithDebInfo", "Debug", ""):
        for n in names:
            p = os.path.join(ROOT, "build", "bin", sub, n)
            if os.path.isfile(p):
                return p
    return None


def run_shot(shot, out_dir, captures):
    path = os.path.join(out_dir, f"{shot.name}.png")
    args = []
    for a in shot.args:
        if "{capture}" in a:
            a = a.replace("{capture}", os.path.join(captures or "", shot.capture))
        if "{triangle}" in a:
            a = a.replace("{triangle}", triangle() or "")
        if "{d3d12}" in a:
            a = a.replace("{d3d12}", d3d12_triangle() or "")
        args.append(a)
    cmd = [electron(), ".", *args, f"--screenshot={path}", f"--screenshot-delay={shot.delay_ms}",
           "--quit-after-screenshot"]
    env = dict(os.environ)
    env.pop("ELECTRON_RUN_AS_NODE", None)   # VS Code's terminal exports it: it would start plain Node
    if os.path.exists(path):
        os.remove(path)
    started = time.time()
    proc = subprocess.Popen(cmd, cwd=APP, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        proc.wait(timeout=shot.delay_ms / 1000 + 60)
    except subprocess.TimeoutExpired:
        proc.kill()
        return f"did not quit within {shot.delay_ms / 1000 + 60:.0f} s", 0
    if not os.path.isfile(path) or os.path.getsize(path) < 1000:
        return "no screenshot was written", time.time() - started
    if shot.crop in ("dialog", "bar"):
        note = crop_dialog(path) if shot.crop == "dialog" else crop_bar(path)
        if note:
            print(f"{shot.name:<16} note: {note}")
    return None, time.time() - started


def main():
    ap = argparse.ArgumentParser(description="Regenerate the documentation screenshots.")
    ap.add_argument("--captures", default=os.environ.get("GPU_INSPECTOR_DOC_CAPTURES"),
                    help="directory holding the .gpucap files the shots use")
    ap.add_argument("--out", default=os.path.join(ROOT, "docs", "images"), help="where to write the PNGs")
    ap.add_argument("--only", action="append", help="take only these shots (repeatable)")
    ap.add_argument("--list", action="store_true", help="list the shots and exit")
    args = ap.parse_args()

    if args.list:
        for s in SHOTS:
            needs = s.capture or ("the test application" if s.launch
                                  else "the D3D12 test application" if s.d3d12 else "-")
            print(f"{s.name:<16} {needs}")
        return 0
    if not electron():
        print("electron is not installed: run npm install in src/app/", file=sys.stderr)
        return 2
    os.makedirs(args.out, exist_ok=True)

    failures = 0
    for shot in SHOTS:
        if args.only and shot.name not in args.only:
            continue
        if shot.capture and not (args.captures and os.path.isfile(os.path.join(args.captures, shot.capture))):
            print(f"{shot.name:<16} skipped: {shot.capture} not in --captures")
            continue
        if shot.launch and not triangle():
            print(f"{shot.name:<16} skipped: the test application is not built")
            continue
        if shot.d3d12 and not d3d12_triangle():
            print(f"{shot.name:<16} skipped: the Direct3D 12 test application is not built")
            continue
        error, secs = run_shot(shot, args.out, args.captures)
        if error:
            print(f"{shot.name:<16} FAILED: {error} ({secs:.0f} s)")
            failures += 1
        else:
            size = os.path.getsize(os.path.join(args.out, f"{shot.name}.png"))
            print(f"{shot.name:<16} ok ({size // 1024} KB, {secs:.0f} s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
