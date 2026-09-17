// A timing capture's report: the frame-time graph, the hitches in it and what caused them
// (renderer/frame_timing.ts).
//
// The graph is the point. A list of percentiles says a run was uneven; only the shape says whether
// it was one stall at load, a periodic spike every few seconds, or a slow drift — and those have
// nothing to do with each other.
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import type { Widget } from "./widget/widget.js";
import { hitchThresholdMs, summarizeTiming, type TimingCapture } from "./frame_timing.js";

/** Drawn at this many device pixels per frame at most, so a short run does not become a smear. */
const MAX_BAR_WIDTH = 4;

/**
 * Draws the per-frame times as bars, hitches in their own colour, with the median and the hitch
 * threshold as lines across it. More frames than pixels are reduced by taking the *worst* frame in
 * each column rather than the average: averaging is what hid the hitch in the frame report to begin
 * with, and doing it again here would hide it in the picture too.
 */
function drawGraph(canvas: HTMLCanvasElement, capture: TimingCapture, medianMs: number, threshold: number): void {
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
}

/**
 * Renders the report into `container`, replacing what was there. `onJump` is given a frame number
 * when a hitch is clicked, for a caller that can do something with it.
 */
export function renderTimingReport(container: Widget, capture: TimingCapture,
                                   onJump?: (frame: number) => void): void {
  container.html = "";
  const s = summarizeTiming(capture);
  if (!s) {
    new Div(container, { text: "No frames recorded yet.", class: "text-muted" });
    return;
  }
  new Div(container, { text: s.verdict, class: "frame-bound-verdict" });

  const canvas = document.createElement("canvas");
  canvas.className = "timing-graph";
  canvas.width = 900;
  canvas.height = 90;
  canvas.style.width = "100%";
  canvas.style.height = "90px";
  container.element.appendChild(canvas);
  drawGraph(canvas, capture, s.medianMs, hitchThresholdMs(s.medianMs));

  const row = (label: string, value: string): void => {
    const r = new Div(container, { class: "draw-state-row" });
    new Span(r, { text: label, class: "draw-state-label device-info-label" });
    new Span(r, { text: value, class: "device-info-value" });
  };
  const ms = (v: number): string => `${v.toFixed(2)} ms`;
  row("Frames", `${s.frames} over ${(s.totalMs / 1000).toFixed(1)} s`);
  // The median says what an ordinary frame costs; the percentiles say what the worst ones do, which
  // is what a player actually notices.
  row("Median", ms(s.medianMs));
  row("95th / 99th", `${ms(s.p95Ms)} / ${ms(s.p99Ms)}`);
  row("Worst", ms(s.maxMs));

  for (const c of s.categories) {
    row(c.label, `${ms(c.ms)} over the run, ${(100 * c.share).toFixed(1)}% of its wall time`);
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
