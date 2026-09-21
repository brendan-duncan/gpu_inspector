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
import { Button } from "./widget/button.js";
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import { Widget } from "./widget/widget.js";
import { LIMITER_ADVICE, LIMITER_LABEL, counterLabel, formatCounter, hwCountersByPass } from "./hw_counters.js";
import {
  BOUND_ADVICE, BOUND_LABEL, HEALTHY_OVERDRAW, MICROTRIANGLE_LIMIT,
  collectPassMetrics, formatPercent, formatRatio, frameStageVerdict, passAdvice, type FrameMetrics,
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
 * The hardware counters section: the GPU's own counters per render pass, which say which unit
 * inside the shader core the pass saturates rather than inferring it from overdraw and triangle
 * size. A Vulkan capture is replayed to read them (renderer/hw_counters.ts); until it has been,
 * this offers to do it.
 */
function renderHwCounters(root: Widget, data: CaptureData, m: FrameMetrics, onJump: (commandIndex: number) => void,
                          measure?: () => Promise<boolean>): void {
  const section = new Div(root, { class: "frame-stats-section" });
  new Div(section, { text: "Hardware counters", class: "frame-stats-heading" });
  const file = data.hwCounters;

  if (!file) {
    if (data.api !== "vulkan") {
      new Div(section, {
        text: data.api === "metal"
          ? "Apple's own instrumentation has these and no public Metal API exposes them; \"Xcode Trace\" in the capture bar writes a .gputrace that does."
          : "The GPU's own counters are read by replaying the capture, which only Vulkan captures can be.",
        class: "text-muted",
      });
      return;
    }
    new Div(section, {
      text: "Which unit inside the shader core each pass saturates — shader throughput, memory bandwidth, cache, occupancy — read from the GPU's own counters by replaying the capture. The frame is replayed once per collection pass the counters need, so this takes a while.",
      class: "text-muted",
    });
    if (measure) {
      const button = new Button(section, {
        label: "Measure hardware counters", class: "btn btn-sm",
        tooltip: "Replay the capture on this machine's GPU reading its hardware counters around each render pass. Needs NVIDIA's Nsight Perf SDK or VK_KHR_performance_query, and GPU performance-counter access enabled.",
        callback: () => {
          button.disabled = true;
          button.text = "Replaying...";
          void measure().finally(() => {
            button.disabled = false;
            button.text = "Measure hardware counters";
          });
        },
      });
    }
    return;
  }

  if (!file.counters.length || (!file.passes.length && !file.draws.length)) {
    new Div(section, { text: file.notes[0] ?? "No hardware counters were collected.", class: "text-muted" });
    return;
  }

  const where = file.backend === "nvperf" ? `NVIDIA ${file.chip}` : "VK_KHR_performance_query";
  new Div(section, {
    text: `${file.counters.length} counters from ${where}, collected over ${file.rounds} replay${file.rounds === 1 ? "" : "s"} of the frame.`,
    class: "text-muted",
  });

  const byPass = hwCountersByPass(file);
  // Render passes only: a compute pass shares its neighbor's key, and no counter range wraps one.
  const rows = m.passes.map((p, i) => ({ p, i, r: p.compute ? undefined : byPass.get(`${p.frame}:${p.commandBuffer}:${p.passIndex}`) }))
    .filter((x) => x.r).sort((a, b) => (b.p.durationMs ?? 0) - (a.p.durationMs ?? 0));
  if (!rows.length) {
    new Div(section, { text: "No replayed render pass matched a pass of this capture.", class: "text-muted" });
    return;
  }

  // One column per counter, named by its hardware unit so the header stays readable; the full
  // counter name and what it measures are in each header's tooltip.
  const grid = new Div(section, { class: "bottleneck-table hw-counter-table" });
  grid.element.style.gridTemplateColumns = `minmax(140px, 2fr) minmax(64px, 0.8fr) repeat(${file.counters.length}, minmax(76px, 1fr))`;
  const header = new Div(grid, { class: "bottleneck-row bottleneck-header" });
  cell(header, "Pass");
  cell(header, "GPU ms");
  for (const c of file.counters) cell(header, counterLabel(c.name), `${c.name}${c.description ? ` — ${c.description}` : ""}`);
  for (const { p, r } of rows) {
    const row = new Div(grid, { class: "bottleneck-row" });
    const name = new Div(row, { text: p.label, class: "bottleneck-cell bottleneck-pass-name" });
    name.tooltip = "Select this pass in the command list";
    name.element.onclick = () => onJump(p.commandIndex);
    cell(row, p.durationMs === null ? "—" : p.durationMs.toFixed(3));
    for (let i = 0; i < file.counters.length; ++i) {
      cell(row, formatCounter(r!.values[i] ?? null, file.counters[i].unit), file.counters[i].name);
    }
  }
  if (file.draws.length) {
    new Div(section, { text: `${file.draws.length} draws were measured as well; get_hw_counters in the MCP server lists them.`, class: "text-muted" });
  }
  for (const note of file.notes) new Div(section, { text: note, class: "text-muted" });
}

/**
 * Renders the report. `onJump` selects a command in the list, so every pass and every piece of
 * advice can be followed back to what raised it. `measureHwCounters` replays the capture for the
 * GPU's own counters, when the view can run one.
 */
export function renderBottleneckReport(container: Widget, data: CaptureData, db: ObjectLookup, onJump: (commandIndex: number) => void,
                                       measureHwCounters?: () => Promise<boolean>): void {
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
  if (slowest && (slowest.pass.bound || slowest.pass.limiter)) {
    const card = new Div(actions, { class: "bottleneck-advice bottleneck-advice-info" });
    const head = new Div(card, { class: "bottleneck-advice-head" });
    new Span(head, { text: `Slowest pass: ${slowest.pass.label}`, class: "bottleneck-advice-title" });
    new Span(head, { text: `${(slowest.pass.durationMs ?? 0).toFixed(3)} ms`, class: "text-muted" });
    const link = new Span(head, { text: "go to pass", class: "perf-line-link dependency_link" });
    link.element.onclick = () => onJump(slowest.pass.commandIndex);
    // Measured first: the counters name the unit, where the stage verdict only infers one.
    const l = slowest.pass.limiter;
    if (l && l.kind !== "unsaturated") {
      new Div(card, {
        text: `${LIMITER_LABEL[l.kind]}: ${l.label} ${l.saturated ? "is at" : "is the busiest unit, at"} ${l.percent.toFixed(0)}% of peak, measured.`,
        class: "bottleneck-advice-body",
      });
      new Div(card, { text: LIMITER_ADVICE[l.kind], class: "bottleneck-advice-body text-muted" });
    } else if (slowest.pass.bound) {
      new Div(card, { text: `${BOUND_LABEL[slowest.pass.bound]}: ${slowest.pass.boundReason}.`, class: "bottleneck-advice-body" });
      new Div(card, { text: BOUND_ADVICE[slowest.pass.bound], class: "bottleneck-advice-body text-muted" });
    }
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
    // The measured verdict where the GPU's counters gave one, since it names the unit rather than
    // inferring a stage; the inferred one otherwise.
    if (p.limiter && p.limiter.kind !== "unsaturated") {
      new Span(verdict, {
        text: LIMITER_LABEL[p.limiter.kind], class: `bottleneck-tag bottleneck-tag-limiter`,
        tooltip: `${p.limiter.label} at ${p.limiter.percent.toFixed(0)}% of peak (${p.limiter.counter}), measured from the GPU's own counters`
          + (p.bound ? `. By stage timing alone: ${BOUND_LABEL[p.bound]}, ${p.boundReason}` : ""),
      });
    } else if (p.bound) {
      new Span(verdict, { text: BOUND_LABEL[p.bound], class: `bottleneck-tag bottleneck-tag-${p.bound}`, tooltip: p.boundReason });
    } else {
      new Span(verdict, { text: "—", class: "text-muted" });
    }
  }

  // ---- The GPU's own counters, which name the saturated unit instead of inferring it.
  renderHwCounters(root, data, m, onJump, measureHwCounters);

  // ---- What this cannot measure, and where to get it.
  const limits = new Div(root, { class: "frame-stats-section" });
  new Div(limits, { text: "Going further", class: "frame-stats-heading" });
  const metal = data.api === "metal";
  const d3d12 = data.api === "d3d12";
  const counterNote = m.withCounters === 0
    ? (metal
      ? "This GPU exposes only the timestamp counter set through public Metal, so the columns above that need invocation counts are empty."
      : d3d12
        ? "No pass carried counters. The library puts a pipeline statistics query around every render pass; a pass whose command list had a query of the application's open is not counted."
        : "No pass carried counters. The device may not support pipelineStatisticsQuery, or the application enabled a feature set that excludes it; the layer's log says which.")
    : `${m.withCounters} of ${m.timed} timed passes carried counters.`;
  new Div(limits, { text: counterNote, class: "text-muted" });
  if (d3d12) {
    new Div(limits, {
      text: "The vertex and fragment spans are Metal only: they come from timestamps at a pass's stage boundaries. Depth rejection needs the samples that survived the depth and stencil tests, which the D3D12 library counts with an occlusion query around each pass. Per-draw timings and shader costs come from replaying the capture, which only Vulkan captures can be.",
      class: "text-muted",
    });
  } else if (!metal) {
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
