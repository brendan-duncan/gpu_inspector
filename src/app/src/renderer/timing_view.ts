// A timing capture's report: the frame-time graph, the hitches in it and what caused them
// (renderer/frame_timing.ts).
//
// The graph is the point. A list of percentiles says a run was uneven; only the shape says whether
// it was one stall at load, a periodic spike every few seconds, or a slow drift — and those have
// nothing to do with each other.
//
// The graph is also how a stretch of the run gets asked about on its own. Figures over a twenty
// minute recording describe the whole of it, which is the wrong question once the shape shows where
// the interesting part is: a median taken across a loading screen and then across play is neither.
// So a range dragged out on the graph is what the figures below it are of, while the graph keeps
// drawing the whole run — it is the map, and a map that redrew itself to the selection would leave
// nothing to select against.
import { Button } from "./widget/button.js";
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import type { Widget } from "./widget/widget.js";
import { hitchThresholdMs, rangeIndices, summarizeTiming, type FrameRange, type TimingCapture } from "./frame_timing.js";

/** Drawn at this many device pixels per frame at most, so a short run does not become a smear. */
const MAX_BAR_WIDTH = 4;

/** A drag shorter than this is a click: it clears the selection rather than selecting a sliver. */
const DRAG_SLOP_PX = 3;

/** What the report is showing, and how to change it. */
export interface TimingViewOptions {
  /** Given a frame number when a hitch is clicked, for a caller that can do something with it. */
  onJump?: (frame: number) => void;
  /** The stretch the figures are of. Null, or a range that has aged out, means the whole run. */
  range?: FrameRange | null;
  /** A range dragged out on the graph, or null when the drag was a click: the caller stores it. */
  onRange?: (range: FrameRange | null) => void;
}

/**
 * Draws the per-frame times as bars, hitches in their own colour, with the median and the hitch
 * threshold as lines across it. More frames than pixels are reduced by taking the *worst* frame in
 * each column rather than the average: averaging is what hid the hitch in the frame report to begin
 * with, and doing it again here would hide it in the picture too.
 *
 * `selected` is the stretch the figures are of, as indices into the frames, drawn by veiling the
 * rest: the whole run stays visible and legible, and what is being reported on is what is bright.
 */
function drawGraph(canvas: HTMLCanvasElement, capture: TimingCapture, medianMs: number, threshold: number,
                   selected: { from: number; to: number } | null): void {
  const frames = capture.frames;
  const width = Math.max(1, Math.min(frames.length * MAX_BAR_WIDTH, canvas.width));
  const ctx = canvas.getContext("2d");
  if (!ctx || !frames.length) return;
  const h = canvas.height;
  ctx.clearRect(0, 0, canvas.width, h);

  const columns = Math.min(width, frames.length);
  const worst: number[] = new Array(columns).fill(0);
  frames.forEach((f, i) => {
    const c = Math.min(columns - 1, Math.floor((i / frames.length) * columns));
    if (f.durationMs > worst[c]) worst[c] = f.durationMs;
  });
  // Scaled to the worst frame, with a floor so a steady run does not fill the height with noise.
  const top = Math.max(threshold * 1.2, ...worst);
  const y = (ms: number): number => h - Math.max(1, Math.round((ms / top) * (h - 2)));
  // Positioned as a fraction of the canvas rather than at a whole number of pixels each: rounding
  // the width down leaves a run of a few hundred frames drawn across half the graph.
  const columnWidth = canvas.width / columns;

  for (let c = 0; c < columns; c++) {
    ctx.fillStyle = worst[c] > threshold ? "#d05f5f" : "#5f8ad0";
    const bar = y(worst[c]);
    const x = Math.floor(c * columnWidth);
    ctx.fillRect(x, bar, Math.max(1, Math.floor((c + 1) * columnWidth) - x), h - bar);
  }
  const line = (ms: number, color: string): void => {
    ctx.strokeStyle = color;
    ctx.beginPath();
    ctx.moveTo(0, y(ms) - 0.5);
    ctx.lineTo(canvas.width, y(ms) - 0.5);
    ctx.stroke();
  };
  line(medianMs, "#808080");
  line(threshold, "#c08040");

  if (!selected) return;
  const x0 = Math.round((selected.from / frames.length) * canvas.width);
  const x1 = Math.round((selected.to / frames.length) * canvas.width);
  ctx.fillStyle = "rgba(0, 0, 0, 0.45)";
  ctx.fillRect(0, 0, x0, h);
  ctx.fillRect(x1, 0, canvas.width - x1, h);
  ctx.strokeStyle = "#d0d0d0";
  ctx.beginPath();
  ctx.moveTo(x0 + 0.5, 0);
  ctx.lineTo(x0 + 0.5, h);
  ctx.moveTo(x1 - 0.5, 0);
  ctx.lineTo(x1 - 0.5, h);
  ctx.stroke();
}

/**
 * Renders the report into `container`, replacing what was there: the graph of the whole run, and
 * the figures for `options.range` of it.
 */
export function renderTimingReport(container: Widget, capture: TimingCapture,
                                   options: TimingViewOptions = {}): void {
  const { onJump, onRange } = options;
  container.html = "";
  // The whole run, which the graph is drawn against: it keeps the run's own median and threshold,
  // so the picture does not change as a range is dragged across it and no frame outside the range
  // is coloured by a threshold taken from inside it. The graph is the map.
  const all = summarizeTiming(capture);
  if (!all) {
    new Div(container, { text: "No frames recorded yet.", class: "text-muted" });
    return;
  }
  // The stretch the figures are of, resolved against the frames the capture still holds. A range
  // whose frames have all aged out of the ring reports on the whole run instead of on nothing.
  const selected = options.range ? rangeIndices(capture, options.range) : null;
  const s = (selected && summarizeTiming(capture, selected.from, selected.to)) || all;
  new Div(container, {
    text: selected
      ? `Frames ${capture.frames[selected.from].frame}–${capture.frames[selected.to - 1].frame} of the run. ${s.verdict}`
      : s.verdict,
    class: "frame-bound-verdict",
  });

  const canvas = document.createElement("canvas");
  canvas.className = "timing-graph";
  canvas.width = 900;
  canvas.height = 90;
  canvas.style.width = "100%";
  canvas.style.height = "90px";
  canvas.title = "Drag across the graph to report on that stretch of the run; click it to go back to the whole run.";
  container.element.appendChild(canvas);
  const draw = (range: { from: number; to: number } | null): void =>
    drawGraph(canvas, capture, all.medianMs, hitchThresholdMs(all.medianMs), range);
  draw(selected);

  // Dragging a range out. The graph is drawn against the whole run, so a position on it is a
  // fraction of the frames — which is also what makes the drag work while frames keep arriving.
  const frameAt = (clientX: number): number => {
    const rect = canvas.getBoundingClientRect();
    const fraction = Math.min(1, Math.max(0, (clientX - rect.left) / Math.max(1, rect.width)));
    return Math.min(capture.frames.length - 1, Math.floor(fraction * capture.frames.length));
  };
  if (onRange) {
    canvas.onmousedown = (e: MouseEvent) => {
      if (e.button !== 0) return;
      e.preventDefault();
      const startX = e.clientX;
      const startIndex = frameAt(startX);
      let moved = false;
      const move = (m: MouseEvent): void => {
        if (Math.abs(m.clientX - startX) > DRAG_SLOP_PX) moved = true;
        if (!moved) return;
        const at = frameAt(m.clientX);
        // Redrawn rather than re-summarized on every move: the figures are for the range that was
        // asked for, and asking changes with the mouse button, not with the mouse.
        draw({ from: Math.min(startIndex, at), to: Math.max(startIndex, at) + 1 });
      };
      const up = (m: MouseEvent): void => {
        window.removeEventListener("mousemove", move);
        window.removeEventListener("mouseup", up);
        if (!moved) {
          onRange(null);                       // a click on the graph: the whole run again
          return;
        }
        const at = frameAt(m.clientX);
        const from = capture.frames[Math.min(startIndex, at)];
        const to = capture.frames[Math.max(startIndex, at)];
        onRange({ fromFrame: from.frame, toFrame: to.frame });
      };
      window.addEventListener("mousemove", move);
      window.addEventListener("mouseup", up);
    };
  }

  // The way back, and how much of the run is being left out — which is what says whether the
  // figures are of a stretch worth trusting or of a dozen frames.
  const bar = new Div(container, { class: "timing-range-row" });
  if (selected && onRange) {
    new Button(bar, { label: "Whole run", class: "track-btn", title: "Report on every recorded frame again",
      callback: () => onRange(null) });
    new Span(bar, { text: `${s.frames} of the ${capture.frames.length} recorded frames selected.`,
      class: "text-muted font-sm" });
  } else if (onRange) {
    new Span(bar, { text: "Drag across the graph to report on a stretch of the run.", class: "text-muted font-sm" });
  }

  const row = (label: string, value: string): void => {
    const r = new Div(container, { class: "draw-state-row" });
    new Span(r, { text: label, class: "draw-state-label device-info-label" });
    new Span(r, { text: value, class: "device-info-value" });
  };
  const ms = (v: number): string => `${v.toFixed(2)} ms`;
  const over = selected ? "the selected frames" : "the run";
  row("Frames", `${s.frames} over ${(s.totalMs / 1000).toFixed(1)} s`);
  // The median says what an ordinary frame costs; the percentiles say what the worst ones do, which
  // is what a player actually notices.
  row("Median", ms(s.medianMs));
  row("95th / 99th", `${ms(s.p95Ms)} / ${ms(s.p99Ms)}`);
  row("Worst", ms(s.maxMs));

  for (const c of s.categories) {
    row(c.label, `${ms(c.ms)} over ${over}, ${(100 * c.share).toFixed(1)}% of its wall time`);
  }

  if (!s.hitches.length) return;
  new Div(container, { text: `Hitches (${s.hitches.length})`, class: "frame-stats-heading" });
  for (const h of s.hitches.slice(0, 20)) {
    const r = new Div(container, { class: "draw-state-row timing-hitch" });
    new Span(r, { text: `Frame ${h.frame}`, class: "draw-state-label device-info-label" });
    const cause = h.cause
      ? `${h.cause.label.toLowerCase()} took ${ms(h.cause.ms)} of it`
      : "no timed call accounts for it: the application's own work between them";
    new Span(r, { text: `${ms(h.durationMs)}, ${h.times.toFixed(1)}x an ordinary frame — ${cause}`, class: "device-info-value" });
    if (onJump) r.element.onclick = () => onJump(h.frame);
  }
  if (s.hitches.length > 20) {
    new Div(container, { text: `${s.hitches.length - 20} more, worst first.`, class: "text-muted font-sm" });
  }
}

/** The start/stop button's label for the current state. */
export function timingButtonLabel(running: boolean): string {
  return running ? "Stop Timing" : "Timing Capture";
}
