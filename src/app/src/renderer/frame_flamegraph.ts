// The Shader Flame Graph of a capture (the button next to "Analyze Shaders"): the frame's GPU
// work by pass, pipeline (or draw), shader stage and function, colored by the kind of work that
// dominates. See frame_cost_tree.ts for what the numbers mean.
import { Button } from "./widget/button.js";
import { Checkbox } from "./widget/checkbox.js";
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import { Widget } from "./widget/widget.js";
import { FlameGraph } from "./widget/flamegraph.js";
import { DIMENSION_COLORS, KIND_COLORS, buildFrameCostTree, formatCostValue, type CostTreeOptions, type CostUnits, type FlameNode } from "./frame_cost_tree.js";
import { partShare, type ShaderAblation, type ShaderMeasureTarget } from "./shader_ablation.js";

const LEGEND: [string, string][] = [["alu", "ALU"], ["sfu", "SFU"], ["texture", "Texture"], ["memory", "Memory"]];

export interface FlameGraphPanelOptions extends Omit<CostTreeOptions, "perDraw" | "estimateFragments"> {
  onSelectCommand?: (index: number) => void;
  onInspect?: (objectId: number) => void;
  /**
   * Vulkan: replays the capture to time and count every draw (draw_stats.ts), which weighs the
   * draws of a pass by what they measured instead of by the model. Absent where it cannot run.
   */
  measureDraws?: () => Promise<boolean>;
  /**
   * Vulkan: times a stage at one draw with variants that leave out each of its functions, lines and
   * textures (shader_ablation.ts), which sizes the stage's frames by what they measured.
   */
  measureShader?: (target: ShaderMeasureTarget) => Promise<boolean>;
}

function colorOf(n: FlameNode): string {
  if (n.dimension) return DIMENSION_COLORS[n.dimension];
  return KIND_COLORS[n.kind] ?? "#4a8db8";
}

const ms = (v: number | null): string => (v === null ? "not timed" : `${v.toFixed(4)} ms`);

/** The stage frames of a tree, with the frames under each. */
function stageFrames(root: FlameNode): { stage: FlameNode; nodes: Set<FlameNode> }[] {
  const out: { stage: FlameNode; nodes: Set<FlameNode> }[] = [];
  const under = (n: FlameNode, set: Set<FlameNode>): void => { set.add(n); for (const c of n.children) under(c, set); };
  const walk = (n: FlameNode): void => {
    if (n.kind === "stage") {
      const nodes = new Set<FlameNode>();
      under(n, nodes);
      out.push({ stage: n, nodes });
    } else for (const c of n.children) walk(c);
  };
  walk(root);
  return out;
}

export function renderFrameFlameGraph(parent: Widget, o: FlameGraphPanelOptions): void {
  const root = new Div(parent, { class: "flame-panel" });
  new Div(root, { text: "Shader Flame Graph", class: "frame-stats-title" });
  let perDraw = false;
  let estimateFragments = true;
  let units: CostUnits = "ops";
  let tree: FlameNode | null = null;
  let selected: FlameNode | null = null;

  const controls = new Div(root, { class: "flame-controls" });
  new Checkbox(controls, { label: "Per draw", checked: false, tooltip: "One frame per draw or dispatch instead of one per pipeline", onChange: (v) => { perDraw = v; update(); } });
  new Checkbox(controls, { label: "Estimate fragments from the scissor area", checked: true,
    tooltip: "Weight fragment stages by the scissor (or render area) in pixels, where the pass has no measured fragment counters: an upper bound without overdraw and before the depth test. Off: those stages have no width. A pass whose counters measured its fragment invocations uses them either way.",
    onChange: (v) => { estimateFragments = v; update(); } });
  const reset = new Button(controls, { label: "Reset zoom", class: "btn btn-sm", tooltip: "Zoom back out to the whole frame", callback: () => graph.resetZoom() });
  void reset;
  // Per-draw timings and counters: measured by replaying the capture, once per capture.
  if (o.measureDraws && !o.data.drawStats) {
    const measure = new Button(controls, { label: "Measure draws", class: "btn btn-sm",
      tooltip: "Replay the capture on this machine's GPU with a timestamp pair and a pipeline statistics query around every draw, so each draw's share of its pass and its fragment count are measured rather than modeled",
      callback: () => {
        measure.disabled = true;
        measure.text = "Replaying...";
        void o.measureDraws!().then((ok) => {
          measure.disabled = false;
          measure.text = "Measure draws";
          // Measured once per capture: the button has nothing left to do.
          if (ok) measure.element.style.display = "none";
          if (ok) update();
        });
      } });
  }

  // The stage to measure: the one the selected frame is in, else the widest fragment or compute stage.
  const measureTarget = (): { node: FlameNode; target: ShaderMeasureTarget } | null => {
    if (!tree) return null;
    const stages = stageFrames(tree).filter(({ stage }) => stage.pipelineId !== undefined && stage.command && stage.stage
      && (stage.stage === "fragment" || stage.stage === "compute"));
    const withSpirv = (n: FlameNode): ShaderMeasureTarget | null => {
      const model = o.models.get(n.pipelineId!)?.find((m) => m.stage === n.stage && m.entryPoint === n.entryPoint && m.objectId === n.objectId);
      if (!model?.spirv) return null;
      return { command: n.command!.index, pipeline: n.pipelineId!, stage: n.stage!, entryPoint: n.entryPoint!, spirv: model.spirv,
               ...(model.dxil ? { dxil: model.dxil } : {}) };
    };
    const picked = selected ? stages.find((s) => s.nodes.has(selected!)) : undefined;
    const candidates = picked ? [picked] : stages.slice().sort((a, b) => b.stage.totalCost - a.stage.totalCost);
    for (const { stage } of candidates) {
      const target = withSpirv(stage);
      if (target) return { node: stage, target };
    }
    return null;
  };
  let measureShader: Button | null = null;
  let measuring = false;
  const updateMeasureButton = (): void => {
    if (!measureShader || measuring) return;
    const t = measureTarget();
    measureShader.disabled = !t;
    measureShader.text = t ? `Measure ${t.target.stage} shader at #${t.target.command}` : "Measure shader";
  };
  if (o.measureShader) {
    measureShader = new Button(controls, { label: "Measure shader", class: "btn btn-sm",
      tooltip: "Replay the capture and time the selected stage (or the widest fragment or compute stage) at its draw with variants that leave out each "
        + "function, source line and texture, so its frames are sized by what taking them out saved on this GPU. Select a frame in a stage to measure that stage.",
      callback: () => {
        const t = measureTarget();
        if (!t || !measureShader) return;
        measuring = true;
        measureShader.disabled = true;
        measureShader.text = "Replaying variants...";
        void o.measureShader!(t.target).then((ok) => {
          measuring = false;
          if (ok) update();
          else updateMeasureButton();
        });
      } });
  }

  const summary = new Div(root, { class: "flame-summary text-muted" });
  const legend = new Div(root, { class: "flame-legend" });
  new Span(legend, { text: "Cost:", class: "flame-legend-label" });
  for (const [dim, label] of LEGEND) {
    const item = new Span(legend, { class: "flame-legend-item" });
    new Span(item, { class: "flame-legend-swatch", style: `background: ${DIMENSION_COLORS[dim as keyof typeof DIMENSION_COLORS]};` });
    new Span(item, { text: label });
  }
  new Span(legend, { text: "Click a frame to zoom in; Ctrl+Wheel zooms about the pointer and dragging pans. A draw frame selects the draw, a shader frame reveals the shader.",
    class: "flame-legend-hint text-muted font-sm" });

  const graph = new FlameGraph<FlameNode>(root, {
    formatValue: (n) => formatCostValue(n.totalCost, units),
    colorOf,
    tooltipOf: (n) => {
      const lines = [n.name, formatCostValue(n.totalCost, units)];
      if (n.kind === "function" && n.selfCost > 0) lines.push(`Own cost: ${formatCostValue(n.selfCost, units)}`);
      if (n.durationMs != null) lines.push(`Pass GPU time: ${n.durationMs.toFixed(3)} ms`);
      if (n.confidence) lines.push(`Invocation count: ${n.confidence}`);
      if (n.dimension) lines.push(`Dominant cost: ${n.dimension.toUpperCase()}`);
      if (n.ablation) {
        lines.push(`Measured by ablation at #${n.ablation.command}: the stage ${n.ablation.stageMs.toFixed(4)} ms of the draw's ${n.ablation.drawMs.toFixed(4)} ms (noise ${n.ablation.noiseMs.toFixed(4)} ms); its frames are sized by what they measured.`);
      }
      if (n.measured) {
        const share = `${(n.measured.share * 100).toFixed(1)}% of the stage`;
        lines.push(n.kind === "line"
          ? `Measured: saves ${n.measured.savedMs.toFixed(4)} ms per draw taken out, ${n.measured.ownMs.toFixed(4)} ms of it its own (${share}).`
          : `Measured: saves ${n.measured.savedMs.toFixed(4)} ms per draw taken out (${share}).`);
      } else if (n.kind === "line") {
        lines.push("Own cost of this source line's instructions (loops weighted).");
      }
      if (n.estimated) lines.push("Includes modeled assumptions.");
      return lines.join("\n");
    },
    onSelect: (n) => {
      selected = n;
      updateMeasureButton();
      if ((n.kind === "item" || n.kind === "pass") && n.command && o.onSelectCommand) o.onSelectCommand(n.command.index);
      else if ((n.kind === "stage" || n.kind === "function" || n.kind === "line") && n.objectId !== undefined && o.onInspect && !n.children.length) o.onInspect(n.objectId);
    },
  });
  const notes = new Div(root, { class: "flame-notes" });
  const measured = new Div(root, { class: "flame-measured" });

  // Each measured stage's parts, textures included (they have no frame of their own).
  const renderMeasured = (): void => {
    measured.html = "";
    for (const a of o.data.ablations) renderAblation(measured, a, o.db.getObject(a.pipeline)?.name ?? `Pipeline ${a.pipeline}`);
  };

  const update = (): void => {
    const result = buildFrameCostTree({ ...o, perDraw, estimateFragments });
    units = result.units;
    tree = result.root;
    selected = null;
    graph.setData(result.root);
    const unitNote = units !== "ms" ? "modeled op units"
      : result.stats.measuredDrawPasses > 0 ? "measured GPU time, split by measured draws"
      : "measured GPU time, modeled split";
    summary.text = `${result.stats.passes} pass${result.stats.passes === 1 ? "" : "es"}, ${result.stats.items} draws and dispatches: ${formatCostValue(result.root.totalCost, units)} (${unitNote})`;
    notes.html = "";
    for (const note of result.notes) new Div(notes, { text: note, class: "flame-note text-muted font-sm" });
    renderMeasured();
    updateMeasureButton();
  };
  update();
}

function renderAblation(parent: Widget, a: ShaderAblation, pipelineName: string): void {
  new Div(parent, { class: "flame-measured-title",
    text: `${pipelineName}, ${a.stage}: ${a.entryPoint}, measured at #${a.command} on ${a.device}: stage ${ms(a.stageMs)} of ${a.baselineMs.toFixed(4)} ms per draw, noise ${a.noiseMs.toFixed(4)} ms` });
  const header = new Div(parent, { class: "flame-measured-row text-muted" });
  for (const t of ["Part", "Saved", "Own", "Share"]) new Span(header, { text: t });
  const order = { function: 0, texture: 1, line: 2, stage: 3 } as const;
  const parts = a.parts.slice().sort((x, y) => order[x.kind] - order[y.kind] || (y.savedMs ?? -1) - (x.savedMs ?? -1));
  for (const p of parts) {
    const row = new Div(parent, { class: "flame-measured-row" });
    const share = partShare(a, p);
    const noise = p.savedMs !== null && p.savedMs <= a.noiseMs;
    new Span(row, { text: `${p.kind === "texture" ? "texture " : p.kind === "function" ? "function " : ""}${p.name}`, tooltip: p.note });
    new Span(row, { text: ms(p.savedMs), class: noise ? "text-muted" : "" });
    new Span(row, { text: p.kind === "line" ? ms(p.ownMs) : "" });
    new Span(row, { text: share === null ? "" : `${(share * 100).toFixed(1)}%`, class: noise ? "text-muted" : "" });
  }
  for (const s of a.skipped) new Div(parent, { text: `Not measured: ${s.name}: ${s.reason}`, class: "flame-note text-muted font-sm" });
  for (const n of [...(a.note ? [a.note] : []), ...(a.notes ?? [])]) new Div(parent, { text: n, class: "flame-note text-muted font-sm" });
}
