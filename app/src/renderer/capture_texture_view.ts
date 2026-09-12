// A capture's render target in a tab of its own: the image at any zoom, the pass's overdraw heatmap
// over it when asked, and the history of whichever pixel you click beside it.
//
// Follows WebGPU Inspector's capture texture viewer (devtools/capture_texture_viewer.js): one view
// per attachment, overdraw a checkbox over the image rather than a view of its own, and the pixel
// history filled by clicking a pixel rather than by typing coordinates. Where the measurements come
// from differs per API: a Metal capture carries its overdraw and the one pixel history it was taken
// with (metal/src/overdraw.h, metal/src/pixel_history.mm), a Vulkan capture is replayed for both
// (vkinsp_replay, docs/REPLAY.md).
import { Button } from "./widget/button.js";
import { Checkbox } from "./widget/checkbox.js";
import { Div } from "./widget/div.js";
import { NumberInput } from "./widget/number_input.js";
import { Select } from "./widget/select.js";
import { Span } from "./widget/span.js";
import type { CaptureData, CapturedOverdraw, CapturedTexture } from "./capture_data.js";
import { ImageView, type ImageOverlay } from "./image_view.js";
import {
  OVERDRAW_LEGEND, isMeasured, overdrawCount, overdrawHistogramText, overdrawRgba, overdrawSummary,
  type OverdrawPassKey,
} from "./overdraw.js";
import type { PixelHistory, PixelRequest } from "./pixel_history.js";
import { PixelHistoryView } from "./pixel_history_view.js";
import type { SessionContext } from "./session_panel.js";
import { fmt, type VulkanObject } from "./vulkan/vulkan_object.js";

/** What the view shows: one render target of one render pass. */
export interface CaptureTarget {
  key: OverdrawPassKey;
  texture: CapturedTexture;
}

/** What the view needs of the capture tab it belongs to. */
export interface CaptureTextureHost {
  readonly data: CaptureData;
  readonly session: SessionContext;
  /** The pass's label, as the command tree shows it. */
  passLabelOf(key: OverdrawPassKey): string;
  /** Shows the capture's tab with the pass's begin command selected. */
  selectPass(key: OverdrawPassKey): void;
  /** Shows the capture's tab with a command selected. */
  selectCommand(index: number): void;
  objectName(id: number): string;
  imageObject(id: number): VulkanObject | null;
  showObject(id: number): void;
  /** Runs a pixel's history; the answer comes back through setHistoryRunning / Result / Error. */
  followPixel(request: PixelRequest): void;
  /** Metal: whether the capture already followed this pixel while it was taken. */
  storedHistory(request: PixelRequest): boolean;
  /** Metal: captures the application's next frame following the pixel. */
  captureHistory(request: PixelRequest): void;
  /** Vulkan: replays the capture to measure its overdraw. False when it could not be measured. */
  measureOverdraw(): Promise<boolean>;
}

export interface CaptureTextureOptions {
  /** Open with the overdraw overlay on (from a pass's heatmap, or the Overdraw report). */
  overdraw?: boolean;
  depthTested?: boolean;
  /** Open following this pixel (a Metal capture's own pixel history, or --debug-view). */
  pixel?: PixelRequest;
}

const COUNT_LABELS = ["Fragments passing depth and stencil", "Every rasterized fragment"];

export class CaptureTextureView {
  readonly host: CaptureTextureHost;
  readonly root: Div;
  private _target: CaptureTarget;
  private _overdraw = false;
  private _depthTested = true;
  /** Percent of the heat colour over the render target. */
  private _opacity = 70;
  private _measuring = false;
  private _measureError = "";

  private _image: ImageView | null = null;
  private _history: PixelHistoryView | null = null;
  private _overdrawRow: Div | null = null;
  private _heat: { counts: Uint8Array; rgba: Uint8ClampedArray } | null = null;
  private _picked: PixelRequest | null = null;
  /** Percent of the width the image takes, dragged on the handle between the panes. */
  private _split = 60;
  /** Refits the image (zoom 0) when its pane changes width. */
  private _resize: ResizeObserver | null = null;

  private readonly _onOverdraw = (): void => {
    this._heat = null;
    this._renderOverdrawRow();
    this._image?.refreshOverlay();
  };
  private readonly _onTexture = (tex: CapturedTexture): void => {
    if (tex === this._target.texture) this._rebuild();
  };

  constructor(host: CaptureTextureHost, target: CaptureTarget, options: CaptureTextureOptions = {}) {
    this.host = host;
    this._target = target;
    this.root = new Div(null, { class: "capture-texture-view" });
    host.data.onOverdraw.addListener(this._onOverdraw);
    host.data.onTextureLoaded.addListener(this._onTexture);
    this._apply(options);
    this._rebuild();
  }

  /** The tab's label: the pass and the attachment. */
  get label(): string {
    const info = this._target.texture.info;
    return `Pass ${this._target.key.passIndex} ${info.aspect === "depth" ? "Depth" : `Target ${info.attachment}`}`;
  }

  get target(): CaptureTarget {
    return this._target;
  }

  /** Points the view at another render target, or at another pixel of this one. */
  show(target: CaptureTarget, options: CaptureTextureOptions = {}): void {
    const same = target.texture === this._target.texture;
    this._target = target;
    if (!same) {
      this._heat = null;
      this._picked = null;
    }
    this._apply(options);
    this._rebuild();
  }

  setHistoryRunning(request: PixelRequest): void {
    this._history?.setRunning(request);
  }

  setHistoryResult(history: PixelHistory): void {
    this._history?.setResult(history);
  }

  setHistoryError(message: string): void {
    this._history?.setError(message);
  }

  dispose(): void {
    this.host.data.onOverdraw.disconnect(this._onOverdraw);
    this.host.data.onTextureLoaded.disconnect(this._onTexture);
    this._resize?.disconnect();
    this._resize = null;
  }

  /** The UI tests' view of the tab (tools/ui_tests.py). */
  debugState(): Record<string, unknown> {
    const m = this._measurement();
    return {
      pass: this._target.key, attachment: this._target.texture.info.attachment, image: this._target.texture.info.id,
      overdraw: this._overdraw, depthTested: this._depthTested, measured: m ? isMeasured(m.info) : false, counts: !!m?.data,
      picked: this._picked ? { x: this._picked.x, y: this._picked.y } : null,
      history: this._history?.debugState() ?? null,
    };
  }

  // ---------------------------------------------------------------------------------------

  private _apply(options: CaptureTextureOptions): void {
    if (options.overdraw !== undefined) this._overdraw = options.overdraw;
    if (options.depthTested !== undefined) this._depthTested = options.depthTested;
    if (options.pixel) this._picked = { mip: 0, layer: 0, ...options.pixel };
  }

  /** The pass's measurement the overlay draws, if the capture has it. */
  private _measurement(): CapturedOverdraw | null {
    const k = this._target.key;
    const all = this.host.data.overdrawForPass(k.frame, k.commandBuffer, k.passIndex);
    return all.find((m) => m.info.depthTested === this._depthTested) ?? all[0] ?? null;
  }

  private _rebuild(): void {
    const tex = this._target.texture;
    const info = tex.info;
    this.root.html = "";
    this._image = null;
    this._overdrawRow = null;

    new Div(this.root, { class: "capture-texture-head", text: `${this.host.passLabelOf(this._target.key)} — `
      + `${info.aspect === "depth" ? "depth attachment" : `colour attachment ${info.attachment}`}${info.resolve ? " (resolve)" : ""}: `
      + `${this.host.objectName(info.id)} ${fmt(info.format).replace(/^VK_FORMAT_/, "")} ${info.width}x${info.height}` });

    const split = new Div(this.root, { class: "capture-texture-split" });
    const left = new Div(split, { class: "capture-texture-image" });
    const handle = new Div(split, { class: "capture-texture-handle", tooltip: "Drag to give the image or the history more room" });
    const right = new Div(split, { class: "capture-texture-history" });
    left.element.style.flex = `0 0 ${this._split}%`;
    this._dragSplit(handle, split, left);

    // The history pane, which the image's clicks fill.
    const metal = this.host.data.api === "metal";
    this._history = new PixelHistoryView({
      objectName: (id) => this.host.objectName(id),
      passLabelOf: (k) => this.host.passLabelOf(k),
      selectCommand: (index) => this.host.selectCommand(index),
      showObject: (id) => this.host.showObject(id),
      run: (r) => this._follow(r),
      captures: metal,
      compact: true,
    }, this._picked ?? { image: info.id, x: 0, y: 0, mip: info.mip, layer: 0 });
    right.element.appendChild(this._history.root.element);

    if (!tex.data) {
      new Div(left, { text: info.error || "The render target's pixels were not read back.", class: "text-muted", style: "padding: 12px;" });
      this._history.setPrompt("A pixel's history needs the render target's pixels, which this capture did not read back.");
      return;
    }

    this._image = new ImageView(left, this.host.session, this.host.imageObject(info.id), { info, data: tex.data }, {
      fit: true,
      onPick: (x, y, mip, layer) => this._follow({ image: info.id, x, y, mip, layer }),
      overlay: this._overlay(),
      extras: {
        toolbar: (bar) => this._buildToolbar(bar),
        row: (parent) => {
          this._overdrawRow = new Div(parent, { class: "overdraw-bar" });
          this._renderOverdrawRow();
        },
      },
    });

    this._resize?.disconnect();
    this._resize = new ResizeObserver(() => this._image?.refit());
    this._resize.observe(left.element);

    if (this._picked) {
      const pixel = this._picked;
      this._image.setPicked({ x: pixel.x, y: pixel.y });
      // After the constructor returns: the tab the panel runs the history into is the one it is
      // building here, so the first follow waits for it to exist.
      queueMicrotask(() => { if (this._picked === pixel) this._follow(pixel); });
    } else {
      this._history.setPrompt("Click a pixel in the image: every clear and draw of the frame that touched it shows up here.");
    }
  }

  /** The divider between the image and the history: dragging it moves the split. */
  private _dragSplit(handle: Div, split: Div, left: Div): void {
    handle.element.onmousedown = (e: MouseEvent) => {
      e.preventDefault();
      const rect = split.element.getBoundingClientRect();
      const move = (m: MouseEvent): void => {
        this._split = Math.min(85, Math.max(15, ((m.clientX - rect.left) / Math.max(1, rect.width)) * 100));
        left.element.style.flex = `0 0 ${this._split}%`;
      };
      const up = (): void => {
        window.removeEventListener("mousemove", move);
        window.removeEventListener("mouseup", up);
      };
      window.addEventListener("mousemove", move);
      window.addEventListener("mouseup", up);
    };
  }

  private _buildToolbar(bar: Div): void {
    const check = new Checkbox(bar, { label: "Overdraw", checked: this._overdraw,
      tooltip: "Draw the pass's overdraw over the image: how many fragments landed on each pixel, with the counts in the tooltip" });
    check.input.onchange = () => {
      this._overdraw = check.checked;
      this._renderOverdrawRow();
      this._image?.refreshOverlay();
      if (this._overdraw) void this._ensureMeasured();
    };
    new Button(bar, { label: "Go to Pass", class: "btn btn-sm", tooltip: "Select the pass's first command in the capture's tab",
      callback: () => this.host.selectPass(this._target.key) });
  }

  /** The heat over the image, and the counts under the pointer. */
  private _overlay(): ImageOverlay {
    return {
      rgba: (width, height) => {
        const m = this._overdraw ? this._measurement() : null;
        if (!m || !m.data || m.info.width !== width || m.info.height !== height) return null;
        if (this._heat?.counts !== m.data) {
          const rgba = overdrawRgba(m, true);
          this._heat = rgba ? { counts: m.data, rgba } : null;
        }
        return this._heat?.rgba ?? null;
      },
      opacity: () => this._opacity / 100,
      lines: (x, y) => {
        if (!this._overdraw) return [];
        const k = this._target.key;
        const lines: string[] = [];
        for (const m of this.host.data.overdrawForPass(k.frame, k.commandBuffer, k.passIndex)) {
          if (!m.data) continue;
          const n = overdrawCount(m, x, y);
          lines.push(`${m.info.depthTested ? "Passing depth and stencil" : "Rasterized"}: ${n} fragment${n === 1 ? "" : "s"}`);
        }
        return lines;
      },
    };
  }

  /** The row under the toolbar while the overlay is on: where the counts stand, and the legend. */
  private _renderOverdrawRow(): void {
    const row = this._overdrawRow;
    if (!row) return;
    row.html = "";
    if (!this._overdraw) {
      row.element.style.display = "none";
      return;
    }
    row.element.style.display = "flex";
    const note = (text: string): Div => new Div(row, { text, class: "text-muted" });
    if (this._measuring) {
      note("Replaying the capture on this machine's GPU to measure overdraw...");
      return;
    }
    const m = this._measurement();
    if (!m) {
      if (this.host.data.api === "metal") {
        note("This capture did not measure overdraw: capture again with Overdraw ticked.");
      } else {
        note(this._measureError || "A Vulkan capture's overdraw is measured by replaying it on this machine's GPU.");
        new Button(row, { label: "Measure Overdraw", class: "btn btn-sm", callback: () => void this._ensureMeasured() });
      }
      return;
    }
    if (!isMeasured(m.info)) {
      note(`Not measured: ${m.info.note ?? "the pass could not be measured"}`);
      return;
    }
    if (!m.data) {
      note(m.info.size ? "Waiting for the per-pixel counts..." : "The per-pixel counts were not kept for this pass.");
      return;
    }

    const k = this._target.key;
    const both = this.host.data.overdrawForPass(k.frame, k.commandBuffer, k.passIndex);
    if (both.length > 1) {
      new Select(row, {
        options: COUNT_LABELS,
        index: this._depthTested ? 0 : 1,
        onChange: (_v: string, index: number) => {
          this._depthTested = index === 0;
          this._heat = null;
          this._renderOverdrawRow();
          this._image?.refreshOverlay();
        },
      });
    } else {
      new Span(row, { text: COUNT_LABELS[m.info.depthTested ? 0 : 1], class: "text-muted" });
    }
    new Span(row, { text: "Heat %", class: "launch-label", tooltip: "How much of the heat colour covers the render target" });
    new NumberInput(row, { value: this._opacity, step: 10, min: 0, max: 100, precision: 0, onChange: (v: string) => {
      const n = parseFloat(v);
      if (Number.isFinite(n)) {
        this._opacity = Math.min(100, Math.max(0, n));
        this._image?.refreshOverlay();
      }
    } });
    const histogram = overdrawHistogramText(m.info);
    const summary = new Span(row, { text: overdrawSummary(m.info).replace(/^Fragments [^:]+: /, ""), class: "text-muted" });
    summary.tooltip = [overdrawSummary(m.info), histogram ? `Pixels by count: ${histogram}` : "",
      m.info.capturedFragments !== undefined ? `The capture's pipeline statistics measured ${m.info.capturedFragments.toLocaleString()} fragment shader invocations for the pass.` : "",
      m.info.note ?? ""].filter(Boolean).join("\n");

    const legend = new Span(row, { class: "overdraw-legend" });
    for (const entry of OVERDRAW_LEGEND) {
      const item = new Span(legend, { class: "overdraw-legend-item" });
      const swatch = new Span(item, { class: "overdraw-swatch" });
      swatch.style.background = `rgb(${entry.color.join(",")})`;
      new Span(item, { text: entry.label });
    }
  }

  /** Vulkan: measures the capture's overdraw when the overlay is switched on without it. */
  private async _ensureMeasured(): Promise<void> {
    if (this._measuring || this._measurement() || this.host.data.api === "metal") return;
    this._measuring = true;
    this._measureError = "";
    this._renderOverdrawRow();
    const ok = await this.host.measureOverdraw();
    this._measuring = false;
    if (!ok) this._measureError = "The capture could not be measured (the capture's status line says why).";
    this._renderOverdrawRow();
    this._image?.refreshOverlay();
  }

  /**
   * Follows a pixel: a Vulkan capture replays, a Metal capture answers for the pixel it was taken
   * with and otherwise offers to capture the next frame following this one.
   */
  private _follow(request: PixelRequest): void {
    const pixel: PixelRequest = { mip: 0, layer: 0, ...request };
    this._picked = pixel;
    if (this.host.data.api === "metal" && !this.host.storedHistory(pixel)) {
      this._history?.setPrompt(
        `This capture did not follow pixel (${pixel.x}, ${pixel.y}). A Metal application follows a pixel while it captures, `
        + "so another pixel means capturing the application's next frame.",
        { label: "Capture Next Frame", callback: () => this.host.captureHistory(pixel) });
      return;
    }
    this.host.followPixel(pixel);
  }
}
