// Reassembles a frame capture streamed by the layer: command batches, then render target
// descriptors and their pixel data, then the buffer ranges that were bound during the frame and
// their contents. Follows WebGPU Inspector's capture_data.js.
import { setsFor, type CommandSets } from "./command_sets.js";
import type { CaptureApi } from "../shared/protocol.js";
import { Signal } from "./utils/signal.js";
import type { LoadedCapture } from "./capture_format.js";
import type { CaptureBufferInfo, CaptureCommand, CaptureTextureInfo, LayerMessage, OverdrawMeasurement, PassTiming } from "../shared/protocol.js";

export interface CapturedTexture {
  info: CaptureTextureInfo;
  data: Uint8Array | null;
  canvas: HTMLCanvasElement | null;   // set by the panel while waiting for data
}

/** A buffer range read back when it was bound (referenced by id from the binding command). */
export interface CapturedBuffer {
  info: CaptureBufferInfo;
  data: Uint8Array | null;
}

/** An overdraw measurement of a render pass, with its per-pixel counts (u16 little endian) once they arrive. */
export interface CapturedOverdraw {
  info: OverdrawMeasurement;
  data: Uint8Array | null;
}

/**
 * Inlines the commands of secondary command buffers right after the vkCmdExecuteCommands that
 * ran them, so the command list, draw-state reconstruction and pass lookup see one stream per
 * primary command buffer (engines such as Unity record every draw in secondaries). Inlined
 * commands keep the primary as their `object` and note the secondary in `secondary`.
 */
function flattenSecondaries(commands: CaptureCommand[]): CaptureCommand[] {
  if (!commands.some((c) => c && c.children && c.children.length)) return commands;
  const out: CaptureCommand[] = [];
  for (const c of commands) {
    if (!c) continue;
    out.push(c);
    for (const child of c.children ?? []) {
      for (const cc of child.commands) {
        out.push({
          index: 0, frame: c.frame, method: cc.method, object: c.object, args: cc.args, secondary: child.commandBuffer,
          children: cc.children, descriptors: cc.descriptors, bufferData: cc.bufferData, slot: cc.slot, stack: cc.stack,
        });
      }
    }
  }
  out.forEach((c, i) => { c.index = i; });
  return out;
}

/** Key of a pass in CaptureData.passTimings: "frame:commandBuffer:index" for render passes, "...:cN" for compute. */
export function passKey(frame: number, commandBufferId: number, passIndex: number, compute = false): string {
  return `${frame}:${commandBufferId}:${compute ? "c" : ""}${passIndex}`;
}

/** The parts of a pass key (see passKey). */
export function parsePassKey(key: string): { frame: number; commandBuffer: number; passIndex: number; compute: boolean } {
  const [f, cb, p] = key.split(":");
  const compute = p.startsWith("c");
  return { frame: Number(f), commandBuffer: Number(cb), passIndex: Number(compute ? p.slice(1) : p), compute };
}

export class CaptureData {
  /** Frame number of the first captured frame, and how many frames the capture spans. */
  frame = 0;
  frames = 1;
  /** Which API produced it. Captures made before the field existed are Vulkan. */
  api: CaptureApi = "vulkan";
  commands: CaptureCommand[] = [];
  textures: CapturedTexture[] = [];
  buffers = new Map<number, CapturedBuffer>();
  /** GPU pass timings (Profile passes), keyed "frame:commandBuffer:passIndex". */
  passTimings = new Map<string, PassTiming>();
  /** Overdraw measurements (a Metal capture with "Overdraw"): two per render pass. */
  overdraw: CapturedOverdraw[] = [];
  /** The pixel a Metal capture with "pixelHistory" followed, as it sent it (renderer/pixel_history.ts parses it). */
  pixelHistory: Record<string, unknown> | null = null;
  private _expectedCommands = 0;
  private _pendingBuffers = 0;

  readonly onCaptureStatus = new Signal<(text: string) => void>();
  readonly onCommandsComplete = new Signal<() => void>();
  readonly onTextureLoaded = new Signal<(texture: CapturedTexture) => void>();
  readonly onTexturesAnnounced = new Signal<() => void>();
  readonly onBuffersAnnounced = new Signal<() => void>();
  readonly onBufferLoaded = new Signal<(buffer: CapturedBuffer) => void>();
  /** Every announced buffer's data has arrived (or failed). */
  readonly onBuffersComplete = new Signal<() => void>();
  readonly onPassTimings = new Signal<() => void>();
  /** Overdraw measurements were announced, or one's per-pixel counts arrived. */
  readonly onOverdraw = new Signal<() => void>();
  /** A Metal capture's pixel history arrived. */
  readonly onPixelHistory = new Signal<() => void>();

  /** The command classification for this capture's API (see ../command_sets.ts). */
  get sets(): CommandSets {
    return setsFor(this.api);
  }

  reset(): void {
    this.frame = 0;
    this.frames = 1;
    this.api = "vulkan";
    this.commands = [];
    this.textures = [];
    this.buffers = new Map();
    this.passTimings = new Map();
    this.overdraw = [];
    this.pixelHistory = null;
    this._expectedCommands = 0;
    this._pendingBuffers = 0;
  }

  /** A render pass's overdraw measurements: the depth-tested one first. */
  overdrawForPass(frame: number, commandBufferId: number, passIndex: number): CapturedOverdraw[] {
    return this.overdraw.filter((o) => o.info.frame === frame && o.info.commandBuffer === commandBufferId && o.info.passIndex === passIndex)
      .sort((a, b) => Number(b.info.depthTested) - Number(a.info.depthTested));
  }

  passTiming(frame: number, commandBufferId: number, passIndex: number, compute = false): PassTiming | null {
    return this.passTimings.get(passKey(frame, commandBufferId, passIndex, compute)) ?? null;
  }

  texturesForPass(frame: number, commandBufferId: number, passIndex: number): CapturedTexture[] {
    return this.textures.filter((t) => t.info.kind !== "sampled" && t.info.frame === frame && t.info.commandBuffer === commandBufferId && t.info.passIndex === passIndex)
      .sort((a, b) => a.info.attachment - b.info.attachment);
  }

  /** A sampled / storage image read back for a descriptor, by the id the descriptor carries in `data`. */
  capturedImage(captureId: number | undefined | null): CapturedTexture | null {
    if (!captureId) return null;
    return this.textures.find((t) => t.info.kind === "sampled" && t.info.capture === captureId) ?? null;
  }

  /** Any captured contents of an image (a sampled read-back or a render target), with data. */
  imageContents(imageId: number): CapturedTexture | null {
    return this.textures.find((t) => t.info.id === imageId && t.data && !t.info.error) ?? null;
  }

  /** Sampled image read-backs: [captured, failed]. */
  get sampledImageCounts(): [number, number] {
    let ok = 0;
    let failed = 0;
    for (const t of this.textures) {
      if (t.info.kind !== "sampled") continue;
      if (t.info.error) failed++;
      else ok++;
    }
    return [ok, failed];
  }

  /** The commands of one captured frame (indices stay those of the full list). */
  commandsForFrame(frame: number): CaptureCommand[] {
    return this.commands.filter((c) => c.frame === frame);
  }

  /** The captured contents of a bound buffer range, by the id the binding command carries. */
  buffer(id: number | undefined | null): CapturedBuffer | null {
    if (!id) return null;
    return this.buffers.get(id) ?? null;
  }

  get buffersLoading(): boolean {
    return this._pendingBuffers > 0;
  }

  /** Takes over a capture file's contents, emitting the signals a streamed capture would. */
  load(c: LoadedCapture): void {
    this.reset();
    this.frame = c.manifest.frame;
    this.frames = Math.max(1, c.manifest.frames ?? 1);
    this.api = c.api;
    this.commands = c.commands;          // saved after flattenSecondaries: already one stream per primary
    this.textures = c.textures;
    this.buffers = c.buffers;
    this.passTimings = c.passTimings;
    this.overdraw = c.overdraw;
    this.pixelHistory = c.pixelHistory;
    this.onCaptureStatus.emit(`${this.commands.length} commands`);
    this.onCommandsComplete.emit();
    this.onTexturesAnnounced.emit();
    for (const t of this.textures) if (t.data) this.onTextureLoaded.emit(t);
    this.onBuffersAnnounced.emit();
    this.onBuffersComplete.emit();
    if (this.passTimings.size) this.onPassTimings.emit();
    if (this.overdraw.length) this.onOverdraw.emit();
    if (this.pixelHistory) this.onPixelHistory.emit();
  }

  handleMessage(msg: LayerMessage): void {
    switch (msg.action) {
      case "CaptureFrameResults":
        this.reset();
        this.frame = msg.frame;
        this.frames = Math.max(1, msg.frames ?? 1);
        this.api = msg.api ?? "vulkan";
        this._expectedCommands = msg.count;
        this.onCaptureStatus.emit(`receiving ${msg.count} commands...`);
        if (msg.count === 0) this.onCommandsComplete.emit();
        break;
      case "CaptureFrameCommands":
        for (const c of msg.commands) this.commands[c.index] = c;
        if (this.commands.length >= this._expectedCommands) {
          this.commands = flattenSecondaries(this.commands);
          this.onCaptureStatus.emit(`${this.commands.length} commands`);
          this.onCommandsComplete.emit();
        }
        break;
      case "CaptureTextureFrames":
        this.textures = msg.textures.map((info) => ({ info, data: null, canvas: null }));
        this.onTexturesAnnounced.emit();
        break;
      case "CaptureTextureData": {
        const tex = msg.capture
          ? this.textures.find((t) => t.info.kind === "sampled" && t.info.capture === msg.capture)
          : this.textures.find((t) => t.info.kind !== "sampled" && t.info.frame === (msg.frame ?? 0) && t.info.commandBuffer === msg.commandBuffer && t.info.passIndex === msg.passIndex && t.info.attachment === msg.attachment);
        if (tex) {
          tex.data = msg.__binary ?? null;
          this.onTextureLoaded.emit(tex);
        }
        break;
      }
      case "CaptureBuffers":
        this.buffers = new Map();
        this._pendingBuffers = 0;
        for (const info of msg.buffers ?? []) {
          this.buffers.set(info.id, { info, data: null });
          if (!info.error && info.size > 0) this._pendingBuffers++;
        }
        this.onBuffersAnnounced.emit();
        if (this._pendingBuffers === 0) this.onBuffersComplete.emit();
        break;
      case "CapturePassTimings":
        this.passTimings = new Map();
        for (const p of msg.passes ?? []) this.passTimings.set(passKey(p.frame, p.commandBuffer, p.passIndex, p.kind === "compute"), p);
        this.onPassTimings.emit();
        break;
      case "CaptureOverdraw":
        this.overdraw = (msg.passes ?? []).map((info) => ({ info, data: null }));
        this.onOverdraw.emit();
        break;
      case "CapturePixelHistory":
        this.pixelHistory = msg.history ?? null;
        this.onPixelHistory.emit();
        break;
      case "CaptureOverdrawData": {
        const o = this.overdraw.find((m) => m.info.frame === msg.frame && m.info.commandBuffer === msg.commandBuffer
          && m.info.passIndex === msg.passIndex && m.info.depthTested === msg.depthTested);
        if (o) {
          o.data = msg.__binary ?? null;
          this.onOverdraw.emit();
        }
        break;
      }
      case "CaptureBufferData": {
        const buf = this.buffers.get(msg.id);
        if (buf) {
          if (!buf.data) this._pendingBuffers = Math.max(0, this._pendingBuffers - 1);
          buf.data = msg.__binary ?? new Uint8Array(0);
          this.onBufferLoaded.emit(buf);
          if (this._pendingBuffers === 0) this.onBuffersComplete.emit();
        }
        break;
      }
      default:
        break;
    }
  }
}
