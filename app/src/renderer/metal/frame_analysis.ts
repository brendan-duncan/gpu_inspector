// Frame-level performance analysis of a Metal capture: the rules of ../vulkan/frame_analysis.ts
// over Metal's command stream, plus what Xcode's Insights flag that a tile-based GPU cares about.
// A render pass descriptor names its attachments and their load and store actions outright, so
// the attachment rules read them straight off the pass-begin command.
//
//   undefined-load         loading an attachment whose last pass did not store it
//   color-load             a color attachment loaded on its first use in the frame
//   color-store            a color attachment stored although nothing reads it afterwards
//   depth-store            a depth attachment stored although nothing reads it afterwards
//   msaa-store             a multisampled attachment stored (rather than only resolved)
//   memoryless-candidate   a private texture only ever cleared and discarded, or resolved, in
//                          a pass: it could be MTLStorageModeMemoryless and never touch memory
//   mergeable-passes       a render pass that loads what the pass right before it stored to the
//                          same target: one encoder would keep it in tile memory
//   redundant-pipeline-bind  binding the pipeline the encoder already has
//   redundant-buffer-bind  binding the buffer, offset and slot the encoder already has
//   single-threadgroup-dispatch  a dispatch of one threadgroup
//   tiny-draws             many draws of a handful of vertices
//
// Four more read the GPU counters the library sampled around each pass, rather than the command
// stream (metal/pass_metrics.ts does the arithmetic; the GPU Bottlenecks report shows the same
// numbers in full). They are silent on a GPU that exposes only timestamps.
//
//   high-overdraw          a pass shading each pixel far more than once
//   microtriangles         triangles too small for the rasterizer's 2x2 quad
//   late-depth-rejection   a pass that overdraws while its depth test rejects almost nothing
//   unmipped-texture       a large content texture sampled with no mip chain
//
// Every finding names the command it is about so the UI can jump to it.
import { METAL_SETS } from "./command_sets.js";
import {
  LOW_REJECTION_RATE, MICROTRIANGLE_LIMIT, OVERDRAW_LIMIT, collectPassMetrics, formatRatio,
} from "./pass_metrics.js";
import { isHandleRef, isObject, num, refId, str } from "../vulkan/vulkan_object.js";
import { SEVERITY_RANK, type Confidence, type Severity } from "../vulkan/spirv_analysis.js";
import type { FrameAnalysisDatabase, FrameFinding } from "../vulkan/frame_analysis.js";
import type { CaptureData } from "../capture_data.js";
import type { ArgObject, ArgValue, CaptureCommand } from "../../shared/protocol.js";

const TINY_DRAW_VERTICES = 12;
const TINY_DRAW_COUNT = 32;
/**
 * A texture this big, sampled with one mip level, thrashes the texture cache as soon as it is
 * drawn smaller than itself. Below it the whole texture fits in cache and the mip chain saves
 * little, so the rule stays quiet.
 */
const LARGE_TEXTURE_PIXELS = 1024 * 1024;

const RULE_ORDER = ["high-overdraw", "microtriangles", "undefined-load", "mergeable-passes", "msaa-store", "memoryless-candidate",
  "late-depth-rejection", "color-store", "depth-store", "color-load", "unmipped-texture",
  "tiny-draws", "redundant-pipeline-bind", "redundant-buffer-bind", "single-threadgroup-dispatch"];

interface Attachment {
  textureId: number;
  load: string;      // "Load", "Clear", "DontCare"
  store: string;     // "Store", "DontCare", "MultisampleResolve", "StoreAndMultisampleResolve", ...
  resolveId: number | null;
  depth: boolean;
  index: number;
}

interface PassInfo {
  command: CaptureCommand;
  ordinal: number;
  commandBuffer: number;
  attachments: Attachment[];
  draws: number;
}

/** One finding per rule with the commands it applies to folded in: the first is named, the rest counted. */
class Folded {
  first: CaptureCommand | null = null;
  count = 0;
  commands: CaptureCommand[] = [];
  add(cmd: CaptureCommand): void {
    if (!this.first) this.first = cmd;
    this.count++;
    if (this.commands.length < 64) this.commands.push(cmd);
  }
}

function argKey(v: ArgValue | undefined): string {
  if (isHandleRef(v)) return `#${v.__id}`;
  if (Array.isArray(v)) return `[${v.map(argKey).join(",")}]`;
  if (isObject(v)) return `{${Object.entries(v).map(([k, e]) => `${k}:${argKey(e)}`).join(",")}}`;
  return str(v);
}

/** Binding a texture to a shader stage: the texture will be sampled, not written. */
const SAMPLER_BINDS = new Set([
  "setFragmentTexture:atIndex:", "setFragmentTextures:withRange:",
  "setVertexTexture:atIndex:", "setVertexTextures:withRange:",
  "setTexture:atIndex:", "setTextures:withRange:",
]);

const BLIT_READS: Record<string, string[]> = {
  "copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:": ["sourceTexture"],
  "copyFromTexture:toTexture:": ["sourceTexture"],
  "copyFromTexture:sourceSlice:sourceLevel:toTexture:destinationSlice:destinationLevel:sliceCount:levelCount:": ["sourceTexture"],
  "copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:": ["sourceTexture"],
  "copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:options:": ["sourceTexture"],
  "generateMipmapsForTexture:": ["texture"],
  "synchronizeTexture:slice:level:": ["texture"],
  "synchronizeResource:": ["resource"],
  "presentDrawable:": ["texture"],
  "presentDrawable:atTime:": ["texture"],
  "presentDrawable:afterMinimumDuration:": ["texture"],
};

export class MetalFrameAnalysis {
  findings: FrameFinding[] = [];
  private _db: FrameAnalysisDatabase;
  private _passes: PassInfo[] = [];
  private _byCommand = new Map<number, FrameFinding[]>();
  /** Texture id -> command indices that read it (a sample, a blit source, a load, a present). */
  private _reads = new Map<number, number[]>();
  /** Texture id -> every pass that used it as an attachment, in order. */
  private _attachmentUses = new Map<number, { pass: PassInfo; att: Attachment }[]>();
  /** Texture id -> the first command that bound it to a shader stage (the unmipped-texture rule). */
  private _sampled = new Map<number, CaptureCommand>();

  byCommand(): Map<number, FrameFinding[]> {
    return this._byCommand;
  }

  constructor(db: FrameAnalysisDatabase) {
    this._db = db;
  }

  analyze(data: CaptureData): FrameFinding[] {
    this.findings = [];
    this._passes = [];
    this._byCommand = new Map();
    this._reads = new Map();
    this._attachmentUses = new Map();
    this._sampled = new Map();
    this._walk(data.commands);
    this._attachmentRules();
    this._memorylessRule();
    this._counterRules(data);
    this._samplingRules();
    this.findings.sort((a, b) => SEVERITY_RANK[b.severity] - SEVERITY_RANK[a.severity] || RULE_ORDER.indexOf(a.rule) - RULE_ORDER.indexOf(b.rule));
    return this.findings;
  }

  // ---------------------------------------------------------------------------- the walk

  private _walk(commands: CaptureCommand[]): void {
    const sets = METAL_SETS;
    const boundPipeline = new Map<number, number>();      // encoder id -> pipeline id
    const boundBuffers = new Map<number, Map<string, string>>();  // encoder id -> "stage:index" -> key
    // The last render pass per command buffer, and whether another encoder came between.
    const lastPass = new Map<number, PassInfo>();
    const openPass = new Map<number, PassInfo>();           // command buffer id -> the pass being encoded
    const redundantPipeline = new Folded();
    const redundantBuffer = new Folded();
    const tinyDraws = new Folded();
    const singleGroup = new Folded();
    const mergeable = new Folded();
    let ordinal = 0;

    for (const cmd of commands) {
      const a = cmd.args;
      const cb = cmd.object?.__id ?? 0;
      const encoder = cmd.encoder?.__id ?? 0;
      const m = cmd.method;

      if (sets.PASS_BEGIN.has(m)) {
        const previous = lastPass.get(cb);
        lastPass.delete(cb);
        if (m === "renderCommandEncoderWithDescriptor:" || m === "parallelRenderCommandEncoderWithDescriptor:") {
          const pass = this._decodePass(cmd, ordinal++, cb);
          this._passes.push(pass);
          openPass.set(cb, pass);
          for (const att of pass.attachments) {
            const uses = this._attachmentUses.get(att.textureId) ?? [];
            uses.push({ pass, att });
            this._attachmentUses.set(att.textureId, uses);
            if (att.load === "Load") this._read(att.textureId, cmd.index);
          }
          // Back to back with the previous render pass of this command buffer, on the same
          // first color target, loading what that one stored: one encoder would do.
          if (previous) {
            const before = previous.attachments.find((x) => !x.depth && x.index === 0);
            const now = pass.attachments.find((x) => !x.depth && x.index === 0);
            if (before && now && before.textureId === now.textureId && now.load === "Load" && before.store.startsWith("Store")) {
              mergeable.add(cmd);
            }
          }
        }
        continue;
      }
      if (sets.PASS_END.has(m)) {
        const pass = openPass.get(cb);
        if (pass && cmd.encoder && pass.command.encoder && cmd.encoder.__id === pass.command.encoder.__id) {
          openPass.delete(cb);
          lastPass.set(cb, pass);
        } else if (!pass) {
          // A compute or blit encoder ended: the next render pass is not back to back.
          lastPass.delete(cb);
        }
        continue;
      }
      if (sets.SUBMIT.has(m)) {
        lastPass.delete(cb);
      }
      if (!a) continue;

      // Reads: bound textures, blit sources, presents.
      const sampling = SAMPLER_BINDS.has(m);
      if (a.texture !== undefined && a.index !== undefined) {
        const id = refId(a.texture);
        if (id !== null) {
          this._read(id, cmd.index);
          if (sampling && !this._sampled.has(id)) this._sampled.set(id, cmd);
        }
      }
      if (Array.isArray(a.textures)) {
        for (const t of a.textures) {
          const id = refId(t);
          if (id === null) continue;
          this._read(id, cmd.index);
          if (sampling && !this._sampled.has(id)) this._sampled.set(id, cmd);
        }
      }
      const readKeys = BLIT_READS[m];
      if (readKeys) {
        for (const key of readKeys) { const id = refId(a[key]); if (id !== null) this._read(id, cmd.index); }
      }

      if (sets.BIND_PIPELINE.has(m)) {
        const id = refId(a.pipeline);
        if (id !== null) {
          if (boundPipeline.get(encoder) === id) redundantPipeline.add(cmd);
          boundPipeline.set(encoder, id);
        }
        continue;
      }
      if (sets.BIND_STAGE_BUFFER?.has(m) && sets.stageBuffersOf) {
        let binds = boundBuffers.get(encoder);
        if (!binds) boundBuffers.set(encoder, (binds = new Map()));
        for (const sb of sets.stageBuffersOf(cmd)) {
          if (sb.inline) continue;
          const slot = `${sb.stage}:${sb.index}`;
          const key = `${argKey(sb.buffer)}+${sb.offset}`;
          if (binds.get(slot) === key) redundantBuffer.add(cmd);
          binds.set(slot, key);
        }
        continue;
      }
      if (sets.DRAW.has(m)) {
        const pass = openPass.get(cb);
        if (pass) pass.draws++;
        const vertices = num(a.indexCount) || num(a.vertexCount);
        if (vertices > 0 && vertices <= TINY_DRAW_VERTICES) tinyDraws.add(cmd);
        continue;
      }
      if (m === "dispatchThreadgroups:threadsPerThreadgroup:" && isObject(a.threadgroupsPerGrid)) {
        const g = a.threadgroupsPerGrid;
        if (num(g.width) === 1 && num(g.height) === 1 && num(g.depth) === 1) singleGroup.add(cmd);
      }
    }

    if (mergeable.count) {
      this._addFolded("mergeable-passes", "high", "medium",
        `${mergeable.count} render pass${mergeable.count === 1 ? "" : "es"} load${mergeable.count === 1 ? "s" : ""} the target the pass right before stored, with nothing between them: on a tile-based GPU the store and load round-trip the whole target through memory. Encoding both in one render encoder keeps it in tile memory.`, mergeable);
    }
    if (redundantPipeline.count) {
      this._addFolded("redundant-pipeline-bind", "low", "high", `${redundantPipeline.count} pipeline bind${redundantPipeline.count === 1 ? "" : "s"} of the pipeline the encoder already had.`, redundantPipeline);
    }
    if (redundantBuffer.count) {
      this._addFolded("redundant-buffer-bind", "low", "high", `${redundantBuffer.count} buffer bind${redundantBuffer.count === 1 ? "" : "s"} of the buffer, offset and slot the encoder already had.`, redundantBuffer);
    }
    if (tinyDraws.count >= TINY_DRAW_COUNT) {
      this._addFolded("tiny-draws", "low", "medium", `${tinyDraws.count} draws of ${TINY_DRAW_VERTICES} vertices or fewer: candidates for instancing or merging into one buffer.`, tinyDraws);
    }
    if (singleGroup.count) {
      this._addFolded("single-threadgroup-dispatch", "low", "medium", `${singleGroup.count} dispatch${singleGroup.count === 1 ? "" : "es"} of a single threadgroup: the rest of the GPU idles while it runs.`, singleGroup);
    }
  }

  private _decodePass(cmd: CaptureCommand, ordinal: number, cb: number): PassInfo {
    const a = cmd.args ?? {};
    const attachments: Attachment[] = [];
    const decode = (att: ArgValue | undefined, depth: boolean, index: number): void => {
      if (!isObject(att)) return;
      const textureId = refId(att.texture);
      if (textureId === null) return;
      attachments.push({
        textureId, depth, index,
        load: str(att.loadAction).replace("MTLLoadAction", ""),
        store: str(att.storeAction).replace("MTLStoreAction", ""),
        resolveId: refId(att.resolveTexture),
      });
    };
    if (Array.isArray(a.colorAttachments)) {
      for (const c of a.colorAttachments) if (isObject(c)) decode(c, false, num(c.index));
    }
    decode(a.depthAttachment, true, 0);
    return { command: cmd, ordinal, commandBuffer: cb, attachments, draws: 0 };
  }

  private _read(textureId: number, commandIndex: number): void {
    const list = this._reads.get(textureId);
    if (list) list.push(commandIndex); else this._reads.set(textureId, [commandIndex]);
  }

  // ---------------------------------------------------------------------------- the rules

  /**
   * The measured rules. Each pass's counters, divided out into overdraw, fragments per primitive
   * and the depth rejection rate (metal/pass_metrics.ts), against the thresholds a profiling
   * session uses. A pass whose GPU did not expose the statistic counter set yields nulls and
   * raises nothing.
   */
  private _counterRules(data: CaptureData): void {
    const metrics = collectPassMetrics(data, this._db);
    const overdrawn = new Folded();
    const micro = new Folded();
    const shadedThenDropped = new Folded();
    let worstOverdraw = 0;
    let worstFragments = Infinity;
    for (const p of metrics.passes) {
      const cmd = data.commands[p.commandIndex];
      if (!cmd) continue;
      if (p.overdraw !== null && p.overdraw > OVERDRAW_LIMIT) {
        overdrawn.add(cmd);
        worstOverdraw = Math.max(worstOverdraw, p.overdraw);
      }
      if (p.fragmentsPerPrimitive !== null && p.fragmentsPerPrimitive < MICROTRIANGLE_LIMIT) {
        micro.add(cmd);
        worstFragments = Math.min(worstFragments, p.fragmentsPerPrimitive);
      }
      if (p.depthRejectRate !== null && p.overdraw !== null && p.overdraw > 1.5 && p.depthRejectRate < LOW_REJECTION_RATE) {
        shadedThenDropped.add(cmd);
      }
    }
    if (overdrawn.count) {
      this._addFolded("high-overdraw", "high", "high",
        `${overdrawn.count} pass${overdrawn.count === 1 ? "" : "es"} shade each pixel more than ${OVERDRAW_LIMIT} times over (worst ${formatRatio(worstOverdraw)}): stacked transparency, a full-screen effect drawn more than once, or opaque geometry drawn back to front.`,
        overdrawn);
    }
    if (micro.count) {
      this._addFolded("microtriangles", "high", "high",
        `${micro.count} pass${micro.count === 1 ? "" : "es"} rasterize triangles covering fewer than ${MICROTRIANGLE_LIMIT} fragments each (worst ${formatRatio(worstFragments, 1)}): the 2x2 rasterization quad shades lanes that are then thrown away. Mesh level of detail at distance is the usual answer.`,
        micro);
    }
    if (shadedThenDropped.count) {
      this._addFolded("late-depth-rejection", "medium", "medium",
        `${shadedThenDropped.count} pass${shadedThenDropped.count === 1 ? "" : "es"} overdraw while the depth test rejects little: fragments are shaded and then replaced. Drawing opaque geometry front to back, or a depth prepass, rejects that work before the fragment shader runs.`,
        shadedThenDropped);
    }
  }

  /**
   * Sampling state, from the descriptors alone: a large content texture with no mip chain misses
   * the texture cache as soon as it is drawn smaller than itself. Render targets are excluded —
   * a target sampled by the next pass is normally read at its own size, where mips would not help.
   */
  private _samplingRules(): void {
    const unmipped = new Folded();
    for (const [id, cmd] of this._sampled) {
      const d = this._texture(id);
      if (!d) continue;
      if (num(d.mipmapLevelCount) > 1) continue;
      if (str(d.usage).includes("RenderTarget")) continue;
      if (num(d.width) * num(d.height) < LARGE_TEXTURE_PIXELS) continue;
      unmipped.add(cmd);
    }
    if (unmipped.count) {
      this._addFolded("unmipped-texture", "medium", "medium",
        `${unmipped.count} texture${unmipped.count === 1 ? " is" : "s are"} sampled with a single mip level at ${LARGE_TEXTURE_PIXELS / (1024 * 1024)} megapixel or more. Drawn smaller than itself, such a texture reads scattered texels and misses the cache on most of them; a mip chain costs a third more memory and reads one texel per sample.`,
        unmipped);
    }
  }

  private _texture(id: number): ArgObject | null {
    return this._db.getObject(id)?.args ?? null;
  }

  private _isDrawable(id: number): boolean {
    return this._texture(id)?.drawable === true;
  }

  private _readAfter(textureId: number, commandIndex: number): boolean {
    const reads = this._reads.get(textureId);
    return !!reads && reads.some((i) => i > commandIndex);
  }

  private _attachmentRules(): void {
    const undefinedLoad = new Folded();
    const colorLoad = new Folded();
    const colorStore = new Folded();
    const depthStore = new Folded();
    const msaaStore = new Folded();
    for (const pass of this._passes) {
      for (const att of pass.attachments) {
        const uses = this._attachmentUses.get(att.textureId) ?? [];
        const before = uses.filter((u) => u.pass.ordinal < pass.ordinal);
        const previous = before.length ? before[before.length - 1] : null;
        const texture = this._texture(att.textureId);
        const samples = num(texture?.sampleCount) || 1;
        const stored = att.store.startsWith("Store");
        if (att.load === "Load") {
          if (previous && previous.att.store === "DontCare") undefinedLoad.add(pass.command);
          else if (!previous && !att.depth && !this._isDrawable(att.textureId)) colorLoad.add(pass.command);
        }
        if (samples > 1 && stored) msaaStore.add(pass.command);
        if (stored && !this._isDrawable(att.textureId) && !this._readAfter(att.textureId, pass.command.index)) {
          (att.depth ? depthStore : colorStore).add(pass.command);
        }
      }
    }
    if (undefinedLoad.count) {
      this._addFolded("undefined-load", "high", "high", `${undefinedLoad.count} pass${undefinedLoad.count === 1 ? "" : "es"} load${undefinedLoad.count === 1 ? "s" : ""} an attachment the previous pass on it did not store (storeAction DontCare): the contents are undefined. Either store it, or clear instead of loading.`, undefinedLoad);
    }
    if (colorLoad.count) {
      this._addFolded("color-load", "medium", "medium", `${colorLoad.count} pass${colorLoad.count === 1 ? "" : "es"} load${colorLoad.count === 1 ? "s" : ""} a color attachment on its first use in the frame. If the pass covers the whole target, MTLLoadActionClear or DontCare skips reading it from memory.`, colorLoad);
    }
    if (msaaStore.count) {
      this._addFolded("msaa-store", "medium", "high", `${msaaStore.count} pass${msaaStore.count === 1 ? "" : "es"} store${msaaStore.count === 1 ? "s" : ""} a multisampled attachment. Resolving alone (MTLStoreActionMultisampleResolve) writes one sample per pixel; storing writes them all, and a memoryless multisample target needs no memory at all.`, msaaStore);
    }
    if (colorStore.count) {
      this._addFolded("color-store", "medium", "medium", `${colorStore.count} pass${colorStore.count === 1 ? "" : "es"} store${colorStore.count === 1 ? "s" : ""} a color attachment nothing reads afterwards in the frame. MTLStoreActionDontCare skips the write, unless a later frame reads it.`, colorStore);
    }
    if (depthStore.count) {
      this._addFolded("depth-store", "medium", "medium", `${depthStore.count} pass${depthStore.count === 1 ? "" : "es"} store${depthStore.count === 1 ? "s" : ""} a depth attachment nothing reads afterwards. MTLStoreActionDontCare keeps depth in tile memory, and a depth texture used that way can be MTLStorageModeMemoryless.`, depthStore);
    }
  }

  /**
   * A texture the frame only ever clears or discards on load and discards or resolves on store,
   * with no other use, never leaves the tile: MTLStorageModeMemoryless would give it no memory
   * at all. The usual cases are a multisample target with a resolve, and a depth buffer.
   */
  private _memorylessRule(): void {
    const folded = new Folded();
    const names: string[] = [];
    for (const [textureId, uses] of this._attachmentUses) {
      const texture = this._texture(textureId);
      if (!texture || texture.drawable === true) continue;
      if (str(texture.storageMode) !== "MTLStorageModePrivate") continue;
      if (this._reads.has(textureId)) continue;
      const transient = uses.every((u) => u.att.load !== "Load" && (u.att.store === "DontCare" || u.att.store === "MultisampleResolve"));
      if (!transient) continue;
      folded.add(uses[0].pass.command);
      if (names.length < 4) names.push(this._db.getObject(textureId)?.name ?? `texture ${textureId}`);
    }
    if (folded.count) {
      this._addFolded("memoryless-candidate", "medium", "medium", `${folded.count} texture${folded.count === 1 ? "" : "s"} (${names.join(", ")}${folded.count > names.length ? ", ..." : ""}) ${folded.count === 1 ? "is" : "are"} only ever cleared or discarded and never stored or read: MTLStorageModeMemoryless would keep ${folded.count === 1 ? "it" : "them"} in tile memory with no allocation behind.`, folded);
    }
  }

  private _add(rule: string, severity: Severity, confidence: Confidence, message: string, cmd: CaptureCommand | null, count = 1): FrameFinding {
    const f: FrameFinding = { rule, severity, confidence, message, commandIndex: cmd?.index, count };
    this.findings.push(f);
    if (cmd) this._attach(cmd.index, f);
    return f;
  }

  private _addFolded(rule: string, severity: Severity, confidence: Confidence, message: string, folded: Folded): void {
    const f = this._add(rule, severity, confidence, message, folded.first, folded.count);
    for (const cmd of folded.commands) if (cmd !== folded.first) this._attach(cmd.index, f);
  }

  private _attach(index: number, f: FrameFinding): void {
    const list = this._byCommand.get(index);
    if (list) list.push(f); else this._byCommand.set(index, [f]);
  }
}

export function analyzeMetalFrame(data: CaptureData, db: FrameAnalysisDatabase): { findings: FrameFinding[]; byCommand: Map<number, FrameFinding[]> } {
  const analysis = new MetalFrameAnalysis(db);
  const findings = analysis.analyze(data);
  return { findings, byCommand: analysis.byCommand() };
}
