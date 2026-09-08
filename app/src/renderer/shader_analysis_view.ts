// Views of the static shader analysis (renderer/vulkan/spirv_analysis.ts): the findings list
// with a severity filter and the modeled cost per entry point and function, after WebGPU
// Inspector's shader_analysis_view.js and Shader Cost section. Used by the Inspect tab on every
// shader payload and by the capture's "Analyze Shaders" report.
import { Checkbox } from "./widget/checkbox.js";
import { collapsible } from "./widget/collapsible.js";
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import { Widget } from "./widget/widget.js";
import {
  COST_DIMENSIONS, COST_WEIGHTS, LOOP_TRIPS, SEVERITY_RANK, severitySummary, weighCost, worstSeverity,
  type CostDimension, type CostVec, type Finding, type LineCost, type Severity, type ShaderAnalysis,
} from "./vulkan/spirv_analysis.js";
import { stageLabel } from "./shader_cache.js";

const SEVERITY_LABEL: Record<Severity, string> = { high: "High", medium: "Med", low: "Low", info: "Info" };
const DIMENSION_LABEL: Record<CostDimension, string> = { alu: "ALU", sfu: "SFU", texture: "Texture", memory: "Memory" };

export type LineHandler = (file: string | undefined, line: number) => void;

function addFinding(parent: Widget, f: Finding, onLine?: LineHandler): void {
  const row = new Div(parent, { class: `perf-finding perf-row-${f.severity}${f.confidence !== "high" ? " perf-lowconf" : ""}` });
  const head = new Div(row, { class: "perf-finding-head" });
  new Span(head, { text: f.severity.toUpperCase(), class: `perf-badge perf-${f.severity}` });
  new Span(head, { text: f.rule, class: "perf-rule" });
  if (f.line !== undefined) {
    const text = `${f.file ? `${f.file}:` : "line "}${f.line}`;
    if (onLine) {
      const link = new Span(head, { text, class: "perf-line-link dependency_link" });
      const line = f.line;
      link.element.onclick = () => onLine(f.file, line);
    } else {
      new Span(head, { text, class: "perf-line text-muted" });
    }
  }
  if (f.count > 1) new Span(head, { text: `×${f.count}`, class: "perf-count text-muted" });
  new Div(row, { text: f.message, class: "perf-msg" });
  const meta: string[] = [];
  if (f.function) meta.push(`fn ${f.function}`);
  if (f.loopDepth) meta.push(`loop depth ${f.loopDepth}`);
  if (f.confidence !== "high") meta.push(`${f.confidence} confidence`);
  if (meta.length) new Div(row, { text: meta.join(" · "), class: "perf-finding-meta text-muted font-sm" });
}

/** The findings with a severity filter above them. */
export function renderFindings(parent: Widget, findings: Finding[], onLine?: LineHandler): void {
  if (!findings.length) {
    new Div(parent, { text: "No performance issues found.", class: "perf-empty text-muted" });
    return;
  }
  const present = new Set(findings.map((f) => f.severity));
  const toggles: Severity[] = ["high", "medium", "low"];
  if (present.has("info")) toggles.push("info");
  const list = new Div(null, { class: "perf-findings" });
  if (present.size > 1) {
    const filterRow = new Div(parent, { class: "perf-filter-row" });
    new Span(filterRow, { text: "Show:", class: "text-muted font-sm" });
    for (const sev of toggles) {
      const cb = new Checkbox(filterRow, { label: SEVERITY_LABEL[sev], checked: true, class: "inspector-filter-field" });
      cb.input.onchange = () => list.classList.toggle(`perf-hide-${sev}`, !cb.checked);
    }
  }
  parent.appendChild(list);
  for (const f of findings) addFinding(list, f, onLine);
}

/** A "Performance Analysis" section: collapsed when clean, its label carrying the summary. */
export function renderAnalysisSection(parent: Widget, analysis: ShaderAnalysis, onLine?: LineHandler, label = "Performance Analysis"): collapsible {
  const findings = analysis.findings;
  const grp = new collapsible(parent, {
    label: findings.length ? `${label} (${findings.length}): ${severitySummary(findings)}` : `${label}: no issues found`,
    collapsed: findings.length === 0, class: `perf-section perf-title-${worstSeverity(findings)}`,
  });
  if (!analysis.hasLines && findings.length) {
    new Div(grp.body, { text: "No source lines: the module has no debug information (compile with -g to get line numbers).", class: "text-muted font-sm perf-note" });
  }
  renderFindings(grp.body, findings, analysis.hasLines ? onLine : undefined);
  return grp;
}

function costCells(row: Widget, cost: { alu: number; sfu: number; texture: number; memory: number }): void {
  for (const d of COST_DIMENSIONS) new Span(row, { text: cost[d] ? String(Math.round(cost[d])) : "·", class: `perf-cost-cell perf-dim-${d}` });
  new Span(row, { text: String(Math.round(weighCost(cost))), class: "perf-cost-cell perf-cost-total" });
}

/** The "Shader Cost" section: per entry point, the modeled cost by dimension and by function. */
export function renderCostSection(parent: Widget, analysis: ShaderAnalysis, entryPoint?: string, onLine?: LineHandler): collapsible {
  const entries = entryPoint ? analysis.entryPoints.filter((e) => e.name === entryPoint) : analysis.entryPoints;
  const top = entries.reduce((best, e) => (e.weighted > (best?.weighted ?? -1) ? e : best), entries[0] ?? null);
  const label = top ? `Shader Cost (modeled): ${Math.round(top.weighted)} units, ${DIMENSION_LABEL[top.dominant]} dominant` : "Shader Cost (modeled)";
  const grp = new collapsible(parent, { label, collapsed: true, class: "perf-section" });
  const body = new Div(grp.body, { class: "perf-cost" });
  new Div(body, {
    text: `Cost of one invocation in modeled op units: ALU x${COST_WEIGHTS.alu}, special-function x${COST_WEIGHTS.sfu}, texture x${COST_WEIGHTS.texture}, memory x${COST_WEIGHTS.memory}; loops assume ${LOOP_TRIPS} iterations per level. Compare shaders and functions with each other, not with time.`,
    class: "text-muted font-sm perf-note",
  });
  const t = analysis.totals;
  new Div(body, {
    text: `${t.instructions} instructions in ${t.functions} function${t.functions === 1 ? "" : "s"}, ${t.loops} loop${t.loops === 1 ? "" : "s"}, ${t.branches} branch${t.branches === 1 ? "" : "es"}, ${t.textureOps} texture op${t.textureOps === 1 ? "" : "s"}, ${t.memoryOps} memory op${t.memoryOps === 1 ? "" : "s"}, ${t.sfuOps} SFU op${t.sfuOps === 1 ? "" : "s"}${t.atomics ? `, ${t.atomics} atomics` : ""}${t.barriers ? `, ${t.barriers} barriers` : ""}${t.derivatives ? `, ${t.derivatives} derivatives` : ""}${t.discards ? `, ${t.discards} discards` : ""}${t.workgroupBytes ? `, ${t.workgroupBytes >= 1024 ? `${(t.workgroupBytes / 1024).toFixed(1)} KB` : `${t.workgroupBytes} B`} shared memory` : ""}`,
    class: "text-muted font-sm",
  });
  for (const e of entries) {
    const head = new Div(body, { class: "perf-cost-row perf-cost-head" });
    new Span(head, { text: `${stageLabel(e.stage)} ${e.name}`, class: "perf-cost-name" });
    for (const d of COST_DIMENSIONS) new Span(head, { text: DIMENSION_LABEL[d], class: `perf-cost-cell perf-dim-${d}` });
    new Span(head, { text: "Cost", class: "perf-cost-cell perf-cost-total" });
    const total = new Div(body, { class: "perf-cost-row perf-cost-entry" });
    new Span(total, { text: "entry point (inclusive)", class: "perf-cost-name" });
    costCells(total, e.cost);
    for (const f of e.functions) {
      const row = new Div(body, { class: "perf-cost-row" });
      new Span(row, { text: `${f.name}${f.loops ? `  (${f.loops} loop${f.loops === 1 ? "" : "s"})` : ""}`, class: "perf-cost-name perf-cost-fn", tooltip: `${f.instructions} instructions, own cost ${Math.round(weighCost(f.cost))}, inclusive ${Math.round(weighCost(f.inclusive))}` });
      costCells(row, f.inclusive);
    }
  }
  if (!entries.length) new Div(body, { text: "No entry point.", class: "text-muted" });
  // The costliest source lines over the entry's functions (modules with line information).
  const lines: (LineCost & { fn: string })[] = [];
  for (const e of entries) for (const f of e.functions) for (const l of f.lines) lines.push({ ...l, fn: f.name });
  if (lines.length) {
    lines.sort((x, y) => y.weighted - x.weighted);
    const total = lines.reduce((acc, l) => acc + l.weighted, 0);
    new Div(body, { text: "Costliest lines (own cost of the line's instructions, loops weighted):", class: "text-muted font-sm perf-lines-head" });
    const list = new Div(body, { class: "perf-lines" });
    for (const l of lines.slice(0, 12)) {
      const row = new Div(list, { class: "perf-cost-row perf-line-row" });
      const where = `${l.file ? `${l.file}:` : "line "}${l.line}`;
      if (onLine) {
        const link = new Span(row, { text: where, class: "perf-line-link perf-cost-name" });
        link.element.onclick = () => onLine(l.file, l.line);
      } else {
        new Span(row, { text: where, class: "perf-cost-name" });
      }
      const share = total > 0 ? ` ${((l.weighted / total) * 100).toFixed(0)}%` : "";
      new Span(row, { text: `${Math.round(l.weighted)}${share}`, class: "perf-cost-cell perf-cost-total", tooltip: `${l.instructions} instruction${l.instructions === 1 ? "" : "s"} in ${l.fn}: ${costText(l.cost)}` });
      new Span(row, { text: DIMENSION_LABEL[l.dominant], class: `perf-cost-cell perf-dim-${l.dominant}` });
    }
    if (lines.length > 12) new Div(list, { text: `and ${lines.length - 12} more lines`, class: "text-muted font-sm" });
  }
  return grp;
}

function costText(c: CostVec): string {
  return `ALU ${Math.round(c.alu)}, SFU ${Math.round(c.sfu)}, texture ${Math.round(c.texture)}, memory ${Math.round(c.memory)}`;
}

/** One shader of the frame report: what it is, how often it ran, and its findings. */
export interface FrameShaderReport {
  label: string;             // "Cube pipeline: fragment main"
  objectId: number;          // the pipeline or module to reveal
  stage: string;
  uses: number;              // draws / dispatches that used it
  analysis: ShaderAnalysis | null;
}

/** The "Analyze Shaders" report over the frame's shaders, worst first. */
export function renderFrameReport(parent: Widget, reports: FrameShaderReport[], onInspect: (objectId: number) => void): void {
  const root = new Div(parent, { class: "perf-report" });
  new Div(root, { text: "Shader Analysis", class: "frame-stats-title" });
  const withFindings = reports.filter((r) => r.analysis && r.analysis.findings.length);
  const clean = reports.filter((r) => r.analysis && !r.analysis.findings.length).length;
  const failed = reports.filter((r) => !r.analysis).length;
  const total = withFindings.reduce((n, r) => n + r.analysis!.findings.length, 0);
  withFindings.sort((x, y) => {
    const sx = SEVERITY_RANK[worstSeverity(x.analysis!.findings)];
    const sy = SEVERITY_RANK[worstSeverity(y.analysis!.findings)];
    return sy - sx || y.uses - x.uses || y.analysis!.findings.length - x.analysis!.findings.length;
  });
  new Div(root, {
    text: withFindings.length
      ? `${total} issue${total === 1 ? "" : "s"} in ${withFindings.length} of ${reports.length} shader${reports.length === 1 ? "" : "s"} used by this frame${clean ? ` (${clean} clean)` : ""}${failed ? `, ${failed} not analyzable` : ""}.`
      : `No performance issues found in the ${reports.length} shader${reports.length === 1 ? "" : "s"} used by this frame${failed ? ` (${failed} not analyzable)` : ""}.`,
    class: "text-muted",
  });
  const costs = reports.filter((r) => r.analysis && r.analysis.entryPoints.length);
  if (costs.length) {
    costs.sort((x, y) => Math.max(...y.analysis!.entryPoints.map((e) => e.weighted)) * y.uses - Math.max(...x.analysis!.entryPoints.map((e) => e.weighted)) * x.uses);
    const card = new collapsible(root, { label: "Modeled cost by shader (per invocation × uses)", collapsed: true, class: "perf-section" });
    const body = new Div(card.body, { class: "perf-cost" });
    const head = new Div(body, { class: "perf-cost-row perf-cost-head" });
    new Span(head, { text: "Shader", class: "perf-cost-name" });
    for (const d of COST_DIMENSIONS) new Span(head, { text: DIMENSION_LABEL[d], class: `perf-cost-cell perf-dim-${d}` });
    new Span(head, { text: "Cost", class: "perf-cost-cell perf-cost-total" });
    new Span(head, { text: "Uses", class: "perf-cost-cell" });
    for (const r of costs) {
      const e = r.analysis!.entryPoints.reduce((best, x) => (x.weighted > best.weighted ? x : best));
      const row = new Div(body, { class: "perf-cost-row" });
      const name = new Span(row, { text: r.label, class: "perf-cost-name dependency_link" });
      name.element.onclick = () => onInspect(r.objectId);
      costCells(row, e.cost);
      new Span(row, { text: String(r.uses), class: "perf-cost-cell" });
    }
  }
  for (const r of withFindings) {
    const a = r.analysis!;
    const grp = new collapsible(root, {
      label: `${r.label}  (${a.findings.length}: ${severitySummary(a.findings)}; ${r.uses} use${r.uses === 1 ? "" : "s"})`,
      collapsed: false, class: `perf-section perf-title-${worstSeverity(a.findings)}`,
    });
    const link = new Div(grp.body, { text: "Open in the Inspect tab", class: "dependency_link font-sm perf-inspect-link" });
    link.element.onclick = () => onInspect(r.objectId);
    renderFindings(grp.body, a.findings);
  }
}
