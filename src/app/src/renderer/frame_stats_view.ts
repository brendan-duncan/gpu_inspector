// The Frame Stats report: the capture's statistics (capture_statistics.ts) as WebGPU Inspector
// shows them, one card per section, with the Frame Bound card, the Frame Issues list and the pass
// timings above them.
import { Button } from "./widget/button.js";
import { Checkbox } from "./widget/checkbox.js";
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import { Widget } from "./widget/widget.js";
import { REFRESH_SOURCE_NOTE, frameBound, type CaptureStatistics } from "./capture_statistics.js";
import { cpuVerdict, summarizeCpuTimeline } from "./cpu_timeline.js";
import {
  MIN_VIEW_MS, axisTicks, buildTimelineTracks, clampView, fullView, gpuGaps, panView, tracksVerdict, visibleBoxes,
  zoomView, type LabelledPass, type SpanBox, type TimelineInput, type TimelineView, type Track,
} from "./timeline_tracks.js";
import type { CpuTimelineMessage } from "../shared/protocol.js";
import type { FrameFinding } from "./vulkan/frame_analysis.js";
import { formatBytes } from "./vulkan/vulkan_object.js";

/** GPU pass timings of a capture (Profile passes) with the live frame and submit times for the Frame Bound card. */
export interface FrameTimingInfo {
  frameMs: number;       // live frame interval
  refreshMs: number;     // display refresh interval while vsync was on (the budget), 0 without
  refreshSource?: string; // "present_timing" | "display_timing" | "monitor" | "estimate"
  submitMs: number;      // CPU time per frame inside the submit call
  gpuSpanMs: number;     // first pass start to last pass end
  gpuTotalMs: number;    // sum of pass durations
  frames: number;
  passes: { label: string; durationMs: number; startMs: number; onJump: () => void }[];
  /**
   * What submitting is called on this backend, for the notes: naming vkQueueSubmit to someone
   * reading a Metal capture describes a function their application never calls.
   */
  submitCall?: string;
}

/** The call the submit time is spent in, per backend (SUBMIT_CALL[api]). */
export const SUBMIT_CALL: Record<string, string> = {
  vulkan: "vkQueueSubmit", metal: "commit", d3d12: "ExecuteCommandLists",
};

/** "Frame Bound" card: the verdict of frameBound() with the three times it compared as bars. */
function renderFrameBound(root: Widget, t: FrameTimingInfo): void {
  const bound = frameBound(t);
  if (!bound) return;
  const budget = bound.budgetMs;
  const card = new Div(root, { class: "frame-stats-section" });
  new Div(card, { text: "Frame Bound", class: "frame-stats-heading" });
  const body = new Div(card, { class: "frame-stats-list" });
  new Div(body, { text: bound.verdict, class: `frame-bound-verdict frame-bound-${bound.kind}` });
  const bar = (label: string, ms: number, color: string): void => {
    const row = new Div(body, { class: "frame-bound-row" });
    new Div(row, { text: label, class: "frame-bound-label" });
    const track = new Div(row, { class: "frame-bound-track" });
    const fill = new Div(track, { class: "frame-bound-fill" });
    fill.style.width = `${Math.min(100, (ms / budget) * 100).toFixed(1)}%`;
    fill.style.background = color;
    new Div(row, { text: `${ms.toFixed(2)} ms  (${((ms / budget) * 100).toFixed(0)}% of ${budget.toFixed(2)} ms)`, class: "frame-bound-value" });
  };
  bar("GPU (pass span)", bound.gpuMs, "#4a8db8");
  bar("CPU (submit)", t.submitMs, "#5fd08a");
  if (bound.vsync) bar("Frame interval", t.frameMs, "#a0a0a0");
  if (bound.distorted) {
    new Div(body, {
      text: "The GPU bar is the captured passes and the frame interval is the application running normally, "
        + "without a capture. Capturing adds queries around every pass and reads every render target back, so "
        + "the captured frame is the more expensive one; the two bars are not on the same footing here.",
      class: "text-muted font-sm",
    });
    return;
  }
  const submitCall = t.submitCall ?? SUBMIT_CALL.vulkan;
  new Div(body, {
    text: bound.vsync
      ? `The budget is the display refresh period (${(1000 / t.refreshMs).toFixed(0)} Hz, ${REFRESH_SOURCE_NOTE[t.refreshSource ?? ""] ?? "estimated from the frame intervals while vsync is on"}). GPU time is the span of this capture's timed passes; CPU is the time inside ${submitCall}, so work outside submission counts as headroom here.`
      : `The budget is the live frame interval (vsync is off, so no display refresh period applies). GPU time is the span of this capture's timed passes; CPU is the time inside ${submitCall}, so work outside submission counts as headroom here.`,
    class: "text-muted font-sm",
  });
}

/**
 * "Where the CPU went": the calls the layer timed on the host during the capture
 * (src/vulkan/src/cpu_timeline.h). The Frame Bound card above compares three aggregates and infers
 * a verdict; this one measures where the CPU actually was, which tells a frame waiting for the GPU
 * apart from a frame paced by the display — the same totals, opposite fixes.
 */
function renderCpuTimeline(root: Widget, timeline: CpuTimelineMessage | null): void {
  const s = summarizeCpuTimeline(timeline);
  if (!s) return;
  const card = new Div(root, { class: "frame-stats-section" });
  new Div(card, { text: "Where the CPU went", class: "frame-stats-heading" });
  const body = new Div(card, { class: "frame-stats-list" });
  new Div(body, { text: cpuVerdict(s), class: "frame-bound-verdict" });
  for (const t of s.totals) {
    const row = new Div(body, { class: "frame-bound-row" });
    new Div(row, { text: t.label, class: "frame-bound-label" });
    const track = new Div(row, { class: "frame-bound-track" });
    const fill = new Div(track, { class: "frame-bound-fill" });
    fill.style.width = `${Math.min(100, s.spanMs > 0 ? (t.ms / s.spanMs) * 100 : 0).toFixed(1)}%`;
    // Waiting for the GPU, waiting for the display, and doing work each read differently.
    fill.style.background = t.kind === "gpuWait" ? "#4a8db8" : t.kind === "displayWait" ? "#a0a0a0" : "#5fd08a";
    new Div(row, { text: `${t.ms.toFixed(2)} ms in ${t.calls} call${t.calls === 1 ? "" : "s"}`, class: "frame-bound-value" });
  }
  const notes = [`Measured over ${s.spanMs.toFixed(2)} ms and ${s.frames} frame${s.frames === 1 ? "" : "s"} on ${s.threads} thread${s.threads === 1 ? "" : "s"}.`];
  notes.push(s.calibrated
    ? "The GPU's clock was related to the CPU's, so the pass timings above sit on this same axis."
    : "This device has no calibrated-timestamps extension, so the GPU pass times keep their own origin and cannot be laid over these.");
  if (s.dropped) notes.push(`${s.dropped} later calls were not recorded: the capture's event limit was reached.`);
  new Div(body, { text: notes.join(" "), class: "text-muted font-sm" });
}

/** The GPU half of the Timeline card: the capture's timed passes and the tick they are measured from. */
export interface GpuTrackInput {
  passes: LabelledPass[];
  originTicks: number | null;
}

/** The color of a span, which is the color "Where the CPU went" already uses for that kind. */
const SPAN_COLOR: Record<string, string> = {
  gpuWait: "#4a8db8", displayWait: "#a0a0a0", work: "#5fd08a", gpu: "#8a6fd0", compile: "#d07a3a",
};

/** The tooltip of a box: what it was, how long, and — when it stands for several — how many. */
function boxTitle(box: SpanBox): string {
  if (box.count === 1 && box.span) {
    return `${box.span.label}: ${box.span.durationMs.toFixed(3)} ms at ${box.span.startMs.toFixed(3)} ms`
      + (box.span.select ? "\nClick to select it in the command list" : "");
  }
  return `${box.count} spans between ${box.startMs.toFixed(3)} and ${(box.startMs + box.durationMs).toFixed(3)} ms, `
    + `${box.busyMs.toFixed(3)} ms of them inside a call.\nToo close together to draw apart at this zoom — click to zoom in.`;
}

/**
 * "Timeline": every thread's timed calls and every timed pass drawn against one axis
 * (renderer/timeline_tracks.ts). The two cards above total time per category and per pass, which
 * answers how much but never when — and a GPU left idle waiting for a late submission is a gap
 * between spans, so it has no size in any total and only a drawing shows it.
 *
 * The lanes are zoomable and pannable, because at frame scale a real frame is not readable: 4,000
 * draws over 16 ms put every span inside a pixel. What the card draws is therefore a view of the
 * range rather than the whole of it, and only the spans that view reaches become boxes — so the
 * cost of drawing follows the width of the lane and not the size of the frame.
 */
function renderTimelineTracks(root: Widget, input: TimelineInput): void {
  const t = buildTimelineTracks(input);
  if (!t) return;
  const card = new Div(root, { class: "frame-stats-section" });
  new Div(card, { text: "Timeline", class: "frame-stats-heading" });
  const body = new Div(card, { class: "frame-stats-list" });
  new Div(body, { text: tracksVerdict(t), class: "frame-bound-verdict" });

  const gaps = gpuGaps(t);
  let view = fullView(t);

  const bar = new Div(body, { class: "track-toolbar" });
  // The buttons come before the readout so that they keep still: the readout's width changes with
  // every zoom, and a control that moves under the pointer is a control that gets missed.
  const controls = new Div(bar, { class: "track-controls" });
  const readout = new Div(bar, { class: "track-range" });
  const lanesBox = new Div(body, { class: "track-lanes", tabIndex: 0 });
  // The rows are built once and only their boxes are replaced, so panning does not rebuild the
  // labels, the busy figures or the lane elements the pointer is working against.
  const lanes: { track: Track; lane: Div; boxes: SpanBox[] }[] = [];
  for (const track of t.tracks) {
    const row = new Div(lanesBox, { class: "track-row" });
    new Div(row, { text: track.label, class: "track-label", title: track.label });
    const entry = { track, lane: new Div(row, { class: `track-lane${track.kind === "gpu" ? " track-lane-gpu" : ""}` }), boxes: [] as SpanBox[] };
    lanes.push(entry);
    new Div(row, { text: `${((track.busyMs / t.spanMs) * 100).toFixed(0)}%`, class: "track-busy",
      title: `${track.spans.length} spans, ${track.busyMs.toFixed(3)} ms of the ${t.spanMs.toFixed(3)} ms range` });
    // The lane takes the click rather than the boxes on it. A box can be a single pixel wide —
    // that is the whole reason this view zooms — and a target that narrow cannot be hit, so a
    // click takes the nearest box within a few pixels instead of only a direct one.
    entry.lane.element.onclick = (e: MouseEvent) => {
      if (dragged) return;                       // the end of a pan, not a click on what it ended over
      const box = boxNear(entry, e.clientX);
      if (!box) return;
      // A box that names one pass selects it; one that stands for several can only be taken apart
      // by looking closer, so that is what clicking it does.
      if (box.count === 1 && box.span?.select) box.span.select();
      else zoomTo(box.startMs, box.durationMs);
    };
  }
  const axis = new Div(body, { class: "track-axis" });
  // The window this view is of the whole range: an affordance that a zoomed lane is part of
  // something longer, and a way to move it that does not need the lane itself.
  const scrub = new Div(body, { class: "track-scrub", title: "The part of the capture shown above. Drag to move it." });
  const scrubWindow = new Div(scrub, { class: "track-scrub-window" });

  const laneWidth = (): number => Math.max(1, lanes[0]?.lane.element.clientWidth || 0);
  /** Where in the view a page x sits, as a fraction, which is what a zoom anchors on. */
  const fractionAt = (clientX: number): number => {
    const rect = lanes[0].lane.element.getBoundingClientRect();
    return Math.min(1, Math.max(0, (clientX - rect.left) / Math.max(1, rect.width)));
  };

  /** How far a click may land from a box and still count as on it. */
  const CLICK_SLACK_PX = 4;

  /** The box a click at `clientX` means: the one under it, or the nearest within a few pixels. */
  const boxNear = (entry: { lane: Div; boxes: SpanBox[] }, clientX: number): SpanBox | null => {
    const rect = entry.lane.element.getBoundingClientRect();
    const at = view.startMs + view.spanMs * Math.min(1, Math.max(0, (clientX - rect.left) / Math.max(1, rect.width)));
    const slack = CLICK_SLACK_PX * (view.spanMs / Math.max(1, rect.width));
    let best: SpanBox | null = null;
    let bestDistance = Infinity;
    for (const b of entry.boxes) {
      const distance = Math.max(0, b.startMs - at, at - (b.startMs + b.durationMs));
      if (distance > slack || distance >= bestDistance) continue;
      best = b;
      bestDistance = distance;
    }
    return best;
  };

  const draw = (): void => {
    const width = laneWidth();
    // One pixel's worth of time: what decides both the floor on a box's width and which spans are
    // too close together to draw apart. It comes from the lane's own width rather than a constant,
    // so zooming in really does separate them instead of stopping at a fixed resolution.
    const minMs = view.spanMs / width;
    const pct = (ms: number): string => `${(((ms - view.startMs) / view.spanMs) * 100).toFixed(4)}%`;
    const widthPct = (ms: number): string => `${((Math.max(ms, minMs) / view.spanMs) * 100).toFixed(4)}%`;
    for (const entry of lanes) {
      const { track, lane } = entry;
      lane.removeAllChildren();
      // The idle stretches go in first, so a span drawn over one still reads on top.
      if (track.kind === "gpu") {
        for (const g of gaps) {
          if (g.startMs + g.durationMs <= view.startMs || g.startMs >= view.startMs + view.spanMs) continue;
          const box = new Div(lane, { class: "track-gap" });
          box.style.left = pct(g.startMs);
          box.style.width = widthPct(g.durationMs);
          box.element.title = `${g.durationMs.toFixed(2)} ms with no pass running`;
        }
      }
      entry.boxes = visibleBoxes(track, view, minMs);
      for (const b of entry.boxes) {
        const box = new Div(lane, { class: `track-span${b.count === 1 && b.span?.select ? " track-span-link" : ""}` });
        box.style.left = pct(b.startMs);
        // A call lasting microseconds would otherwise be sub-pixel and vanish.
        box.style.width = `max(1px, ${widthPct(b.durationMs)})`;
        box.style.background = SPAN_COLOR[b.kind] ?? "#5fd08a";
        box.element.title = boxTitle(b);
      }
    }
    axis.removeAllChildren();
    for (const tick of axisTicks(view)) {
      const mark = new Div(axis, { class: "track-tick", text: tick.label });
      mark.style.left = pct(tick.ms);
    }
    readout.text = view.spanMs >= t.spanMs
      ? `0 – ${t.spanMs.toFixed(3)} ms (the whole capture)`
      : `${view.startMs.toFixed(3)} – ${(view.startMs + view.spanMs).toFixed(3)} ms `
        + `(${view.spanMs.toFixed(3)} ms of ${t.spanMs.toFixed(3)} ms, ${(t.spanMs / view.spanMs).toFixed(0)}x)`;
    scrubWindow.style.left = `${((view.startMs / t.spanMs) * 100).toFixed(4)}%`;
    scrubWindow.style.width = `${Math.max(0.5, (view.spanMs / t.spanMs) * 100).toFixed(4)}%`;
    scrub.classList.toggle("track-scrub-full", view.spanMs >= t.spanMs);
  };

  const setView = (next: TimelineView): void => { view = clampView(next, t); draw(); };
  /** Zoom onto a stretch, with a margin either side so what is beside it is still in the picture. */
  const zoomTo = (startMs: number, durationMs: number): void => {
    const spanMs = Math.max(MIN_VIEW_MS, durationMs * 1.4);
    setView({ startMs: startMs + durationMs / 2 - spanMs / 2, spanMs });
  };

  const zoomButton = (label: string, title: string, factor: number): void => {
    new Button(controls, { label, title, class: "track-btn", callback: () => setView(zoomView(view, t, factor)) });
  };
  zoomButton("−", "Zoom out (or Ctrl and the wheel over the lanes)", 1 / 2);
  zoomButton("+", "Zoom in (or Ctrl and the wheel over the lanes)", 2);
  new Button(controls, { label: "Fit", title: "Show the whole capture again", class: "track-btn",
    callback: () => setView(fullView(t)) });
  new Div(bar, {
    text: "Drag to pan · Ctrl and the wheel to zoom · double-click to zoom in · click a pass to select it",
    class: "text-muted font-sm track-hint",
  });

  // Ctrl and the wheel zooms, as it does on the texture viewer; shift and the wheel pans. A plain
  // wheel is left alone, since this card sits in a long report the reader is scrolling through.
  lanesBox.element.addEventListener("wheel", (e: WheelEvent) => {
    if (e.ctrlKey || e.metaKey) {
      e.preventDefault();
      setView(zoomView(view, t, e.deltaY < 0 ? 1.25 : 1 / 1.25, fractionAt(e.clientX)));
    } else if (e.shiftKey) {
      e.preventDefault();
      setView(panView(view, t, (e.deltaY > 0 ? 0.15 : -0.15)));
    }
  }, { passive: false });

  // Dragging the lanes pans them. A drag that moved is not also a click on the span it ended over.
  let dragged = false;
  lanesBox.element.addEventListener("mousedown", (e: MouseEvent) => {
    if (e.button !== 0) return;
    const startX = e.clientX;
    const startMs = view.startMs;
    const msPerPixel = view.spanMs / laneWidth();
    dragged = false;
    const move = (m: MouseEvent): void => {
      if (Math.abs(m.clientX - startX) > 3) dragged = true;
      if (dragged) setView({ startMs: startMs - (m.clientX - startX) * msPerPixel, spanMs: view.spanMs });
    };
    const up = (): void => {
      window.removeEventListener("mousemove", move);
      window.removeEventListener("mouseup", up);
      lanesBox.classList.remove("track-dragging");
      // Cleared after the click the release produces, so the click knows it ended a drag.
      setTimeout(() => { dragged = false; }, 0);
    };
    window.addEventListener("mousemove", move);
    window.addEventListener("mouseup", up);
    lanesBox.classList.add("track-dragging");
  });
  lanesBox.element.addEventListener("dblclick", (e: MouseEvent) => {
    // Zoom in on what was double-clicked, whatever it was: the lane background as well as a span.
    const at = view.startMs + view.spanMs * fractionAt(e.clientX);
    zoomTo(at - view.spanMs / 8, view.spanMs / 4);
  });
  lanesBox.element.addEventListener("keydown", (e: KeyboardEvent) => {
    const key = e.key;
    if (key === "ArrowRight") setView(panView(view, t, 0.2));
    else if (key === "ArrowLeft") setView(panView(view, t, -0.2));
    else if (key === "+" || key === "=") setView(zoomView(view, t, 2));
    else if (key === "-" || key === "_") setView(zoomView(view, t, 1 / 2));
    else if (key === "0" || key === "Home") setView(fullView(t));
    else return;
    e.preventDefault();
  });

  // The scrub bar moves the view without touching the lanes: a click or a drag centers it there.
  const scrubTo = (clientX: number): void => {
    const rect = scrub.element.getBoundingClientRect();
    const at = t.spanMs * Math.min(1, Math.max(0, (clientX - rect.left) / Math.max(1, rect.width)));
    setView({ startMs: at - view.spanMs / 2, spanMs: view.spanMs });
  };
  scrub.element.addEventListener("mousedown", (e: MouseEvent) => {
    if (e.button !== 0) return;
    e.preventDefault();
    scrubTo(e.clientX);
    const move = (m: MouseEvent): void => scrubTo(m.clientX);
    const up = (): void => { window.removeEventListener("mousemove", move); window.removeEventListener("mouseup", up); };
    window.addEventListener("mousemove", move);
    window.addEventListener("mouseup", up);
  });

  // The boxes are sized in pixels' worth of time, so a lane that changes width has to be redrawn:
  // the panel is resizable, and the card is often laid out before it has its final width.
  if (typeof ResizeObserver !== "undefined") {
    let width = 0;
    new ResizeObserver(() => {
      const now = laneWidth();
      if (now === width) return;
      width = now;
      draw();
    }).observe(lanesBox.element);
  }
  draw();

  const legend = new Div(body, { class: "track-legend" });
  const key = (color: string, label: string): void => {
    const item = new Span(legend, {});
    const swatch = new Span(item, { class: "track-key" });
    swatch.style.background = color;
    new Span(item, { text: label });
  };
  key(SPAN_COLOR.work, "Submitting");
  key(SPAN_COLOR.gpuWait, "Waiting for the GPU");
  key(SPAN_COLOR.displayWait, "Paced by the display");
  // Only when the frame actually built one: a legend entry for something absent reads as a
  // category that happened to be empty rather than one that never applies.
  if (t.tracks.some((track) => track.spans.some((s) => s.kind === "compile"))) {
    key(SPAN_COLOR.compile, "Creating pipelines");
  }
  if (t.hasGpu) key(SPAN_COLOR.gpu, "GPU pass");
  if (t.gpuNote) new Div(body, { text: t.gpuNote, class: "text-muted font-sm" });
  if (t.hasGpu) {
    new Div(body, {
      text: "The GPU lane holds only the passes this capture timed, while the axis reaches wider to cover the CPU "
        + "calls. An empty stretch at either end of the lane is time outside the timed region — where the GPU may "
        + "have been running the previous frame — not a measured idle GPU; only the gaps between passes are that.",
      class: "text-muted font-sm",
    });
  }
}

function renderPassTimings(root: Widget, t: FrameTimingInfo): void {
  const card = new Div(root, { class: "frame-stats-section" });
  new Div(card, { text: `Pass Timings (${t.passes.length} passes, ${t.gpuTotalMs.toFixed(3)} ms GPU, ${t.gpuSpanMs.toFixed(3)} ms span)`, class: "frame-stats-heading" });
  const list = new Div(card, { class: "frame-stats-list" });
  const shown = t.passes.slice(0, 40);
  for (const p of shown) {
    const row = new Div(list, { class: "frame-stats-row frame-stats-pass" });
    new Div(row, { text: p.label, class: "frame-stats-label" });
    const pct = t.gpuTotalMs > 0 ? (p.durationMs / t.gpuTotalMs) * 100 : 0;
    new Div(row, { text: `${p.durationMs.toFixed(3)} ms  (${pct.toFixed(1)}%)`, class: "frame-stats-value" });
    row.element.onclick = p.onJump;
    row.element.title = "Jump to the pass in the command list";
  }
  if (t.passes.length > shown.length) new Div(list, { text: `... ${t.passes.length - shown.length} more`, class: "text-muted font-sm" });
}

/** The frame analysis findings (vulkan/frame_analysis.ts) and how to jump to a command. */
export interface FrameIssues {
  findings: FrameFinding[];
  onJump: (commandIndex: number) => void;
}

const SEVERITY_LABEL: Record<string, string> = { high: "High", medium: "Med", low: "Low", info: "Info" };

function renderFrameIssues(root: Widget, issues: FrameIssues): void {
  const card = new Div(root, { class: "frame-stats-section" });
  const n = issues.findings.length;
  new Div(card, { text: `Frame Issues (${n})`, class: "frame-stats-heading" });
  const body = new Div(card, { class: "frame-stats-list" });
  if (!n) {
    new Div(body, { text: "No issues found by the frame rules (attachment loads and stores, clears, stereo passes, draw batching).", class: "perf-empty text-muted" });
    return;
  }
  // Filters: by severity (the shader findings' classes) and by rule, for a frame where one
  // rule fires everywhere and hides the others.
  const severities = [...new Set(issues.findings.map((f) => f.severity))];
  const rules = [...new Set(issues.findings.map((f) => f.rule))];
  const list = new Div(null, { class: "perf-findings" });
  if (severities.length > 1 || rules.length > 1) {
    const filterRow = new Div(body, { class: "perf-filter-row" });
    new Span(filterRow, { text: "Show:", class: "text-muted font-sm" });
    for (const sev of ["high", "medium", "low", "info"]) {
      if (!severities.includes(sev as typeof severities[number])) continue;
      const cb = new Checkbox(filterRow, { label: SEVERITY_LABEL[sev], checked: true, class: "inspector-filter-field" });
      cb.input.onchange = () => list.classList.toggle(`perf-hide-${sev}`, !cb.checked);
    }
    if (rules.length > 1) {
      const ruleRow = new Div(body, { class: "perf-filter-row perf-rule-row" });
      new Span(ruleRow, { text: "Rules:", class: "text-muted font-sm" });
      for (const rule of rules) {
        const cb = new Checkbox(ruleRow, { label: rule, checked: true, class: "inspector-filter-field perf-rule-toggle" });
        cb.input.onchange = () => { for (const row of list.element.querySelectorAll(`.perf-rule-${rule}`)) (row as HTMLElement).hidden = !cb.checked; };
      }
    }
  }
  body.appendChild(list);
  for (const f of issues.findings) {
    const row = new Div(list, { class: `perf-finding perf-row-${f.severity} perf-rule-${f.rule}${f.confidence !== "high" ? " perf-lowconf" : ""}` });
    const head = new Div(row, { class: "perf-finding-head" });
    new Span(head, { text: f.severity.toUpperCase(), class: `perf-badge perf-${f.severity}` });
    new Span(head, { text: f.rule, class: "perf-rule" });
    if (f.commandIndex !== undefined) {
      const link = new Span(head, { text: `command ${f.commandIndex}`, class: "perf-line-link dependency_link" });
      const index = f.commandIndex;
      link.element.onclick = () => issues.onJump(index);
      link.element.title = "Select the command";
    }
    if (f.count > 1) new Span(head, { text: `×${f.count}`, class: "perf-count text-muted" });
    new Div(row, { text: f.message, class: "perf-msg" });
    if (f.confidence !== "high") new Div(row, { text: `${f.confidence} confidence`, class: "perf-finding-meta text-muted font-sm" });
  }
}

/** Renders the statistics as WebGPU Inspector's Frame Stats view: one card per section. */
export function renderFrameStats(container: Widget, stats: CaptureStatistics, timing: FrameTimingInfo | null = null, issues: FrameIssues | null = null,
                                 cpuTimeline: CpuTimelineMessage | null = null, gpuTrack: GpuTrackInput | null = null): void {
  const root = new Div(container, { class: "frame-stats" });
  new Div(root, { text: "Frame Statistics", class: "frame-stats-title" });
  if (stats.frames > 1) new Div(root, { text: `Totals over ${stats.frames} captured frames.`, class: "text-muted font-sm" });
  if (timing) renderFrameBound(root, timing);
  renderCpuTimeline(root, cpuTimeline);
  renderTimelineTracks(root, { timeline: cpuTimeline, passes: gpuTrack?.passes ?? [], originTicks: gpuTrack?.originTicks ?? null });
  if (issues) renderFrameIssues(root, issues);
  if (timing) renderPassTimings(root, timing);
  for (const section of stats.sections()) {
    const card = new Div(root, { class: "frame-stats-section" });
    new Div(card, { text: section.title, class: "frame-stats-heading" });
    const list = new Div(card, { class: "frame-stats-list" });
    for (const row of section.rows) {
      if (!row.value && row.label !== "Frames") continue;
      const line = new Div(list, { class: "frame-stats-row" });
      new Div(line, { text: row.label, class: "frame-stats-label" });
      new Div(line, { text: row.bytes ? `${formatBytes(row.value)} (${row.value.toLocaleString()})` : row.value.toLocaleString(), class: "frame-stats-value" });
    }
  }
}
