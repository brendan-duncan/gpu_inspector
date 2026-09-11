// A pixel's history in a tab of its own: every pass start, clear and draw of the frame that touched
// the pixel, what each draw's fragments met, and the value and depth after each. A Vulkan capture
// is replayed for it (CaptureView.pixelHistory, vkinsp_replay --pixel); a Metal application follows
// the pixel while capturing the next frame (metal/src/pixel_history.mm). The pixel is picked in the
// overdraw tab or a render target's image viewer, and can be changed here.
import { Button } from "./widget/button.js";
import { Checkbox } from "./widget/checkbox.js";
import { Div } from "./widget/div.js";
import { NumberInput } from "./widget/number_input.js";
import { Span } from "./widget/span.js";
import {
  drawOutcome, eventSummary, sampleCountsText, texelCss, texelLines, touchesPixel,
  type PixelEvent, type PixelHistory, type PixelRequest,
} from "./pixel_history.js";

/** What the tab needs of the capture it belongs to. */
export interface PixelHistoryHost {
  /** An object's name as the Inspect panel shows it. */
  objectName(id: number): string;
  passLabelOf(key: { frame: number; commandBuffer: number; passIndex: number }): string;
  /** Shows the capture's tab with a command selected. */
  selectCommand(index: number): void;
  showObject(id: number): void;
  /** Follows another pixel (the result arrives through setRunning / setResult / setError). */
  run(request: PixelRequest): void;
  /** Metal: a pixel is followed by capturing the next frame with it, not by replaying this one. */
  captures?: boolean;
}

export class PixelHistoryView {
  readonly host: PixelHistoryHost;
  readonly root: Div;
  private _request: PixelRequest;
  private _history: PixelHistory | null = null;
  private _error = "";
  private _running = false;
  private _showAll = false;

  constructor(host: PixelHistoryHost, request: PixelRequest) {
    this.host = host;
    this._request = { mip: 0, layer: 0, ...request };
    this.root = new Div(null, { class: "pixel-history-view" });
    this._render();
  }

  get request(): PixelRequest {
    return this._request;
  }

  setRunning(request: PixelRequest): void {
    this._request = { mip: 0, layer: 0, ...request };
    this._running = true;
    this._error = "";
    this._render();
  }

  setResult(history: PixelHistory): void {
    this._running = false;
    this._history = history;
    this._error = "";
    this._render();
  }

  setError(message: string): void {
    this._running = false;
    this._history = null;
    this._error = message;
    this._render();
  }

  dispose(): void {
    // Nothing held outside the tab's own elements.
  }

  /** The UI tests' view of the tab (tools/ui_tests.py). */
  debugState(): Record<string, unknown> {
    const h = this._history;
    return {
      request: this._request, running: this._running, error: this._error || null,
      events: h?.events.length ?? 0,
      touched: h ? h.events.filter(touchesPixel).map(eventSummary) : [],
      notes: h?.notes ?? [],
    };
  }

  // ---------------------------------------------------------------------------------------

  private _render(): void {
    const r = this._request;
    this.root.html = "";
    const bar = new Div(this.root, { class: "image-view-toolbar" });
    const label = (text: string, tooltip?: string): Span => new Span(bar, { text, class: "launch-label", tooltip });
    label("Image");
    const image = new Span(bar, { text: this.host.objectName(r.image), class: "dependency_link", tooltip: "Show the image in the Inspect panel" });
    image.element.onclick = () => this.host.showObject(r.image);
    // The pixel to follow next, edited here and run with the button (or Enter in a field).
    const next = { x: r.x, y: r.y, mip: r.mip ?? 0, layer: r.layer ?? 0 };
    const field = (name: string, key: keyof typeof next, tooltip: string): void => {
      label(name, tooltip);
      const input = new NumberInput(bar, { value: next[key], step: 1, min: 0, precision: 0, onChange: (v: string) => {
        const n = parseInt(v, 10);
        if (Number.isFinite(n)) next[key] = Math.max(0, n);
      } });
      input.element.addEventListener("keydown", (e: KeyboardEvent) => {
        if (e.key === "Enter") this.host.run({ image: r.image, ...next });
      });
    };
    field("X", "x", "The pixel's column, at the mip level");
    field("Y", "y", "The pixel's row, at the mip level");
    field("Mip", "mip", "The image's mip level the pass renders to");
    field("Layer", "layer", "The image's array layer");
    const captures = this.host.captures === true;
    const run = new Button(bar, { label: this._running ? (captures ? "Capturing..." : "Replaying...") : "Follow Pixel", class: "btn btn-sm", disabled: this._running,
      tooltip: captures ? "Capture the application's next frame following this pixel" : "Replay the capture following this pixel",
      callback: () => this.host.run({ image: r.image, ...next }) });
    run.disabled = this._running;
    const all = new Checkbox(bar, { label: "Draws that miss the pixel", checked: this._showAll,
      tooltip: "Also list the draws of these passes whose primitives do not reach the pixel, or whose scissor leaves it out" });
    all.input.onchange = () => {
      this._showAll = all.checked;
      this._render();
    };

    const status = new Div(this.root, { class: "pixel-history-status" });
    if (this._running) {
      status.text = captures
        ? `Capturing the application's next frame, following pixel (${r.x}, ${r.y}) of mip ${r.mip ?? 0}, layer ${r.layer ?? 0}...`
        : `Replaying the capture on this machine's GPU, following pixel (${r.x}, ${r.y}) of mip ${r.mip ?? 0}, layer ${r.layer ?? 0}...`;
      return;
    }
    if (this._error) {
      status.classList.add("pixel-history-error");
      status.text = `The pixel could not be followed: ${this._error}`;
      return;
    }
    const h = this._history;
    if (!h) return;
    status.text = `Pixel (${h.x}, ${h.y}), mip ${h.mip}, layer ${h.layer}`
      + (h.pixelFormat ? `  ·  ${h.pixelFormat.replace(/^VK_FORMAT_/, "")}` : "")
      + (h.depthFormat ? `, depth ${h.depthFormat.replace(/^VK_FORMAT_/, "")}` : "")
      + (h.image !== h.requestedImage ? `  ·  in the frame's drawable, ${this.host.objectName(h.image)}` : "")
      + (h.device ? `  ·  ${captures ? "measured" : "replayed"} on ${h.device}` : "");

    const list = new Div(this.root, { class: "pixel-history-list" });
    let hidden = 0;
    for (const e of h.events) {
      if (!this._showAll && !touchesPixel(e)) {
        hidden++;
        continue;
      }
      this._renderEvent(list, h, e);
    }
    if (!h.events.length) {
      new Div(list, { text: `No ${captures ? "captured" : "replayed"} render pass renders to this image at this mip and layer.`, class: "text-muted", style: "padding: 6px 0;" });
    }
    if (hidden) {
      new Div(list, { text: `${hidden} draw${hidden === 1 ? "" : "s"} in these passes do${hidden === 1 ? "es" : ""} not reach the pixel.`, class: "text-muted font-sm", style: "padding: 6px 0;" });
    }
    for (const note of h.notes) new Div(list, { text: `Note: ${note}`, class: "text-muted font-sm", style: "padding: 2px 0;" });
    if (h.problems.length) {
      new Div(list, {
        text: `${h.problems.length}${h.problems.length >= 100 ? "+" : ""} parts of the capture could not be replayed; passes that depend on them are missing or may differ from the frame.`,
        class: "text-muted font-sm", style: "padding: 2px 0;", tooltip: h.problems.slice(0, 20).join("\n"),
      });
    }
  }

  private _renderEvent(list: Div, h: PixelHistory, e: PixelEvent): void {
    const outcome = e.kind === "draw" ? drawOutcome(e) : e.kind;
    const row = new Div(list, { class: `pixel-event outcome-${outcome}` });
    const swatch = new Span(row, { class: "pixel-swatch", tooltip: "The pixel after the event" });
    const color = texelCss(h.pixelFormat, e.value);
    if (color) new Span(swatch, { class: "pixel-swatch-fill" }).style.background = color;

    const main = new Div(row, { class: "pixel-event-main" });
    const head = new Div(main, { class: "pixel-event-head" });
    const link = new Span(head, { text: `[${e.command}]`, class: "dependency_link", tooltip: "Select the command in the capture" });
    link.element.onclick = () => this.host.selectCommand(e.command);
    new Span(head, { text: eventSummary(e), class: "pixel-event-summary", tooltip: sampleCountsText(e) || undefined });

    const where = new Div(main, { class: "pixel-event-where text-muted font-sm" });
    new Span(where, { text: this.host.passLabelOf(e) });
    if (e.pipeline) {
      new Span(where, { text: "·  pipeline" });
      const pipeline = new Span(where, { text: this.host.objectName(e.pipeline), class: "dependency_link" });
      pipeline.element.onclick = () => this.host.showObject(e.pipeline);
    }
    const values = [...texelLines(h.pixelFormat, e.value), ...texelLines(h.depthFormat, e.depth, true)];
    if (values.length) new Div(main, { text: values.join("   "), class: "pixel-event-values font-sm" });
  }
}
