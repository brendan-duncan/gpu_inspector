// Live contents of a VkImage / VkImageView in the Inspect panel: mip / layer selection, channel
// and exposure controls, auto range, zoom, a texel tooltip under the mouse, and a canvas showing
// the subresource read back from the running application (RequestImage -> ImageData). Follows
// WebGPU Inspector's TextureViewer (devtools/texture_viewer.js).
import { Button } from "./widget/button.js";
import { Checkbox } from "./widget/checkbox.js";
import { Div } from "./widget/div.js";
import { NumberInput } from "./widget/number_input.js";
import { Select } from "./widget/select.js";
import { Span } from "./widget/span.js";
import type { Widget } from "./widget/widget.js";
import {
  decodeTexels, displayTexels, formatFloat, formatTexel, isFormatSupported, sliceBytes,
  type ChannelMode, type DisplaySettings, type TexelData,
} from "./vulkan/texture_decode.js";
import { isObject, num, refId, type VulkanObject } from "./vulkan/vulkan_object.js";
import type { SessionContext } from "./session_panel.js";
import type { CaptureTextureInfo, ImageDataMessage } from "../shared/protocol.js";

/** Pixels read back during a capture (a render target), shown instead of the live image. */
export interface CapturedImageSource {
  info: CaptureTextureInfo;
  data: Uint8Array;
}

const CHANNEL_MODES: [string, ChannelMode][] = [["RGB", "rgb"], ["Red", "r"], ["Green", "g"], ["Blue", "b"], ["Alpha", "a"], ["Luminance", "luminance"]];

// Toolbar icons (inline SVG, drawn in the button's text color).
const ICON_REFRESH = '<svg viewBox="0 0 16 16" aria-label="Refresh"><path d="M13.2 9.2A5.3 5.3 0 1 1 12 4.3" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/><path d="M13.6 1.8v3.6h-3.6" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/></svg>';
const ICON_COPY = '<svg viewBox="0 0 16 16" aria-label="Copy"><rect x="5.5" y="5.5" width="8" height="8" rx="1" fill="none" stroke="currentColor" stroke-width="1.5"/><path d="M10.5 3.5v-1a1 1 0 0 0-1-1h-6a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h1" fill="none" stroke="currentColor" stroke-width="1.5"/></svg>';

/** Display settings are remembered per image across selections, like WebGPU Inspector does. */
const displayByImage = new Map<number, DisplaySettings & { zoom: number }>();

/** One tooltip element shared by every image view. */
let tooltip: HTMLPreElement | null = null;
function getTooltip(): HTMLPreElement {
  if (!tooltip) {
    tooltip = document.createElement("pre");
    tooltip.className = "image-tooltip";
    tooltip.style.display = "none";
    document.body.appendChild(tooltip);
  }
  return tooltip;
}

export class ImageView {
  readonly session: SessionContext;
  readonly object: VulkanObject | null;
  /** The VkImage whose contents are shown (the view's image for a VkImageView). */
  readonly imageId: number;
  /** Captured pixels (all layers back to back) rather than live read-backs. */
  captured: CapturedImageSource | null;
  /** The captured source was substituted for a live read-back (no connection). */
  private _fromCapture = false;

  private _is3D = false;
  private _mipCount = 1;
  private _layerCount = 1;   // array layers, or depth slices for 3D images
  private _baseMip = 0;
  private _baseLayer = 0;
  private _mip = 0;
  private _layer = 0;
  private _display: DisplaySettings & { zoom: number };
  private _data: ImageDataMessage | null = null;
  private _texels: TexelData | null = null;
  private _pinned = "";

  private _zoomInput!: NumberInput;
  private _autoRangeCheck!: Checkbox;
  private _smoothCheck!: Checkbox;
  private _status!: Span;
  private _pixelInfo!: Span;
  private _scroll!: Div;
  private _canvas: HTMLCanvasElement;

  constructor(parent: Widget, session: SessionContext, object: VulkanObject | null, captured: CapturedImageSource | null = null) {
    this.session = session;
    this.object = object;
    this.captured = captured;
    const db = session.database;

    let image: VulkanObject | null = object;
    let isDepth = false;
    if (captured) {
      // Captured pixels: the mips and layers the capture read back, no live request.
      const info = captured.info;
      this.imageId = info.id;
      this._layerCount = Math.max(1, info.layers || 1);
      this._baseMip = info.mip;
      this._mipCount = info.mip + Math.max(1, info.mips ?? 1);
      isDepth = info.aspect === "depth";
    } else {
      // Resolve the image and the subresource range this object covers.
      if (object?.type === "VkImageView") {
        image = db.getObject(refId(object.descriptor?.image));
        const range = isObject(object.descriptor?.subresourceRange) ? object.descriptor.subresourceRange : null;
        this._baseMip = num(range?.baseMipLevel);
        this._baseLayer = num(range?.baseArrayLayer);
      }
      this.imageId = image?.id ?? 0;
      const d = image?.descriptor;
      if (image?.cmd === "vkGetSwapchainImagesKHR") {
        const sd = db.getObject(image.parentId)?.descriptor;
        this._layerCount = Math.max(1, num(sd?.imageArrayLayers) || 1);
      } else if (d) {
        this._is3D = d.imageType === "VK_IMAGE_TYPE_3D";
        this._mipCount = Math.max(1, num(d.mipLevels) || 1);
        const e = isObject(d.extent) ? d.extent : null;
        this._layerCount = this._is3D ? Math.max(1, num(e?.depth) || 1) : Math.max(1, num(d.arrayLayers) || 1);
        isDepth = /_D\d+_|_D\d+$|_S8_UINT/.test(String(d.format ?? ""));
      }
      if (object?.type === "VkImageView") {
        const range = isObject(object.descriptor?.subresourceRange) ? object.descriptor.subresourceRange : null;
        const levels = num(range?.levelCount);
        const layers = num(range?.layerCount);
        // VK_REMAINING_* is serialized as a large number; keep the image's count in that case.
        if (levels > 0 && levels < 1024) this._mipCount = Math.min(this._mipCount, this._baseMip + levels);
        if (!this._is3D && layers > 0 && layers < 65536) this._layerCount = Math.min(this._layerCount, this._baseLayer + layers);
      }
    }
    // Without an application to read from (a capture file, a stopped target), show what the
    // most recent capture read back of this image, if anything.
    if (!captured && !session.connected && this.imageId) {
      const t = session.capturedImage(this.imageId);
      if (t?.data) {
        captured = { info: t.info, data: t.data };
        this.captured = captured;
        this._is3D = false;
        this._baseMip = t.info.mip;
        this._mipCount = t.info.mip + Math.max(1, t.info.mips ?? 1);
        this._baseLayer = 0;
        this._layerCount = Math.max(1, t.info.layers || 1);
        this._fromCapture = true;
      }
    }
    this._mip = this._baseMip;
    this._layer = this._baseLayer;

    let display = displayByImage.get(this.imageId);
    if (!display) {
      display = { channels: "rgb", exposure: 1, autoRange: isDepth, zoom: 100 };
      displayByImage.set(this.imageId, display);
    }
    this._display = display;

    this._canvas = document.createElement("canvas");
    this._build(parent, image);
    if (captured) {
      this._showCapturedMip();
    } else if (image) {
      this.request();
    } else {
      this._status.text = "image not available";
    }
  }

  /**
   * Captured pixels hold the read-back mips back to back, each with all its layers (or 3D
   * slices): the selected mip is cut out and decoded as if it had arrived from the layer.
   */
  private _showCapturedMip(): void {
    const captured = this.captured;
    if (!captured) return;
    const info = captured.info;
    const mip = Math.min(Math.max(this._mip, info.mip), info.mip + Math.max(1, info.mips ?? 1) - 1);
    const dims = (m: number): { width: number; height: number; depth: number } => ({
      width: Math.max(1, info.width >> (m - info.mip)), height: Math.max(1, info.height >> (m - info.mip)), depth: Math.max(1, (info.depth || 1) >> (m - info.mip)),
    });
    const layers = Math.max(1, info.layers || 1);
    const bytesOf = (m: number): number => {
      const d = dims(m);
      return sliceBytes({ format: info.format, aspect: info.aspect, width: d.width, height: d.height }) * Math.max(d.depth, layers);
    };
    let offset = 0;
    for (let m = info.mip; m < mip; m++) offset += bytesOf(m);
    const d = dims(mip);
    const size = bytesOf(mip);
    const data = captured.data ? captured.data.subarray(offset, offset + size) : undefined;
    this._data = {
      action: "ImageData", id: info.id, mip, layer: 0, depth: d.depth, layers, size,
      format: info.format, aspect: info.aspect, width: d.width, height: d.height, __binary: data,
    };
    this._decode();
  }

  /** Layers (or 3D slices) arrive together and are picked locally rather than requested. */
  private get _slices(): boolean {
    return this._is3D || this.captured !== null;
  }

  private _sliceCount(msg: ImageDataMessage): number {
    return Math.max(1, this._is3D ? msg.depth : this.captured ? msg.layers : 1);
  }

  private _build(parent: Widget, image: VulkanObject | null): void {
    const bar = new Div(parent, { class: "image-view-toolbar" });
    const label = (text: string, tip?: string): Span => new Span(bar, { text, class: "launch-label", tooltip: tip });

    if (this._mipCount > 1) {
      label("Mip");
      const options: string[] = [];
      for (let i = this._baseMip; i < this._mipCount; i++) options.push(this._mipLabel(image, i));
      new Select(bar, { options, onChange: (_v: string, index: number) => {
        this._mip = this._baseMip + index;
        if (this.captured) this._showCapturedMip(); else this.request();
      } });
    }
    if (this._layerCount > 1) {
      label(this._is3D ? "Slice" : "Layer");
      const options: string[] = [];
      for (let i = this._is3D ? 0 : this._baseLayer; i < this._layerCount; i++) options.push(String(i));
      new Select(bar, { options, onChange: (_v: string, index: number) => {
        if (this._slices) {
          this._layer = index;
          this._decode();  // all slices of a 3D mip (or of a captured target) arrive together
        } else {
          this._layer = this._baseLayer + index;
          this.request();
        }
      } });
    }

    label("Channels", "Which channels to display");
    new Select(bar, {
      options: CHANNEL_MODES.map((m) => m[0]),
      index: Math.max(0, CHANNEL_MODES.findIndex((m) => m[1] === this._display.channels)),
      onChange: (_v: string, index: number) => {
        this._display.channels = CHANNEL_MODES[index][1];
        this._draw();
      },
    });

    label("Exposure", "Multiplier applied to the displayed values");
    new NumberInput(bar, { value: this._display.exposure, step: 0.05, min: 0, precision: 2, onChange: (v: string) => {
      const e = parseFloat(v);
      if (Number.isFinite(e)) {
        this._display.exposure = Math.max(0, e);
        this._draw();
      }
    } });

    this._autoRangeCheck = new Checkbox(bar, { label: "Auto Range", checked: this._display.autoRange,
      tooltip: "Stretch the values so the smallest shows as black and the largest as white" });
    this._autoRangeCheck.input.onchange = () => {
      this._display.autoRange = this._autoRangeCheck.checked;
      this._draw();
    };

    label("Zoom %", "Zoom level of the image (0 = fit), Ctrl + mouse wheel");
    this._zoomInput = new NumberInput(bar, { value: this._display.zoom, step: 10, min: 0, precision: 0, onChange: (v: string) => {
      const z = parseFloat(v);
      if (Number.isFinite(z)) {
        this._display.zoom = Math.max(0, z);
        this._applyZoom();
      }
    } });
    this._smoothCheck = new Checkbox(bar, { label: "Smooth", checked: false, tooltip: "Filter when scaling instead of showing texels" });
    this._smoothCheck.input.onchange = () => this._canvas.classList.toggle("smooth", this._smoothCheck.checked);
    if (!this.captured) new Button(bar, { html: ICON_REFRESH, class: "btn btn-sm btn-icon", tooltip: "Refresh: read the image again from the application", callback: () => this.request() });
    new Button(bar, { html: ICON_COPY, class: "btn btn-sm btn-icon", tooltip: "Copy the displayed image as PNG", callback: () => void this._copy() });

    const info = new Div(parent, { class: "image-view-toolbar" });
    this._status = new Span(info, { text: "", class: "image-view-status" });
    this._pixelInfo = new Span(info, { text: "", class: "image-view-pixel" });

    this._scroll = new Div(parent, { class: "image-view-scroll" });
    this._canvas.className = "image-view-canvas";
    this._canvas.width = 0;
    this._canvas.height = 0;
    this._scroll.element.appendChild(this._canvas);
    this._setupCanvasEvents();
  }

  private _mipLabel(image: VulkanObject | null, mip: number): string {
    const d = image?.descriptor;
    const e = d && isObject(d.extent) ? d.extent : null;
    if (!e) return String(mip);
    const w = Math.max(1, num(e.width) >> mip);
    const h = Math.max(1, num(e.height) >> mip);
    return `${mip} (${w}x${h})`;
  }

  // ---------------------------------------------------------------------------------------
  // Data

  /** Asks the layer for the selected subresource. */
  request(): void {
    if (!this.imageId || this.captured) return;
    if (!this.session.connected) {
      this._status.text = "not connected";
      return;
    }
    this._status.text = "reading...";
    void this.session.send({ action: "RequestImage", id: this.imageId, mip: this._mip, layer: this._is3D ? 0 : this._layer });
  }

  handleImageData(msg: ImageDataMessage): void {
    if (msg.id !== this.imageId) return;
    if (msg.error) {
      this._data = null;
      this._texels = null;
      this._status.text = msg.error;
      this._canvas.width = 0;
      this._canvas.height = 0;
      return;
    }
    this._data = msg;
    this._decode();
  }

  /** Decodes the current slice to texel values, then draws it. */
  private _decode(): void {
    const msg = this._data;
    if (!msg || !msg.__binary) return;
    if (!isFormatSupported(msg)) {
      this._texels = null;
      this._status.text = `${msg.format}: display of this format is not supported yet`;
      this._canvas.width = 0;
      this._canvas.height = 0;
      return;
    }
    const slice = this._slices ? Math.min(this._layer, this._sliceCount(msg) - 1) : 0;
    this._texels = decodeTexels(msg, msg.__binary, slice);
    if (!this._texels) {
      this._status.text = "decode failed";
      return;
    }
    this._pinned = "";
    this._draw();
  }

  /** Applies the display settings to the decoded texels and updates the canvas and status. */
  private _draw(): void {
    const tex = this._texels;
    const msg = this._data;
    if (!tex || !msg) return;
    const rgba = displayTexels(tex, this._display);
    this._canvas.width = tex.width;
    this._canvas.height = tex.height;
    this._canvas.getContext("2d")!.putImageData(new ImageData(rgba, tex.width, tex.height), 0, 0);

    const slice = this._slices ? Math.min(this._layer, this._sliceCount(msg) - 1) : 0;
    const where = `${tex.width}x${tex.height} mip ${msg.mip}${this._is3D ? ` slice ${slice}` : this._layerCount > 1 ? ` layer ${this.captured ? slice : msg.layer}` : ""}`;
    const fmt = (v: number): string => tex.integer ? String(v) : formatFloat(v);
    const range = tex.channels === 1
      ? `  Min ${fmt(tex.min[0])}  Max ${fmt(tex.max[0])}`
      : `  Min ${tex.min.slice(0, tex.channels).map(fmt).join(", ")}  Max ${tex.max.slice(0, tex.channels).map(fmt).join(", ")}`;
    this._status.text = `${msg.format.replace(/^VK_FORMAT_/, "")} ${where}${range}${this._fromCapture ? "  (from the capture)" : ""}`;
    this._pixelInfo.text = this._pinned;
    this._applyZoom();
  }

  private _applyZoom(): void {
    const c = this._canvas;
    if (!c.width) return;
    let zoom = this._display.zoom / 100;
    if (zoom <= 0) {
      // Fit: scale (up or down) so the image fills the available width or ~60% of the window
      // height, whichever is reached first. Small textures become visible instead of a dot.
      const availW = Math.max(64, this._scroll.element.clientWidth - 16);
      const availH = Math.max(64, window.innerHeight * 0.6);
      zoom = Math.min(availW / c.width, availH / c.height);
    }
    c.style.width = `${Math.max(1, Math.round(c.width * zoom))}px`;
    c.style.height = `${Math.max(1, Math.round(c.height * zoom))}px`;
  }

  // ---------------------------------------------------------------------------------------
  // Mouse: texel tooltip, click to pin, Ctrl + wheel zoom

  private _texelAt(e: MouseEvent): { x: number; y: number } | null {
    const c = this._canvas;
    if (!c.width || !c.clientWidth) return null;
    const x = Math.min(c.width - 1, Math.max(0, Math.floor(e.offsetX * c.width / c.clientWidth)));
    const y = Math.min(c.height - 1, Math.max(0, Math.floor(e.offsetY * c.height / c.clientHeight)));
    return { x, y };
  }

  private _texelText(x: number, y: number, separator: string): string {
    const tex = this._texels;
    if (!tex) return "";
    return [`X: ${x}  Y: ${y}`, ...formatTexel(tex, x, y)].join(separator);
  }

  private _setupCanvasEvents(): void {
    const c = this._canvas;
    c.addEventListener("mouseenter", () => {
      if (this._texels) getTooltip().style.display = "block";
    });
    c.addEventListener("mouseleave", () => {
      getTooltip().style.display = "none";
    });
    c.addEventListener("mousemove", (e: MouseEvent) => {
      const t = this._texelAt(e);
      if (!t || !this._texels) return;
      const tip = getTooltip();
      tip.textContent = this._texelText(t.x, t.y, "\n");
      tip.style.display = "block";
      // Keep the tooltip inside the window: flip to the left / above the cursor near the edges.
      const margin = 12;
      const tw = tip.offsetWidth || 140;
      const th = tip.offsetHeight || 90;
      let left = e.clientX + margin;
      let top = e.clientY + margin;
      if (left + tw > window.innerWidth) left = Math.max(0, e.clientX - tw - margin);
      if (top + th > window.innerHeight) top = Math.max(0, e.clientY - th - margin);
      tip.style.left = `${left}px`;
      tip.style.top = `${top}px`;
      if (e.buttons === 1) this._pin(t.x, t.y);
    });
    c.addEventListener("mousedown", (e: MouseEvent) => {
      const t = this._texelAt(e);
      if (t && e.button === 0) this._pin(t.x, t.y);
    });
    c.addEventListener("wheel", (e: WheelEvent) => {
      if (!e.ctrlKey) return;
      e.preventDefault();
      let zoom = this._display.zoom;
      if (zoom <= 0) zoom = Math.round(100 * this._canvas.clientWidth / Math.max(1, this._canvas.width));
      zoom = Math.max(10, zoom + (e.deltaY < 0 ? 10 : -10));
      this._display.zoom = zoom;
      this._zoomInput.setValue(zoom, true);
      this._applyZoom();
    }, { passive: false });
  }

  /** Shows the texel under a click in the info line, where it stays until the next click. */
  private _pin(x: number, y: number): void {
    this._pinned = `Pixel ${this._texelText(x, y, "  ")}`;
    this._pixelInfo.text = this._pinned;
  }

  private async _copy(): Promise<void> {
    if (!this._canvas.width) return;
    try {
      const blob = await new Promise<Blob | null>((resolve) => this._canvas.toBlob(resolve, "image/png"));
      if (!blob) throw new Error("PNG encoding failed");
      await navigator.clipboard.write([new ClipboardItem({ "image/png": blob })]);
      this._pixelInfo.text = "Copied image to the clipboard";
    } catch (e) {
      this._pixelInfo.text = `Copy failed: ${e instanceof Error ? e.message : String(e)}`;
    }
  }
}
