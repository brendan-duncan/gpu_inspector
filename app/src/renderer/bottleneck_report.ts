// The "GPU Bottlenecks" report: what limits each pass, and what to do about it.
//
// Frame Stats counts what the frame did; the flame graph says where the time went. Neither says
// why a pass is slow. This one answers that in the terms a profiling session uses — is the pass
// waiting on vertex work or fragment work, how many times is each pixel being shaded, are the
// triangles big enough to be worth rasterizing, is the depth test earning its keep — and for each
// answer says what usually causes it.
//
// It is deliberately honest about its limits. Every figure comes from a GPU counter or from the
// command stream, and a pass whose counters the GPU does not expose shows what is known and says
// so, with a pointer at the Xcode trace for the rest. docs/PROFILING.md is the how-to that walks
// through using it.
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import { Widget } from "./widget/widget.js";
import {
  BOUND_ADVICE, BOUND_LABEL, HEALTHY_OVERDRAW, MICROTRIANGLE_LIMIT,
  collectPassMetrics, formatPercent, formatRatio, frameStageVerdict, passAdvice,
} from "./pass_metrics.js";
import type { ObjectLookup } from "./vulkan/vulkan_object.js";
import type { CaptureData } from "./capture_data.js";

function bar(parent: Widget, vertexMs: number, fragmentMs: number): void {
  const total = vertexMs + fragmentMs;
  const box = new Div(parent, { class: "bottleneck-bar" });
  if (total <= 0) return;
  new Div(box, {
    class: "bottleneck-bar-vertex", style: `width: ${(100 * vertexMs / total).toFixed(1)}%;`,
    tooltip: `vertex stage ${vertexMs.toFixed(3)} ms`,
  });
  new Div(box, {
    class: "bottleneck-bar-fragment", style: `width: ${(100 * fragmentMs / total).toFixed(1)}%;`,
    tooltip: `fragment stage ${fragmentMs.toFixed(3)} ms`,
  });
}

function cell(row: Widget, text: string, tooltip?: string): Div {
  const d = new Div(row, { text, class: "bottleneck-cell" });
  if (tooltip) d.tooltip = tooltip;
  return d;
}

/**
 * Renders the report. `onJump` selects a command in the list, so every pass and every piece of
 * advice can be followed back to what raised it.
 */
export function renderBottleneckReport(container: Widget, data: CaptureData, db: ObjectLookup, onJump: (commandIndex: number) => void): void {
  const m = collectPassMetrics(data, db);
  const root = new Div(container, { class: "frame-stats bottleneck-report" });
  new Div(root, { text: "GPU Bottlenecks", class: "frame-stats-title" });

  if (!m.passes.length) {
    new Div(root, { text: "No passes in this capture.", class: "text-muted" });
    return;
  }
  if (!m.timed) {
    new Div(root, {
      text: "No pass was timed. Turn on \"Profile passes\" in the capture bar before capturing: the timings and the counters this report is built from are sampled during the capture, not afterwards.",
      class: "perf-empty text-muted",
    });
    return;
  }

  // ---- The frame in one card.
  const summary = new Div(root, { class: "frame-stats-section bottleneck-summary" });
  new Div(summary, { text: frameStageVerdict(m), class: "bottleneck-verdict" });
  const facts = new Div(summary, { class: "frame-stats-list" });
  const fact = (label: string, value: string, tooltip?: string): void => {
    const line = new Div(facts, { class: "frame-stats-row" });
    const l = new Div(line, { text: label, class: "frame-stats-label" });
    if (tooltip) l.tooltip = tooltip;
    new Div(line, { text: value, class: "frame-stats-value" });
  };
  fact("GPU time in passes", `${m.gpuMs.toFixed(3)} ms`, "The timed passes' durations, summed. Gaps between passes are not included.");
  if (m.vertexMs > 0 || m.fragmentMs > 0) {
    fact("Vertex stages", `${m.vertexMs.toFixed(3)} ms`);
    fact("Fragment stages", `${m.fragmentMs.toFixed(3)} ms`, "On a tile-based GPU the two stages of one pass overlap, so these add up to more than the pass durations.");
  }
  if (m.totals) {
    const frameOverdraw = m.passes.reduce((sum, p) => sum + (p.overdraw ?? 0), 0);
    fact("Fragment invocations", m.totals.fragmentInvocations.toLocaleString(), "Fragment shader runs over the whole frame.");
    fact("Overdraw, summed over passes", formatRatio(frameOverdraw), `Each pass's fragment invocations divided by its target's pixels, added up. A frame doing well sits near ${HEALTHY_OVERDRAW} per pass.`);
    if (m.totals.primitives > 0) {
      fact("Fragments per primitive", formatRatio(m.totals.fragmentInvocations / m.totals.primitives),
        `Averaged over the frame. Below ${MICROTRIANGLE_LIMIT} means triangles too small for the 2x2 rasterization quad.`);
    }
  }

  // ---- What to do, worst pass first.
  const ranked = m.passes
    .filter((p) => p.durationMs !== null)
    .map((p) => ({ pass: p, advice: passAdvice(p) }))
    .sort((a, b) => (b.pass.durationMs ?? 0) - (a.pass.durationMs ?? 0));
  const withAdvice = ranked.filter((r) => r.advice.length);

  const actions = new Div(root, { class: "frame-stats-section" });
  new Div(actions, { text: "What to look at", class: "frame-stats-heading" });
  if (!withAdvice.length && m.withCounters === 0) {
    new Div(actions, {
      text: "This GPU exposes only the timestamp counter set, so the measurements that name a cause (overdraw, fragments per primitive, depth rejection) are not available. The stage split below still says which stage each pass waits on.",
      class: "text-muted",
    });
  } else if (!withAdvice.length) {
    new Div(actions, { text: "Nothing measured crosses the thresholds this report checks. The passes below are ordered by GPU time.", class: "text-muted" });
  }
  // The slowest pass is worth naming whether or not a rule fired: it is where any win is largest.
  const slowest = ranked[0];
  if (slowest && slowest.pass.bound) {
    const card = new Div(actions, { class: "bottleneck-advice bottleneck-advice-info" });
    const head = new Div(card, { class: "bottleneck-advice-head" });
    new Span(head, { text: `Slowest pass: ${slowest.pass.label}`, class: "bottleneck-advice-title" });
    new Span(head, { text: `${(slowest.pass.durationMs ?? 0).toFixed(3)} ms`, class: "text-muted" });
    const link = new Span(head, { text: "go to pass", class: "perf-line-link dependency_link" });
    link.element.onclick = () => onJump(slowest.pass.commandIndex);
    new Div(card, { text: `${BOUND_LABEL[slowest.pass.bound]}: ${slowest.pass.boundReason}.`, class: "bottleneck-advice-body" });
    new Div(card, { text: BOUND_ADVICE[slowest.pass.bound], class: "bottleneck-advice-body text-muted" });
  }
  for (const { pass, advice } of withAdvice) {
    for (const a of advice) {
      const card = new Div(actions, { class: `bottleneck-advice bottleneck-advice-${a.severity}` });
      const head = new Div(card, { class: "bottleneck-advice-head" });
      new Span(head, { text: a.title, class: "bottleneck-advice-title" });
      new Span(head, { text: pass.label, class: "text-muted" });
      const link = new Span(head, { text: "go to pass", class: "perf-line-link dependency_link" });
      link.element.onclick = () => onJump(pass.commandIndex);
      new Div(card, { text: a.body, class: "bottleneck-advice-body" });
    }
  }

  // ---- Every pass, measured.
  const table = new Div(root, { class: "frame-stats-section" });
  new Div(table, { text: "Passes", class: "frame-stats-heading" });
  const grid = new Div(table, { class: "bottleneck-table" });
  const header = new Div(grid, { class: "bottleneck-row bottleneck-header" });
  cell(header, "Pass");
  cell(header, "GPU ms");
  cell(header, "Vertex / fragment", "The two stages' own spans. They overlap on a tile-based GPU.");
  cell(header, "Draws");
  cell(header, "Overdraw", `Fragment shader runs per target pixel. Healthy is about ${HEALTHY_OVERDRAW}.`);
  cell(header, "Frags/prim", `Fragments per primitive. Below ${MICROTRIANGLE_LIMIT} is a microtriangle problem.`);
  cell(header, "Depth reject", "Share of shaded fragments the depth and stencil tests threw away.");
  cell(header, "Verdict");

  for (const { pass: p } of ranked) {
    const row = new Div(grid, { class: "bottleneck-row" });
    const name = new Div(row, { text: p.label, class: "bottleneck-cell bottleneck-pass-name" });
    name.tooltip = "Select this pass in the command list";
    name.element.onclick = () => onJump(p.commandIndex);
    cell(row, p.durationMs === null ? "—" : p.durationMs.toFixed(3));
    const split = new Div(row, { class: "bottleneck-cell" });
    if (p.vertexMs !== null && p.fragmentMs !== null) bar(split, p.vertexMs, p.fragmentMs);
    else new Span(split, { text: "—", class: "text-muted" });
    cell(row, String(p.draws));
    cell(row, formatRatio(p.overdraw), p.pixels ? `${p.pixels.toLocaleString()} pixels in the target` : "The target's size could not be resolved");
    cell(row, formatRatio(p.fragmentsPerPrimitive, 1));
    cell(row, formatPercent(p.depthRejectRate));
    const verdict = new Div(row, { class: "bottleneck-cell" });
    if (p.bound) new Span(verdict, { text: BOUND_LABEL[p.bound], class: `bottleneck-tag bottleneck-tag-${p.bound}`, tooltip: p.boundReason });
    else new Span(verdict, { text: "—", class: "text-muted" });
  }

  // ---- What this cannot measure, and where to get it.
  const limits = new Div(root, { class: "frame-stats-section" });
  new Div(limits, { text: "Going further", class: "frame-stats-heading" });
  const metal = data.api === "metal";
  const counterNote = m.withCounters === 0
    ? (metal
      ? "This GPU exposes only the timestamp counter set through public Metal, so the columns above that need invocation counts are empty."
      : "No pass carried counters. The device may not support pipelineStatisticsQuery, or the application enabled a feature set that excludes it; the layer's log says which.")
    : `${m.withCounters} of ${m.timed} timed passes carried counters.`;
  new Div(limits, { text: counterNote, class: "text-muted" });
  if (!metal) {
    new Div(limits, {
      text: "The vertex and fragment spans are Metal only: they come from timestamps at a pass's stage boundaries, which Vulkan has no portable equivalent for. Depth rejection needs the samples that survived the depth and stencil tests, which the Vulkan layer counts with an occlusion query around each pass (skipped for a pass where the application has a query of its own open).",
      class: "text-muted",
    });
  } else {
    new Div(limits, {
      text: "Shader occupancy, the ALU and texture limiters, and per-line shader cost come from Apple's own instrumentation and have no public Metal API. \"Xcode Trace\" in the capture bar writes the next frame as a .gputrace document, which opens in Xcode's Metal debugger with all of them.",
      class: "text-muted",
    });
  }
  new Div(limits, {
    text: "docs/PROFILING.md walks through finding a bottleneck with these numbers, and what each one means when it is high.",
    class: "text-muted",
  });
}
