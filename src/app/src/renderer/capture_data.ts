// Reassembles a frame capture streamed by the layer: command batches, then render target
// descriptors and their pixel data, then the buffer ranges that were bound during the frame and
// their contents. Follows WebGPU Inspector's capture_data.js.
import { setsFor, type CommandSets } from "./command_sets.js";
import type { CaptureApi } from "../shared/protocol.js";
import { Signal } from "./utils/signal.js";
import type { LoadedCapture } from "./capture_format.js";
import type { DrawOverlay } from "./draw_overlay.js";
import type { MeshOutput } from "./mesh_output.js";
import type { DrawStat } from "./draw_stats.js";
import type { HwCounters } from "./hw_counters.js";
import { ablationKey, type ShaderAblation } from "./shader_ablation.js";
import type { CaptureBufferInfo, CaptureCommand, CaptureTextureInfo, CpuTimelineMessage, LayerMessage, OverdrawMeasurement, PassTiming } from "../shared/protocol.js";

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
          children: cc.children, descriptors: cc.descriptors, bufferData: cc.bufferData, textureData: cc.textureData,
          imageData: cc.imageData, slot: cc.slot, stack: cc.stack,
        });
      }
    }
  }
  out.forEach((c, i) => { c.index = i; });
  return out;
}

/** A render pass attachment's read-back, rather than a sampled image or what an image held at the start of the frame. */
export function isRenderTarget(info: CaptureTextureInfo): boolean {
  return !info.kind || info.kind === "attachment";
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
  /** The device tick those pass starts are measured from, for placing them on the CPU axis (protocol.ts). */
  passTimingOrigin: number | null = null;
  /** Overdraw measurements (a Metal capture with "Overdraw"): two per render pass. */
  overdraw: CapturedOverdraw[] = [];
  /** The pixel a Metal capture with "pixelHistory" followed, as it sent it (renderer/pixel_history.ts parses it). */
  pixelHistory: Record<string, unknown> | null = null;
  /** Per-draw timings and counters from a replay of the capture (renderer/draw_stats.ts). */
  drawStats: DrawStat[] | null = null;
  /** The GPU's own hardware counters per pass from a replay (renderer/hw_counters.ts). */
  hwCounters: HwCounters | null = null;
  /** Where the frame's CPU time went, and how to place GPU times on the same axis (cpu_timeline.h). */
  cpuTimeline: CpuTimelineMessage | null = null;
  /** Shader stages whose functions and lines a replay measured by ablation (renderer/shader_ablation.ts), one per pipeline stage. */
  ablations: ShaderAblation[] = [];
  /** Draw-call overlays replayed so far, by command index (renderer/draw_overlay.ts); not kept in capture files. */
  drawOverlays = new Map<number, DrawOverlay>();
  /** What a draw's vertex shader wrote, when the capture streamed it out (D3D12). */
  meshOutputs = new Map<number, MeshOutput>();
  readonly onMeshOutputs = new Signal<() => void>();
  /** (command buffer, slot) -> the command's index in this capture, built on first use (_commandAt). */
  private _slotIndex: Map<string, number> | null = null;

  /**
   * The index of the command a capture library named by its command list and the slot it took in
   * that list's recording, which is how a measurement taken inside the application refers to a
   * draw (src/d3d12/src/capture.cpp; validation messages name a command the same way). A slot is
   * per list, so it is not the command's index in this capture: a frame of several lists numbers
   * its commands across all of them. Returns the slot unchanged when nothing matches, which leaves
   * the measurement keyed by something rather than dropping it.
   */
  private _commandAt(commandBuffer: number, slot: number): number {
    if (!this._slotIndex) {
      this._slotIndex = new Map();
      for (const c of this.commands) {
        const list = c.secondary ?? c.object?.__id;
        if (list !== undefined) this._slotIndex.set(`${list}:${c.slot}`, c.index);
      }
    }
    return this._slotIndex.get(`${commandBuffer}:${slot}`) ?? slot;
  }
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
  /** Per-draw measurements arrived (a replay finished, or a capture file carried them). */
  readonly onDrawStats = new Signal<() => void>();
  /** The GPU's hardware counters arrived from a replay. */
  readonly onHwCounters = new Signal<() => void>();
  /** The capture's CPU timeline arrived. */
  readonly onCpuTimeline = new Signal<() => void>();
  /** Draw-call overlays arrived from a replay. */
  readonly onDrawOverlays = new Signal<() => void>();
  /** A shader stage was measured by ablation. */
  readonly onAblations = new Signal<() => void>();

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
    this.passTimingOrigin = null;
    this.overdraw = [];
    this.pixelHistory = null;
    this.drawStats = null;
    this.hwCounters = null;
    this.cpuTimeline = null;
    this.ablations = [];
    this.drawOverlays = new Map();
    this.meshOutputs = new Map();
    this._slotIndex = null;
    this._expectedCommands = 0;
    this._pendingBuffers = 0;
  }

  /** A render pass's overdraw measurements: the depth-tested one first. */
  overdrawForPass(frame: number, commandBufferId: number, passIndex: number): CapturedOverdraw[] {
    return this.overdraw.filter((o) => o.info.frame === frame && o.info.commandBuffer === commandBufferId && o.info.passIndex === passIndex)
      .sort((a, b) => Number(b.info.depthTested) - Number(a.info.depthTested));
  }

  /** The ablation measured for a pipeline's stage, if any. */
  ablation(pipeline: number, stage: string, entryPoint: string): ShaderAblation | null {
    const key = ablationKey(pipeline, stage, entryPoint);
    return this.ablations.find((a) => ablationKey(a.pipeline, a.stage, a.entryPoint) === key) ?? null;
  }

  /** Keeps a stage's measurement, replacing an earlier one of the same stage. */
  addAblation(a: ShaderAblation): void {
    const key = ablationKey(a.pipeline, a.stage, a.entryPoint);
    this.ablations = [...this.ablations.filter((x) => ablationKey(x.pipeline, x.stage, x.entryPoint) !== key), a];
    this.onAblations.emit();
  }

  passTiming(frame: number, commandBufferId: number, passIndex: number, compute = false): PassTiming | null {
    return this.passTimings.get(passKey(frame, commandBufferId, passIndex, compute)) ?? null;
  }

  texturesForPass(frame: number, commandBufferId: number, passIndex: number): CapturedTexture[] {
    return this.textures.filter((t) => isRenderTarget(t.info) && t.info.frame === frame && t.info.commandBuffer === commandBufferId && t.info.passIndex === passIndex)
      .sort((a, b) => a.info.attachment - b.info.attachment);
  }

  /**
   * An image read back by capture id: a sampled / storage image by the id its descriptor carries in `data`, or what
   * an image held at the start of the frame by an id in a command's `imageData`.
   */
  capturedImage(captureId: number | undefined | null): CapturedTexture | null {
    if (!captureId) return null;
    return this.textures.find((t) => !isRenderTarget(t.info) && t.info.capture === captureId) ?? null;
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

  /** Textures the capture library announced whose pixels have not arrived yet. */
  get texturesLoading(): boolean {
    return this.textures.some((t) => !t.data && !t.info.error && t.info.size > 0);
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
    this.passTimingOrigin = c.passTimingOrigin;
    this.overdraw = c.overdraw;
    this.pixelHistory = c.pixelHistory;
    this.drawStats = c.drawStats;
    this.hwCounters = c.hwCounters;
    this.cpuTimeline = c.cpuTimeline;
    this.ablations = c.ablations;
    this.onCaptureStatus.emit(`${this.commands.length} commands`);
    this.onCommandsComplete.emit();
    this.onTexturesAnnounced.emit();
    for (const t of this.textures) if (t.data) this.onTextureLoaded.emit(t);
    this.onBuffersAnnounced.emit();
    this.onBuffersComplete.emit();
    if (this.passTimings.size) this.onPassTimings.emit();
    if (this.overdraw.length) this.onOverdraw.emit();
    if (this.pixelHistory) this.onPixelHistory.emit();
    if (this.drawStats) this.onDrawStats.emit();
    if (this.hwCounters) this.onHwCounters.emit();
    if (this.cpuTimeline) this.onCpuTimeline.emit();
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
          ? this.textures.find((t) => !isRenderTarget(t.info) && t.info.capture === msg.capture)
          : this.textures.find((t) => isRenderTarget(t.info) && t.info.frame === (msg.frame ?? 0) && t.info.commandBuffer === msg.commandBuffer && t.info.passIndex === msg.passIndex && t.info.attachment === msg.attachment
            && (!msg.aspect || t.info.aspect === msg.aspect));
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
        // A string past 2^53 from a real device (protocol.ts); a number from here on.
        this.passTimingOrigin = msg.originTicks === undefined ? null : Number(msg.originTicks);
        for (const p of msg.passes ?? []) this.passTimings.set(passKey(p.frame, p.commandBuffer, p.passIndex, p.kind === "compute"), p);
        this.onPassTimings.emit();
        break;
      case "CaptureCpuTimeline":
        // Where the frame's CPU time went, beside where its GPU time went (cpu_timeline.h).
        this.cpuTimeline = msg;
        this.onCpuTimeline.emit();
        break;
      case "CaptureOverdraw":
        this.overdraw = (msg.passes ?? []).map((info) => ({ info, data: null }));
        this.onOverdraw.emit();
        break;
      case "CaptureMeshOutput": {
        // Streamed out while this capture recorded (D3D12); a Vulkan capture is replayed for the
        // same records afterwards. The data follows in CaptureMeshOutputData.
        const meshCommand = this._commandAt(msg.commandBuffer, msg.command);
        this.meshOutputs.set(meshCommand, {
          command: meshCommand, method: msg.method, frame: msg.frame, commandBuffer: msg.commandBuffer,
          passIndex: msg.passIndex, measured: msg.measured, topology: msg.topology ?? "", stride: msg.stride,
          vertices: msg.vertices, truncated: msg.truncated,
          outputs: (msg.outputs ?? []).map((o) => ({
            name: o.name, offset: o.offset, components: o.components, base: o.base,
            ...(o.builtin ? { builtin: o.builtin } : {}),
          })),
          ...(msg.note ? { note: msg.note } : {}), data: null,
        });
        this.onMeshOutputs.emit();
        break;
      }
      case "CaptureMeshOutputData": {
        const mesh = this.meshOutputs.get(this._commandAt(msg.commandBuffer, msg.command));
        if (mesh) {
          mesh.data = msg.__binary ?? null;
          this.onMeshOutputs.emit();
        }
        break;
      }
      case "CaptureDrawOverlay": {
        const command = this._commandAt(msg.commandBuffer, msg.command);
        // Measured while this capture recorded (D3D12), rather than by replaying it afterwards.
        // The mask arrives next, in CaptureDrawOverlayData.
        this.drawOverlays.set(command, {
          command, method: msg.method, frame: msg.frame, commandBuffer: msg.commandBuffer,
          passIndex: msg.passIndex, measured: msg.measured, width: msg.width, height: msg.height,
          fragments: msg.fragments, pixelsCovered: msg.pixelsCovered, pixelsPassed: msg.pixelsPassed,
          pixelsRejected: msg.pixelsRejected, depthTested: msg.depthTested, wireframe: msg.wireframe,
          ...(msg.note ? { note: msg.note } : {}), mask: null,
        });
        this.onDrawOverlays.emit();
        break;
      }
      case "CaptureDrawOverlayData": {
        const o = this.drawOverlays.get(this._commandAt(msg.commandBuffer, msg.command));
        if (o) {
          o.mask = msg.__binary ?? null;
          this.onDrawOverlays.emit();
        }
        break;
      }
      case "CaptureDrawStats":
        // A D3D12 capture measures its draws while it is taken; a Vulkan one is replayed for the
        // same numbers (measureDraws). Either way they arrive as DrawStat, keyed by command.
        this.drawStats = (msg.draws ?? []).map((d) => ({
          command: this._commandAt(d.commandBuffer, d.command), frame: d.frame, commandBuffer: d.commandBuffer,
          ...(d.passIndex === 0xffffffff ? {} : { passIndex: d.passIndex }),
          timed: d.timed, ms: d.ms, counted: d.counted,
          vertexInvocations: d.vertexInvocations, primitives: d.primitives,
          fragmentInvocations: d.fragmentInvocations, computeInvocations: d.computeInvocations,
          sampled: d.sampled, samplesPassed: d.samplesPassed,
        }));
        this.onDrawStats.emit();
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
