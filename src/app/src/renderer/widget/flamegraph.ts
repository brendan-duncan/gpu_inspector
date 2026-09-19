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
// How far Ctrl+Wheel can zoom into the focused frame. Past this the frames are
// wider than the panel however small their cost, so there is nothing to gain.
const maxZoom = 1e6;
// Wheel delta to zoom factor. A notch of a typical mouse is 100, which this
// makes a 22% step.
const zoomPerDelta = 0.002;
// A press that moves further than this pans instead of selecting a frame, so a
// drag never leaves the graph zoomed into whatever was under the pointer.
const dragSlopPx = 3;

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
 *
 * Besides that stepwise zoom the graph has a continuous one: Ctrl+Wheel (or a
 * trackpad pinch, which Chromium reports the same way) zooms about the pointer
 * and dragging pans, which is how a frame narrower than a pixel is reached
 * without clicking down to it. Both are a window over the focused frame's span
 * rather than a wider canvas, so nothing scrolls sideways and the frames stay
 * clipped to the panel.
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
  /**
   * The window shown of the focused frame, in its cost units: `[_viewStart,
   * _viewStart + _viewWidth)` of `[0, focus.totalCost)`. Ctrl+Wheel narrows it
   * and dragging slides it; clicking a frame or resetting puts it back to the
   * whole of the new focus.
   */
  private _viewStart = 0;
  private _viewWidth = 0;
  /** Set while a press is panning, so releasing it does not also select a frame. */
  private _panned = false;

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
    this._bindZoom();
  }

  /** Ctrl+Wheel zooms about the pointer, dragging pans; a plain wheel is left to scroll the report. */
  private _bindZoom(): void {
    const el = this._canvas.element;
    el.addEventListener("wheel", (e: WheelEvent) => {
      if (!e.ctrlKey && !e.metaKey) return;
      e.preventDefault();
      // deltaMode 1 is lines, 2 pages; normalize both to something wheel-sized.
      const delta = e.deltaY * (e.deltaMode === 1 ? 16 : e.deltaMode === 2 ? 100 : 1);
      this.zoomBy(Math.exp(-delta * zoomPerDelta), this._pointerFraction(e));
    }, { passive: false });

    let panning = false;
    let lastX = 0;
    el.addEventListener("pointerdown", (e: PointerEvent) => {
      if (e.button !== 0 && e.button !== 1) return;
      panning = true;
      this._panned = false;
      lastX = e.clientX;
    });
    el.addEventListener("pointermove", (e: PointerEvent) => {
      if (!panning) return;
      const dx = e.clientX - lastX;
      if (!this._panned && Math.abs(dx) < dragSlopPx) return;
      // Capture only once the press is a drag, so a plain click still reaches the frame under it.
      if (!this._panned) el.setPointerCapture(e.pointerId);
      this._panned = true;
      lastX = e.clientX;
      const width = el.getBoundingClientRect().width;
      if (width > 0) this._panBy(-(dx / width) * this._viewWidth);
    });
    const end = (e: PointerEvent): void => {
      if (!panning) return;
      panning = false;
      if (el.hasPointerCapture(e.pointerId)) el.releasePointerCapture(e.pointerId);
      // The click that follows the release is swallowed by the frame handler; clear the flag after it.
      if (this._panned) setTimeout(() => { this._panned = false; }, 0);
    };
    el.addEventListener("pointerup", end);
    el.addEventListener("pointercancel", end);
  }

  /** Where the pointer is across the graph, 0 at its left edge and 1 at its right. */
  private _pointerFraction(e: MouseEvent): number {
    const rect = this._canvas.element.getBoundingClientRect();
    if (rect.width <= 0) return 0.5;
    return Math.min(1, Math.max(0, (e.clientX - rect.left) / rect.width));
  }

  /**
   * Zooms the view by `factor` (above 1 zooms in) about `at`, a fraction across the graph that
   * stays put. Outside a wheel event this is the programmatic zoom: `zoomBy(2, 0.5)` doubles.
   */
  zoomBy(factor: number, at = 0.5): void {
    const focus = this.focus;
    if (!focus || !(factor > 0) || this._viewWidth <= 0) return;
    const full = focus.totalCost;
    if (full <= 0) return;
    const width = Math.min(full, Math.max(full / maxZoom, this._viewWidth / factor));
    // The cost under the pointer before the zoom stays under it after.
    const anchor = this._viewStart + this._viewWidth * at;
    this._setView(anchor - width * at, width);
  }

  private _panBy(costs: number): void {
    this._setView(this._viewStart + costs, this._viewWidth);
  }

  private _setView(start: number, width: number): void {
    const focus = this.focus;
    if (!focus) return;
    const clamped = Math.min(Math.max(start, 0), Math.max(0, focus.totalCost - width));
    if (clamped === this._viewStart && width === this._viewWidth) return;
    this._viewStart = clamped;
    this._viewWidth = width;
    this._render();
  }

  /** The whole focused frame shown, after a click zoom or a reset. */
  private _resetView(): void {
    this._viewStart = 0;
    this._viewWidth = this.focus?.totalCost ?? 0;
  }

  /** How far the view is zoomed into the focused frame; 1 when the whole of it is shown. */
  get zoom(): number {
    const full = this.focus?.totalCost ?? 0;
    return full > 0 && this._viewWidth > 0 ? full / this._viewWidth : 1;
  }

  /** @param root - a `{ name, totalCost, selfCost, children }` tree */
  setData(root: N | null | undefined): void {
    this._root = root ?? null;
    this._stack = root ? [root] : [];
    this._resetView();
    this._render();
  }

  clear(): void {
    this._root = null;
    this._stack = [];
    this._viewStart = 0;
    this._viewWidth = 0;
    this._canvas.element.innerHTML = "";
    this._breadcrumb.element.innerHTML = "";
    this._canvas.element.style.height = "0";
  }

  /** Zoom back out to the full tree: the root in focus and the whole of it in view. */
  resetZoom(): void {
    const zoomed = this._stack.length > 1 || this.zoom !== 1;
    if (!zoomed) return;
    this._stack.length = Math.min(this._stack.length, 1);
    this._resetView();
    this._render();
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
    this._resetView();
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

    // The view window spans the full width; everything below scales to it. Un-
    // zoomed that window is the whole focused frame. A zero-cost focus would
    // divide by zero, so bail to an empty graph.
    if (this._viewWidth <= 0) this._resetView();
    const scale = this._viewWidth > 0 ? 100 / this._viewWidth : 0;
    const viewEnd = this._viewStart + this._viewWidth;
    let maxDepth = 0;
    let culled = 0;

    const emit = (node: N, depth: number, offsetCost: number, ancestors: N[]): void => {
      // Outside the window: the subtree is inside its parent's span, so it goes with it.
      if (offsetCost >= viewEnd || offsetCost + node.totalCost <= this._viewStart) return;
      const widthPct = node.totalCost * scale;
      // Cull subtrees narrower than a pixel or so. A frame graph over a real
      // capture can hold thousands of nodes, most of them sub-pixel at full
      // zoom; emitting them costs DOM for something nobody can see. They come
      // back as soon as the user zooms in, by click or by wheel.
      if (depth > 0 && widthPct < minVisiblePct) {
        culled++;
        return;
      }
      if (depth > maxDepth) {
        maxDepth = depth;
      }
      // Clipped to the window rather than left to overflow it, so a zoomed
      // graph needs no horizontal scrolling and no clipping container.
      const left = (offsetCost - this._viewStart) * scale;
      this._emitFrame(node, depth, Math.max(0, left), Math.min(100, left + widthPct) - Math.max(0, left), ancestors);

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
      this._renderZoomCrumb();
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
    this._renderZoomCrumb();
  }

  /**
   * How far Ctrl+Wheel has zoomed in, after the breadcrumb, with a click to undo it. Without this
   * a zoomed graph looks like a graph of something else: the frames are wider than their cost and
   * nothing says so.
   */
  private _renderZoomCrumb(): void {
    const zoom = this.zoom;
    if (zoom <= 1.001) return;
    if (this._stack.length > 1) {
      const sep = document.createElement("span");
      sep.textContent = "  ";   // plain spaces collapse, and the crumb would touch the breadcrumb
      this._breadcrumb.element.appendChild(sep);
    }
    const crumb = document.createElement("span");
    crumb.className = "flamegraph-crumb-link";
    crumb.textContent = `${zoom >= 10 ? Math.round(zoom) : zoom.toFixed(1)}× zoom`;
    crumb.title = "Zoomed in with Ctrl+Wheel; click to zoom back out. Drag the graph to pan.";
    crumb.onclick = () => {
      this._resetView();
      this._render();
    };
    this._breadcrumb.element.appendChild(crumb);
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
      if (this._panned) return;   // the press panned the graph; it was not a click on this frame
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
