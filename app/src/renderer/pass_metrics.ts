// Per-pass measurements, in the terms a bottleneck is actually described in.
//
// Both capture backends sample GPU counters around every pass: the Metal library takes Metal's
// counter sets (metal/src/capture.mm) and the Vulkan layer a pipeline statistics query beside its
// timestamps (layer/src/pipeline_stats.h). Those are raw totals. What a profiling session asks is
// "how many times was each pixel shaded", "how big are the triangles", "is the depth test doing
// its job" — each of which is one division away, against the render target's size or another
// counter. This module does those divisions once, so the report, the rules and the pass headers
// all say the same numbers, whichever API produced the capture.
//
// Everything here is optional, and the two APIs offer different subsets. Metal alone splits a
// pass into its vertex and fragment spans and counts the fragments that survived the depth test;
// Vulkan's pipeline statistics carry the invocation and primitive counts but neither of those.
// A GPU that exposes no counters at all still gives durations. Every field a capture cannot
// answer is null rather than zero, and the report says which case it is.
import { isObject, num, refId, str, type ObjectLookup } from "./vulkan/vulkan_object.js";
import type { ArgObject, ArgValue, CaptureCommand, PassTiming } from "../shared/protocol.js";
import type { CaptureData } from "./capture_data.js";

/** Average overdraw a frame is doing well to stay near, from Apple's and Unity's guidance. */
export const HEALTHY_OVERDRAW = 1.2;
/** Overdraw worth reporting: twice the healthy figure. */
export const OVERDRAW_LIMIT = 2;
/**
 * Fragments per primitive below which a triangle is a microtriangle: the rasterizer works in 2x2
 * quads, so a triangle covering fewer than four fragments has shaded lanes it throws away.
 */
export const MICROTRIANGLE_LIMIT = 4;
/** A depth test rejecting less than this, in a pass that overdraws, is not earning its keep. */
export const LOW_REJECTION_RATE = 0.25;

export type Bound = "vertex" | "fragment" | "target" | "balanced";

export interface PassMetrics {
  /** The pass-begin command, for jumping to it. */
  commandIndex: number;
  label: string;
  frame: number;
  commandBuffer: number;
  passIndex: number;
  compute: boolean;
  /** Draws (or dispatches, for a compute pass) the pass encoded. */
  draws: number;
  /** Vertices the draws asked for, summed; 0 when none of them said. */
  vertices: number;
  /** Pixels of the first colour target (or the depth target), 0 when it could not be resolved. */
  pixels: number;
  /** Samples per pixel of that target. */
  samples: number;
  timing: PassTiming | null;
  durationMs: number | null;
  vertexMs: number | null;
  fragmentMs: number | null;

  // Derived, each null when the counter it needs is absent.

  /** Fragment shader invocations per target pixel: how many times the average pixel was shaded. */
  overdraw: number | null;
  /** Fragment invocations per primitive out of the clipper: small means microtriangles. */
  fragmentsPerPrimitive: number | null;
  /** Fraction of shaded fragments the depth and stencil tests threw away. */
  depthRejectRate: number | null;
  /** Nanoseconds of vertex-stage time per vertex invocation. */
  nsPerVertex: number | null;
  /** Nanoseconds of fragment-stage time per fragment invocation. */
  nsPerFragment: number | null;
  /** Share of the pass's GPU cycles per stage, from the stage-utilization set. */
  cycleShare: { vertex: number; fragment: number; target: number } | null;
  /** Which stage the pass is limited by, and why that was concluded. */
  bound: Bound | null;
  boundReason: string;
}

export interface FrameMetrics {
  passes: PassMetrics[];
  /** Sum of the timed passes' durations. */
  gpuMs: number;
  vertexMs: number;
  fragmentMs: number;
  /** Passes that carry statistic counters, and how many were timed at all. */
  withCounters: number;
  timed: number;
  /** True when at least one pass was timed, so the report has something to say. */
  usable: boolean;
  /** Frame-wide totals, when the counters are there. */
  totals: { vertexInvocations: number; fragmentInvocations: number; primitives: number; fragmentsPassed: number } | null;
}

function counter(t: PassTiming | null, name: string): number | null {
  const v = t?.counters?.[name];
  return typeof v === "number" ? v : null;
}

/** A ratio, or null when either side is missing or the denominator is zero. */
function ratio(top: number | null, bottom: number | null): number | null {
  if (top === null || bottom === null || bottom <= 0) return null;
  return top / bottom;
}

/** Vertices a draw asks for, before instancing (which multiplies the work but not the mesh). */
function drawVertices(a: ArgObject): number {
  const instances = Math.max(1, num(a.instanceCount));
  const vertices = num(a.indexCount) || num(a.vertexCount);
  return vertices * instances;
}

/**
 * Every pass of the capture with what was measured over it. Passes appear in encode order,
 * whether or not they were timed.
 */
export function collectPassMetrics(data: CaptureData, db: ObjectLookup): FrameMetrics {
  const sets = data.sets;
  const passes: PassMetrics[] = [];
  // How a pass is numbered differs, and both backends have to be matched exactly or a pass finds
  // no timing. A Metal encoder is the pass and every encoder of a command buffer takes the next
  // number whatever its kind, with compute encoders keyed apart (g_passCounters in
  // metal/src/capture.mm). Vulkan's PASS_BEGIN is render-only and its runs of dispatches are
  // counted separately (NextPassIndex / NextComputeIndex in layer/src/command_recorder.h). One
  // counter per command buffer for PASS_BEGIN, and a second for compute runs, does both.
  const passIndexOf = new Map<number, number>();
  const computeIndexOf = new Map<number, number>();
  let open: PassMetrics | null = null;
  let computeRun: PassMetrics | null = null;
  let inPass = false;
  let currentCb = -1;
  let currentSecondary = 0;

  const closeComputeRun = (): void => {
    computeRun = null;
  };

  for (const cmd of data.commands) {
    const m = cmd.method;
    const cb = cmd.object?.__id ?? 0;
    const a = cmd.args;
    if (cb !== currentCb || (cmd.secondary ?? 0) !== currentSecondary) {
      closeComputeRun();
      currentCb = cb;
      currentSecondary = cmd.secondary ?? 0;
    }

    if (sets.PASS_BEGIN.has(m)) {
      closeComputeRun();
      inPass = true;
      const index = passIndexOf.get(cb) ?? 0;
      passIndexOf.set(cb, index + 1);
      open = blank(cmd, index, sets.passIsCompute?.(m) ?? false, cb, targetOf(cmd, db));
      passes.push(open);
      continue;
    }
    if (sets.PASS_END.has(m)) {
      inPass = false;
      open = null;
      continue;
    }
    if (sets.SUBMIT.has(m) || m === "vkEndCommandBuffer" || sets.COMPUTE_PASS_END.has(m)
        || sets.LABEL_BEGIN.has(m) || sets.LABEL_END.has(m)) {
      closeComputeRun();
    }
    if (!a) continue;
    if (sets.DRAW.has(m)) {
      if (open) {
        open.draws++;
        open.vertices += drawVertices(a);
      }
      continue;
    }
    if (sets.DISPATCH.has(m)) {
      if (open) {
        open.draws++;                        // Metal: the dispatch belongs to its compute encoder
      } else if (!inPass) {
        // Vulkan: a run of dispatches outside a render pass is its own timed pass.
        if (!computeRun) {
          const key = cmd.secondary || cb;
          const index = computeIndexOf.get(key) ?? 0;
          computeIndexOf.set(key, index + 1);
          computeRun = blank(cmd, index, true, key, null);
          passes.push(computeRun);
        }
        computeRun.draws++;
      }
    }
  }

  let gpuMs = 0;
  let vertexMs = 0;
  let fragmentMs = 0;
  let withCounters = 0;
  let timed = 0;
  const totals = { vertexInvocations: 0, fragmentInvocations: 0, primitives: 0, fragmentsPassed: 0 };
  let anyCounters = false;

  for (const p of passes) {
    const t = data.passTiming(p.frame, p.commandBuffer, p.passIndex, p.compute);
    p.timing = t;
    if (!t) continue;
    timed++;
    p.durationMs = t.durationMs;
    p.vertexMs = t.vertexMs ?? null;
    p.fragmentMs = t.fragmentMs ?? null;
    gpuMs += t.durationMs;
    vertexMs += t.vertexMs ?? 0;
    fragmentMs += t.fragmentMs ?? 0;

    const fragments = counter(t, "fragmentInvocations");
    const primitives = counter(t, "clipperPrimitivesOut");
    const vertexInvocations = counter(t, "vertexInvocations");
    const passed = counter(t, "fragmentsPassed");
    if (fragments !== null || vertexInvocations !== null) {
      withCounters++;
      anyCounters = true;
      totals.fragmentInvocations += fragments ?? 0;
      totals.vertexInvocations += vertexInvocations ?? 0;
      totals.primitives += primitives ?? 0;
      totals.fragmentsPassed += passed ?? 0;
    }
    p.overdraw = p.pixels > 0 ? ratio(fragments, p.pixels) : null;
    p.fragmentsPerPrimitive = ratio(fragments, primitives);
    // The counter counts fragments that survived the depth and stencil tests; the rest were
    // rejected, early or late.
    p.depthRejectRate = fragments !== null && passed !== null && fragments > 0 ? 1 - passed / fragments : null;
    p.nsPerVertex = p.vertexMs !== null ? ratio(p.vertexMs * 1e6, vertexInvocations) : null;
    p.nsPerFragment = p.fragmentMs !== null ? ratio(p.fragmentMs * 1e6, fragments) : null;

    const u = t.utilization;
    const totalCycles = u?.totalCycles ?? 0;
    if (u && totalCycles > 0) {
      p.cycleShare = {
        vertex: (u.vertexCycles ?? 0) / totalCycles,
        fragment: (u.fragmentCycles ?? 0) / totalCycles,
        target: (u.renderTargetCycles ?? 0) / totalCycles,
      };
    }
    decideBound(p);
  }

  return {
    passes, gpuMs, vertexMs, fragmentMs, withCounters, timed,
    usable: timed > 0,
    totals: anyCounters ? totals : null,
  };
}

/**
 * Pixels the pass rendered over, which is what fragment work scales with. Vulkan states it
 * outright as the render area; a Metal pass descriptor names its attachments, so the first
 * colour target's size (or the depth target's) stands in.
 */
function targetOf(cmd: CaptureCommand, db: ObjectLookup): { pixels: number; samples: number } | null {
  const a = cmd.args;
  if (!a) return null;
  for (const key of ["pRenderPassBegin", "pRenderingInfo"]) {
    const info = a[key];
    if (!isObject(info) || !isObject(info.renderArea)) continue;
    const extent = info.renderArea.extent;
    if (!isObject(extent)) continue;
    const pixels = num(extent.width) * num(extent.height);
    if (pixels > 0) return { pixels, samples: 1 };
  }
  const attachment = (att: ArgValue | undefined): { pixels: number; samples: number } | null => {
    if (!isObject(att)) return null;
    const id = refId(att.texture);
    if (id === null) return null;
    const d = db.getObject(id)?.descriptor;
    if (!d) return null;
    const pixels = num(d.width) * num(d.height);
    return pixels > 0 ? { pixels, samples: Math.max(1, num(d.sampleCount)) } : null;
  };
  if (Array.isArray(a.colorAttachments)) {
    for (const c of a.colorAttachments) {
      const hit = attachment(c);
      if (hit) return hit;
    }
  }
  return attachment(a.depthAttachment);
}

function blank(cmd: CaptureCommand, passIndex: number, compute: boolean, cb: number,
               target: { pixels: number; samples: number } | null): PassMetrics {
  return {
    commandIndex: cmd.index, label: passLabel(cmd, passIndex), frame: cmd.frame ?? 0,
    commandBuffer: cb, passIndex, compute, draws: 0, vertices: 0,
    pixels: target?.pixels ?? 0, samples: target?.samples ?? 1,
    timing: null, durationMs: null, vertexMs: null, fragmentMs: null,
    overdraw: null, fragmentsPerPrimitive: null, depthRejectRate: null,
    nsPerVertex: null, nsPerFragment: null, cycleShare: null, bound: null, boundReason: "",
  };
}

/** The encoder's label when it set one, else the pass's kind and number. */
function passLabel(cmd: CaptureCommand, passIndex: number): string {
  const a = cmd.args;
  const descriptor = a && isObject(a.descriptor) ? a.descriptor : null;
  const label = str(a?.label) || str(descriptor?.label);
  const m = cmd.method;
  const kind = m.startsWith("computeCommandEncoder") ? "Compute"
    : m.startsWith("blitCommandEncoder") ? "Blit"
    : m.startsWith("renderCommandEncoder") || m.startsWith("parallelRenderCommandEncoder") ? "Render Pass"
    : m.startsWith("vkCmdBeginRender") ? "Render Pass"
    : m.startsWith("vkCmdDispatch") ? "Compute" : "Pass";
  return label ? `${kind} ${passIndex}: ${label}` : `${kind} ${passIndex}`;
}

/**
 * Which stage limits the pass. The stage timestamps are the first answer: on a tile-based GPU the
 * vertex and fragment stages of one pass overlap, so the longer of the two is what the pass waits
 * on. The cycle counters are a second opinion and can name the render target write, which the
 * timestamps cannot separate from fragment work.
 */
function decideBound(p: PassMetrics): void {
  const share = p.cycleShare;
  if (share && share.target > 0.4 && share.target > share.fragment) {
    p.bound = "target";
    p.boundReason = `${(100 * share.target).toFixed(0)}% of the pass's GPU cycles went to writing the render target`;
    return;
  }
  const v = p.vertexMs;
  const f = p.fragmentMs;
  if (v !== null && f !== null && (v > 0 || f > 0)) {
    if (f > v * 1.3) {
      p.bound = "fragment";
      p.boundReason = `the fragment stage ran ${f.toFixed(3)} ms against the vertex stage's ${v.toFixed(3)} ms`;
    } else if (v > f * 1.3) {
      p.bound = "vertex";
      p.boundReason = `the vertex stage ran ${v.toFixed(3)} ms against the fragment stage's ${f.toFixed(3)} ms`;
    } else {
      p.bound = "balanced";
      p.boundReason = `the vertex and fragment stages ran for about as long (${v.toFixed(3)} and ${f.toFixed(3)} ms)`;
    }
    return;
  }
  if (share) {
    if (share.fragment > share.vertex * 1.3) {
      p.bound = "fragment";
      p.boundReason = `${(100 * share.fragment).toFixed(0)}% of the pass's GPU cycles were fragment work`;
    } else if (share.vertex > share.fragment * 1.3) {
      p.bound = "vertex";
      p.boundReason = `${(100 * share.vertex).toFixed(0)}% of the pass's GPU cycles were vertex work`;
    }
  }
}

/** "1.84x", "—". */
export function formatRatio(v: number | null, digits = 2): string {
  return v === null ? "—" : v.toFixed(digits);
}

/** "82%", "—". */
export function formatPercent(v: number | null): string {
  return v === null ? "—" : `${(100 * v).toFixed(0)}%`;
}
