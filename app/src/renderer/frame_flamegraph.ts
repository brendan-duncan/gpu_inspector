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

const LEGEND: [string, string][] = [["alu", "ALU"], ["sfu", "SFU"], ["texture", "Texture"], ["memory", "Memory"]];

export interface FlameGraphPanelOptions extends Omit<CostTreeOptions, "perDraw" | "estimateFragments"> {
  onSelectCommand?: (index: number) => void;
  onInspect?: (objectId: number) => void;
}

function colorOf(n: FlameNode): string {
  if (n.dimension) return DIMENSION_COLORS[n.dimension];
  return KIND_COLORS[n.kind] ?? "#4a8db8";
}

export function renderFrameFlameGraph(parent: Widget, o: FlameGraphPanelOptions): void {
  const root = new Div(parent, { class: "flame-panel" });
  new Div(root, { text: "Shader Flame Graph", class: "frame-stats-title" });
  let perDraw = false;
  let estimateFragments = true;
  let units: CostUnits = "ops";

  const controls = new Div(root, { class: "flame-controls" });
  new Checkbox(controls, { label: "Per draw", checked: false, tooltip: "One frame per draw or dispatch instead of one per pipeline", onChange: (v) => { perDraw = v; update(); } });
  new Checkbox(controls, { label: "Estimate fragments from the scissor area", checked: true,
    tooltip: "Weight fragment stages by the scissor (or render area) in pixels: an upper bound without overdraw and before the depth test. Off: fragment stages have no width.",
    onChange: (v) => { estimateFragments = v; update(); } });
  const reset = new Button(controls, { label: "Reset zoom", class: "btn btn-sm", tooltip: "Zoom back out to the whole frame", callback: () => graph.resetZoom() });
  void reset;

  const summary = new Div(root, { class: "flame-summary text-muted" });
  const legend = new Div(root, { class: "flame-legend" });
  new Span(legend, { text: "Cost:", class: "flame-legend-label" });
  for (const [dim, label] of LEGEND) {
    const item = new Span(legend, { class: "flame-legend-item" });
    new Span(item, { class: "flame-legend-swatch", style: `background: ${DIMENSION_COLORS[dim as keyof typeof DIMENSION_COLORS]};` });
    new Span(item, { text: label });
  }
  new Span(legend, { text: "Click a frame to zoom in; a draw frame selects the draw, a shader frame reveals the shader.", class: "flame-legend-hint text-muted font-sm" });

  const graph = new FlameGraph<FlameNode>(root, {
    formatValue: (n) => formatCostValue(n.totalCost, units),
    colorOf,
    tooltipOf: (n) => {
      const lines = [n.name, formatCostValue(n.totalCost, units)];
      if (n.kind === "function" && n.selfCost > 0) lines.push(`Own cost: ${formatCostValue(n.selfCost, units)}`);
      if (n.durationMs != null) lines.push(`Pass GPU time: ${n.durationMs.toFixed(3)} ms`);
      if (n.confidence) lines.push(`Invocation count: ${n.confidence}`);
      if (n.dimension) lines.push(`Dominant cost: ${n.dimension.toUpperCase()}`);
      if (n.estimated) lines.push("Includes modeled assumptions.");
      return lines.join("\n");
    },
    onSelect: (n) => {
      if ((n.kind === "item" || n.kind === "pass") && n.command && o.onSelectCommand) o.onSelectCommand(n.command.index);
      else if ((n.kind === "stage" || n.kind === "function") && n.objectId !== undefined && o.onInspect && !n.children.length) o.onInspect(n.objectId);
    },
  });
  const notes = new Div(root, { class: "flame-notes" });

  const update = (): void => {
    const result = buildFrameCostTree({ ...o, perDraw, estimateFragments });
    units = result.units;
    graph.setData(result.root);
    const unitNote = units === "ms" ? "measured GPU time, modeled split" : "modeled op units";
    summary.text = `${result.stats.passes} pass${result.stats.passes === 1 ? "" : "es"}, ${result.stats.items} draws and dispatches: ${formatCostValue(result.root.totalCost, units)} (${unitNote})`;
    notes.html = "";
    for (const note of result.notes) new Div(notes, { text: note, class: "flame-note text-muted font-sm" });
  };
  update();
}
