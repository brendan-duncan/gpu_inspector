// Ported from WebGPU Inspector (https://github.com/brendan-duncan/webgpu_inspector), MIT license.
import { Widget, WidgetOptions } from "./widget.js";
import { Div } from "./div.js";

// Row geometry. Frames are laid out as absolutely positioned divs, one row per
// depth, which keeps hover/click/tooltips plain DOM. Trees here are hundreds of
// frames, not the millions a sampling profiler produces, so this is fast enough
// and far simpler than a canvas renderer.
const rowHeightPx = 18;
const rowGapPx = 1;
const minFramePx = 2;
// Frames narrower than this fraction of the current view are not emitted at
// all. At a typical panel width this is well under a pixel, so nothing visible
// is lost, and it keeps the DOM bounded on captures with thousands of frames.
const minVisiblePct = 0.04;

// Palette keyed by what dominates a frame's cost, so the graph reads as "why is
// this expensive" and not just "how expensive". Falls back to the neutral ramp
// when the caller doesn't classify frames.
const dimensionColors: Readonly<Record<string, string>> = {
  alu: "#4a8db8",
  sfu: "#c98a3a",
  texture: "#c0504d",
  memory: "#7b62c9",
};

const kindColors: Readonly<Record<string, string>> = {
  entry: "#3f7f5f",
  function: "#4a8db8",
  loop: "#a87cd0",
  branch: "#6c7a89",
  switch: "#6c7a89",
  case: "#6c7a89",
  statement: "#4a8db8",
  recursive: "#8a5a5a",
};

const neutralColor = "#4a8db8";

/**
 * Shape of one frame in the tree. `N` is the concrete node type so a caller's
 * richer node interface flows through `children` and the option callbacks.
 */
export interface FlameGraphNodeBase<N> {
  name: string;
  totalCost: number;
  selfCost?: number;
  children?: N[];
  /** Cost dimension that dominates this frame; keys `FlameGraph.dimensionColors`. */
  dimension?: string;
  /** Structural kind of the frame; keys `FlameGraph.kindColors`. */
  kind?: string;
}

/** A self-similar `{ name, totalCost, selfCost, children }` tree node. */
export interface FlameGraphNode extends FlameGraphNodeBase<FlameGraphNode> {}

export interface FlameGraphOptions<N extends FlameGraphNodeBase<N> = FlameGraphNode> extends WidgetOptions {
  /** Value text for tooltips. */
  formatValue?: (node: N) => string;
  /** CSS color per frame. */
  colorOf?: (node: N) => string;
  /** Full tooltip text. */
  tooltipOf?: (node: N) => string;
  /** Fired on click, before zooming. */
  onSelect?: (node: N) => void;
  /** Defaults to true. */
  zoomOnClick?: boolean;
}

/**
 * A zoomable flame graph over any tree of `{ name, totalCost, children }`.
 *
 * Width is proportional to `totalCost`, depth is nesting. Clicking a frame
 * zooms into it (the frame becomes full width); a breadcrumb above the graph
 * walks back out. The widget is deliberately agnostic about what a "cost" is —
 * pass `formatValue` to render abstract ops, milliseconds, or invocations.
 */
export class FlameGraph<N extends FlameGraphNodeBase<N> = FlameGraphNode> extends Widget {
  static readonly dimensionColors = dimensionColors;
  static readonly kindColors = kindColors;
  static override _idPrefix = "FLAMEGRAPH";

  private _formatValue: (node: N) => string;
  private _colorOf: (node: N) => string;
  private _tooltipOf: ((node: N) => string) | null;
  private _onSelect: ((node: N) => void) | null;
  private _zoomOnClick: boolean;
  private _breadcrumb: Div;
  private _canvas: Div;
  private _root: N | null;
  // The zoom stack: [root, ...ancestors, focus]. Index 0 is always the tree's
  // real root so the breadcrumb can always get home.
  private _stack: N[];
  /** Number of frames skipped by the last render because they were sub-pixel. */
  private _culled = 0;

  constructor(parent?: Widget | HTMLElement | null, options: FlameGraphOptions<N> = {}) {
    super("div", parent, options);
    this.classList.add("flamegraph");

    this._formatValue = options.formatValue ?? ((n: N) => n.totalCost.toFixed(1));
    this._colorOf = options.colorOf ?? defaultColorOf;
    this._tooltipOf = options.tooltipOf ?? null;
    this._onSelect = options.onSelect ?? null;
    this._zoomOnClick = options.zoomOnClick !== false;

    this._breadcrumb = new Div(this, { class: "flamegraph-breadcrumb" });
    this._canvas = new Div(this, { class: "flamegraph-canvas" });

    this._root = null;
    this._stack = [];
  }

  /** @param root - a `{ name, totalCost, selfCost, children }` tree */
  setData(root: N | null | undefined): void {
    this._root = root ?? null;
    this._stack = root ? [root] : [];
    this._render();
  }

  clear(): void {
    this._root = null;
    this._stack = [];
    this._canvas.element.innerHTML = "";
    this._breadcrumb.element.innerHTML = "";
    this._canvas.element.style.height = "0";
  }

  /** Zoom back out to the full tree. */
  resetZoom(): void {
    if (this._stack.length > 1) {
      this._stack.length = 1;
      this._render();
    }
  }

  get focus(): N | null {
    return this._stack[this._stack.length - 1] ?? null;
  }

  private _zoomTo(node: N, ancestors: N[]): void {
    const root = this._root;
    if (!root) {
      return;
    }
    // The root is always the full view; zooming into it would only repeat the breadcrumb.
    this._stack = node === root ? [root] : [root, ...ancestors.slice(1), node];
    this._render();
  }

  private _render(): void {
    this._canvas.element.innerHTML = "";
    this._breadcrumb.element.innerHTML = "";

    const focus = this.focus;
    if (!focus) {
      this._canvas.element.style.height = "0";
      return;
    }

    this._renderBreadcrumb();

    // The focused frame spans the full width; everything below scales to it.
    // A zero-cost focus would divide by zero, so bail to an empty graph.
    const scale = focus.totalCost > 0 ? 100 / focus.totalCost : 0;
    let maxDepth = 0;
    let culled = 0;

    const emit = (node: N, depth: number, offsetCost: number, ancestors: N[]): void => {
      const widthPct = node.totalCost * scale;
      // Cull subtrees narrower than a pixel or so. A frame graph over a real
      // capture can hold thousands of nodes, most of them sub-pixel at full
      // zoom; emitting them costs DOM for something nobody can see. They come
      // back as soon as the user zooms into an ancestor.
      if (depth > 0 && widthPct < minVisiblePct) {
        culled++;
        return;
      }
      if (depth > maxDepth) {
        maxDepth = depth;
      }
      this._emitFrame(node, depth, offsetCost * scale, widthPct, ancestors);

      // Children are laid out left-to-right in their own order, each taking a
      // slice of the parent proportional to its total. Self cost shows up as
      // the uncovered remainder on the right of the parent frame.
      let cursor = offsetCost;
      const childAncestors = ancestors.concat([node]);
      for (const child of node.children ?? []) {
        emit(child, depth + 1, cursor, childAncestors);
        cursor += child.totalCost;
      }
    };

    emit(focus, 0, 0, []);
    this._culled = culled;

    // Say so rather than letting the graph look complete when it isn't. Unlike
    // a data cap this is only visual — zooming in brings the frames back.
    if (culled > 0) {
      const hint = document.createElement("span");
      hint.textContent = `  (${culled} frame${culled === 1 ? "" : "s"} too small to draw; zoom in)`;
      hint.className = "flamegraph-hint";
      this._breadcrumb.element.appendChild(hint);
    }

    this._canvas.element.style.height = `${(maxDepth + 1) * (rowHeightPx + rowGapPx)}px`;
  }

  private _renderBreadcrumb(): void {
    if (this._stack.length <= 1) {
      this._breadcrumb.element.textContent = "";
      return;
    }
    this._stack.forEach((node, i) => {
      if (i > 0) {
        const sep = document.createElement("span");
        sep.textContent = " › ";
        sep.className = "flamegraph-crumb-sep";
        this._breadcrumb.element.appendChild(sep);
      }
      const crumb = document.createElement("span");
      crumb.textContent = node.name;
      const isLast = i === this._stack.length - 1;
      crumb.className = isLast ? "flamegraph-crumb-current" : "flamegraph-crumb-link";
      if (!isLast) {
        crumb.onclick = () => {
          this._stack.length = i + 1;
          this._render();
        };
      }
      this._breadcrumb.element.appendChild(crumb);
    });
  }

  private _emitFrame(node: N, depth: number, leftPct: number, widthPct: number, ancestors: N[]): void {
    const frame = document.createElement("div");
    frame.className = "flamegraph-frame";   // geometry inline, looks in app.css (a hairline between siblings)
    frame.style.cssText = [
      `top: ${depth * (rowHeightPx + rowGapPx)}px`,
      `left: ${leftPct}%`,
      `width: ${widthPct}%`,
      `height: ${rowHeightPx}px`,
      `min-width: ${minFramePx}px`,
      `line-height: ${rowHeightPx}px`,
      `background: ${this._colorOf(node)}`,
    ].join(";");

    // Only label frames wide enough to show something legible; the tooltip
    // carries the detail for the rest.
    if (widthPct > 1.5) {
      frame.textContent = node.name;
    }
    frame.title = this._tooltipOf ? this._tooltipOf(node) : `${node.name}\n${this._formatValue(node)}`;

    frame.addEventListener("click", (e: MouseEvent) => {
      e.stopPropagation();
      if (this._onSelect) {
        this._onSelect(node);
      }
      if (this._zoomOnClick && (node.children?.length ?? 0) > 0) {
        this._zoomTo(node, ancestors);
      }
    });

    this._canvas.element.appendChild(frame);
  }
}

function defaultColorOf(node: FlameGraphNodeBase<unknown>): string {
  if (node.dimension && dimensionColors[node.dimension]) {
    return dimensionColors[node.dimension];
  }
  if (node.kind && kindColors[node.kind]) {
    return kindColors[node.kind];
  }
  return neutralColor;
}
