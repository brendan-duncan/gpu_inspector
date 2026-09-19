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
    # The pixel history of the centre of the first colour target (--debug-view=pixel-history, no
    # click needed): the pass it starts from, the draw that wrote it, and which of that draw's
    # primitives the winning fragment came from (the replay's primitive-id pass).
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect(not h.get("error"), f"the pixel history failed: {h.get('error')}") + \
        expect(bool(touched), f"the pixel history lists no events: {h}") + \
        expect(any("begins" in e for e in touched), f"no pass start in the pixel history: {touched}") + \
        expect(any("wrote the pixel" in e for e in touched), f"no draw wrote the pixel: {touched}") + \
        expect(any("primitive" in e for e in touched),
               f"no draw names the primitive that won the pixel (the primitive-id pass): {touched}")


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
    colour = next((o.get("value") for o in outputs if o.get("location") == 0), None) or []
    target = (d.get("targetPixel") or {}).get("value") or []
    diff = max((abs(a - b) for a, b in zip(colour, target)), default=None)
    # The cube's fragment shader at a pixel the draw covers: its inputs rasterized from the replay's
    # vertex outputs, the checker texture sampled with derivatives from the pixel quad, and the colour
    # it writes compared with the render target (the cube is the pass's only draw there).
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect(bool(d), "--debug-view=debugger opened no debugger tab") + \
        expect(not d.get("error"), f"the debugger could not prepare the pixel: {d.get('error')}") + \
        expect(d.get("mode") == "source" and (d.get("codeLines") or 0) >= 10, f"cube.frag's source is not shown: {d.get('mode')}, {d.get('codeLines')} lines") + \
        expect(d.get("status") == "returned", f"the fragment did not run to the end: {d.get('status')} {d.get('invocationError')}") + \
        expect(not d.get("warnings"), f"the interpreter warned: {d.get('warnings')}") + \
        expect(len(colour) == 4 and len(target) >= 3, f"no colour to compare: output {colour}, render target {target}") + \
        expect(diff is not None and diff < 0.02, f"the output {colour} is not the render target's {target}")


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
    colour = next((o.get("value") for o in outputs if o.get("location") == 0), None) or []
    target = (d.get("targetPixel") or {}).get("value") or []
    diff = max((abs(a - b) for a, b in zip(colour, target)), default=None)
    # The triangle pass's fragment_main is specialized: its tint branch is behind
    # `[[function_constant(0)]]`, and the values are only knowable because the capture library
    # watched the setters of the MTLFunctionConstantValues (src/metal/src/function_constants.h). Without
    # them the branch is not taken and the colour is the untinted one, which the comparison catches
    # — and the interpreter says so in its warnings, which is the clearer diagnosis of the two.
    return check_connected(state, log) + check_metal_capture(state, log) + \
        expect(bool(d), "--debug-view=debugger:pixel opened no debugger tab") + \
        expect(not d.get("error"), f"the debugger could not prepare the pixel: {d.get('error')}") + \
        expect(d.get("status") == "returned", f"the fragment did not run to the end: {d.get('status')} {d.get('invocationError')}") + \
        expect(not d.get("warnings"), f"the interpreter warned: {d.get('warnings')}") + \
        expect(len(colour) == 4 and len(target) >= 3, f"no colour to compare: output {colour}, render target {target}") + \
        expect(diff is not None and diff < 0.02,
               f"the output {colour} is not the render target's {target}: the function constants the "
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
    colour = next((o.get("value") for o in outputs if o.get("location") == 0), None) or []
    target = (d.get("targetPixel") or {}).get("value") or []
    diff = max((abs(a - b) for a, b in zip(colour, target)), default=None)
    # The blit pass's fragment: its varyings rasterized from the vertex shader the interpreter ran,
    # its texture sampled from the read-back the capture made of what the draw bound, and the colour
    # it writes compared with the render target (the blit is the pass's only draw).
    return check_connected(state, log) + check_metal_capture(state, log) + \
        expect(bool(d), "--debug-view=debugger:pixel opened no debugger tab") + \
        expect(not d.get("error"), f"the debugger could not prepare the pixel: {d.get('error')}") + \
        expect(d.get("status") == "returned", f"the fragment did not run to the end: {d.get('status')} {d.get('invocationError')}") + \
        expect(not d.get("warnings"), f"the interpreter warned: {d.get('warnings')}") + \
        expect(len(colour) == 4 and len(target) >= 3, f"no colour to compare: output {colour}, render target {target}") + \
        expect(diff is not None and diff < 0.02, f"the output {colour} is not the render target's {target}")


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
    # The pixel history of the centre of the first colour target, which in this sample is the
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


def metal_cases(triangle):
    launch = [f"--launch={triangle}"]
    return [
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
    return check_connected(state, log) + check_capture_basic(state, log) +         expect(set(f) <= {"unread-store", "oversynchronized-barrier"}, f"unexpected findings {f}") +         expect(s.get("frameBoundary") == "present", f"frame boundary {s.get('frameBoundary')!r}") +         expect(s.get("refreshSource") == "monitor", f"refresh source {s.get('refreshSource')!r}") +         expect("injected" in log, "the launcher did not report injecting the D3D12 library") +         expect("capture sent" in log, "the library never finished the capture")


def d3d12_render_pass(state, log):
    # BeginRenderPass / EndRenderPass with a multisampled target: the colour target is read back
    # through a resolve; multisampled depth is reported as not read back, not silently missing.
    # Statistics and occlusion queries are not begun inside a render pass region, so the pass
    # has a timing but no counters (check_capture_basic would ask for them).
    c = capture(state)
    return check_connected(state, log) +         expect(bool(c), "no capture tab") +         expect((c.get("commands") or 0) > 5, f"{c.get('commands')} commands captured") +         expect((c.get("draws") or 0) >= 1, f"{c.get('draws')} draws") +         expect((c.get("textures") or 0) >= 2, f"{c.get('textures')} render targets read back") +         expect((c.get("textureErrors") or 0) == 1, f"{c.get('textureErrors')} render targets failed to read back (the multisampled depth target is expected to)") +         expect((c.get("texturesLoaded") or 0) == (c.get("textures") or 0) - 1, "not every readable render target's data arrived") +         expect((c.get("passTimings") or 0) >= 1, "no pass timings") +         expect("multisampled depth" in log, "the multisampled depth target's read-back was not reported")


def d3d12_stencil(state, log):
    # A D24S8 depth buffer: the pass's targets gain the stencil plane's read-back beside the depth's.
    c = capture(state)
    s = session(state)
    return check_connected(state, log) + check_capture_basic(state, log, textures=4) + \
        expect((c.get("textures") or 0) == 4, f"{c.get('textures')} textures (expected the colour, depth and stencil targets and the sampled texture)") + \
        expect((s.get("validationErrors") or 0) == 0, f"{s.get('validationErrors')} validation errors")


def d3d12_bundle(state, log):
    # The draw sits in a bundle recorded at start-up: only record-always from launch sees it.
    return check_connected(state, log) + check_capture_basic(state, log)


def d3d12_offscreen(state, log):
    # No swap chain and no present (the Dawn-in-Chrome case): the frame boundary falls back to the
    # per-frame ExecuteCommandLists, and the capture is a full frame all the same.
    s = session(state)
    return check_connected(state, log) + check_capture_basic(state, log) +         expect(s.get("frameBoundary") == "submit", f"frame boundary {s.get('frameBoundary')!r} (expected the submit fallback)") +         expect(s.get("refreshSource") in ("", None), f"refresh source {s.get('refreshSource')!r} (a device that never presents has no display period)") +         expect("no present after" in log, "the layer did not fall back to the submit frame boundary") +         expect("submit boundary" in log, "the capture did not start on a submit boundary")


def d3d12_debug_pixel(state, log):
    d = debugger_tab(state)
    outputs = d.get("outputs") or []
    colour = next((o.get("value") for o in outputs if o.get("location") == 0), None) or []
    target = (d.get("targetPixel") or {}).get("value") or []
    diff = max((abs(a - b) for a, b in zip(colour, target)), default=None)
    # cube.hlsl's pixel shader at a pixel the draw covers: the HLSL dxc embedded (-Zi) compiled to
    # SPIR-V and stepped by line, its inputs rasterized from the vertex shader run in the
    # interpreter, the checker texture sampled through the root signature's static sampler, the root
    # constants read, and the colour compared with the render target (the cube is the pass's only draw).
    return check_connected(state, log) + check_capture_basic(state, log) + \
        expect(bool(d), "--debug-view=debugger:pixel opened no debugger tab") + \
        expect(not d.get("error"), f"the debugger could not prepare the pixel: {d.get('error')}") + \
        expect(d.get("mode") == "source" and (d.get("codeLines") or 0) >= 10, f"cube.hlsl's source is not shown: {d.get('mode')}, {d.get('codeLines')} lines") + \
        expect(any("compiled to SPIR-V by dxc" in n for n in d.get("notes") or []), f"the notes do not say the HLSL was compiled to SPIR-V: {d.get('notes')}") + \
        expect(d.get("status") == "returned", f"the pixel shader did not run to the end: {d.get('status')} {d.get('invocationError')}") + \
        expect(not d.get("warnings"), f"the interpreter warned: {d.get('warnings')}") + \
        expect(len(colour) == 4 and len(target) >= 3, f"no colour to compare: output {colour}, render target {target}") + \
        expect(diff is not None and diff < 0.02, f"the output {colour} is not the render target's {target}")


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
    return [
        Case("d3d12-plain", launch + ["--args=--compute", "--debug-capture", "--debug-command=22", "--debug-expand=Vertex Shader",
                                      f"--debug-save={saved}"], d3d12_plain, delay_ms=16000),
        Case("d3d12-render-pass", launch + ["--args=--render-pass --msaa --indirect", "--debug-capture"], d3d12_render_pass),
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
