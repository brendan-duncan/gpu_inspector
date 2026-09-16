// The Frame Stats report: the capture's statistics (capture_statistics.ts) as WebGPU Inspector
// shows them, one card per section, with the Frame Bound card, the Frame Issues list and the pass
// timings above them.
import { Checkbox } from "./widget/checkbox.js";
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import { Widget } from "./widget/widget.js";
import { REFRESH_SOURCE_NOTE, frameBound, type CaptureStatistics } from "./capture_statistics.js";
import { cpuVerdict, summarizeCpuTimeline } from "./cpu_timeline.js";
import { MIN_SPAN_MS, buildTimelineTracks, gpuGaps, tracksVerdict, type LabelledPass, type TimelineInput } from "./timeline_tracks.js";
import type { CpuTimelineMessage } from "../shared/protocol.js";
import type { FrameFinding } from "./vulkan/frame_analysis.js";
import { formatBytes } from "./vulkan/vulkan_object.js";

/** GPU pass timings of a capture (Profile passes) with the live frame and submit times for the Frame Bound card. */
export interface FrameTimingInfo {
  frameMs: number;       // live frame interval
  refreshMs: number;     // display refresh interval while vsync was on (the budget), 0 without
  refreshSource?: string; // "present_timing" | "display_timing" | "monitor" | "estimate"
  submitMs: number;      // CPU time per frame inside vkQueueSubmit
  gpuSpanMs: number;     // first pass start to last pass end
  gpuTotalMs: number;    // sum of pass durations
  frames: number;
  passes: { label: string; durationMs: number; startMs: number; onJump: () => void }[];
}

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
  new Div(body, {
    text: bound.vsync
      ? `The budget is the display refresh period (${(1000 / t.refreshMs).toFixed(0)} Hz, ${REFRESH_SOURCE_NOTE[t.refreshSource ?? ""] ?? "estimated from the frame intervals while vsync is on"}). GPU time is the span of this capture's timed passes; CPU is the time inside vkQueueSubmit, so work outside submission counts as headroom here.`
      : "The budget is the live frame interval (vsync is off, so no display refresh period applies). GPU time is the span of this capture's timed passes; CPU is the time inside vkQueueSubmit, so work outside submission counts as headroom here.",
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

/** The colour of a span, which is the colour "Where the CPU went" already uses for that kind. */
const SPAN_COLOR: Record<string, string> = {
  gpuWait: "#4a8db8", displayWait: "#a0a0a0", work: "#5fd08a", gpu: "#8a6fd0",
};

/**
 * "Timeline": every thread's timed calls and every timed pass drawn against one axis
 * (renderer/timeline_tracks.ts). The two cards above total time per category and per pass, which
 * answers how much but never when — and a GPU left idle waiting for a late submission is a gap
 * between spans, so it has no size in any total and only a drawing shows it.
 */
function renderTimelineTracks(root: Widget, input: TimelineInput): void {
  const t = buildTimelineTracks(input);
  if (!t) return;
  const card = new Div(root, { class: "frame-stats-section" });
  new Div(card, { text: "Timeline", class: "frame-stats-heading" });
  const body = new Div(card, { class: "frame-stats-list" });
  new Div(body, { text: tracksVerdict(t), class: "frame-bound-verdict" });

  const pct = (ms: number): string => `${((ms / t.spanMs) * 100).toFixed(4)}%`;
  const gaps = gpuGaps(t);
  for (const track of t.tracks) {
    const row = new Div(body, { class: "track-row" });
    new Div(row, { text: track.label, class: "track-label" });
    const lane = new Div(row, { class: `track-lane${track.kind === "gpu" ? " track-lane-gpu" : ""}` });
    // The idle stretches go in first, so a span drawn over one still reads on top.
    if (track.kind === "gpu") {
      for (const g of gaps) {
        const box = new Div(lane, { class: "track-gap" });
        box.style.left = pct(g.startMs);
        box.style.width = pct(g.durationMs);
        box.element.title = `${g.durationMs.toFixed(2)} ms with no pass running`;
      }
    }
    for (const s of track.spans) {
      const box = new Div(lane, { class: "track-span" });
      box.style.left = pct(s.startMs);
      // A call lasting microseconds would otherwise be sub-pixel and vanish.
      box.style.width = `max(1px, ${pct(Math.max(s.durationMs, MIN_SPAN_MS))})`;
      box.style.background = SPAN_COLOR[s.kind] ?? "#5fd08a";
      box.element.title = `${s.label}: ${s.durationMs.toFixed(3)} ms at ${s.startMs.toFixed(3)} ms`;
    }
    new Div(row, { text: `${((track.busyMs / t.spanMs) * 100).toFixed(0)}%`, class: "track-busy" });
  }
  const axis = new Div(body, { class: "track-axis" });
  new Div(axis, { text: "0 ms" });
  new Div(axis, { text: `${t.spanMs.toFixed(2)} ms` });

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
