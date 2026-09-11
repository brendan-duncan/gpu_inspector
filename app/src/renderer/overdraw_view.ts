// A capture's overdraw in a tab of its own: one pass's heatmap at any zoom, over the pass's render
// target when asked, with what lies under the pointer in a tooltip (both counts, and the render
// target's texel). The pass and which count it shows are picked in the toolbar. Metal captures
// carry their measurements (metal/src/overdraw.h); a Vulkan capture gets them from vkinsp_replay
// (CaptureView.measureOverdraw).
import { Button } from "./widget/button.js";
import { Checkbox } from "./widget/checkbox.js";
import { Div } from "./widget/div.js";
import { NumberInput } from "./widget/number_input.js";
import { Select } from "./widget/select.js";
import { Span } from "./widget/span.js";
import type { CaptureData, CapturedOverdraw, CapturedTexture } from "./capture_data.js";
import { OVERDRAW_LEGEND, isMeasured, overdrawCount, overdrawHistogramText, overdrawRgba, overdrawSummary } from "./overdraw.js";
import { decodeImage, decodeTexels, formatTexel, type TexelData } from "./vulkan/texture_decode.js";
import { fmt } from "./vulkan/vulkan_object.js";

export interface OverdrawPassKey {
  frame: number;
  commandBuffer: number;
  passIndex: number;
}

/** What the view needs of the capture tab it belongs to. */
export interface OverdrawHost {
  readonly data: CaptureData;
  /** The pass's label, as the command tree shows it. */
  passLabelOf(key: OverdrawPassKey): string;
  /** Shows the capture's tab with the pass's begin command selected. */
  selectPass(key: OverdrawPassKey): void;
}

const ICON_COPY = '<svg viewBox="0 0 16 16" aria-label="Copy"><rect x="5.5" y="5.5" width="8" height="8" rx="1" fill="none" stroke="currentColor" stroke-width="1.5"/><path d="M10.5 3.5v-1a1 1 0 0 0-1-1h-6a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h1" fill="none" stroke="currentColor" stroke-width="1.5"/></svg>';

const sameKey = (a: OverdrawPassKey, b: OverdrawPassKey): boolean =>
  a.frame === b.frame && a.commandBuffer === b.commandBuffer && a.passIndex === b.passIndex;

export class OverdrawView {
  readonly host: OverdrawHost;
  readonly root: Div;
  private _key: OverdrawPassKey;
  private _depthTested: boolean;
  /** Percent; 0 fits the image to the tab. */
  private _zoom = 0;
  private _overlay = false;
  /** Percent of the heat colour over the render target. */
  private _opacity = 65;
  private _pinned = "";

  private _measurement: CapturedOverdraw | null = null;
  private _other: CapturedOverdraw | null = null;
  /** The pass's first colour target of the measurement's size, decoded once. */
  private _target: { tex: CapturedTexture; rgba: Uint8ClampedArray | null; texels: TexelData | null } | null = null;

  private _canvas = document.createElement("canvas");
  private _scroll: Div | null = null;
  private _pixelInfo: Span | null = null;
  private _tooltip: HTMLPreElement;
  private _resize: ResizeObserver;
  private _rebuildTimer: ReturnType<typeof setTimeout> | null = null;
  private readonly _onData = (): void => this._scheduleRebuild();

  constructor(host: OverdrawHost, key: OverdrawPassKey, depthTested = true) {
    this.host = host;
    this._key = key;
    this._depthTested = depthTested;
    this.root = new Div(null, { class: "overdraw-view" });
    this._tooltip = document.createElement("pre");
    this._tooltip.className = "image-tooltip";
    this._tooltip.style.display = "none";
    document.body.appendChild(this._tooltip);
    this._canvas.className = "image-view-canvas";
    this._setupCanvasEvents();
    this._resize = new ResizeObserver(() => {
      if (this._zoom <= 0) this._applyZoom();
    });
    host.data.onOverdraw.addListener(this._onData);
    host.data.onTextureLoaded.addListener(this._onData);
    this._rebuild();
  }

  /** The pass and the count the tab shows. */
  get key(): OverdrawPassKey {
    return this._key;
  }

  get depthTested(): boolean {
    return this._depthTested;
  }

  show(key: OverdrawPassKey, depthTested: boolean): void {
    this._key = key;
    this._depthTested = depthTested;
    this._pinned = "";
    this._rebuild();
  }

  dispose(): void {
    this.host.data.onOverdraw.disconnect(this._onData);
    this.host.data.onTextureLoaded.disconnect(this._onData);
    this._resize.disconnect();
    if (this._rebuildTimer) clearTimeout(this._rebuildTimer);
    this._tooltip.remove();
  }

  /** The UI tests' view of the tab (tools/ui_tests.py). */
  debugState(): Record<string, unknown> {
    const o = this._measurement;
    return {
      pass: this._key, depthTested: this._depthTested, overlay: this._overlay, hasTarget: !!this._target,
      width: o?.info.width ?? 0, height: o?.info.height ?? 0, counts: !!o?.data, canvas: [this._canvas.width, this._canvas.height],
    };
  }

  // ---------------------------------------------------------------------------------------

  /** The passes with a measurement, in the order the capture lists them. */
  private _passes(): OverdrawPassKey[] {
    const out: OverdrawPassKey[] = [];
    for (const o of this.host.data.overdraw) {
      if (!out.some((k) => sameKey(k, o.info))) out.push({ frame: o.info.frame, commandBuffer: o.info.commandBuffer, passIndex: o.info.passIndex });
    }
    return out;
  }

  private _scheduleRebuild(): void {
    if (this._rebuildTimer) return;
    this._rebuildTimer = setTimeout(() => {
      this._rebuildTimer = null;
      this._rebuild();
    }, 200);
  }

  private _rebuild(): void {
    const data = this.host.data;
    this.root.html = "";
    this._scroll = null;
    this._pixelInfo = null;
    this._resize.disconnect();
    const passes = this._passes();
    if (!passes.length) {
      new Div(this.root, { text: "This capture has no overdraw measurements.", class: "text-muted", style: "padding: 12px;" });
      return;
    }
    if (!passes.some((k) => sameKey(k, this._key))) this._key = passes[0];
    const measurements = data.overdrawForPass(this._key.frame, this._key.commandBuffer, this._key.passIndex);
    this._measurement = measurements.find((m) => m.info.depthTested === this._depthTested) ?? measurements[0] ?? null;
    if (this._measurement) this._depthTested = this._measurement.info.depthTested;
    this._other = measurements.find((m) => m !== this._measurement) ?? null;
    this._target = this._findTarget();

    const bar = new Div(this.root, { class: "image-view-toolbar" });
    const label = (text: string, tooltip?: string): Span => new Span(bar, { text, class: "launch-label", tooltip });
    label("Pass");
    new Select(bar, {
      options: passes.map((k) => this.host.passLabelOf(k)),
      index: Math.max(0, passes.findIndex((k) => sameKey(k, this._key))),
      onChange: (_v: string, index: number) => this.show(passes[index], this._depthTested),
    });
    label("Count", "Which of the pass's two measurements to show");
    new Select(bar, {
      options: ["Fragments passing depth and stencil", "Every rasterized fragment"],
      index: this._depthTested ? 0 : 1,
      onChange: (_v: string, index: number) => this.show(this._key, index === 0),
    });
    const overlay = new Checkbox(bar, { label: "Over render target", checked: this._overlay && !!this._target,
      tooltip: this._target ? "Draw the heat colours over the pass's first colour target" : "The pass has no colour target of this size read back" });
    overlay.input.disabled = !this._target;
    overlay.input.onchange = () => {
      this._overlay = overlay.checked;
      this._draw();
    };
    label("Heat %", "How much of the heat colour covers the render target");
    new NumberInput(bar, { value: this._opacity, step: 5, min: 0, max: 100, precision: 0, onChange: (v: string) => {
      const n = parseFloat(v);
      if (Number.isFinite(n)) {
        this._opacity = Math.min(100, Math.max(0, n));
        if (this._overlay) this._draw();
      }
    } });
    label("Zoom %", "Zoom level (0 = fit), Ctrl + mouse wheel");
    const zoom = new NumberInput(bar, { value: this._zoom, step: 25, min: 0, precision: 0, onChange: (v: string) => {
      const z = parseFloat(v);
      if (Number.isFinite(z)) {
        this._zoom = Math.max(0, z);
        this._applyZoom();
      }
    } });
    this._zoomInput = zoom;
    new Button(bar, { label: "Go to Pass", class: "btn btn-sm", tooltip: "Select the pass's first command in the capture's tab", callback: () => this.host.selectPass(this._key) });
    new Button(bar, { html: ICON_COPY, class: "btn btn-sm btn-icon", tooltip: "Copy the displayed image as PNG", callback: () => void this._copy() });

    const o = this._measurement;
    const info = new Div(this.root, { class: "overdraw-view-info" });
    if (o) {
      new Div(info, { text: overdrawSummary(o.info) });
      if (this._other) new Div(info, { text: overdrawSummary(this._other.info), class: "text-muted" });
      const histogram = overdrawHistogramText(o.info);
      if (histogram) new Div(info, { text: `Pixels by count: ${histogram}`, class: "text-muted" });
      if (o.info.capturedFragments !== undefined) {
        new Div(info, { text: `The capture's pipeline statistics measured ${o.info.capturedFragments.toLocaleString()} fragment shader invocations for the pass.`, class: "text-muted" });
      }
      if (o.info.note) new Div(info, { text: o.info.note, class: "text-muted" });
    }
    const legend = new Div(this.root, { class: "overdraw-legend" });
    new Span(legend, { text: "Fragments per pixel:", class: "text-muted" });
    for (const entry of OVERDRAW_LEGEND) {
      const item = new Span(legend, { class: "overdraw-legend-item" });
      const swatch = new Span(item, { class: "overdraw-swatch" });
      swatch.style.background = `rgb(${entry.color.join(",")})`;
      new Span(item, { text: entry.label });
    }
    this._pixelInfo = new Span(new Div(this.root, { class: "image-view-toolbar" }), { text: this._pinned || "Click a pixel to keep its counts here.", class: "image-view-pixel" });

    this._scroll = new Div(this.root, { class: "image-view-scroll" });
    if (!o || !isMeasured(o.info)) {
      new Div(this._scroll, { text: o?.info.note ? `Not measured: ${o.info.note}` : "The pass was not measured.", class: "text-muted" });
      return;
    }
    if (!o.data) {
      new Div(this._scroll, { text: o.info.size ? "Waiting for the per-pixel counts..." : "The per-pixel counts were not kept.", class: "text-muted" });
      return;
    }
    this._scroll.element.appendChild(this._canvas);
    this._resize.observe(this._scroll.element);
    this._draw();
  }

  private _zoomInput: NumberInput | null = null;

  private _findTarget(): OverdrawView["_target"] {
    const o = this._measurement;
    if (!o) return null;
    const tex = this.host.data.texturesForPass(this._key.frame, this._key.commandBuffer, this._key.passIndex)
      .find((t) => t.info.aspect === "color" && !t.info.error && t.data && t.info.width === o.info.width && t.info.height === o.info.height);
    if (!tex?.data) return null;
    if (this._target?.tex === tex) return this._target;
    return { tex, rgba: decodeImage(tex.info, tex.data), texels: decodeTexels(tex.info, tex.data) };
  }

  private _draw(): void {
    const o = this._measurement;
    if (!o) return;
    const heat = overdrawRgba(o);
    if (!heat) return;
    const { width, height } = o.info;
    const background = this._overlay ? this._target?.rgba ?? null : null;
    if (background) {
      const a = this._opacity / 100;
      for (let i = 0; i < width * height * 4; i += 4) {
        heat[i] = background[i] * (1 - a) + heat[i] * a;
        heat[i + 1] = background[i + 1] * (1 - a) + heat[i + 1] * a;
        heat[i + 2] = background[i + 2] * (1 - a) + heat[i + 2] * a;
      }
    }
    this._canvas.width = width;
    this._canvas.height = height;
    this._canvas.getContext("2d")!.putImageData(new ImageData(heat, width, height), 0, 0);
    this._applyZoom();
  }

  private _applyZoom(): void {
    const c = this._canvas;
    if (!c.width || !this._scroll) return;
    let zoom = this._zoom / 100;
    if (zoom <= 0) {
      const el = this._scroll.element;
      zoom = Math.min(Math.max(64, el.clientWidth - 16) / c.width, Math.max(64, el.clientHeight - 16) / c.height);
    }
    c.style.width = `${Math.max(1, Math.round(c.width * zoom))}px`;
    c.style.height = `${Math.max(1, Math.round(c.height * zoom))}px`;
  }

  // ---------------------------------------------------------------------------------------
  // Mouse: the tooltip, click to pin, Ctrl + wheel zoom

  private _pixelAt(e: MouseEvent): { x: number; y: number } | null {
    const c = this._canvas;
    if (!c.width || !c.clientWidth) return null;
    return {
      x: Math.min(c.width - 1, Math.max(0, Math.floor((e.offsetX * c.width) / c.clientWidth))),
      y: Math.min(c.height - 1, Math.max(0, Math.floor((e.offsetY * c.height) / c.clientHeight))),
    };
  }

  /** What is under a pixel: both counts, and the render target's texel. */
  private _pixelLines(x: number, y: number): string[] {
    const lines = [`X: ${x}  Y: ${y}`];
    const count = (m: CapturedOverdraw): string => {
      const n = overdrawCount(m, x, y);
      return `${m.info.depthTested ? "Passing depth and stencil" : "Every rasterized fragment"}: ${n} fragment${n === 1 ? "" : "s"}`;
    };
    if (this._measurement) lines.push(count(this._measurement));
    if (this._other?.data) lines.push(count(this._other));
    const target = this._target;
    if (target?.texels) {
      lines.push(`Target ${target.tex.info.attachment} ${fmt(target.tex.info.format).replace(/^VK_FORMAT_/, "")}`);
      for (const line of formatTexel(target.texels, x, y)) lines.push(`  ${line}`);
    }
    return lines;
  }

  private _setupCanvasEvents(): void {
    const c = this._canvas;
    const tip = this._tooltip;
    c.addEventListener("mouseleave", () => {
      tip.style.display = "none";
    });
    c.addEventListener("mousemove", (e: MouseEvent) => {
      const p = this._pixelAt(e);
      if (!p) return;
      tip.textContent = this._pixelLines(p.x, p.y).join("\n");
      tip.style.display = "block";
      // Inside the window: flipped to the left of or above the cursor near the edges.
      const margin = 12;
      const tw = tip.offsetWidth || 180;
      const th = tip.offsetHeight || 90;
      let left = e.clientX + margin;
      let top = e.clientY + margin;
      if (left + tw > window.innerWidth) left = Math.max(0, e.clientX - tw - margin);
      if (top + th > window.innerHeight) top = Math.max(0, e.clientY - th - margin);
      tip.style.left = `${left}px`;
      tip.style.top = `${top}px`;
      if (e.buttons === 1) this._pin(p.x, p.y);
    });
    c.addEventListener("mousedown", (e: MouseEvent) => {
      const p = this._pixelAt(e);
      if (p && e.button === 0) this._pin(p.x, p.y);
    });
    c.addEventListener("wheel", (e: WheelEvent) => {
      if (!e.ctrlKey) return;
      e.preventDefault();
      let zoom = this._zoom;
      if (zoom <= 0) zoom = Math.round((100 * c.clientWidth) / Math.max(1, c.width));
      zoom = Math.max(10, zoom + (e.deltaY < 0 ? 25 : -25));
      this._zoom = zoom;
      this._zoomInput?.setValue(zoom, true);
      this._applyZoom();
    }, { passive: false });
  }

  private _pin(x: number, y: number): void {
    this._pinned = `Pixel ${this._pixelLines(x, y).join("   ")}`;
    if (this._pixelInfo) this._pixelInfo.text = this._pinned;
  }

  private async _copy(): Promise<void> {
    if (!this._canvas.width) return;
    try {
      const blob = await new Promise<Blob | null>((resolve) => this._canvas.toBlob(resolve, "image/png"));
      if (!blob) throw new Error("PNG encoding failed");
      await navigator.clipboard.write([new ClipboardItem({ "image/png": blob })]);
      if (this._pixelInfo) this._pixelInfo.text = "Copied the image to the clipboard";
    } catch (e) {
      if (this._pixelInfo) this._pixelInfo.text = `Copy failed: ${e instanceof Error ? e.message : String(e)}`;
    }
  }
}
