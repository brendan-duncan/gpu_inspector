// The captures the MCP server has open. Each is a capture file loaded the way the UI's file
// session loads one (FileSessionPanel in session_panel.ts): an ObjectDatabase built from the
// manifest's objects, a CaptureData holding the commands, targets, buffers and timings. The
// analyses the reports run are computed on first use and kept.
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { CaptureData, type CapturedOverdraw } from "../renderer/capture_data.js";
import type { DrawStat } from "../renderer/draw_stats.js";
import { parseCaptureFile, type CaptureFileManifest } from "../renderer/capture_format.js";
import { CaptureStatistics } from "../renderer/capture_statistics.js";
import { labelNameOf } from "../renderer/command_sets.js";
import { frameRenderGraph } from "../renderer/frame_graph.js";
import { collectPassMetrics, type FrameMetrics } from "../renderer/pass_metrics.js";
import type { RenderGraph } from "../renderer/render_graph.js";
import { analyzeFrame, type FrameFinding } from "../renderer/vulkan/frame_analysis.js";
import { ObjectDatabase } from "../renderer/vulkan/object_database.js";
import { reflectSpirv, type ShaderReflection } from "../renderer/vulkan/spirv_reflect.js";
import { isObject, refId, type VulkanObject } from "../renderer/vulkan/vulkan_object.js";
import type { CaptureTextureInfo, ValidationMessage } from "../shared/protocol.js";

export class Capture {
  readonly db = new ObjectDatabase();
  readonly data = new CaptureData();
  readonly manifest: CaptureFileManifest;
  readonly fileBytes: number;
  private _graph: RenderGraph | null = null;
  private _analysis: { findings: FrameFinding[]; byCommand: Map<number, FrameFinding[]> } | null = null;
  private _metrics: FrameMetrics | null = null;
  private _statistics: CaptureStatistics | null = null;
  private _passOf: Int32Array | null = null;
  private _labels: string[] | null = null;
  private _validationCommands: Map<string, number> | null = null;
  private _reflections = new Map<string, ShaderReflection | null>();

  constructor(readonly id: string, readonly path: string, readonly mtimeMs: number, bytes: Uint8Array) {
    const capture = parseCaptureFile(bytes);
    const m = capture.manifest;
    this.manifest = m;
    this.fileBytes = bytes.byteLength;
    this.db.loadObjects(capture.objects, capture.blobs, {
      frame: m.frame, frameTimeMs: m.frameTimeMs ?? 0, submitMs: m.submitMs ?? 0, refreshMs: m.refreshMs ?? 0, refreshSource: m.refreshSource ?? "",
      displayRefreshMs: m.displayRefreshMs ?? 0, frameBoundary: m.frameBoundary ?? "",
    });
    this.db.loadValidation(capture.validation);
    for (const [a, f] of Object.entries(m.symbols ?? {})) this.db.symbols.set(a, f);
    for (const [id, frames] of Object.entries(m.stacks ?? {})) this.db.stacks.set(Number(id), frames);
    this.db.stacksAvailable = m.stacks !== undefined;
    this.data.load(capture);
  }

  get graph(): RenderGraph {
    return (this._graph ??= frameRenderGraph(this.data, this.db));
  }

  /** Frame Issues: the per-command rules, the counter and sampling rules and the render graph rules. */
  get analysis(): { findings: FrameFinding[]; byCommand: Map<number, FrameFinding[]> } {
    return (this._analysis ??= analyzeFrame(this.data, this.db, this.graph));
  }

  get metrics(): FrameMetrics {
    return (this._metrics ??= collectPassMetrics(this.data, this.db));
  }

  /** Overdraw measured after the capture was saved (vkinsp_replay, for a Vulkan capture): what reads it is recomputed. */
  setOverdraw(measurements: CapturedOverdraw[]): void {
    this.data.overdraw = measurements;
    this._metrics = null;
    this._analysis = null;
  }

  /** Per-draw timings and counters measured by replaying the capture (renderer/draw_stats.ts). */
  setDrawStats(draws: DrawStat[]): void {
    this.data.drawStats = draws;
    // The pass metrics take depth rejection from these where the capture's own counter is missing.
    this._metrics = null;
    this._analysis = null;
  }

  get statistics(): CaptureStatistics {
    return (this._statistics ??= new CaptureStatistics().compute(this.data, this.db));
  }

  /** Index into metrics.passes of the pass a command belongs to; -1 outside every pass. */
  passOf(commandIndex: number): number {
    if (!this._passOf) {
      const commands = this.data.commands;
      const map = new Int32Array(commands.length).fill(-1);
      this.metrics.passes.forEach((p, i) => {
        for (let c = p.commandIndex; c <= p.endIndex && c < commands.length; c++) {
          const cmd = commands[c];
          // A compute run is keyed by the stream that recorded it; a render pass by its command buffer.
          const stream = p.compute ? (cmd.secondary || cmd.object?.__id || 0) : (cmd.object?.__id ?? 0);
          if (stream === p.commandBuffer && map[c] < 0) map[c] = i;
        }
      });
      this._passOf = map;
    }
    return this._passOf[commandIndex] ?? -1;
  }

  /** The debug groups open at a command, outermost first: "Frame / Shadows / Cascade 0". */
  labelsOf(commandIndex: number): string {
    if (!this._labels) {
      const sets = this.data.sets;
      const stacks = new Map<string, string[]>();
      this._labels = this.data.commands.map((c) => {
        const key = `${c.object?.__id ?? 0}:${c.secondary ?? 0}`;
        let stack = stacks.get(key);
        if (!stack || c.method === "vkBeginCommandBuffer") stacks.set(key, (stack = []));
        if (sets.LABEL_BEGIN.has(c.method)) stack.push(labelNameOf(c));
        const labels = stack.join(" / ");
        if (sets.LABEL_END.has(c.method)) stack.pop();
        return labels;
      });
    }
    return this._labels[commandIndex] ?? "";
  }

  /** A pass's label with its render pass object's name and the debug groups around it. */
  passName(passIndex: number): string {
    const p = this.metrics.passes[passIndex];
    if (!p) return "";
    let label = p.label;
    const begin = this.data.commands[p.commandIndex]?.args?.pRenderPassBegin;
    if (!label.includes(":") && isObject(begin)) {
      const rp = this.db.getObject(refId(begin.renderPass));
      if (rp?.label) label += `: ${rp.label}`;
    }
    const labels = this.labelsOf(p.commandIndex);
    return labels ? `${label} [${labels}]` : label;
  }

  /** The metrics pass a render target was read back at the end of; -1 for sampled images. */
  passOfTexture(info: CaptureTextureInfo): number {
    if (info.kind === "sampled") return -1;
    return this.metrics.passes.findIndex((p) => !p.compute && p.frame === info.frame && p.commandBuffer === info.commandBuffer && p.passIndex === info.passIndex);
  }

  /** The captured command a validation message fired on, when it names one. */
  commandOfValidation(v: ValidationMessage): number | undefined {
    if (!v.command) return undefined;
    if (!this._validationCommands) {
      this._validationCommands = new Map();
      for (const c of this.data.commands) {
        if (c.slot !== undefined) this._validationCommands.set(`${c.secondary ?? c.object?.__id}:${c.slot}`, c.index);
      }
    }
    return this._validationCommands.get(`${v.command.commandBuffer}:${v.command.slot}`);
  }

  /** The SPIR-V of an object's payload, when the file carries it. */
  spirv(object: VulkanObject, blobIndex: number): Uint8Array | null {
    return this.db.blobData.get(`${object.id}:${blobIndex}`) ?? null;
  }

  /** Reflection of an object's SPIR-V payload, parsed once. */
  reflection(object: VulkanObject, blobIndex: number): ShaderReflection | null {
    const key = `${object.id}:${blobIndex}`;
    if (!this._reflections.has(key)) {
      const data = this.spirv(object, blobIndex);
      let r: ShaderReflection | null = null;
      try {
        r = data ? reflectSpirv(data) : null;
      } catch {
        r = null;
      }
      this._reflections.set(key, r);
    }
    return this._reflections.get(key) ?? null;
  }
}

export class CaptureStore {
  private readonly _captures = new Map<string, Capture>();
  private _counter = 0;
  private _latest: Capture | null = null;

  /** Opens a capture file, or returns the open capture of that file when it has not changed on disk. */
  open(file: string): { capture: Capture; reused: boolean } {
    const abs = path.resolve(file);
    let stat: fs.Stats;
    try {
      stat = fs.statSync(abs);
    } catch {
      throw new Error(`No file at ${abs}.`);
    }
    for (const c of this._captures.values()) {
      if (c.path === abs && c.mtimeMs === stat.mtimeMs) {
        this._latest = c;
        return { capture: c, reused: true };
      }
    }
    // A plain Uint8Array over the file's bytes rather than the Buffer itself: payloads are views into
    // it at any byte offset, and the SPIR-V parsers copy a misaligned view with slice() before taking
    // word views of it, which Buffer.slice does not do (it returns another view).
    const contents = fs.readFileSync(abs);
    const bytes = new Uint8Array(contents.buffer, contents.byteOffset, contents.byteLength);
    const capture = new Capture(`cap-${++this._counter}`, abs, stat.mtimeMs, bytes);
    this._captures.set(capture.id, capture);
    this._latest = capture;
    return { capture, reused: false };
  }

  /** An open capture by id, a capture file by path (opened on the way), or the latest opened. */
  resolve(ref: string | undefined): Capture {
    if (!ref) {
      if (!this._latest) throw new Error("No capture is open. Call open_capture with the path of a .gpucap file (list_captures shows the captures GPU Inspector opened recently).");
      return this._latest;
    }
    const open = this._captures.get(ref);
    if (open) return open;
    if (/\.gpucap$/i.test(ref) || fs.existsSync(ref)) return this.open(ref).capture;
    const ids = [...this._captures.keys()];
    throw new Error(`No open capture "${ref}". ${ids.length ? `Open: ${ids.join(", ")}.` : "None is open."}`);
  }

  close(ref: string): boolean {
    const c = this._captures.get(ref) ?? [...this._captures.values()].find((x) => x.path === path.resolve(ref));
    if (!c) return false;
    this._captures.delete(c.id);
    if (this._latest === c) this._latest = [...this._captures.values()].pop() ?? null;
    return true;
  }

  list(): Capture[] {
    return [...this._captures.values()];
  }
}

/**
 * GPU Inspector's settings file (Electron's userData for the app name "gpu-inspector"), where
 * the app keeps its recent captures. GPU_INSPECTOR_SETTINGS names another.
 */
export function settingsFile(): string {
  if (process.env.GPU_INSPECTOR_SETTINGS) return process.env.GPU_INSPECTOR_SETTINGS;
  const home = os.homedir();
  const base = process.platform === "win32" ? (process.env.APPDATA ?? path.join(home, "AppData", "Roaming"))
    : process.platform === "darwin" ? path.join(home, "Library", "Application Support")
      : (process.env.XDG_CONFIG_HOME ?? path.join(home, ".config"));
  return path.join(base, "gpu-inspector", "settings.json");
}

/** A value from GPU Inspector's settings, when the file exists and has it. */
export function appSetting(key: string): unknown {
  try {
    return (JSON.parse(fs.readFileSync(settingsFile(), "utf8")) as Record<string, unknown>)[key];
  } catch {
    return undefined;
  }
}

/** The capture files GPU Inspector opened or saved most recently, newest first. */
export function recentCaptureFiles(): string[] {
  const recent = appSetting("recentCaptures");
  return Array.isArray(recent) ? recent.filter((p): p is string => typeof p === "string" && !!p) : [];
}
