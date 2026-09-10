// The Render Graph view: a captured frame drawn as the passes it ran and the resources that tie
// them together (render_graph.ts builds the graph this shows).
//
// The obvious drawing of a render graph is a node-link diagram, and it is the wrong one for a
// real frame: a Unity player's frame has hundreds of passes and thousands of edges, and laid out
// as a DAG it is a hairball whatever the layout algorithm. So the main view is a resource
// lifetime chart instead — passes along the top in execution order, one row per resource, a bar
// across the passes where that resource is live, marked where each pass reads or writes it. It
// scales to any frame, it needs no layout pass, and "what does this pass depend on" is still
// answered by reading up its column.
//
// The node-link drawing is kept where it is actually readable: the neighbourhood of whatever is
// selected. Pick a pass and the panel below draws its immediate producers on the left and its
// immediate consumers on the right, with the resource on each edge, which is the "why is this
// pass here" question a graph is really being asked.
import { Button } from "./widget/button.js";
import { Checkbox } from "./widget/checkbox.js";
import { Div } from "./widget/div.js";
import { Select } from "./widget/select.js";
import { Span } from "./widget/span.js";
import { TextInput } from "./widget/text_input.js";
import { orderResources, usageClass } from "./render_graph.js";
import type { GraphNode, GraphResource, GraphUse, RenderGraph } from "./render_graph.js";
import type { Widget } from "./widget/widget.js";

export interface RenderGraphViewOptions {
  /** Select a command in the capture's command list (the pass' begin command). */
  onSelectCommand: (commandIndex: number) => void;
  /** Show an object in the Inspect tab. */
  onInspect: (objectId: number) => void;
}

/** Column widths in pixels, by the zoom control's label; "Fit" divides the panel between the passes. */
const ZOOM: Record<string, number> = { "Fit": 0, "Small": 8, "Medium": 16, "Large": 28 };
const ROW_HEIGHT = 18;
const LABEL_WIDTH = 220;
/** Below this the chart is unreadable, so "Fit" stops shrinking and the track scrolls instead. */
const MIN_COLUMN = 3;
/** A frame of a few passes would otherwise get absurd columns; above this the name fits in one. */
const MAX_FIT_COLUMN = 240;
const NAMED_COLUMN = 56;

type Selection = { kind: "node"; node: GraphNode } | { kind: "resource"; resource: GraphResource } | null;

class RenderGraphView {
  private _graph: RenderGraph;
  private _options: RenderGraphViewOptions;
  private _root: Div;
  private _chart: Div;
  private _detail: Div;
  private _status: Div;
  private _filter = "";
  private _showImages = true;
  private _showBuffers = true;
  /** Vertex, index, indirect and uniform buffers: real edges, but they crowd out the interesting ones. */
  private _showInputs = false;
  private _zoom = "Fit";
  private _selection: Selection = null;
  private _columnWidth = ZOOM.Medium;
  /** Rows and header cells of the last draw, for highlighting without rebuilding. */
  private _nodeCells: Div[] = [];
  private _rows = new Map<string, Div>();

  constructor(container: Widget, graph: RenderGraph, options: RenderGraphViewOptions) {
    this._graph = graph;
    this._options = options;
    this._root = new Div(container, { class: "render-graph" });
    new Div(this._root, { text: "Render Graph", class: "frame-stats-title" });
    this._status = new Div(this._root, { class: "render-graph-summary text-muted font-sm" });
    for (const warning of graph.warnings) {
      new Div(this._root, { text: warning, class: "render-graph-warning font-sm" });
    }
    this._buildControls();
    this._chart = new Div(this._root, { class: "render-graph-chart" });
    this._detail = new Div(this._root, { class: "render-graph-detail" });
    this._draw();
    // Open on the frame's last presented write, which is what the frame is for; failing that, the
    // most expensive pass, which is what someone opening this view is usually looking for.
    const presented = [...graph.nodes].reverse().find((n) => n.writes.some((w) => w.resource.presented));
    const costly = [...graph.nodes].sort((a, b) => (b.durationMs ?? 0) - (a.durationMs ?? 0))[0];
    const initial = presented ?? costly ?? graph.nodes[0];
    if (initial) this._select({ kind: "node", node: initial });
  }

  private _buildControls(): void {
    const row = new Div(this._root, { class: "render-graph-controls" });
    new Span(row, { text: "Filter", class: "inspector-filter-label-sm" });
    const filter = new TextInput(row, { placeholder: "resource...", class: "inspector-filter-input-sm", style: "width: 110px;" });
    filter.element.oninput = () => {
      this._filter = filter.value.trim().toLowerCase();
      this._draw();
    };
    const images = new Checkbox(row, { label: "Images", checked: true, class: "inspector-filter-field" });
    images.input.onchange = () => { this._showImages = images.checked; this._draw(); };
    const buffers = new Checkbox(row, { label: "Buffers", checked: true, class: "inspector-filter-field" });
    buffers.input.onchange = () => { this._showBuffers = buffers.checked; this._draw(); };
    const inputs = new Checkbox(row, {
      label: "Geometry & uniforms", checked: false, class: "inspector-filter-field",
      tooltip: "Vertex, index, indirect and uniform buffers. Real dependencies, but a frame has one per draw, and they bury the render targets the graph is usually read for.",
    });
    inputs.input.onchange = () => { this._showInputs = inputs.checked; this._draw(); };
    new Span(row, { text: "Zoom", class: "inspector-filter-label-sm" });
    const zoom = new Select(row, { options: Object.keys(ZOOM), value: this._zoom, class: "render-graph-zoom" });
    zoom.onChange.addListener((value: string) => { this._zoom = value; this._draw(); });
    if (this._graph.criticalPath.length) {
      new Button(row, {
        label: "Critical path", class: "btn btn-sm",
        tooltip: "Select the longest chain of dependent passes by GPU time",
        callback: () => this._select({ kind: "node", node: this._graph.criticalPath[0] }),
      });
    }
  }

  // ------------------------------------------------------------------------------- the chart

  /** The resources the filters leave, in row order. */
  private _visibleResources(): GraphResource[] {
    return orderResources(this._graph.resources).filter((r) => {
      if (r.type === "image" && !this._showImages) return false;
      if (r.type === "buffer" && !this._showBuffers) return false;
      if (!this._showInputs && r.uses.every((u) => usageClass(u.usage) === "input")) return false;
      if (this._filter && !r.label.toLowerCase().includes(this._filter) && !r.detail.toLowerCase().includes(this._filter)) return false;
      return true;
    });
  }

  private _draw(): void {
    const graph = this._graph;
    const resources = this._visibleResources();
    this._nodeCells = [];
    this._rows = new Map();
    this._chart.html = "";

    const nodes = graph.nodes;
    const timed = nodes.filter((n) => n.durationMs !== null).length;
    const parts = [plural(nodes.length, "pass", "passes"), plural(graph.resources.length, "resource"), plural(graph.edges.length, "dependency", "dependencies")];
    if (timed) parts.push(`${timed} timed`);
    if (graph.criticalPathMs > 0) parts.push(`critical path ${graph.criticalPathMs.toFixed(3)} ms through ${plural(graph.criticalPath.length, "pass", "passes")}`);
    if (graph.externalInputs.length) parts.push(`${graph.externalInputs.length} read from before the frame`);
    if (graph.unreadNodes.length) parts.push(`${plural(graph.unreadNodes.length, "pass", "passes")} nothing reads`);
    this._status.text = parts.join(", ");

    if (!nodes.length) {
      new Div(this._chart, { text: "No passes in this capture touch a resource the graph can name.", class: "text-muted", style: "padding: 12px;" });
      return;
    }
    if (!resources.length) {
      new Div(this._chart, { text: "No resources match the filters.", class: "text-muted", style: "padding: 12px;" });
      return;
    }

    // "Fit" divides the panel between the passes; the others are fixed widths that scroll.
    const available = Math.max(200, this._chart.element.clientWidth - LABEL_WIDTH - 16);
    this._columnWidth = this._zoom === "Fit"
      ? Math.min(MAX_FIT_COLUMN, Math.max(MIN_COLUMN, Math.floor(available / nodes.length)))
      : ZOOM[this._zoom];
    const colW = this._columnWidth;
    const trackWidth = colW * nodes.length;

    const scroller = new Div(this._chart, { class: "render-graph-scroll" });
    const grid = new Div(scroller, { class: "render-graph-grid", style: `width: ${LABEL_WIDTH + trackWidth}px;` });

    // Pass header: one cell per node, colored by kind, sized by GPU time when the frame was timed.
    const header = new Div(grid, { class: "render-graph-header" });
    new Div(header, { text: `Passes (${nodes.length})`, class: "render-graph-corner", style: `width: ${LABEL_WIDTH}px;` });
    const headerTrack = new Div(header, { class: "render-graph-header-track", style: `width: ${trackWidth}px;` });
    const maxMs = Math.max(...nodes.map((n) => n.durationMs ?? 0), 0);
    const onPath = new Set(graph.criticalPath);
    for (const node of nodes) {
      const cell = new Div(headerTrack, {
        class: `render-graph-pass render-graph-pass-${node.kind}${node.unread ? " render-graph-unread" : ""}${onPath.has(node) ? " render-graph-onpath" : ""}`,
        style: `left: ${node.ordinal * colW}px; width: ${Math.max(1, colW - 1)}px;`,
      });
      // The filled part of the cell is the pass' share of the slowest pass, so the header doubles
      // as a bar chart of where the frame's GPU time went.
      if (maxMs > 0) {
        const share = Math.round(((node.durationMs ?? 0) / maxMs) * 100);
        new Div(cell, { class: "render-graph-pass-bar", style: `height: ${share}%;` });
      }
      if (colW >= NAMED_COLUMN) {
        new Span(cell, { text: ellipsis(node.label, Math.floor(colW / 6)), class: "render-graph-pass-name" });
      }
      cell.tooltip = this._nodeTooltip(node);
      cell.element.onclick = () => this._select({ kind: "node", node });
      this._nodeCells.push(cell);
    }

    // One row per resource: the lifetime bar, and a mark at every pass that touched it.
    const body = new Div(grid, { class: "render-graph-body" });
    for (const resource of resources) {
      const row = new Div(body, { class: "render-graph-row", style: `height: ${ROW_HEIGHT}px;` });
      const label = new Div(row, { class: `render-graph-label render-graph-${resource.type}`, style: `width: ${LABEL_WIDTH}px;` });
      new Span(label, { text: resource.label, class: "render-graph-name" });
      if (resource.detail) new Span(label, { text: resource.detail, class: "render-graph-detail-text text-muted" });
      label.tooltip = this._resourceTooltip(resource);
      label.element.onclick = () => this._select({ kind: "resource", resource });
      const track = new Div(row, { class: "render-graph-track", style: `width: ${trackWidth}px;` });
      const first = resource.first * colW;
      const span = Math.max(colW, (resource.last - resource.first + 1) * colW);
      new Div(track, { class: "render-graph-life", style: `left: ${first}px; width: ${span}px;` });
      if (resource.externalInput) {
        // A tick before the first use: the contents came from before the capture.
        new Div(track, { class: "render-graph-external", style: `left: ${Math.max(0, first - 3)}px;`, tooltip: "Read before anything in the capture wrote it: the previous frame, a host upload, or a pass outside the captured range." });
      }
      for (const use of resource.uses) {
        const mark = new Div(track, {
          class: `render-graph-use render-graph-use-${use.mode} render-graph-usage-${usageClass(use.usage)}`,
          style: `left: ${use.node.ordinal * colW}px; width: ${Math.max(2, colW - 1)}px;`,
        });
        mark.tooltip = `${use.node.label}\n${use.mode === "read" ? "reads" : use.mode === "write" ? "writes" : "reads and writes"} ${resource.label} as ${use.usage}`;
        mark.element.onclick = () => this._select({ kind: "node", node: use.node });
      }
      this._rows.set(resource.key, row);
    }
    this._applyHighlight();
    this._renderLegend();
  }

  private _renderLegend(): void {
    const legend = new Div(this._chart, { class: "render-graph-legend font-sm text-muted" });
    for (const [cls, text] of [["attachment", "attachment"], ["sampled", "sampled"], ["storage", "storage"], ["transfer", "copy"], ["input", "geometry / uniform"]]) {
      const item = new Span(legend, { class: "render-graph-legend-item" });
      new Span(item, { class: `render-graph-swatch render-graph-usage-${cls}` });
      new Span(item, { text });
    }
    const item = new Span(legend, { class: "render-graph-legend-item" });
    new Span(item, { class: "render-graph-swatch render-graph-use-read" });
    new Span(item, { text: "read (hollow) vs write (filled)" });
  }

  // ---------------------------------------------------------------------------- the selection

  private _select(selection: Selection): void {
    this._selection = selection;
    this._applyHighlight();
    this._renderDetail();
  }

  /** Marks the selected pass' column, and the rows and columns it is connected to. */
  private _applyHighlight(): void {
    for (const cell of this._nodeCells) cell.classList.remove("render-graph-selected", "render-graph-related");
    for (const row of this._rows.values()) row.classList.remove("render-graph-selected", "render-graph-related");
    const selection = this._selection;
    if (!selection) return;
    if (selection.kind === "node") {
      this._nodeCells[selection.node.ordinal]?.classList.add("render-graph-selected");
      for (const edge of [...selection.node.inputs, ...selection.node.outputs]) {
        const other = edge.from === selection.node ? edge.to : edge.from;
        this._nodeCells[other.ordinal]?.classList.add("render-graph-related");
      }
      for (const use of [...selection.node.reads, ...selection.node.writes]) {
        this._rows.get(use.resource.key)?.classList.add("render-graph-related");
      }
      return;
    }
    this._rows.get(selection.resource.key)?.classList.add("render-graph-selected");
    for (const use of selection.resource.uses) this._nodeCells[use.node.ordinal]?.classList.add("render-graph-related");
  }

  private _renderDetail(): void {
    this._detail.html = "";
    const selection = this._selection;
    if (!selection) return;
    if (selection.kind === "resource") {
      this._renderResourceDetail(selection.resource);
      return;
    }
    this._renderNodeDetail(selection.node);
  }

  private _renderNodeDetail(node: GraphNode): void {
    const card = new Div(this._detail, { class: "frame-stats-section" });
    const head = new Div(card, { class: "render-graph-detail-head" });
    new Span(head, { text: node.label, class: "frame-stats-heading" });
    const jump = new Span(head, { text: `command ${node.commandIndex}`, class: "perf-line-link dependency_link" });
    jump.element.onclick = () => this._options.onSelectCommand(node.commandIndex);
    jump.tooltip = "Select the pass in the command list";
    const facts = [
      node.kind === "transfer" ? "transfer" : `${node.draws} ${node.kind === "compute" ? "dispatch" : "draw"}${node.draws === 1 ? "" : "es"}`,
      node.durationMs !== null ? `${node.durationMs.toFixed(3)} ms` : "",
      node.pathMs > 0 ? `${node.pathMs.toFixed(3)} ms to the end of the frame` : "",
    ].filter(Boolean);
    new Div(card, { text: facts.join("  ·  "), class: "text-muted font-sm" });
    if (node.unread) {
      new Div(card, {
        text: "Nothing later in this capture reads what this pass wrote, and none of it is presented. It may still be read by the host, by the next frame, or through a binding the capture cannot see.",
        class: "render-graph-note font-sm",
      });
    }
    if (node.unresolvedReads) {
      new Div(card, {
        text: `${node.unresolvedReads} binding${node.unresolvedReads === 1 ? "" : "s"} of this pass could not be resolved to a resource, so it may read more than is shown.`,
        class: "render-graph-note font-sm",
      });
    }
    this._renderNeighbourhood(card, node);
    this._renderUses(card, "Reads", node.reads, (u) => u.resource);
    this._renderUses(card, "Writes", node.writes, (u) => u.resource);
  }

  /**
   * The node-link view, over the one part of the graph small enough to draw as one: the selected
   * pass, everything that feeds it and everything it feeds, with the resource named on each edge.
   */
  private _renderNeighbourhood(card: Widget, node: GraphNode): void {
    const producers = dedupeEdges(node.inputs.map((e) => ({ node: e.from, label: `${e.version.resource.label}`, usage: e.usage })));
    const consumers = dedupeEdges(node.outputs.map((e) => ({ node: e.to, label: `${e.version.resource.label}`, usage: e.usage })));
    if (!producers.length && !consumers.length) {
      new Div(card, { text: "No pass in this capture feeds this one, and none consumes it.", class: "text-muted font-sm", style: "padding: 4px 0;" });
      return;
    }
    const shownProducers = producers.slice(0, 8);
    const shownConsumers = consumers.slice(0, 8);
    const rows = Math.max(shownProducers.length, shownConsumers.length, 1);
    const rowH = 34;
    const height = rows * rowH + 12;
    const width = 780;
    const boxW = 170;
    // Wide gaps between the columns: the resource name rides on the edge, and a label that
    // overlaps the boxes it runs between is worse than no label.
    const colX = [4, (width - boxW) / 2, width - boxW - 4];
    const boxH = 26;
    const centerY = height / 2 - boxH / 2;

    const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    svg.setAttribute("class", "render-graph-dag");
    svg.setAttribute("viewBox", `0 0 ${width} ${height}`);
    // Both dimensions in pixels, scaled by the stylesheet's width:100%/height:auto. That gives the
    // element an intrinsic ratio to scale by; a percentage width with no height instead makes its
    // height depend on a width that depends on its height, and the layout never settles.
    svg.setAttribute("width", String(width));
    svg.setAttribute("height", String(height));

    const box = (x: number, y: number, target: GraphNode, cls: string): void => {
      const g = document.createElementNS("http://www.w3.org/2000/svg", "g");
      g.setAttribute("class", `render-graph-dag-node ${cls}`);
      const rect = document.createElementNS("http://www.w3.org/2000/svg", "rect");
      rect.setAttribute("x", String(x));
      rect.setAttribute("y", String(y));
      rect.setAttribute("width", String(boxW));
      rect.setAttribute("height", String(boxH));
      rect.setAttribute("rx", "3");
      g.appendChild(rect);
      const text = document.createElementNS("http://www.w3.org/2000/svg", "text");
      text.setAttribute("x", String(x + boxW / 2));
      text.setAttribute("y", String(y + boxH / 2 + 4));
      text.setAttribute("text-anchor", "middle");
      text.textContent = ellipsis(target.label, 26);
      g.appendChild(text);
      const title = document.createElementNS("http://www.w3.org/2000/svg", "title");
      title.textContent = this._nodeTooltip(target);
      g.appendChild(title);
      g.addEventListener("click", () => this._select({ kind: "node", node: target }));
      svg.appendChild(g);
    };

    const edge = (x1: number, y1: number, x2: number, y2: number, label: string): void => {
      const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
      const mid = (x1 + x2) / 2;
      path.setAttribute("d", `M ${x1} ${y1} C ${mid} ${y1}, ${mid} ${y2}, ${x2} ${y2}`);
      path.setAttribute("class", "render-graph-dag-edge");
      svg.appendChild(path);
      const text = document.createElementNS("http://www.w3.org/2000/svg", "text");
      text.setAttribute("x", String(mid));
      text.setAttribute("y", String((y1 + y2) / 2 - 4));
      text.setAttribute("text-anchor", "middle");
      text.setAttribute("class", "render-graph-dag-edge-label");
      text.textContent = ellipsis(label, 26);
      const title = document.createElementNS("http://www.w3.org/2000/svg", "title");
      title.textContent = label;
      text.appendChild(title);
      svg.appendChild(text);
    };

    shownProducers.forEach((p, i) => {
      const y = 6 + i * rowH;
      edge(colX[0] + boxW, y + boxH / 2, colX[1], centerY + boxH / 2, `${p.label} (${p.usage})`);
      box(colX[0], y, p.node, "render-graph-dag-in");
    });
    shownConsumers.forEach((c, i) => {
      const y = 6 + i * rowH;
      edge(colX[1] + boxW, centerY + boxH / 2, colX[2], y + boxH / 2, `${c.label} (${c.usage})`);
      box(colX[2], y, c.node, "render-graph-dag-out");
    });
    box(colX[1], centerY, node, "render-graph-dag-self");
    card.element.appendChild(svg);
    const hidden = (producers.length - shownProducers.length) + (consumers.length - shownConsumers.length);
    if (hidden) new Div(card, { text: `${hidden} more neighbour${hidden === 1 ? "" : "s"} not drawn.`, class: "text-muted font-sm" });
  }

  private _renderUses(card: Widget, title: string, uses: GraphUse[], resourceOf: (u: GraphUse) => GraphResource): void {
    if (!uses.length) return;
    new Div(card, { text: `${title} (${uses.length})`, class: "render-graph-detail-heading font-sm" });
    const list = new Div(card, { class: "frame-stats-list" });
    for (const use of uses.slice(0, 40)) {
      const resource = resourceOf(use);
      const row = new Div(list, { class: "frame-stats-row render-graph-use-row" });
      const name = new Div(row, { text: resource.label, class: "frame-stats-label dependency_link" });
      name.element.onclick = () => this._options.onInspect(resource.objectId);
      name.tooltip = "Show the object in the Inspect tab";
      const producer = use.version.producer;
      const from = use.mode === "read"
        ? producer ? `from ${producer.label}` : "from before the frame"
        : use.version.readers.length ? `read by ${use.version.readers.length} pass${use.version.readers.length === 1 ? "" : "es"}` : "not read again";
      new Div(row, { text: `${use.usage} · ${from}`, class: "frame-stats-value text-muted" });
    }
    if (uses.length > 40) new Div(list, { text: `... ${uses.length - 40} more`, class: "text-muted font-sm" });
  }

  private _renderResourceDetail(resource: GraphResource): void {
    const card = new Div(this._detail, { class: "frame-stats-section" });
    const head = new Div(card, { class: "render-graph-detail-head" });
    new Span(head, { text: resource.label, class: "frame-stats-heading" });
    const inspect = new Span(head, { text: "inspect", class: "perf-line-link dependency_link" });
    inspect.element.onclick = () => this._options.onInspect(resource.objectId);
    const facts = [resource.detail, resource.presented ? "presented" : "", `${resource.versions.length - 1} write${resource.versions.length === 2 ? "" : "s"} in the frame`].filter(Boolean);
    new Div(card, { text: facts.join("  ·  "), class: "text-muted font-sm" });
    new Div(card, { text: "Versions", class: "render-graph-detail-heading font-sm" });
    const list = new Div(card, { class: "frame-stats-list" });
    for (const version of resource.versions) {
      if (!version.producer && !version.readers.length) continue;
      const row = new Div(list, { class: "frame-stats-row" });
      const producer = version.producer;
      const name = new Div(row, { text: producer ? producer.label : "before the frame", class: `frame-stats-label${producer ? " dependency_link" : ""}` });
      if (producer) name.element.onclick = () => this._select({ kind: "node", node: producer });
      const readers = version.readers.length
        ? `read by ${version.readers.map((r) => r.label).slice(0, 3).join(", ")}${version.readers.length > 3 ? ` +${version.readers.length - 3}` : ""}`
        : version.dropped ? "discarded by the pass (store op DONT_CARE)" : "not read again in this capture";
      new Div(row, { text: readers, class: "frame-stats-value text-muted" });
    }
  }

  private _nodeTooltip(node: GraphNode): string {
    const parts = [node.label];
    if (node.durationMs !== null) parts.push(`${node.durationMs.toFixed(3)} ms`);
    if (node.draws) parts.push(`${node.draws} ${node.kind === "compute" ? "dispatches" : "draws"}`);
    parts.push(`${node.reads.length} read, ${node.writes.length} written`);
    if (node.unread) parts.push("nothing in the capture reads its output");
    return parts.join("\n");
  }

  private _resourceTooltip(resource: GraphResource): string {
    const parts = [resource.label, resource.detail].filter(Boolean);
    parts.push(`${resource.uses.length} accesses across passes ${resource.first}-${resource.last}`);
    if (resource.externalInput) parts.push("read before anything in the capture wrote it");
    if (resource.presented) parts.push("presented");
    return parts.join("\n");
  }
}

/** One entry per neighbour pass: several resources between two passes are one row, not five. */
function dedupeEdges(entries: { node: GraphNode; label: string; usage: string }[]): { node: GraphNode; label: string; usage: string }[] {
  const byNode = new Map<GraphNode, { node: GraphNode; label: string; usage: string; count: number }>();
  for (const e of entries) {
    const existing = byNode.get(e.node);
    if (existing) existing.count++;
    else byNode.set(e.node, { ...e, count: 1 });
  }
  return [...byNode.values()].map((e) => ({ node: e.node, label: e.count > 1 ? `${e.label} +${e.count - 1}` : e.label, usage: e.usage }));
}

function plural(n: number, one: string, many = ""): string {
  return `${n} ${n === 1 ? one : many || `${one}s`}`;
}

function ellipsis(text: string, max: number): string {
  return text.length > max ? `${text.slice(0, max - 1)}…` : text;
}

/** Renders the render graph of a capture into a container (the capture panel's details pane). */
export function renderRenderGraph(container: Widget, graph: RenderGraph, options: RenderGraphViewOptions): void {
  new RenderGraphView(container, graph, options);
}
