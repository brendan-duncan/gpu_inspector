"""
End-to-end checks of the inspector against the triangle test application and saved captures:

    python tools/ui_tests.py                      # the triangle cases (needs the built layer, app and UI)
    python tools/ui_tests.py --captures <dir>     # also opens every .gpucap in <dir>
    python tools/ui_tests.py --only hazard,msaa   # a subset
    python tools/ui_tests.py --keep               # keep the logs, dumps and screenshots

Each case runs the Electron UI once with the testing flags (--launch or --debug-open, --debug-capture,
--debug-dump, --debug-view, --debug-expand, --debug-settle, --screenshot, --quit-after-screenshot), then checks the JSON
screenshot time (sessions, captures, frame findings, validation links, symbols) and the layer's
log. A capture directory may hold `<name>.expect.json` next to `<name>.gpucap` with the findings
expected of it ({"findings": {"rule": count, ...}}); without one the file only has to open with
commands and without texture errors. Exit code 1 when a case fails.
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "app")
IS_WIN = sys.platform.startswith("win")


def find_triangle():
    for c in [os.path.join(ROOT, "build", "bin", "Release", "vkinsp_triangle.exe"),
              os.path.join(ROOT, "build", "bin", "vkinsp_triangle.exe"),
              os.path.join(ROOT, "build", "bin", "vkinsp_triangle")]:
        if os.path.isfile(c):
            return c
    return None


def find_replay():
    for c in [os.path.join(ROOT, "build", "bin", "Release", "vkinsp_replay.exe"),
              os.path.join(ROOT, "build", "bin", "vkinsp_replay.exe"),
              os.path.join(ROOT, "build", "bin", "vkinsp_replay")]:
        if os.path.isfile(c):
            return c
    return None


def electron():
    exe = os.path.join(APP, "node_modules", ".bin", "electron.cmd" if IS_WIN else "electron")
    return exe if os.path.isfile(exe) else None


class Case:
    def __init__(self, name, args, checks, delay_ms=12000, companion=None, before=None, after=None):
        self.name = name
        self.args = args
        self.checks = checks      # callable(dump, log) -> list of failure strings
        self.delay_ms = delay_ms
        self.companion = companion  # callable() -> Popen, started a few seconds after the UI (an app it did not launch)
        self.before = before        # callable() run before the UI starts (registration)
        self.after = after          # callable() run when the UI has quit (cleanup)


def run_case(case, work, keep):
    dump = os.path.join(work, f"{case.name}.json")
    log = os.path.join(work, f"{case.name}.log")
    shot = os.path.join(work, f"{case.name}.png")
    for p in (dump, log, shot):
        if os.path.exists(p):
            os.remove(p)
    cmd = [electron(), ".", *case.args, f"--debug-dump={dump}", f"--debug-log={log}", f"--screenshot={shot}",
           f"--screenshot-delay={case.delay_ms}", "--quit-after-screenshot"]
    env = dict(os.environ)
    env.pop("ELECTRON_RUN_AS_NODE", None)   # VS Code's terminal exports it, which would start plain Node
    started = time.time()
    if case.before:
        case.before()
    proc = subprocess.Popen(cmd, cwd=APP, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    companion = None
    try:
        if case.companion:
            time.sleep(4)
            companion = case.companion()
        proc.wait(timeout=case.delay_ms / 1000 + 90)
    except subprocess.TimeoutExpired:
        proc.kill()
        return [f"the UI did not quit within {case.delay_ms / 1000 + 90:.0f} s"], time.time() - started
    finally:
        if companion:
            companion.kill()
        if case.after:
            case.after()
    failures = []
    if not os.path.isfile(dump):
        return ["no dump was written (did the UI start?)"], time.time() - started
    with open(dump, encoding="utf-8") as f:
        state = json.load(f)
    log_text = ""
    if os.path.isfile(log):
        with open(log, encoding="utf-8", errors="replace") as f:
            log_text = f.read()
    if not os.path.isfile(shot) or os.path.getsize(shot) < 1000:
        failures.append("no screenshot")
    if "error" in state:
        failures.append(f"dump failed: {state['error']}")
    else:
        failures += case.checks(state, log_text)
    return failures, time.time() - started


# ------------------------------------------------------------------------------------------ checks

def session(state):
    sessions = state.get("sessions") or []
    return sessions[0] if sessions else {}


def capture(state):
    caps = session(state).get("captures") or []
    return caps[0] if caps else {}


def findings(state):
    out = {}
    for f in capture(state).get("findings") or []:
        out[f["rule"]] = out.get(f["rule"], 0) + f.get("count", 1)
    return out


def expect(cond, message):
    return [] if cond else [message]


def check_connected(state, log):
    s = session(state)
    return expect(s.get("state") == "connected", f"session state is {s.get('state')!r} ({s.get('detail')})") + \
        expect((s.get("objects") or 0) > 20, f"only {s.get('objects')} objects")


def check_capture_basic(state, log, min_draws=1, textures=2, timings=True):
    c = capture(state)
    return expect(bool(c), "no capture tab") + \
        expect((c.get("commands") or 0) > 5, f"{c.get('commands')} commands captured") + \
        expect((c.get("draws") or 0) >= min_draws, f"{c.get('draws')} draws") + \
        expect((c.get("textures") or 0) >= textures, f"{c.get('textures')} render targets read back") + \
        expect((c.get("textureErrors") or 0) == 0, f"{c.get('textureErrors')} render targets failed to read back") + \
        expect((c.get("texturesLoaded") or 0) == (c.get("textures") or 0), "not every render target's data arrived") + \
        expect(not timings or (c.get("passTimings") or 0) >= 1, "no pass timings") + \
        expect(not timings or (c.get("passCounters") or 0) >= 1,
               "no pass carried GPU counters: the layer's pipeline statistics query (layer/src/pipeline_stats.h) "
               "is what the GPU Bottlenecks report is built from")


def triangle_plain(state, log):
    f = findings(state)
    s = session(state)
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect(set(f) <= {"depth-transient"}, f"unexpected findings {f}") + \
        expect(s.get("refreshSource") in ("present_timing", "display_timing", "monitor", "estimate"), f"refresh source {s.get('refreshSource')!r}") + \
        expect("capture finishing" in log, "the layer never finished the capture") + \
        expect("DONT_CARE store ops forced to STORE" in log, "the depth attachment was not stored for the read-back")


def triangle_msaa(state, log):
    c = capture(state)
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect("resolve" in log or (c.get("textures") or 0) >= 2, "no multisampled read-back")


def triangle_offscreen(state, log):
    s = session(state)
    # Nothing presents in this mode, so the render pass stores a colour attachment the capture
    # never sees read: the render graph's rules (render_graph_analysis.ts) must say so, which also
    # checks that they run at all and that their findings reach the capture's finding list.
    return expect("unread-store" in findings(state), f"no unread-store finding for the offscreen target: {findings(state)}") + \
        check_connected(state, log) + check_capture_basic(state, log, textures=1) + \
        expect(s.get("frameBoundary") == "wait", f"frame boundary {s.get('frameBoundary')!r} (expected the fence wait)") + \
        expect("no present after" in log, "the layer did not switch its frame boundary") + \
        expect(s.get("refreshSource") == "estimate", f"refresh source {s.get('refreshSource')!r} (expected the estimate)")


def triangle_scissor(state, log):
    s = session(state)
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect((s.get("validationErrors") or 0) >= 1, "no validation error from --bad-scissor") + \
        expect((s.get("validationLinked") or 0) >= 1, "no validation message linked to a command") + \
        expect((capture(state).get("commandsWithValidation") or 0) >= 1, "no captured command carries a validation message")


def triangle_hazard(state, log):
    s = session(state)
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect("SYNC-HAZARD" in log, "no synchronization hazard reported") + \
        expect((s.get("validationLinked") or 0) >= 1, "no hazard linked to a command") + \
        expect((s.get("validation") or 0) < 50, f"{s.get('validation')} distinct validation messages (the per-submission counters were not folded)")


def graph(state):
    return capture(state).get("renderGraph") or {}


def triangle_graph(state, log):
    g = graph(state)
    # The frame is a compute pass that fills the wave buffer and a render pass that draws the cube
    # into the swapchain image, so the graph must see both, the resources they touch, and the
    # dependency the --hazard mode adds: vkCmdUpdateBuffer writes the vertex buffer the draw reads.
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect(bool(g), "no render graph in the capture state") + \
        expect((g.get("nodes") or 0) >= 9, f"{g.get('nodes')} graph nodes (a transfer, a compute pass and a render pass per captured frame expected)") + \
        expect((g.get("resources") or 0) >= 5, f"{g.get('resources')} graph resources") + \
        expect((g.get("edges") or 0) >= 1, "no dependency between passes: the vertex buffer update should feed the draw") + \
        expect((g.get("unreadNodes") or 0) >= 1, "the compute pass writes a buffer nothing reads and was not reported as such") + \
        expect(g.get("untimedNodes") == 0, f"{g.get('untimedNodes')} graph passes have no timing: the graph's pass keys no longer match the command tree's")


def triangle_bottlenecks(state, log):
    c = capture(state)
    # The counters the report divides out have to survive the whole path: the layer's query, the
    # protocol, and the UI's pass keying (a pass whose key does not resolve carries no counters).
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect("with counters" in log, "the layer never reported pass counters") + \
        expect((c.get("passCounters") or 0) >= 1, f"{c.get('passCounters')} passes carried counters") + \
        expect((c.get("passDepthRejection") or 0) >= 1,
               "no pass carried fragmentsPassed: the layer's occlusion query around each pass "
               "(layer/src/capture.cpp) is what the late-depth-rejection rule needs")


def triangle_overdraw(state, log):
    c = capture(state)
    t = c.get("textureTab") or {}
    h = t.get("history") or {}
    touched = h.get("touched") or []
    # The whole render target tab (renderer/capture_texture_view.ts) in one run: the Overdraw report
    # replays the capture with vkinsp_replay, opens the pass's target with the heat over it, and the
    # click follows that pixel through the frame in the pane beside the image.
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect((c.get("overdraw") or 0) >= 2, f"{c.get('overdraw')} overdraw measurements (the replay takes two per pass)") + \
        expect((c.get("overdrawCounts") or 0) >= 2, f"{c.get('overdrawCounts')} measurements carry per-pixel counts") + \
        expect(bool(t), "the Overdraw report opened no render target tab") + \
        expect(t.get("overdraw") is True, "the tab did not open with the overdraw overlay on") + \
        expect(t.get("measured") is True and t.get("counts") is True, f"the tab's pass has no counts to draw over the image: {t}") + \
        expect(bool(t.get("picked")), "the click on the image picked no pixel") + \
        expect(not h.get("error"), f"the pixel history failed: {h.get('error')}") + \
        expect(bool(touched), f"the pixel history lists no events: {h}") + \
        expect(any("begins" in e for e in touched), f"no pass start in the pixel history: {touched}")


def triangle_overlay(state, log):
    c = capture(state)
    t = c.get("textureTab") or {}
    d = t.get("drawOverlay") or {}
    # A draw overlay end to end: the draw replayed on its own (replay/src/overlay.cpp), its mask
    # parsed, and the render target tab drawing it over the image. With --occluded the cube is drawn
    # twice in place, so the second draw's fragments all fail the depth test.
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect(bool(t), "--debug-view=overlay opened no render target tab") + \
        expect(t.get("overlay") == "depth", f"the tab did not open with the depth test overlay: {t.get('overlay')}") + \
        expect(t.get("draw") is not None, "the tab chose no draw") + \
        expect(not t.get("drawError"), f"the overlay replay failed: {t.get('drawError')}") + \
        expect(d.get("measured") is True and d.get("mask") is True, f"the draw has no mask: {d}") + \
        expect((d.get("pixelsCovered") or 0) > 0, f"the draw covers no pixels: {d}") + \
        expect(d.get("pixelsPassed") == 0 and d.get("pixelsRejected") == d.get("pixelsCovered"),
               f"the second draw of the same cube should fail the depth test everywhere: {d}") + \
        expect(d.get("wireframe") is True, f"no wireframe was drawn: {d}")


def triangle_stacks(state, log):
    c = capture(state)
    s = session(state)
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect((c.get("commandsWithStacks") or 0) >= 5, f"{c.get('commandsWithStacks')} commands carry stacks") + \
        expect((s.get("symbolsWithLines") or 0) >= 1, "no symbol resolved to a source line (PDB next to the app?)")


def triangle_prerecord(state, log):
    s = session(state)
    # Pre-recorded buffers hold no timestamps: recorded before the capture's query pool existed.
    return check_connected(state, log) + check_capture_basic(state, log, timings=False) + \
        expect("read back after their submission" in log, "the pre-recorded passes were not read back after submission") + \
        expect((s.get("validationErrors") or 0) == 0, f"{s.get('validationErrors')} validation errors with pre-recorded buffers")


def implicit_layer(on):
    env = dict(os.environ)
    env.pop("ELECTRON_RUN_AS_NODE", None)
    subprocess.run([electron(), ".", f"--implicit-layer={'on' if on else 'off'}"], cwd=APP, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60)


def triangle_implicit(state, log):
    s = session(state)
    return expect(s.get("state") == "connected", f"the waiting session is {s.get('state')!r} ({s.get('detail')})") + \
        expect("waiting for an application" in log, "the session did not wait for an application") +         check_capture_basic(state, log)


def triangle_sources(state, log):
    s = session(state)
    # The compute shader ships with line information only (tools/strip_shader_source.py); the
    # dispatch's Compute Shader section, expanded by the click, fetches wave.comp from the root.
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect((s.get("hostSources") or 0) >= 1, "the compute shader's source was not found under the source root")


def triangle_cases(triangle):
    launch = [f"--launch={triangle}"]
    source_root = os.path.join(ROOT, "test")

    def start_triangle():
        env = dict(os.environ, VKINSP_ENABLE="1", VKINSP_PORT="47531")
        return subprocess.Popen([triangle, "--frames", "5000"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    cases = [
        # The implicit layer: registered for the case, the triangle started outside the inspector.
        Case("implicit", ["--wait-for-app", "--port=47531", "--debug-capture"], triangle_implicit, delay_ms=16000,
             companion=start_triangle, before=lambda: implicit_layer(True), after=lambda: implicit_layer(False)),
        Case("plain", launch + ["--debug-capture"], triangle_plain),
        Case("sources", launch + [f"--source-roots={source_root}", "--debug-capture", "--debug-command=5",
                                  "--debug-expand=Compute Shader"], triangle_sources, delay_ms=14000),
        Case("prerecord", launch + ["--args=--prerecord", "--record-always", "--validation", "--debug-capture"], triangle_prerecord, delay_ms=16000),
        Case("msaa", launch + ["--args=--msaa", "--debug-capture"], triangle_msaa),
        Case("offscreen", launch + ["--args=--offscreen", "--debug-capture"], triangle_offscreen, delay_ms=14000),
        Case("scissor", launch + ["--args=--bad-scissor", "--validation", "--debug-capture"], triangle_scissor, delay_ms=16000),
        Case("hazard", launch + ["--args=--hazard", "--validation", "--sync-validation", "--debug-capture"], triangle_hazard, delay_ms=18000),
        Case("stacks", launch + ["--debug-capture", "--debug-capture-stacks", "--debug-command=9", "--debug-expand-stacks"], triangle_stacks, delay_ms=16000),
        # The render graph, on the frames whose passes have a dependency to find, with the view
        # open so the screenshot shows the chart. Three frames, because the pass indices the graph
        # keys its timings by have to restart per frame the way the command tree's do.
        Case("graph", launch + ["--args=--hazard", "--debug-capture=3", "--debug-view=graph"], triangle_graph, delay_ms=16000),
        # The GPU Bottlenecks report rendering at all: a throw while building it would leave the
        # details pane empty and the renderer's console with the error.
        Case("bottlenecks", launch + ["--debug-capture", "--debug-view=bottlenecks"], triangle_bottlenecks, delay_ms=16000),
    ]
    # The render target tab measures overdraw and follows a pixel by replaying the capture, so this
    # one only runs where vkinsp_replay is built (replay/, docs/REPLAY.md). The click lands on the
    # image, which fits its pane; --debug-settle waits for the replay the click set going.
    if find_replay():
        cases.append(Case("overdraw", launch + ["--debug-capture", "--debug-view=overdraw",
                                                "--debug-mouse=340,560", "--debug-settle=8000"],
                          triangle_overdraw, delay_ms=20000))
        cases.append(Case("overlay", launch + ["--args=--occluded", "--debug-capture", "--debug-view=overlay:depth:last",
                                               "--debug-settle=8000"],
                          triangle_overlay, delay_ms=20000))
    else:
        print("  (no vkinsp_replay build: skipping the overdraw and overlay cases)")
    return cases


def capture_case(path):
    name = "open_" + os.path.splitext(os.path.basename(path))[0]
    expect_file = os.path.splitext(path)[0] + ".expect.json"
    expected = None
    if os.path.isfile(expect_file):
        with open(expect_file, encoding="utf-8") as f:
            expected = json.load(f)

    def check(state, log):
        c = capture(state)
        out = expect(bool(c), "no capture tab") + expect((c.get("commands") or 0) > 0, "no commands") + \
            expect((c.get("textureErrors") or 0) == 0, f"{c.get('textureErrors')} render targets in error")
        if expected and "findings" in expected:
            got = findings(state)
            for rule, count in expected["findings"].items():
                if got.get(rule, 0) != count:
                    out.append(f"finding {rule}: expected {count}, got {got.get(rule, 0)}")
            extra = set(got) - set(expected["findings"])
            if extra:
                out.append(f"unexpected findings {sorted(extra)}")
        return out
    return Case(name, [f"--debug-open={path}", "--debug-mouse=378,220"], check, delay_ms=9000)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--captures", help="directory of .gpucap files to open")
    ap.add_argument("--only", help="comma-separated case names")
    ap.add_argument("--keep", action="store_true", help="keep the work directory")
    ap.add_argument("--no-triangle", action="store_true", help="skip the live triangle cases")
    args = ap.parse_args()
    if not electron():
        print("electron not installed: run npm install in app/", file=sys.stderr)
        return 2
    cases = []
    if not args.no_triangle:
        triangle = find_triangle()
        if not triangle:
            print("vkinsp_triangle not built (cmake --build build --config Release --target vkinsp_triangle)", file=sys.stderr)
            return 2
        cases += triangle_cases(triangle)
    if args.captures:
        for p in sorted(glob.glob(os.path.join(args.captures, "*.gpucap"))):
            cases.append(capture_case(p))
    if args.only:
        wanted = {n.strip() for n in args.only.split(",")}
        cases = [c for c in cases if c.name in wanted]
    if not cases:
        print("no cases selected", file=sys.stderr)
        return 2
    work = tempfile.mkdtemp(prefix="gpuinsp_ui_")
    print(f"work directory: {work}")
    failed = 0
    for case in cases:
        failures, seconds = run_case(case, work, args.keep)
        status = "PASS" if not failures else "FAIL"
        print(f"{status}  {case.name}  ({seconds:.0f} s)")
        for f in failures:
            print(f"      - {f}")
        failed += bool(failures)
    print(f"{len(cases) - failed} of {len(cases)} cases passed")
    if not args.keep and not failed:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
