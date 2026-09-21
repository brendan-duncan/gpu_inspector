"""
End-to-end checks of the inspector against the triangle test application and saved captures:

    python tools/ui_tests.py                      # the triangle cases (needs the built layer, app and UI)
    python tools/ui_tests.py --captures <dir>     # also opens every .gpucap in <dir>
    python tools/ui_tests.py --only hazard,msaa   # a subset
    python tools/ui_tests.py --keep               # keep the logs, dumps and screenshots

Each case runs the Electron UI once with the testing flags (--launch or --debug-open, --debug-capture,
--debug-dump, --debug-view, --debug-expand, --debug-export, --debug-settle, --screenshot, --quit-after-screenshot), then checks the JSON
screenshot time (sessions, captures, frame findings, validation links, symbols) and the layer's
log. A capture directory may hold `<name>.expect.json` next to `<name>.gpucap` with the findings
expected of it ({"findings": {"rule": count, ...}}); without one the file only has to open with
commands and without texture errors. Exit code 1 when a case fails.
"""
import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "src", "app")
IS_WIN = sys.platform.startswith("win")


def find_triangle():
    for c in [os.path.join(ROOT, "build", "bin", "Release", "vkinsp_triangle.exe"),
              os.path.join(ROOT, "build", "bin", "vkinsp_triangle.exe"),
              os.path.join(ROOT, "build", "bin", "vkinsp_triangle")]:
        if os.path.isfile(c):
            return c
    return None


def find_metal_triangle():
    """The Metal sample (test/metal_triangle), which only exists on macOS."""
    if sys.platform != "darwin":
        return None
    path = os.path.join(ROOT, "build", "bin", "mtlinsp_triangle")
    return path if os.path.isfile(path) else None


def find_metal_path_tracer():
    """mtlinsp_path_tracer (test/path_tracer/metal): the bounding-box and intersection-function
    half of Metal ray tracing, where metal_triangle --ray-tracing is the triangle half."""
    if sys.platform != "darwin":
        return None
    path = os.path.join(ROOT, "build", "bin", "mtlinsp_path_tracer")
    return path if os.path.isfile(path) else None


def find_d3d12_triangle():
    """The Direct3D 12 sample (test/d3d12_triangle), which only builds on Windows."""
    if sys.platform != "win32":
        return None
    for c in [os.path.join(ROOT, "build", "bin", "Release", "dxinsp_triangle.exe"),
              os.path.join(ROOT, "build", "bin", "dxinsp_triangle.exe")]:
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


def find_d3d12_replay():
    c = os.path.join(ROOT, "build", "bin", "Release", "dxinsp_replay.exe")
    return c if os.path.isfile(c) else None


def find_metal_replay():
    """mtlinsp_replay (src/metal/replay/), which Export to C++ on a Metal capture needs."""
    c = os.path.join(ROOT, "build", "bin", "mtlinsp_replay")
    return c if os.path.isfile(c) else None


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
               "no pass carried GPU counters: the layer's pipeline statistics query (src/vulkan/src/pipeline_stats.h) "
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


def triangle_stencil(textures):
    # The depth buffer has a stencil aspect: the pass's target list gains a stencil read-back
    # beside the depth one (both aspects through the resolve with --msaa), stored though the
    # application's stencilStoreOp is DONT_CARE, with no validation error from any of it.
    def check(state, log):
        c = capture(state)
        s = session(state)
        return check_connected(state, log) + check_capture_basic(state, log, textures=textures) + \
            expect((c.get("textures") or 0) == textures, f"{c.get('textures')} textures (expected {textures}, the stencil read-back among them)") + \
            expect((s.get("validationErrors") or 0) == 0, f"{s.get('validationErrors')} validation errors")
    return check


def triangle_suspend(state, log):
    c = capture(state)
    s = session(state)
    # The cube's pass is suspended at the end of the frame's command buffer and resumed in a second
    # one. Nothing may be recorded between the two parts, so the layer reads the attachments back
    # and records the bound buffers' copies (both parts', 7 ranges) after the resumed part only, and
    # times neither part; the compute pass before it still is. The Khronos layer has no check for
    # the rule, so a clean validation log is necessary but not sufficient: the counts are the test.
    return check_connected(state, log) + check_capture_basic(state, log, textures=2, timings=False) + \
        expect((c.get("passes") or 0) >= 2, f"{c.get('passes')} passes (expected the suspended and the resumed part)") + \
        expect((c.get("textures") or 0) == 3, f"{c.get('textures')} textures (expected the resumed part's 2 attachments and the sampled texture)") + \
        expect((c.get("buffers") or 0) >= 7, f"{c.get('buffers')} buffer ranges (the suspended part's must be recorded by the resumed part)") + \
        expect((c.get("passTimings") or 0) == 1, f"{c.get('passTimings')} pass timings (expected the compute pass only)") + \
        expect("suspended and resumed" in log, "the layer did not report the suspended pass") + \
        expect((s.get("validationErrors") or 0) == 0, f"{s.get('validationErrors')} validation errors")


def triangle_offscreen(state, log):
    s = session(state)
    # Nothing presents in this mode, so the render pass stores a color attachment the capture
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


def triangle_oob(state, log):
    """GPU-assisted validation: the shader reads past its storage buffer, and the message that
    reports it arrives after the capture — it is about a submission that has only now finished —
    so linking it to the dispatch it names needs what the capture recorded to outlive it."""
    s = session(state)
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect("access out of bounds" in log, "no out-of-bounds access reported by GPU validation") + \
        expect((s.get("validationLinked") or 0) >= 1, "no GPU validation message linked to a command") + \
        expect((capture(state).get("commandsWithValidation") or 0) >= 1, "no captured command carries the message") + \
        expect((s.get("validation") or 0) < 50, f"{s.get('validation')} distinct validation messages (one per shader invocation was not folded)")


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
               "(src/vulkan/src/capture.cpp) is what the late-depth-rejection rule needs")


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


def triangle_pixel_history(state, log):
    h = (capture(state).get("textureTab") or {}).get("history") or {}
    touched = h.get("touched") or []
    # The pixel history of the center of the first color target (--debug-view=pixel-history, no
    # click needed): the pass it starts from, the draw that wrote it, and which of that draw's
    # primitives the winning fragment came from (the replay's primitive-id pass).
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect(not h.get("error"), f"the pixel history failed: {h.get('error')}") + \
        expect(bool(touched), f"the pixel history lists no events: {h}") + \
        expect(any("begins" in e for e in touched), f"no pass start in the pixel history: {touched}") + \
        expect(any("wrote the pixel" in e for e in touched), f"no draw wrote the pixel: {touched}") + \
        expect(any("primitive" in e for e in touched),
               f"no draw names the primitive that won the pixel (the primitive-id pass): {touched}")


def triangle_backface_overlay(state, log):
    # The Backface Cull overlay (src/replay/src/overlay.cpp, ReissueMode::BackFace): the draw is
    # issued again with nothing culled and a shader that writes only for back faces, so the pixels
    # where its own culling left nothing are marked. --inside-out draws half the cube wound the
    # other way, which is a draw that culls away to nothing -- the bug the overlay is for.
    t = (capture(state).get("textureTab") or {})
    d = t.get("drawOverlay") or {}
    return check_connected(state, log) + \
        expect(t.get("overlay") == "backface", f"the tab did not open with the backface overlay: {t.get('overlay')}") + \
        expect(not t.get("drawError"), f"the overlay failed: {t.get('drawError')}") + \
        expect(d.get("backFaceTested") is True, f"the cull-off run was not made: {d}") + \
        expect((d.get("pixelsBackFacing") or 0) > 0,
               f"the reversed half of the cube should leave pixels its culling removed: {d}") + \
        expect((d.get("pixelsCovered") or 0) > 0, f"the rest of it should still be drawn: {d}")


def triangle_stencil_overlay(state, log):
    # The Stencil Test overlay: the draw re-issued with its stencil test alone, which needs a pass
    # with a stencil aspect (--stencil). This sample's draw compares ALWAYS, so nothing is rejected;
    # what the case holds is that the run is made and reports against the stencil rather than
    # falling back to the depth test's answer.
    t = (capture(state).get("textureTab") or {})
    d = t.get("drawOverlay") or {}
    return check_connected(state, log) + \
        expect(t.get("overlay") == "stencil", f"the tab did not open with the stencil overlay: {t.get('overlay')}") + \
        expect(not t.get("drawError"), f"the overlay failed: {t.get('drawError')}") + \
        expect(d.get("stencilTested") is True, f"the stencil-only run was not made on a pass with a stencil: {d}") + \
        expect((d.get("pixelsCovered") or 0) > 0, f"the draw covers no pixels: {d}") + \
        expect(d.get("pixelsStencilRejected") == 0,
               f"this sample's stencil test compares ALWAYS, so it rejects nothing: {d}")


def triangle_pixel_fragments(state, log):
    # The fragments of one draw (src/replay/src/history.cpp, the fragment round). With --no-cull the
    # cube keeps its back faces, so the one draw puts two fragments on the center pixel: the near
    # face and the far one, each from a different primitive, and the pixel keeps the one that won
    # the depth test -- which is the primitive the draw's own entry names.
    h = (capture(state).get("textureTab") or {}).get("history") or {}
    fragments = h.get("fragments") or []
    first = fragments[0] if fragments else {}
    primitives = first.get("primitives") or []
    return check_connected(state, log) + \
        expect(not h.get("error"), f"the pixel history failed: {h.get('error')}") + \
        expect(len(fragments) == 1, f"one draw should have been broken into fragments: {fragments}") + \
        expect(len(primitives) == 2, f"the draw put two fragments on the pixel: {first}") + \
        expect(len(set(primitives)) == 2, f"the two fragments should come from different primitives: {primitives}") + \
        expect(first.get("values") == 2, f"each fragment should carry what its shader wrote: {first}") + \
        expect(first.get("primitive") in primitives,
               f"the primitive that won the pixel should be one of the fragments: {first}")


def triangle_overlay(state, log):
    c = capture(state)
    t = c.get("textureTab") or {}
    d = t.get("drawOverlay") or {}
    # A draw overlay end to end: the draw replayed on its own (src/replay/src/overlay.cpp), its mask
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


def triangle_mesh(state, log):
    c = capture(state)
    m = c.get("meshTab") or {}
    o = m.get("output") or {}
    stats = o.get("stats") or {}
    preview = m.get("preview") or {}
    # The mesh tab's VS Out end to end: the cube's vertex shader edited to write transform feedback
    # (src/replay/src/xfb_patch.cpp), its 36 vertices read back, and the wireframe drawn in the preview.
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect(bool(m), "--debug-view=mesh opened no mesh tab") + \
        expect(not m.get("error"), f"the mesh replay failed: {m.get('error')}") + \
        expect(o.get("measured") is True and o.get("vertices") == 36, f"the cube's 36 vertices were not captured: {o}") + \
        expect("gl_Position" in (o.get("outputs") or []) and "fragUV" in (o.get("outputs") or []), f"the outputs are not all there: {o.get('outputs')}") + \
        expect(stats.get("behind") == 0 and stats.get("outside") == 0, f"the cube is in view, but: {stats}") + \
        expect(preview.get("webgl") is True and preview.get("edges") == 36, f"the preview did not draw the cube's 12 triangles: {preview}")


def triangle_mesh_input(state, log):
    c = capture(state)
    m = c.get("meshTab") or {}
    i = m.get("input") or {}
    preview = m.get("preview") or {}
    # VS In needs no replay: the captured vertex and index buffers through the pipeline's layout.
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect(bool(m), "--debug-view=mesh:in opened no mesh tab") + \
        expect(i.get("vertices") == 36, f"the cube's 36 indices were not decoded: {i}") + \
        expect(i.get("attributes") == ["inPosition", "inColor", "inUV"], f"the attributes are not named from the shader: {i.get('attributes')}") + \
        expect(i.get("position") == 0, f"inPosition was not taken as the position: {i}") + \
        expect(preview.get("edges") == 36, f"the preview did not draw the cube's 12 triangles: {preview}")


def debugger_tab(state):
    return capture(state).get("debuggerTab") or {}


def triangle_debug_pixel(state, log):
    d = debugger_tab(state)
    outputs = d.get("outputs") or []
    color = next((o.get("value") for o in outputs if o.get("location") == 0), None) or []
    target = (d.get("targetPixel") or {}).get("value") or []
    diff = max((abs(a - b) for a, b in zip(color, target)), default=None)
    # The cube's fragment shader at a pixel the draw covers: its inputs rasterized from the replay's
    # vertex outputs, the checker texture sampled with derivatives from the pixel quad, and the color
    # it writes compared with the render target (the cube is the pass's only draw there).
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect(bool(d), "--debug-view=debugger opened no debugger tab") + \
        expect(not d.get("error"), f"the debugger could not prepare the pixel: {d.get('error')}") + \
        expect(d.get("mode") == "source" and (d.get("codeLines") or 0) >= 10, f"cube.frag's source is not shown: {d.get('mode')}, {d.get('codeLines')} lines") + \
        expect(d.get("status") == "returned", f"the fragment did not run to the end: {d.get('status')} {d.get('invocationError')}") + \
        expect(not d.get("warnings"), f"the interpreter warned: {d.get('warnings')}") + \
        expect(len(color) == 4 and len(target) >= 3, f"no color to compare: output {color}, render target {target}") + \
        expect(diff is not None and diff < 0.02, f"the output {color} is not the render target's {target}")


def triangle_debug_vertex(state, log):
    d = debugger_tab(state)
    replayed = d.get("replayedOutputs") or []
    # Stopped part way through cube.vert: two lines stepped over, one left to run.
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect(bool(d), "--debug-view=debugger:vertex opened no debugger tab") + \
        expect(not d.get("error"), f"the debugger could not prepare the vertex: {d.get('error')}") + \
        expect(d.get("status") == "running" and d.get("line") == 17, f"not paused on line 17 after two steps: {d.get('status')} line {d.get('line')}") + \
        expect((d.get("lineValues") or 0) > 0, "the line stepped over shows no values") + \
        expect(len(replayed) >= 3, f"the replay's outputs of the vertex are missing: {replayed}")


def triangle_debug_compute(state, log):
    d = debugger_tab(state)
    # wave.comp ships without its text: the source roots supply it (like the sources case).
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect(bool(d), "--debug-view=debugger:compute opened no debugger tab") + \
        expect(not d.get("error"), f"the debugger could not prepare the invocation: {d.get('error')}") + \
        expect(d.get("status") == "returned", f"the invocation did not run to the end: {d.get('status')} {d.get('invocationError')}") + \
        expect(d.get("mode") == "source", f"wave.comp's source was not found under the source root: {d.get('mode')}")


def triangle_debug_decompiled(state, log):
    d = debugger_tab(state)
    original = d.get("original") or {}
    # wave.comp without its source roots: no text, so GLSL decompiled from its SPIR-V (spirv-cross,
    # compiled back by glslang) is stepped by line, and the original module run beside it agrees.
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect(bool(d), "--debug-view=debugger:compute::end:decompiled opened no debugger tab") + \
        expect(d.get("decompiled") is True, "the debugger is not stepping the decompiled GLSL") + \
        expect(not d.get("error"), f"the SPIR-V could not be decompiled: {d.get('error')}") + \
        expect(d.get("mode") == "source" and (d.get("codeLines") or 0) >= 10, f"the decompiled source is not shown: {d.get('mode')}, {d.get('codeLines')} lines") + \
        expect(d.get("status") == "returned", f"the invocation did not run to the end: {d.get('status')} {d.get('invocationError')}") + \
        expect(original.get("done") is True and not original.get("error"), f"the original did not run to compare with: {original}") + \
        expect(original.get("matches") is True, f"the translation's results differ from the original's: {original}")


# --------------------------------------------------------------------------------------------
# Metal (test/metal_triangle, captured through src/metal/): the shader debugger on a Metal capture,
# whose shaders are Metal Shading Language rather than SPIR-V. There is no replay on this path: a
# fragment's inputs come from running the draw's own vertex shader in the interpreter.


def check_metal_capture(state, log):
    c = capture(state)
    return expect(bool(c), "no capture tab") + \
        expect((c.get("commands") or 0) > 5, f"{c.get('commands')} commands captured") + \
        expect((c.get("draws") or 0) >= 2, f"{c.get('draws')} draws") + \
        expect((c.get("textureErrors") or 0) == 0, f"{c.get('textureErrors')} textures failed to read back") + \
        expect((c.get("texturesLoaded") or 0) == (c.get("textures") or 0), "not every texture's data arrived")


def metal_debug_compute(state, log):
    d = debugger_tab(state)
    # wave_main: the kernel's source comes from the library the application compiled, so unlike the
    # Vulkan compute case there are no source roots to find it in.
    return check_connected(state, log) + check_metal_capture(state, log) + \
        expect(bool(d), "--debug-view=debugger:compute opened no debugger tab") + \
        expect(not d.get("error"), f"the debugger could not prepare the invocation: {d.get('error')}") + \
        expect(d.get("mode") == "source" and (d.get("codeLines") or 0) >= 20,
               f"the library's Metal Shading Language is not shown: {d.get('mode')}, {d.get('codeLines')} lines") + \
        expect(d.get("status") == "returned", f"the invocation did not run to the end: {d.get('status')} {d.get('invocationError')}") + \
        expect(not d.get("warnings"), f"the interpreter warned: {d.get('warnings')}")


def metal_debug_constants(state, log):
    d = debugger_tab(state)
    outputs = d.get("outputs") or []
    color = next((o.get("value") for o in outputs if o.get("location") == 0), None) or []
    target = (d.get("targetPixel") or {}).get("value") or []
    diff = max((abs(a - b) for a, b in zip(color, target)), default=None)
    # The triangle pass's fragment_main is specialized: its tint branch is behind
    # `[[function_constant(0)]]`, and the values are only knowable because the capture library
    # watched the setters of the MTLFunctionConstantValues (src/metal/src/function_constants.h). Without
    # them the branch is not taken and the color is the untinted one, which the comparison catches
    # — and the interpreter says so in its warnings, which is the clearer diagnosis of the two.
    return check_connected(state, log) + check_metal_capture(state, log) + \
        expect(bool(d), "--debug-view=debugger:pixel opened no debugger tab") + \
        expect(not d.get("error"), f"the debugger could not prepare the pixel: {d.get('error')}") + \
        expect(d.get("status") == "returned", f"the fragment did not run to the end: {d.get('status')} {d.get('invocationError')}") + \
        expect(not d.get("warnings"), f"the interpreter warned: {d.get('warnings')}") + \
        expect(len(color) == 4 and len(target) >= 3, f"no color to compare: output {color}, render target {target}") + \
        expect(diff is not None and diff < 0.02,
               f"the output {color} is not the render target's {target}: the function constants the "
               f"fragment was specialized with may not have reached the interpreter")


def metal_debug_vertex(state, log):
    d = debugger_tab(state)
    outputs = d.get("outputs") or []
    position = next((o.get("value") for o in outputs if o.get("builtin") == 0), None) or []
    return check_connected(state, log) + check_metal_capture(state, log) + \
        expect(bool(d), "--debug-view=debugger:vertex opened no debugger tab") + \
        expect(not d.get("error"), f"the debugger could not prepare the vertex: {d.get('error')}") + \
        expect(d.get("status") == "returned", f"the vertex did not run to the end: {d.get('status')} {d.get('invocationError')}") + \
        expect(not d.get("warnings"), f"the interpreter warned: {d.get('warnings')}") + \
        expect(len(position) == 4 and position[3] == 1 and any(abs(v) > 1e-4 for v in position[:2]),
               f"[[position]] is not a transformed clip position: {position}")


def metal_debug_pixel(state, log):
    d = debugger_tab(state)
    outputs = d.get("outputs") or []
    color = next((o.get("value") for o in outputs if o.get("location") == 0), None) or []
    target = (d.get("targetPixel") or {}).get("value") or []
    diff = max((abs(a - b) for a, b in zip(color, target)), default=None)
    # The blit pass's fragment: its varyings rasterized from the vertex shader the interpreter ran,
    # its texture sampled from the read-back the capture made of what the draw bound, and the color
    # it writes compared with the render target (the blit is the pass's only draw).
    return check_connected(state, log) + check_metal_capture(state, log) + \
        expect(bool(d), "--debug-view=debugger:pixel opened no debugger tab") + \
        expect(not d.get("error"), f"the debugger could not prepare the pixel: {d.get('error')}") + \
        expect(d.get("status") == "returned", f"the fragment did not run to the end: {d.get('status')} {d.get('invocationError')}") + \
        expect(not d.get("warnings"), f"the interpreter warned: {d.get('warnings')}") + \
        expect(len(color) == 4 and len(target) >= 3, f"no color to compare: output {color}, render target {target}") + \
        expect(diff is not None and diff < 0.02, f"the output {color} is not the render target's {target}")


def cpu_categories(state):
    """Which categories "Where the CPU went" totalled, by name -> milliseconds."""
    timeline = capture(state).get("cpuTimeline") or {}
    return {t["category"]: t.get("ms", 0.0) for t in timeline.get("categories") or []}


def metal_cpu_timeline(state, log):
    c = capture(state)
    timeline = c.get("cpuTimeline") or {}
    cats = cpu_categories(state)
    # The CPU timeline on a Metal capture (src/metal/src/cpu_timeline.h): commit is submission and
    # nextDrawable is waiting for the display, which is what a vsynced frame spends itself on and
    # what makes the verdict read as display pacing rather than as a GPU bound. `calibrated` is the
    # sampleTimestamps relation that puts the GPU lane on the same axis as the commits; without it
    # the Timeline card can still draw the CPU spans but the passes keep their own origin.
    return check_connected(state, log) + check_metal_capture(state, log) + \
        expect(bool(timeline), "the capture carries no CPU timeline") + \
        expect("submit" in cats, f"nothing was timed as submission (commit): {sorted(cats)}") + \
        expect("acquire" in cats, f"nextDrawable was not timed as waiting for a swapchain image: {sorted(cats)}") + \
        expect(timeline.get("calibrated") is True,
               "the GPU and CPU clocks were not related, so the GPU lane cannot be laid over the commits") + \
        expect((timeline.get("frames") or 0) >= 1 and (timeline.get("spanMs") or 0) > 0,
               f"the timeline spans nothing: {timeline.get('frames')} frames, {timeline.get('spanMs')} ms") + \
        metal_timeline_lanes(c)


def metal_timeline_lanes(c):
    """The Timeline card's GPU lane sitting where the commits put it, not a frame away from them."""
    t = c.get("timelineTracks") or {}
    gap = t.get("submitToFirstPassMs")
    return expect(bool(t), "the capture builds no timeline tracks") + \
        expect(t.get("hasGpu") is True, f"no GPU lane: {t.get('gpuNote')}") + \
        expect((t.get("gpuStartMs") or -1) >= 0 and (t.get("gpuEndMs") or 0) <= (t.get("spanMs") or 0) + 0.001,
               f"the GPU lane falls outside the drawn range: {t.get('gpuStartMs')}-{t.get('gpuEndMs')} of {t.get('spanMs')} ms") + \
        expect(gap is not None, "no commit precedes the first pass, so the two lanes cannot be related at all") + \
        expect(gap is None or 0 <= gap < 8.0,
               f"the GPU lane sits {gap} ms after the commit that issued it: the clocks are related wrongly "
               f"(a pass cannot precede its commit, and a frame's worth of offset is the calibration being out)")


def metal_compile_hitch(state, log):
    cats = cpu_categories(state)
    # --compile-hitch builds a library and a pipeline inside the frame, so the frame should stop
    # for it under "Creating pipelines" (hooks_device.mm times the six creation calls).
    return check_connected(state, log) + check_metal_capture(state, log) + \
        expect("pipeline" in cats,
               f"the frame's pipeline creation was not timed: {sorted(cats)}") + \
        expect(cats.get("pipeline", 0) > 0.1,
               f"the compile was timed at {cats.get('pipeline')} ms, which is too little to be a real compile")


def metal_no_self_compile(state, log):
    c = capture(state)
    cats = cpu_categories(state)
    # The same application *without* --compile-hitch, captured with Overdraw on: the library
    # compiles counting copies of every pipeline of its own (src/metal/src/overdraw.mm), and the
    # reentry guard is what keeps that out of the application's timeline. If the guard ever stops
    # holding, a frame that compiled nothing grows a "Creating pipelines" row and the verdict
    # blames the application for the inspector's work.
    return check_connected(state, log) + check_metal_capture(state, log) + \
        expect((c.get("overdraw") or 0) >= 1, f"{c.get('overdraw')} overdraw measurements: the library did not measure, so it compiled nothing to be confused by") + \
        expect("pipeline" not in cats,
               f"the inspector's own compiles were timed as the application's: {cats.get('pipeline')} ms under 'pipeline'")


def metal_memory(state, log):
    s = session(state)
    m = s.get("metalMemory") or {}
    groups = {g["label"]: g for g in m.get("groups") or []}
    in_heaps = m.get("inHeaps") or {}
    # Memory Use on an MTLDevice (renderer/metal/metal_memory.ts): the breakdown by object kind
    # Metal has instead of a heap table, and the rows that only an application using heaps draws.
    # The sample reserves 4 MB and takes two small resources out of it, so the reservation is
    # mostly empty and that call-out has something to report too.
    return check_connected(state, log) + \
        expect(bool(m), "the session has no Metal memory breakdown") + \
        expect("Buffers" in groups and "Textures" in groups and "Heaps" in groups,
               f"the breakdown is missing a kind: {sorted(groups)}") + \
        expect((m.get("totalBytes") or 0) > 0, "the breakdown totals nothing") + \
        expect((in_heaps.get("count") or 0) >= 2,
               f"{in_heaps.get('count')} resources were counted as suballocated from a heap, so the 'In heaps' row is missing") + \
        expect((m.get("heapReservedBytes") or 0) >= (m.get("heapUsedBytes") or 0) > 0,
               f"the heap reported {m.get('heapUsedBytes')} used of {m.get('heapReservedBytes')} reserved") + \
        expect((m.get("occupancy") or 1) < 0.5,
               f"the heap is {m.get('occupancy')} full, so the 'mostly empty' call-out this case is for does not apply") + \
        expect((s.get("memorySamples") or 0) > 0, "no memory samples arrived, so the series under the rows is empty")


def metal_pixel_history(state, log):
    # The pixel history of the center of the first color target, which in this sample is the
    # multisampled one — the pass the library used to decline ("a multisampled pass is not
    # followed yet"). Two captures: the first names the pixel, the second follows it through the
    # application's next frame, which is what --debug-view=pixel-history asks for on a backend
    # that measures while capturing.
    caps = session(state).get("captures") or []
    h = next((c.get("textureTab", {}).get("history") or {} for c in reversed(caps)
              if (c.get("textureTab") or {}).get("history", {}).get("events")), {})
    touched = h.get("touched") or []
    notes = h.get("notes") or []
    return check_connected(state, log) + \
        expect(len(caps) >= 2, f"{len(caps)} captures: the second, following the pixel, was not taken") + \
        expect(not h.get("error"), f"the pixel history failed: {h.get('error')}") + \
        expect(not any("multisampled" in n for n in notes), f"the multisampled pass was declined: {notes}") + \
        expect(bool(touched), f"the pixel history lists no events: {h}") + \
        expect(any("begins" in e for e in touched), f"no pass start in the pixel history: {touched}") + \
        expect(any("wrote the pixel" in e for e in touched), f"no draw wrote the pixel: {touched}")


def unity_overdraw(state, log):
    c = capture(state)
    t = c.get("textureTab") or {}
    # A real frame's passes, measured while capturing. Unlike the sample this has passes that draw
    # nothing into the target being viewed, which is what the report opening on the worst pass
    # rather than the first measured one is for.
    return check_connected(state, log) + \
        expect((c.get("draws") or 0) >= 2, f"{c.get('draws')} draws: this is not a frame of the game") + \
        expect((c.get("overdraw") or 0) >= 2, f"{c.get('overdraw')} overdraw measurements") + \
        expect((c.get("overdrawCounts") or 0) == (c.get("overdraw") or 0), "not every measurement carries counts") + \
        expect(t.get("measured") is True and t.get("counts") is True, f"the tab's pass has no counts to draw: {t}")


def unity_pixel_history(state, log):
    caps = session(state).get("captures") or []
    h = next((c.get("textureTab", {}).get("history") or {} for c in reversed(caps)
              if (c.get("textureTab") or {}).get("history", {}).get("events")), {})
    notes = h.get("notes") or []
    return check_connected(state, log) + \
        expect(len(caps) >= 2, f"{len(caps)} captures: the second, following the pixel, was not taken") + \
        expect(not h.get("error"), f"the pixel history failed: {h.get('error')}") + \
        expect((h.get("events") or 0) > 0, f"the pixel history of a real frame is empty: {h}") + \
        expect(not any("not followed yet" in n for n in notes), f"a pass of a real frame was declined: {notes}")


def unity_cases(player):
    """Opt-in (--unity <path>): a real application's frame, which has passes the sample has not."""
    launch = [f"--launch={player}", "--debug-capture", "--debug-capture-delay=20000"]
    return [
        Case("unity-overdraw", launch + ["--debug-capture-with=overdraw", "--debug-view=overdraw"],
             unity_overdraw, delay_ms=42000),
        Case("unity-pixel-history", launch + ["--debug-view=pixel-history", "--debug-settle=12000"],
             unity_pixel_history, delay_ms=52000),
    ]


exported_metal_cpp = os.path.join(tempfile.gettempdir(), "gpuinsp_ui_metal_export_cpp")


def remove_exported_metal_cpp():
    shutil.rmtree(exported_metal_cpp, ignore_errors=True)


def metal_export_cpp(state, log):
    # Export to C++ of a Metal capture (src/metal/replay/src/mtl_exporter.h), which mtlinsp_replay
    # writes. The frame's first pass is multisampled through a parallel encoder and resolves, so the
    # project holds the sub-encoder's draw and reads the resolve back; the second samples that
    # resolve, which is why there is an upload before the frame.
    projects = [os.path.join(exported_metal_cpp, d) for d in os.listdir(exported_metal_cpp)] if os.path.isdir(exported_metal_cpp) else []
    project = projects[0] if projects else ""
    commands = contents = ""
    if project and os.path.isfile(os.path.join(project, "frame_commands.mm")):
        with open(os.path.join(project, "frame_commands.mm"), encoding="utf-8") as f:
            commands = f.read()
    if project and os.path.isfile(os.path.join(project, "frame_contents.mm")):
        with open(os.path.join(project, "frame_contents.mm"), encoding="utf-8") as f:
            contents = f.read()
    data = os.path.join(project, "frame_data.bin")
    shaders = os.path.join(project, "shaders")
    # Every encoder and the command buffer are globals, not locals of Frame. Frame is cut into
    # parts and a part is a function, so a local would go out of scope wherever the cut lands —
    # which on an engine frame, whose passes are longer than one part, it does.
    objects = ""
    if project and os.path.isfile(os.path.join(project, "frame_objects.h")):
        with open(os.path.join(project, "frame_objects.h"), encoding="utf-8") as f:
            objects = f.read()
    return check_connected(state, log) + check_metal_capture(state, log) + \
        expect("id<MTLCommandBuffer> commands;" in objects.replace("extern ", ""),
               "the command buffer is not a global of frame_objects.h") + \
        expect("encoder" in objects, "the encoders are not globals of frame_objects.h") + \
        expect(not re.search(r"^    id<MTL\w*CommandEncoder> \w+ =", commands, re.M),
               "an encoder is declared inside Frame, so a part split would put it out of scope") + \
        expect(len(projects) == 1 and project.endswith("_cpp"), f"one project folder named after the capture in {exported_metal_cpp}: {projects}") + \
        expect(os.path.isfile(os.path.join(project, "CMakeLists.txt")), "the project has no CMakeLists.txt") + \
        expect(os.path.isfile(os.path.join(project, "mtl_support.mm")), "the project has no mtl_support.mm") + \
        expect("parallelRenderCommandEncoderWithDescriptor:" in commands, "frame_commands.mm does not open the parallel encoder") + \
        expect("drawIndexedPrimitives:" in commands, "frame_commands.mm does not hold the triangle draw") + \
        expect("dispatchThreads:" in commands, "frame_commands.mm does not hold the compute dispatch") + \
        expect("ReadbackTexture(" in commands, "frame_commands.mm reads no render target back to compare") + \
        expect("UploadTexture(" in contents, "frame_contents.mm uploads none of the sampled texture") + \
        expect(os.path.isdir(shaders) and any(f.endswith(".metal") for f in os.listdir(shaders)), "the project has no .metal shader source") + \
        expect(os.path.isfile(data) and os.path.getsize(data) > 100000, "frame_data.bin is missing or too small to hold the captured targets")


def accel_tab(state):
    """The acceleration structure tab's state, from the last capture that opened one."""
    for c in reversed(session(state).get("captures") or []):
        tab = c.get("accelTab")
        if tab:
            return tab
    return {}


def metal_accel_triangle(state, log):
    """--ray-tracing: the top level's two instances of one triangle bottom level, drawn.

    The instance transforms are the point. They are not the identity — one moved left, one moved
    right and turned a quarter turn — so the scene's extent is only right if MTLPackedFloat4x3 was
    read as the transpose of the 3x4 the views use (renderer/metal/raytracing.ts). An untransposed
    read puts the translation in the wrong component and the rotation the other way, and neither
    the centre nor the area below would come out.
    """
    a = accel_tab(state)
    tree = a.get("tree") or {}
    preview = a.get("preview") or {}
    center = preview.get("center") or [0, 0, 0]
    return check_connected(state, log) + check_metal_capture(state, log) + \
        expect(bool(a), "--debug-view=accel opened no acceleration structure tab") + \
        expect(a.get("shape") == "triangles", f"the scene drew {a.get('shape')}, not triangles") + \
        expect(a.get("instances") == 2, f"{a.get('instances')} instances, expected 2") + \
        expect(a.get("placed") == 2, f"{a.get('placed')} instances drawn with their own geometry, expected 2") + \
        expect(preview.get("triangles") == 2, f"{preview.get('triangles')} triangles in the preview, expected 2") + \
        expect(abs((tree.get("area") or 0) - 1.2) < 0.01, f"world area {tree.get('area')}, expected 1.2 (0.6 twice)") + \
        expect((tree.get("memory") or 0) > 0, "the tree has no memory for the structures (resultSize)") + \
        expect(abs(center[0] + 0.1) < 0.01, f"the scene's centre is {center}, expected about [-0.1, 0, 0]: "
                                            "the instance transforms were not read as Metal stores them")


def metal_accel_static(state, log):
    """--static-blas: the bottom level is built once at start-up, so the captured frame holds no
    build of it. Everything drawn comes from the read-back the capture library takes when the
    capture begins (src/metal/src/raytracing.h, ReadBackEarlierStructures), out of a *private*
    buffer — which it can only do by blitting it through the capture's own pass."""
    a = accel_tab(state)
    tree = a.get("tree") or {}
    return check_connected(state, log) + check_metal_capture(state, log) + \
        expect(bool(a), "--debug-view=accel opened no acceleration structure tab") + \
        expect(a.get("shape") == "triangles", f"the bottom level drew {a.get('shape')}, not triangles") + \
        expect(not a.get("note"), f"nothing was drawn: {a.get('note')}") + \
        expect((a.get("preview") or {}).get("triangles") == 1, "the triangle was not read back at the capture's start") + \
        expect(abs((tree.get("area") or 0) - 0.6) < 0.01, f"world area {tree.get('area')}, expected 0.6")


def metal_accel_path_tracer(state, log):
    """The path tracer: three bounding-box bottom levels under a top level of three instances, and
    the intersection function table the traversal reaches sphereIntersection through.

    Also the build cost: the two acceleration structure passes are timed, which they were not
    before — CreateOtherEncoder gave them no timing slot at all."""
    a = accel_tab(state)
    tree = a.get("tree") or {}
    overlaps = a.get("overlaps") or {}
    c = capture(state)
    memory = (session(state).get("metalMemory") or {}).get("groups") or []
    structures = next((g for g in memory if g.get("label") == "Acceleration structures"), None)
    return check_connected(state, log) + \
        expect(bool(a), "--debug-view=accel opened no acceleration structure tab") + \
        expect(a.get("instances") == 3, f"{a.get('instances')} instances, expected 3") + \
        expect(a.get("placed") == 3, f"{a.get('placed')} instances drawn with their own geometry, expected 3") + \
        expect(a.get("shape") == "aabbs", f"the scene drew {a.get('shape')}, not the bounding boxes it was built from") + \
        expect(tree.get("primitives") == 484, f"{tree.get('primitives')} primitives, expected 484 (398 + 60 + 26)") + \
        expect(tree.get("children") == 3, f"{tree.get('children')} instances under the top level, expected 3") + \
        expect(overlaps.get("total") == 3, f"{overlaps.get('total')} overlapping pairs, expected 3: "
                                           "the three materials' boxes each span the whole scene") + \
        expect((c.get("passTimings") or 0) >= 4, f"{c.get('passTimings')} timed passes: the build passes are not timed") + \
        expect(bool(structures) and structures.get("count") == 4,
               f"Memory Use does not count the four acceleration structures: {structures}")


def metal_cases(triangle):
    launch = [f"--launch={triangle}"]
    export_cases = [Case("metal-export-cpp", launch + ["--debug-capture", f"--debug-export-cpp={exported_metal_cpp}"],
                         metal_export_cpp, delay_ms=22000, before=remove_exported_metal_cpp)] if find_metal_replay() else []
    if not export_cases:
        print("  (no mtlinsp_replay build: skipping the Metal Export to C++ case)")
    return export_cases + [
        Case("metal-debug-compute", launch + ["--debug-capture", "--debug-view=debugger:compute::end"],
             metal_debug_compute, delay_ms=16000),
        Case("metal-debug-vertex", launch + ["--debug-capture", "--debug-view=debugger:vertex::end"],
             metal_debug_vertex, delay_ms=16000),
        # The first draw's fragment is the specialized one, so this is the function constants case.
        Case("metal-debug-constants", launch + ["--debug-capture", "--debug-view=debugger:pixel::end"],
             metal_debug_constants, delay_ms=18000),
        # The last draw is the blit, which samples a texture: the one case that needs the sampled
        # read-back (src/metal/src/capture.mm, QueueTextureCapture) as well as the interpreter.
        Case("metal-debug-pixel", launch + ["--debug-capture", "--debug-view=debugger:pixel:last:end"],
             metal_debug_pixel, delay_ms=18000),
        # "Where the CPU went" and the Timeline card on a Metal capture, with the Frame Stats view
        # open so the screenshot shows the rows the checks are of.
        Case("metal-cpu-timeline", launch + ["--debug-capture", "--debug-view=stats"],
             metal_cpu_timeline, delay_ms=16000),
        Case("metal-compile-hitch", launch + ["--args=--compile-hitch", "--debug-capture", "--debug-view=stats"],
             metal_compile_hitch, delay_ms=16000),
        # The other side of the same guard: the library compiling for itself must not land in the
        # application's timeline. Overdraw is what makes it compile.
        Case("metal-self-compile", launch + ["--debug-capture", "--debug-capture-with=overdraw", "--debug-view=stats"],
             metal_no_self_compile, delay_ms=20000),
        # One pixel of the multisampled target followed through the frame, which needs a second
        # capture: the library follows a pixel while it captures, so the first only names one.
        Case("metal-pixel-history", launch + ["--debug-capture", "--debug-view=pixel-history", "--debug-settle=10000"],
             metal_pixel_history, delay_ms=26000),
        # Memory Use on the MTLDevice. No --debug-capture: the checks read the live object graph,
        # and taking a capture would leave the Capture tab in front so the screenshot would not
        # show the rows this case is about.
        Case("metal-memory", launch + ["--debug-select=MTLDevice"], metal_memory, delay_ms=16000),
        # Ray tracing. Triangle geometry, which test/path_tracer/metal has none of, and instance
        # transforms that are not the identity, which is what makes the transposed read testable.
        Case("metal-accel-triangle", [f"--launch={triangle}", "--args=--ray-tracing",
                                      "--debug-capture", "--debug-view=accel:scene TLAS"],
             metal_accel_triangle, delay_ms=18000),
        # The same scene with the bottom level built once at start-up, so what is drawn can only
        # have come from the capture-start read-back of a private buffer.
        Case("metal-accel-static-blas", [f"--launch={triangle}", "--args=--static-blas",
                                         "--debug-capture", "--debug-view=accel:triangle BLAS"],
             metal_accel_static, delay_ms=18000),
    ] + metal_path_tracer_cases()


def metal_path_tracer_cases():
    path_tracer = find_metal_path_tracer()
    if not path_tracer:
        print("  (no mtlinsp_path_tracer build: skipping the Metal ray tracing scene case)")
        return []
    # --rebuild so the captured frame holds the builds as well as the trace; small and cheap so the
    # frame is quick to capture and read back.
    return [
        Case("metal-accel-scene",
             [f"--launch={path_tracer}",
              "--args=--rebuild --width 320 --height 180 --spp 1 --depth 4",
              "--debug-capture", "--debug-view=accel:scene TLAS"],
             metal_accel_path_tracer, delay_ms=20000),
    ]


def triangle_stacks(state, log):
    c = capture(state)
    s = session(state)
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect((c.get("commandsWithStacks") or 0) >= 5, f"{c.get('commandsWithStacks')} commands carry stacks") + \
        expect((s.get("symbolsWithLines") or 0) >= 1, "no symbol resolved to a source line (PDB next to the app?)") + \
        expect(not IS_WIN or (s.get("symbolsInlined") or 0) >= 1,
               "no frame carries its inlined callers (DbgHelp's inline walk; the CRT's startup inlines at least one)")


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


def timing_capture(state, log):
    # A timing capture (--debug-timing): every frame's time and where its CPU went, streamed as
    # TimingFrames on the frame report's interval. Both capture libraries keep the same ring.
    s = session(state)
    frames = s.get("timingFrames") or 0
    out = check_connected(state, log) + \
        expect(frames > 60, f"{frames} frames recorded by a three second timing capture") + \
        expect("present" in (s.get("timingCategories") or []), f"the categories are {s.get('timingCategories')}")
    if IS_WIN:
        # Call stacks sampled with it (src/vulkan/src/cpu_sampler.h): several threads, some of the
        # samples finding one running and most finding one blocked (a sample that calls every thread
        # running has mistaken the cost of being sampled for work), and the stacks the report shows
        # named by the library, which is what makes them readable.
        t = s.get("timingSamples") or {}
        out += expect((t.get("threads") or 0) >= 2, f"{t.get('threads')} threads sampled") + \
            expect((t.get("running") or 0) > 20, f"{t.get('running')} samples found a thread running") + \
            expect((t.get("waiting") or 0) > (t.get("running") or 0), f"{t.get('waiting')} samples found a thread blocked, {t.get('running')} running") + \
            expect((t.get("named") or 0) >= 1, f"{t.get('named')} of {t.get('stacks')} stacks have a named frame: the report's stacks were not symbolized")
    return out


def memory_capture(state, log):
    # A memory capture (--debug-memory) of the sample's --churn: a scratch buffer made every frame
    # and freed two frames later, and a 1 MB buffer every 30th frame that is kept. So the report
    # has to find both halves, and name the kept ones: an id the capture library got wrong (a
    # handle the driver reused) would leave them unnamed or named as the scratch buffers.
    m = session(state).get("memoryCapture") or {}
    return check_connected(state, log) + \
        expect((m.get("allocations") or 0) > 60, f"{m.get('allocations')} allocations recorded") + \
        expect((m.get("frees") or 0) > 60, f"{m.get('frees')} frees recorded") + \
        expect(m.get("unnamed") == 0, f"{m.get('unnamed')} allocations could not be tied to an object") + \
        expect((m.get("survivors") or 0) >= 2, f"{m.get('survivors')} allocations still held (the kept buffers)") + \
        expect((m.get("survivorBytes") or 0) >= 2 * 1024 * 1024, f"{m.get('survivorBytes')} bytes still held") + \
        expect((m.get("transient") or 0) > 60, f"{m.get('transient')} transient allocations (the scratch buffers)") + \
        expect((m.get("startBytes") or 0) > 0, "no baseline: the totals are relative to nothing") + \
        expect("still held" in (m.get("verdict") or "") and "pool" in (m.get("verdict") or ""), f"the verdict is {m.get('verdict')!r}")


def app_capture(state, log):
    # The application asked for the capture itself (include/gpu_inspector.h, the sample's
    # --capture-at): nothing on the command line takes one, so a capture tab can only be the
    # request's doing.
    return check_connected(state, log) + check_capture_basic(state, log, timings=False) + \
        expect("capture requested by the application" in log, "the capture library never logged the application's request")


def shader_edit(state, log):
    # A shader edited and run in the capture (Compile & Replay; renderer/shader_replay.ts): the
    # first draw's pixel shader made to write magenta, and the frame replayed with it. The cubes'
    # pixels change and nothing else does: the color target differs in the tens of thousands of
    # texels they cover, its replayed pixels came back to be shown, and the depth target is
    # exactly as captured, since the edit moved no geometry.
    c = capture(state)
    r = c.get("shaderReplay") or {}
    changed = r.get("changed") or []
    color = [t for t in changed if t.get("aspect") == "color"]
    return check_connected(state, log) + check_capture_basic(state, log, timings=False) + \
        expect(bool(r), f"the capture was not replayed with the edit ({c.get('status')})") + \
        expect((r.get("compared") or 0) >= 2, f"{r.get('compared')} render targets compared") + \
        expect(len(color) == 1 and (color[0].get("differingTexels") or 0) > 10000, f"the color target's change: {color}") + \
        expect(bool(color) and (color[0].get("pixels") or 0) > 0, "the changed target's pixels did not come back") + \
        expect(not [t for t in changed if t.get("aspect") != "color"], f"targets the edit should not have touched changed: {changed}") + \
        expect(r.get("problems") == 0, f"{r.get('problems')} problems replaying with the edit") + \
        expect("shader-edit" in (c.get("reportTabs") or []), f"the result did not open in a tab: {c.get('reportTabs')}")


def triangle_cases(triangle):
    launch = [f"--launch={triangle}"]
    source_root = os.path.join(ROOT, "test")
    exported = os.path.join(tempfile.gettempdir(), "gpuinsp_ui_report.html")

    def start_triangle():
        env = dict(os.environ, VKINSP_ENABLE="1", VKINSP_PORT="47531")
        return subprocess.Popen([triangle, "--frames", "5000"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def remove_exported():
        if os.path.exists(exported):
            os.remove(exported)

    exported_cpp = os.path.join(tempfile.gettempdir(), "gpuinsp_ui_export_cpp")

    def remove_exported_cpp():
        shutil.rmtree(exported_cpp, ignore_errors=True)

    def triangle_export_cpp(state, log):
        # Export to C++ (renderer/export_cpp.ts, src/replay/src/exporter.h): the capture bar's button
        # replays the capture and writes a project into a folder named after the capture. Building it
        # is the replay's own test (docs/REPLAY.md); here it has to have been written, with the
        # frame's draw in it and the data the source points into.
        projects = [os.path.join(exported_cpp, d) for d in os.listdir(exported_cpp)] if os.path.isdir(exported_cpp) else []
        project = projects[0] if projects else ""
        commands = ""
        if project and os.path.isfile(os.path.join(project, "frame_commands.cpp")):
            with open(os.path.join(project, "frame_commands.cpp"), encoding="utf-8") as f:
                commands = f.read()
        data = os.path.join(project, "frame_data.bin")
        return check_connected(state, log) + check_capture_basic(state, log) + \
            expect(len(projects) == 1 and project.endswith("_cpp"), f"one project folder named after the capture in {exported_cpp}: {projects}") + \
            expect(os.path.isfile(os.path.join(project, "CMakeLists.txt")), "the project has no CMakeLists.txt") + \
            expect(os.path.isfile(os.path.join(project, "vulkan_headers", "vulkan", "vulkan_core.h")), "the project does not carry its Vulkan headers") + \
            expect(all(os.path.isfile(os.path.join(project, f)) for f in ("frame_window.h", "frame_window_win32.cpp", "frame_window_x11.cpp", "frame_window_cocoa.mm", "frame_restore.cpp")),
                   "the project lacks its window sources (one per platform) or frame_restore.cpp") + \
            expect("vkCmdDrawIndexed(" in commands, "frame_commands.cpp does not hold the cube's draw") + \
            expect("ReadbackImage(" in commands, "frame_commands.cpp reads no render target back to compare") + \
            expect(os.path.isfile(data) and os.path.getsize(data) > 100000, "frame_data.bin is missing or too small to hold the captured targets")

    def triangle_report_export(state, log):
        # Reports open in tabs beside the capture's, and each exports to a standalone HTML file
        # (renderer/report_export.ts). The flame graph is the report that fetches its shaders
        # before it renders, so it is the one worth exercising here.
        c = capture(state)
        html = ""
        if os.path.isfile(exported):
            with open(exported, encoding="utf-8") as f:
                html = f.read()
        return check_connected(state, log) + check_capture_basic(state, log) +             expect("flame" in (c.get("reportTabs") or []), f"the flame graph did not open in a tab of its own: {c.get('reportTabs')}") +             expect(len(html) > 2000, f"{len(html)} bytes exported to {exported}") +             expect("flamegraph-frame" in html, "the exported report has no flame graph frames in it") +             expect("--bg-primary" in html, "the exported report has no stylesheet inlined, so it reads as unstyled text")
    cases = [
        # The implicit layer: registered for the case, the triangle started outside the inspector.
        Case("implicit", ["--wait-for-app", "--port=47531", "--debug-capture"], triangle_implicit, delay_ms=16000,
             companion=start_triangle, before=lambda: implicit_layer(True), after=lambda: implicit_layer(False)),
        Case("plain", launch + ["--debug-capture"], triangle_plain),
        Case("timing-capture", launch + ["--debug-timing=3000"], timing_capture, delay_ms=9000),
        Case("memory-capture", launch + ["--args=--churn", "--debug-memory=3000"], memory_capture, delay_ms=9000),
        Case("app-capture", launch + ["--args=--capture-at 200"], app_capture, delay_ms=14000),
        Case("shader-edit", launch + ["--debug-capture", "--debug-view=shader-edit", "--debug-settle=12000"], shader_edit, delay_ms=26000),
        Case("sources", launch + [f"--source-roots={source_root}", "--debug-capture", "--debug-command=5",
                                  "--debug-expand=Compute Shader"], triangle_sources, delay_ms=14000),
        Case("prerecord", launch + ["--args=--prerecord", "--record-always", "--validation", "--debug-capture"], triangle_prerecord, delay_ms=16000),
        Case("msaa", launch + ["--args=--msaa", "--debug-capture"], triangle_msaa),
        Case("suspend", launch + ["--args=--suspend", "--validation", "--debug-capture"], triangle_suspend, delay_ms=16000),
        Case("stencil", launch + ["--args=--stencil", "--validation", "--debug-capture"], triangle_stencil(4), delay_ms=16000),
        Case("stencil-msaa", launch + ["--args=--stencil --msaa", "--validation", "--debug-capture"], triangle_stencil(5), delay_ms=16000),
        Case("offscreen", launch + ["--args=--offscreen", "--debug-capture"], triangle_offscreen, delay_ms=14000),
        Case("scissor", launch + ["--args=--bad-scissor", "--validation", "--debug-capture"], triangle_scissor, delay_ms=16000),
        Case("hazard", launch + ["--args=--hazard", "--validation", "--sync-validation", "--debug-capture"], triangle_hazard, delay_ms=18000),
        Case("oob", launch + ["--args=--oob", "--validation", "--gpu-validation", "--debug-capture"], triangle_oob, delay_ms=18000),
        Case("stacks", launch + ["--debug-capture", "--debug-capture-stacks", "--debug-command=9", "--debug-expand-stacks"], triangle_stacks, delay_ms=16000),
        # The render graph, on the frames whose passes have a dependency to find, with the view
        # open so the screenshot shows the chart. Three frames, because the pass indices the graph
        # keys its timings by have to restart per frame the way the command tree's do.
        Case("graph", launch + ["--args=--hazard", "--debug-capture=3", "--debug-view=graph"], triangle_graph, delay_ms=16000),
        # The GPU Bottlenecks report rendering at all: a throw while building it would leave the
        # details pane empty and the renderer's console with the error.
        Case("bottlenecks", launch + ["--debug-capture", "--debug-view=bottlenecks"], triangle_bottlenecks, delay_ms=16000),
        Case("mesh-in", launch + ["--debug-capture", "--debug-view=mesh:in"], triangle_mesh_input, delay_ms=16000),
        # Reports in tabs and the HTML export, on the flame graph (the report that fetches shaders first).
        Case("report-export", launch + ["--debug-capture", "--debug-view=flame", f"--debug-export={exported}"],
             triangle_report_export, delay_ms=18000, before=remove_exported),
        Case("debug-compute", launch + [f"--source-roots={source_root}", "--debug-capture", "--debug-view=debugger:compute::end"],
             triangle_debug_compute, delay_ms=16000),
        Case("debug-decompiled", launch + ["--debug-capture", "--debug-view=debugger:compute::end:decompiled", "--debug-settle=4000"],
             triangle_debug_decompiled, delay_ms=18000),
    ]
    # The render target tab measures overdraw and follows a pixel by replaying the capture, so this
    # one only runs where vkinsp_replay is built (src/replay/, docs/REPLAY.md). The click lands on the
    # image, which fits its pane; --debug-settle waits for the replay the click set going.
    if find_replay():
        cases.append(Case("export-cpp", launch + ["--debug-capture", f"--debug-export-cpp={exported_cpp}"],
                          triangle_export_cpp, delay_ms=20000, before=remove_exported_cpp))
        cases.append(Case("overdraw", launch + ["--debug-capture", "--debug-view=overdraw",
                                                "--debug-mouse=340,560", "--debug-settle=8000"],
                          triangle_overdraw, delay_ms=20000))
        cases.append(Case("pixel-history", launch + ["--debug-capture", "--debug-view=pixel-history", "--debug-settle=8000"],
                          triangle_pixel_history, delay_ms=20000))
        cases.append(Case("pixel-fragments", launch + ["--args=--no-cull", "--debug-capture", "--debug-view=pixel-history", "--debug-settle=8000"],
                          triangle_pixel_fragments, delay_ms=22000))
        cases.append(Case("overlay-backface", launch + ["--args=--inside-out", "--debug-capture", "--debug-view=overlay:backface:last",
                                                        "--debug-settle=9000"],
                          triangle_backface_overlay, delay_ms=24000))
        cases.append(Case("overlay-stencil", launch + ["--args=--stencil", "--debug-capture", "--debug-view=overlay:stencil:last",
                                                       "--debug-settle=9000"],
                          triangle_stencil_overlay, delay_ms=24000))
        cases.append(Case("mesh", launch + ["--debug-capture", "--debug-view=mesh:out", "--debug-settle=8000"],
                          triangle_mesh, delay_ms=20000))
        cases.append(Case("overlay", launch + ["--args=--occluded", "--debug-capture", "--debug-view=overlay:depth:last",
                                               "--debug-settle=8000"],
                          triangle_overlay, delay_ms=20000))
        cases.append(Case("debug-pixel", launch + ["--debug-capture", "--debug-view=debugger:pixel::end", "--debug-settle=8000"],
                          triangle_debug_pixel, delay_ms=20000))
        cases.append(Case("debug-vertex", launch + ["--debug-capture", "--debug-view=debugger:vertex::2", "--debug-settle=8000"],
                          triangle_debug_vertex, delay_ms=20000))
    else:
        print("  (no vkinsp_replay build: skipping the overdraw, mesh, overlay and pixel debugger cases)")
    return cases


# ------------------------------------------------------------------------------------------ Direct3D 12

def d3d12_plain(state, log):
    f = findings(state)
    s = session(state)
    # The depth buffer is stored and nothing reads it (D3D12 has no store op to say so): the
    # graph's unread-store is the honest finding. The launcher's line says the library got in.
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect(set(f) <= {"unread-store", "oversynchronized-barrier"}, f"unexpected findings {f}") + \
        expect(s.get("frameBoundary") == "present", f"frame boundary {s.get('frameBoundary')!r}") + \
        expect(s.get("refreshSource") == "monitor", f"refresh source {s.get('refreshSource')!r}") + \
        expect("injected" in log, "the launcher did not report injecting the D3D12 library") + \
        expect("capture sent" in log, "the library never finished the capture")


def d3d12_render_pass(state, log):
    # BeginRenderPass / EndRenderPass with a multisampled target: the color target is read back
    # through a resolve; multisampled depth is reported as not read back, not silently missing.
    # Statistics and occlusion queries are not begun inside a render pass region, so the pass
    # has a timing but no counters (check_capture_basic would ask for them).
    c = capture(state)
    return check_connected(state, log) + \
        expect(bool(c), "no capture tab") + \
        expect((c.get("commands") or 0) > 5, f"{c.get('commands')} commands captured") + \
        expect((c.get("draws") or 0) >= 1, f"{c.get('draws')} draws") + \
        expect((c.get("textures") or 0) >= 2, f"{c.get('textures')} render targets read back") + \
        expect((c.get("textureErrors") or 0) == 1, f"{c.get('textureErrors')} render targets failed to read back (the multisampled depth target is expected to)") + \
        expect((c.get("texturesLoaded") or 0) == (c.get("textures") or 0) - 1, "not every readable render target's data arrived") + \
        expect((c.get("passTimings") or 0) >= 1, "no pass timings") + \
        expect("multisampled depth" in log, "the multisampled depth target's read-back was not reported")


def d3d12_stencil(state, log):
    # A D24S8 depth buffer: the pass's targets gain the stencil plane's read-back beside the depth's.
    c = capture(state)
    s = session(state)
    return check_connected(state, log) + check_capture_basic(state, log, textures=4) + \
        expect((c.get("textures") or 0) == 4, f"{c.get('textures')} textures (expected the color, depth and stencil targets and the sampled texture)") + \
        expect((s.get("validationErrors") or 0) == 0, f"{s.get('validationErrors')} validation errors")


def d3d12_replay_draws(state, log):
    # Measure draws on a D3D12 capture that did not measure them while it was taken: dxinsp_replay
    # runs the frame again with queries around every draw (src/d3d12/replay/src/dx_measure.cpp),
    # and writes the file vkinsp_replay does. The draw's counters say the frame really ran: the
    # two cubes are 72 vertices' worth of invocations and tens of thousands of pixels.
    c = capture(state)
    return check_connected(state, log) + check_capture_basic(state, log, timings=False) + \
        expect((c.get("drawStats") or 0) >= 1, f"{c.get('drawStats')} draws measured by the replay ({c.get('status')})") + \
        expect(c.get("drawStats") == c.get("drawStatsOnDraws"), f"{c.get('drawStatsOnDraws')} of {c.get('drawStats')} measurements name a draw or a dispatch") + \
        expect((c.get("drawStatsTimed") or 0) >= 1 and (c.get("drawStatsCounted") or 0) >= 1, f"timed {c.get('drawStatsTimed')}, counted {c.get('drawStatsCounted')}")


def d3d12_measure_shader(state, log):
    # Measure shader on D3D12: variants of the pixel shader's DXIL, written as LLVM IR text and
    # assembled by dxc (renderer/d3d12/dxil_ablate.ts), timed at the draw by dxinsp_replay. The
    # sample's --heavy shader is built so the answer is known: Fbm is nearly the whole stage, and
    # Blurred (sixteen samples of a small texture) nearly nothing. Function parts carry the id the
    # flame graph's frames have, which comes from the SPIR-V the same HLSL compiles to.
    c = capture(state)
    a = (c.get("ablations") or [{}])[0]
    parts = {p.get("name"): p for p in a.get("parts") or []}
    stage_ms = a.get("stageMs") or 0
    fbm = (parts.get("Fbm") or {}).get("savedMs") or 0
    blurred = (parts.get("Blurred") or {}).get("savedMs")
    return check_connected(state, log) + \
        expect(len(c.get("ablations") or []) == 1, f"{len(c.get('ablations') or [])} stages measured ({c.get('status')})") + \
        expect(a.get("stage") == "fragment" and stage_ms > 0, f"the stage was timed at {stage_ms} ms") + \
        expect(fbm > 0.5 * stage_ms, f"Fbm saved {fbm} ms of a {stage_ms} ms stage: it is nearly all of it") + \
        expect(blurred is not None and blurred < 0.25 * stage_ms, f"Blurred saved {blurred} ms of a {stage_ms} ms stage: it is nearly none of it") + \
        expect((parts.get("Fbm") or {}).get("functionId") is not None, "Fbm has no function id, so no frame of the flame graph is sized by it") + \
        expect(any(p.get("kind") == "line" for p in parts.values()), "no source line was measured") + \
        expect(any(p.get("kind") == "texture" for p in parts.values()), "no texture was measured")


def d3d12_draw_timings(state, log):
    # Measure draws on D3D12: the library puts a timestamp pair, a pipeline statistics query and an
    # occlusion query around every draw and dispatch as the list records (capture.cpp,
    # BeginDrawQueries), and sends them as CaptureDrawStats. The triangle's frame is small enough
    # that every draw should be measured, and counted: it binds its targets with
    # OMSetRenderTargets, so nothing is inside a BeginRenderPass region.
    c = capture(state)
    draws = c.get("draws") or 0
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect((c.get("drawStats") or 0) >= draws, f"{c.get('drawStats')} draws measured of {draws} in the frame") + \
        expect((c.get("drawStatsTimed") or 0) >= draws, f"{c.get('drawStatsTimed')} of {c.get('drawStats')} measured draws were timed") + \
        expect((c.get("drawStatsCounted") or 0) >= draws, f"{c.get('drawStatsCounted')} of {c.get('drawStats')} measured draws carried counters") + \
        expect((c.get("drawStatsOnDraws") or 0) >= draws,
               f"{c.get('drawStatsOnDraws')} of {c.get('drawStats')} measurements landed on a draw or dispatch command: "
               "the library names a command by its slot within its list, which is not its index in the capture") + \
        expect("draw profiling" in log, "the library never reported what it measured per draw")


def d3d12_mesh_output(state, log):
    # VS Out on D3D12: the draw's vertex shader outputs streamed out of the unmodified bytecode
    # while the next frame records (src/d3d12/src/mesh_output.cpp), so the last capture is the
    # measured one and its mesh tab opens on VS Out with the records in it.
    caps = (session(state).get("captures") or [])
    last = caps[-1] if caps else {}
    m = last.get("meshTab") or {}
    o = m.get("output") or {}
    stats = o.get("stats") or {}
    return check_connected(state, log) + \
        expect(len(caps) == 2, f"{len(caps)} captures: streaming a draw out should take one more capture, and only one") + \
        expect(m.get("stage") == "out", f"the mesh tab did not open on VS Out: {m.get('stage')}") + \
        expect(o.get("measured") is True, f"the draw was not streamed out: {o.get('note') or m.get('error')}") + \
        expect((o.get("vertices") or 0) > 0, f"no vertices were streamed out: {o}") + \
        expect((o.get("stride") or 0) > 0, f"the vertex record has no size: {o}") + \
        expect("POSITION" in " ".join(o.get("outputs") or []), f"the outputs hold no position: {o.get('outputs')}") + \
        expect(bool(stats) and (stats.get("primitives") or 0) > 0,
               "the clip-space positions were not read: the position output was not recognized") +         expect(((m.get("preview") or {}).get("edges") or 0) > 0,
               f"the mesh was measured but nothing was drawn: the records arrive after the layout, "
               f"and the view has to draw again when they land ({m.get('preview')})") + \
        expect("mesh output" in log, "the library never reported streaming a draw out")


def d3d12_draw_overlay(state, log):
    # A draw overlay on D3D12: the render target tab asks for one, the library measures it while
    # the *next* frame records (src/d3d12/src/draw_overlay.cpp), and that capture opens its own
    # tab with the overlay on. So the last capture is the measured one, and there must be exactly
    # two: a tab opened on the measured draw must not ask for a capture of its own.
    caps = (session(state).get("captures") or [])
    last = caps[-1] if caps else {}
    t = last.get("textureTab") or {}
    d = t.get("drawOverlay") or {}
    return check_connected(state, log) + \
        expect(len(caps) == 2, f"{len(caps)} captures: the overlay should take one more capture, and only one") + \
        expect(t.get("overlay") == "depth", f"the measured capture's tab did not open with the depth test overlay: {t.get('overlay')}") + \
        expect((t.get("target") or {}).get("kind") != "sampled",
               f"the tab opened on an image the frame sampled rather than the draw's render target: {t.get('target')}") + \
        expect(not t.get("drawError"), f"the overlay was not measured: {t.get('drawError')}") + \
        expect(d.get("measured") is True and d.get("mask") is True, f"the draw has no mask: {d}") + \
        expect((d.get("pixelsCovered") or 0) > 0, f"the draw covers no pixels: {d}") + \
        expect("draw overlay" in log, "the library never reported measuring a draw overlay")


def d3d12_bundle(state, log):
    # The draw sits in a bundle recorded at start-up: only record-always from launch sees it.
    return check_connected(state, log) + check_capture_basic(state, log)


def d3d12_offscreen(state, log):
    # No swap chain and no present (the Dawn-in-Chrome case): the frame boundary falls back to the
    # per-frame ExecuteCommandLists, and the capture is a full frame all the same.
    s = session(state)
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect(s.get("frameBoundary") == "submit", f"frame boundary {s.get('frameBoundary')!r} (expected the submit fallback)") + \
        expect(s.get("refreshSource") in ("", None), f"refresh source {s.get('refreshSource')!r} (a device that never presents has no display period)") + \
        expect("no present after" in log, "the layer did not fall back to the submit frame boundary") + \
        expect("submit boundary" in log, "the capture did not start on a submit boundary")


def d3d12_debug_pixel(state, log):
    d = debugger_tab(state)
    outputs = d.get("outputs") or []
    color = next((o.get("value") for o in outputs if o.get("location") == 0), None) or []
    target = (d.get("targetPixel") or {}).get("value") or []
    diff = max((abs(a - b) for a, b in zip(color, target)), default=None)
    # cube.hlsl's pixel shader at a pixel the draw covers: the HLSL dxc embedded (-Zi) compiled to
    # SPIR-V and stepped by line, its inputs rasterized from the vertex shader run in the
    # interpreter, the checker texture sampled through the root signature's static sampler, the root
    # constants read, and the color compared with the render target (the cube is the pass's only draw).
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect(bool(d), "--debug-view=debugger:pixel opened no debugger tab") + \
        expect(not d.get("error"), f"the debugger could not prepare the pixel: {d.get('error')}") + \
        expect(d.get("mode") == "source" and (d.get("codeLines") or 0) >= 10, f"cube.hlsl's source is not shown: {d.get('mode')}, {d.get('codeLines')} lines") + \
        expect(any("compiled to SPIR-V by dxc" in n for n in d.get("notes") or []), f"the notes do not say the HLSL was compiled to SPIR-V: {d.get('notes')}") + \
        expect(d.get("status") == "returned", f"the pixel shader did not run to the end: {d.get('status')} {d.get('invocationError')}") + \
        expect(not d.get("warnings"), f"the interpreter warned: {d.get('warnings')}") + \
        expect(len(color) == 4 and len(target) >= 3, f"no color to compare: output {color}, render target {target}") + \
        expect(diff is not None and diff < 0.02, f"the output {color} is not the render target's {target}")


def d3d12_debug_vertex(state, log):
    d = debugger_tab(state)
    # Stopped part way through cube.hlsl's VSMain: two lines stepped over, inside the source's own
    # function rather than dxc's wrapper around it. There is no replay to compare the outputs with.
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect(bool(d), "--debug-view=debugger:vertex opened no debugger tab") + \
        expect(not d.get("error"), f"the debugger could not prepare the vertex: {d.get('error')}") + \
        expect(d.get("mode") == "source", f"cube.hlsl's source is not what is stepped: {d.get('mode')}") + \
        expect(d.get("status") == "running" and d.get("line") in (35, 36, 37), f"not paused inside VSMain after two steps: {d.get('status')} line {d.get('line')}") + \
        expect((d.get("lineValues") or 0) > 0, "the line stepped over shows no values") + \
        expect(not d.get("warnings"), f"the interpreter warned: {d.get('warnings')}")


def d3d12_debug_compute(state, log):
    d = debugger_tab(state)
    # wave.hlsl's dispatch (--compute): thread (0, 0, 0) run to the end on the UAV and the root constants.
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect(bool(d), "--debug-view=debugger:compute opened no debugger tab") + \
        expect(not d.get("error"), f"the debugger could not prepare the thread: {d.get('error')}") + \
        expect(d.get("mode") == "source" and (d.get("codeLines") or 0) >= 10, f"wave.hlsl's source is not shown: {d.get('mode')}, {d.get('codeLines')} lines") + \
        expect(d.get("status") == "returned", f"the thread did not run to the end: {d.get('status')} {d.get('invocationError')}") + \
        expect(not d.get("warnings"), f"the interpreter warned: {d.get('warnings')}")


def d3d12_cases(triangle):
    launch = [f"--launch={triangle}"]
    saved = os.path.join(tempfile.gettempdir(), "gpuinsp_ui_d3d12.gpucap")

    def d3d12_open(state, log):
        c = capture(state)
        return expect(session(state).get("state") == "file", f"session state is {session(state).get('state')!r}") +             expect((c.get("commands") or 0) > 5, f"{c.get('commands')} commands in the reopened file") +             expect((c.get("draws") or 0) >= 1, f"{c.get('draws')} draws in the reopened file") +             expect((c.get("texturesLoaded") or 0) >= 2, "the reopened file lost its render targets")
    exported_cpp = os.path.join(tempfile.gettempdir(), "gpuinsp_ui_d3d12_export_cpp")

    def remove_exported_cpp():
        shutil.rmtree(exported_cpp, ignore_errors=True)

    def d3d12_export_cpp(state, log):
        # Export to C++ of a D3D12 capture (src/d3d12/replay/src/dx_exporter.h), which dxinsp_replay
        # writes. The frame draws from a bundle recorded at start-up, so the project holds the
        # bundle's recording, and its vertex data is what the list that executed it read back.
        projects = [os.path.join(exported_cpp, d) for d in os.listdir(exported_cpp)] if os.path.isdir(exported_cpp) else []
        project = projects[0] if projects else ""
        commands = ""
        if project and os.path.isfile(os.path.join(project, "frame_commands.cpp")):
            with open(os.path.join(project, "frame_commands.cpp"), encoding="utf-8") as f:
                commands = f.read()
        data = os.path.join(project, "frame_data.bin")
        return check_connected(state, log) + check_capture_basic(state, log) +             expect(len(projects) == 1 and project.endswith("_cpp"), f"one project folder named after the capture in {exported_cpp}: {projects}") +             expect(os.path.isfile(os.path.join(project, "CMakeLists.txt")), "the project has no CMakeLists.txt") +             expect(os.path.isfile(os.path.join(project, "dx_support.cpp")), "the project has no dx_support.cpp") +             expect(all(os.path.isfile(os.path.join(project, f)) for f in ("frame_window.h", "frame_window_win32.cpp", "frame_restore.cpp")), "the project lacks its window source or frame_restore.cpp") +             expect("->ExecuteBundle(" in commands, "frame_commands.cpp does not execute the bundle") +             expect("->DrawIndexedInstanced(" in commands, "frame_commands.cpp does not hold the bundle's draw") +             expect("ReadbackTexture(" in commands, "frame_commands.cpp reads no render target back to compare") +             expect(os.path.isfile(data) and os.path.getsize(data) > 100000, "frame_data.bin is missing or too small to hold the captured targets")

    export_cases = [Case("d3d12-export-cpp", launch + ["--args=--bundle", "--record-always", "--debug-capture", f"--debug-export-cpp={exported_cpp}"],
                         d3d12_export_cpp, delay_ms=20000, before=remove_exported_cpp)] if find_d3d12_replay() else []
    if not export_cases:
        print("  (no dxinsp_replay build: skipping the D3D12 Export to C++ case)")
    return export_cases + [
        Case("d3d12-plain", launch + ["--args=--compute", "--debug-capture", "--debug-command=22", "--debug-expand=Vertex Shader",
                                      f"--debug-save={saved}"], d3d12_plain, delay_ms=16000),
        Case("d3d12-render-pass", launch + ["--args=--render-pass --msaa --indirect", "--debug-capture"], d3d12_render_pass),
        Case("d3d12-timing-capture", launch + ["--debug-timing=3000"], timing_capture, delay_ms=9000),
        Case("d3d12-memory-capture", launch + ["--args=--churn", "--debug-memory=3000"], memory_capture, delay_ms=9000),
        Case("d3d12-app-capture", launch + ["--args=--capture-at 200"], app_capture, delay_ms=14000),
        Case("d3d12-shader-edit", launch + ["--debug-capture", "--debug-view=shader-edit", "--debug-settle=12000"], shader_edit, delay_ms=26000),
        Case("d3d12-mesh-output", launch + ["--debug-capture", "--debug-view=mesh"],
             d3d12_mesh_output, delay_ms=26000),
        Case("d3d12-draw-overlay", launch + ["--debug-capture", "--debug-view=overlay:depth:last"],
             d3d12_draw_overlay, delay_ms=26000),
        Case("d3d12-draw-timings", launch + ["--args=--compute", "--debug-capture", "--debug-capture-with=draws"],
             d3d12_draw_timings, delay_ms=16000),
        Case("d3d12-replay-draws", launch + ["--args=--compute", "--debug-capture", "--debug-view=flame:draws", "--debug-settle=8000"],
             d3d12_replay_draws, delay_ms=24000),
        Case("d3d12-measure-shader", launch + ["--args=--heavy", "--debug-capture", "--debug-view=flame:shader", "--debug-settle=20000"],
             d3d12_measure_shader, delay_ms=40000),
        Case("d3d12-bundle", launch + ["--args=--bundle", "--record-always", "--debug-capture"], d3d12_bundle),
        Case("d3d12-stencil", launch + ["--args=--stencil", "--validation", "--debug-capture"], d3d12_stencil, delay_ms=16000),
        Case("d3d12-offscreen", launch + ["--args=--offscreen --compute", "--debug-capture"], d3d12_offscreen, delay_ms=14000),
        Case("d3d12-open", [f"--debug-open={saved}", "--debug-command=22"], d3d12_open, delay_ms=9000),
        # The shader debugger on a D3D12 capture: the stage's HLSL (embedded by the sample's -Zi
        # build) compiled to SPIR-V by dxc and stepped; no replay is involved. --debug-settle waits
        # for the compile and, for a pixel, the vertex shader run over the draw.
        Case("d3d12-debug-pixel", launch + ["--debug-capture", "--debug-view=debugger:pixel::end", "--debug-settle=8000"],
             d3d12_debug_pixel, delay_ms=20000),
        Case("d3d12-debug-vertex", launch + ["--debug-capture", "--debug-view=debugger:vertex::2", "--debug-settle=8000"],
             d3d12_debug_vertex, delay_ms=20000),
        Case("d3d12-debug-compute", launch + ["--args=--compute", "--debug-capture", "--debug-view=debugger:compute::end", "--debug-settle=8000"],
             d3d12_debug_compute, delay_ms=20000),
    ]


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
    ap.add_argument("--no-metal", action="store_true", help="skip the live Metal cases (macOS)")
    ap.add_argument("--unity", help="a Unity player (.app) to run the Metal cases against a real frame too")
    ap.add_argument("--no-d3d12", action="store_true", help="skip the live Direct3D 12 cases (Windows)")
    args = ap.parse_args()
    if not electron():
        print("electron not installed: run npm install in src/app/", file=sys.stderr)
        return 2
    cases = []
    if not args.no_triangle:
        triangle = find_triangle()
        if not triangle:
            print("vkinsp_triangle not built (cmake --build build --config Release --target vkinsp_triangle)", file=sys.stderr)
            return 2
        cases += triangle_cases(triangle)
    if not args.no_metal:
        metal_triangle = find_metal_triangle()
        if metal_triangle:
            cases += metal_cases(metal_triangle)
        elif sys.platform == "darwin":
            print("  (mtlinsp_triangle not built: skipping the Metal cases)")
    if args.unity:
        # Independent of --no-metal: a Unity player is a different target, not another sample case.
        if not os.path.exists(args.unity):
            print(f"--unity: {args.unity} does not exist", file=sys.stderr)
            return 2
        cases += unity_cases(args.unity)
    if not args.no_d3d12:
        d3d12_triangle = find_d3d12_triangle()
        if d3d12_triangle:
            cases += d3d12_cases(d3d12_triangle)
        elif sys.platform == "win32":
            print("  (dxinsp_triangle not built: skipping the Direct3D 12 cases)")
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
