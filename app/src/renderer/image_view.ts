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
import { isObject, num, refId, str, type VulkanObject } from "./vulkan/vulkan_object.js";
import type { SessionContext } from "./session_panel.js";
import type { CaptureTextureInfo, ImageDataMessage } from "../shared/protocol.js";

/** The one Metal texture type that makes the layer slider a depth slider. */
const MTL_TEXTURE_TYPE_3D = "MTLTextureType3D";

/** Pixels read back during a capture (a render target), shown instead of the live image. */
export interface CapturedImageSource {
  info: CaptureTextureInfo;
  data: Uint8Array;
}

/**
 * A colour layer drawn over the image: the overdraw heatmap of the pass, in the capture's texture
 * view (capture_texture_view.ts). Pixels the overlay leaves transparent keep the image's own colour.
 */
export interface ImageOverlay {
  /** The overlay for the subresource shown, the image's size; null for none. */
  rgba(width: number, height: number, mip: number, layer: number): Uint8ClampedArray | null;
  /** How much of the overlay's colour covers the image, 0 to 1. */
  opacity(): number;
  /** What the overlay adds to the tooltip under the pointer. */
  lines?(x: number, y: number): string[];
}

export interface ImageViewOptions {
  /** Follows a pixel of a captured render target through the frame, from the toolbar's button. */
  pixelHistory?: (x: number, y: number, mip: number, layer: number) => void;
  /** A pixel was clicked: the capture's texture view follows it straight away. */
  onPick?: (x: number, y: number, mip: number, layer: number) => void;
  overlay?: ImageOverlay;
  /** Widgets of the owner's own: in the toolbar, and in a row under it (the overdraw controls). */
  extras?: { toolbar?: (bar: Div) => void; row?: (parent: Widget) => void };
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

  /** Follows a pixel of a captured render target through the frame (pixel history); absent otherwise. */
  private _pixelHistory: ((x: number, y: number, mip: number, layer: number) => void) | null;
  private _onPick: ((x: number, y: number, mip: number, layer: number) => void) | null;
  private _overlay: ImageOverlay | null;
  private _extras: ImageViewOptions["extras"];
  private _pinnedTexel: { x: number; y: number } | null = null;
  private _historyButton: Button | null = null;
  private _zoomInput!: NumberInput;
  private _autoRangeCheck!: Checkbox;
  private _smoothCheck!: Checkbox;
  private _status!: Span;
  private _pixelInfo!: Span;
  private _scroll!: Div;
  private _canvas: HTMLCanvasElement;
  /** Positions the picked-pixel marker over the canvas. */
  private _holder: HTMLDivElement;
  private _marker: HTMLDivElement;

  constructor(parent: Widget, session: SessionContext, object: VulkanObject | null, captured: CapturedImageSource | null = null,
              options: ImageViewOptions = {}) {
    this.session = session;
    this.object = object;
    this.captured = captured;
    this._pixelHistory = options.pixelHistory ?? null;
    this._onPick = options.onPick ?? null;
    this._overlay = options.overlay ?? null;
    this._extras = options.extras;
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
      } else if (image?.type === "MTLTexture" && d) {
        // Metal's descriptor names the same things differently: mipmapLevelCount for mipLevels,
        // arrayLength for arrayLayers, and a numeric MTLTextureType where Vulkan has a string.
        // The types are disjoint, so branching on the object's own type is unambiguous.
        this._is3D = str(d.textureType) === MTL_TEXTURE_TYPE_3D;
        this._mipCount = Math.max(1, num(d.mipmapLevelCount) || 1);
        this._layerCount = this._is3D ? Math.max(1, num(d.depth) || 1) : Math.max(1, num(d.arrayLength) || 1);
        // The capture library only reads colour back so far; a depth texture answers with an
        // error rather than being mislabelled here.
        isDepth = false;
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
    this._holder = document.createElement("div");
    this._marker = document.createElement("div");
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
    if (this._pixelHistory) {
      this._historyButton = new Button(bar, { label: "Pixel History", class: "btn btn-sm", disabled: true, callback: () => this._followPinned(),
        tooltip: "Every clear and draw that touched the clicked pixel (Vulkan: the capture replayed; Metal: the next frame captured following it). Click a pixel first, or double-click one" });
    }
    this._extras?.toolbar?.(bar);
    this._extras?.row?.(parent);

    const info = new Div(parent, { class: "image-view-toolbar" });
    this._status = new Span(info, { text: "", class: "image-view-status" });
    this._pixelInfo = new Span(info, { text: "", class: "image-view-pixel" });

    this._scroll = new Div(parent, { class: "image-view-scroll" });
    this._canvas.className = "image-view-canvas";
    this._canvas.width = 0;
    this._canvas.height = 0;
    this._holder.className = "image-view-holder";
    this._holder.appendChild(this._canvas);
    this._marker.className = "image-view-marker";
    this._marker.style.display = "none";
    this._holder.appendChild(this._marker);
    this._scroll.element.appendChild(this._holder);
    this._setupCanvasEvents();
  }

  /** The subresource the view is showing, for an owner following one of its pixels. */
  get mip(): number {
    return this._mip;
  }

  get layer(): number {
    return this._layer;
  }

  /** Draws the image again, for an owner whose overlay changed. */
  refreshOverlay(): void {
    this._draw();
  }

  /** Shows the marker on a pixel, as a click does (null clears it). */
  setPicked(pixel: { x: number; y: number } | null): void {
    this._pinnedTexel = pixel;
    if (pixel) this._pin(pixel.x, pixel.y);
    else {
      this._pinned = "";
      this._pixelInfo.text = "";
      this._updateMarker();
    }
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
    const slice = this._slices ? Math.min(this._layer, this._sliceCount(msg) - 1) : 0;
    // The overlay's colour over the image, where the overlay is not transparent.
    const overlay = this._overlay?.rgba(tex.width, tex.height, this._mip, slice) ?? null;
    if (overlay && overlay.length >= rgba.length) {
      const strength = Math.min(1, Math.max(0, this._overlay!.opacity()));
      for (let i = 0; i < rgba.length; i += 4) {
        const a = (overlay[i + 3] / 255) * strength;
        if (a <= 0) continue;
        rgba[i] = rgba[i] * (1 - a) + overlay[i] * a;
        rgba[i + 1] = rgba[i + 1] * (1 - a) + overlay[i + 1] * a;
        rgba[i + 2] = rgba[i + 2] * (1 - a) + overlay[i + 2] * a;
      }
    }
    this._canvas.width = tex.width;
    this._canvas.height = tex.height;
    this._canvas.getContext("2d")!.putImageData(new ImageData(rgba, tex.width, tex.height), 0, 0);

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
    this._updateMarker();
  }

  /** The box around the picked pixel, which stays on it through zooming. */
  private _updateMarker(): void {
    const t = this._pinnedTexel;
    const c = this._canvas;
    if (!t || !c.width || !c.clientWidth) {
      this._marker.style.display = "none";
      return;
    }
    const scale = c.clientWidth / c.width;
    const size = Math.max(scale, 5);
    this._marker.style.display = "block";
    this._marker.style.left = `${t.x * scale - (size - scale) / 2}px`;
    this._marker.style.top = `${t.y * scale - (size - scale) / 2}px`;
    this._marker.style.width = `${size}px`;
    this._marker.style.height = `${size}px`;
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
      const extra = this._overlay?.lines?.(t.x, t.y) ?? [];
      tip.textContent = [this._texelText(t.x, t.y, "\n"), ...extra].join("\n");
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
    // A click follows the pixel (the capture's texture view); clicking it again does not re-run it.
    c.addEventListener("click", (e: MouseEvent) => {
      const t = this._texelAt(e);
      if (!t || !this._onPick) return;
      const same = this._picked?.x === t.x && this._picked?.y === t.y;
      this._pin(t.x, t.y);
      this._picked = { x: t.x, y: t.y };
      if (!same) this._onPick(t.x, t.y, this._mip, this._layer);
    });
    c.addEventListener("dblclick", (e: MouseEvent) => {
      const t = this._texelAt(e);
      if (!t || !this._pixelHistory) return;
      this._pin(t.x, t.y);
      this._followPinned();
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

  /** The pixel the last click followed, so clicking it again does not run it twice. */
  private _picked: { x: number; y: number } | null = null;

  /** Shows the texel under a click in the info line, where it stays until the next click. */
  private _pin(x: number, y: number): void {
    this._pinned = `Pixel ${this._texelText(x, y, "  ")}`;
    this._pixelInfo.text = this._pinned;
    this._pinnedTexel = { x, y };
    this._updateMarker();
    if (this._historyButton) this._historyButton.disabled = false;
  }

  private _followPinned(): void {
    const t = this._pinnedTexel;
    if (t && this._pixelHistory) this._pixelHistory(t.x, t.y, this._mip, this._layer);
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
