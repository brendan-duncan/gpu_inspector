#!/usr/bin/env python3
"""Regenerates the screenshots in docs/images.

Each shot is one run of the app with its testing aids (--debug-open, --debug-view,
--debug-select, --debug-launch-dialog, --screenshot), quitting itself when the shot is written.
The UI is the one in app/dist, so build it first (`npm run build` in app/, or `npm start` once).

    python tools/doc_screenshots.py --captures <dir>      # all of them
    python tools/doc_screenshots.py --only render-graph   # one

Shots taken from capture files need those files: name a directory holding them with --captures
(or GPU_INSPECTOR_DOC_CAPTURES). Shots whose capture is missing are skipped. The captures used
are listed in SHOTS below; any .gpucap of the same shape works, since nothing here depends on
their contents beyond looking like a real frame.
"""

import argparse
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "app")
IS_WIN = sys.platform == "win32"


class Shot:
    def __init__(self, name, args, delay_ms=9000, capture=None, launch=False, crop=None):
        self.name = name
        self.args = args
        self.delay_ms = delay_ms
        self.capture = capture    # a file name in --captures, substituted into args as {capture}
        self.launch = launch      # needs the built test application
        self.crop = crop          # "dialog": trim the shot to the modal dialog over the dimmed page


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
    grey = im.convert("L")
    w, h = grey.size
    px = grey.load()
    top = 45                      # below the launch bar, which is bright everywhere
    cols = [sum(1 for y in range(top, h) if px[x, y] > 55) for x in range(w)]
    rows = [sum(1 for x in range(w) if px[x, y] > 55) for y in range(h)]
    xs = [x for x, c in enumerate(cols) if c > (h - top) * 0.10]
    ys = [y for y, c in enumerate(rows) if y >= top and c > w * 0.10]
    if not xs or not ys:
        return "the dialog was not found: left uncropped"
    box = (max(0, min(xs) - pad), max(0, min(ys) - pad), min(w, max(xs) + pad), min(h, max(ys) + pad))
    im.crop(box).save(path)
    return None


SHOTS = [
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
    # Overdraw and pixel history replay the capture on this machine's GPU: slower. Both open on
    # the first colour target, and pixel history follows its centre pixel, so they want a frame
    # whose first target is small enough to see whole and has something at the middle of it —
    # which the test application's frame is, and a real one usually is not.
    Shot("overdraw", ["--launch={triangle}", "--debug-capture", "--debug-view=overdraw"],
         delay_ms=25000, launch=True),
    Shot("pixel-history", ["--launch={triangle}", "--debug-capture", "--debug-view=pixel-history"],
         delay_ms=30000, launch=True),
    # A shader with its embedded source, in a captured draw.
    Shot("shader-source", ["--debug-open={capture}", "--debug-command=4", "--debug-expand=Fragment Shader"],
         delay_ms=12000, capture="xrstack.gpucap"),
    # The launch dialog, in its desktop and Android forms.
    Shot("launch-dialog", ["--debug-launch-dialog"], delay_ms=5000, crop="dialog"),
    Shot("launch-android", ["--debug-launch-dialog=android"], delay_ms=7000, crop="dialog"),
]


def electron():
    exe = os.path.join(APP, "node_modules", ".bin", "electron.cmd" if IS_WIN else "electron")
    return exe if os.path.isfile(exe) else None


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
    if shot.crop == "dialog":
        note = crop_dialog(path)
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
            print(f"{s.name:<16} {s.capture or ('the test application' if s.launch else '-')}")
        return 0
    if not electron():
        print("electron is not installed: run npm install in app/", file=sys.stderr)
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
