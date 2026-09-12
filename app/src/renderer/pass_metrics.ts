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
import type { ArgObject, ArgValue, CaptureCommand, OverdrawMeasurement, PassTiming } from "../shared/protocol.js";
import type { CaptureData } from "./capture_data.js";
import { drawSumsByPass, passSumKey } from "./draw_stats.js";

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
  /** The pass's last command: its end, or the last dispatch of a Vulkan compute run. */
  endIndex: number;
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
  /** Where `overdraw` came from: the GPU's counters, or the pass drawn again with a counting shader. */
  overdrawSource: "counters" | "measured" | null;
  /**
   * The overdraw measurements of the pass (a Metal capture taken with "Overdraw"): the fragments
   * that passed its depth and stencil tests, and every fragment it rasterized, per pixel.
   */
  measuredOverdraw: { depthTested: OverdrawMeasurement | null; rasterized: OverdrawMeasurement | null } | null;
  /** Fragment invocations per primitive out of the clipper: small means microtriangles. */
  fragmentsPerPrimitive: number | null;
  /** Fraction of shaded fragments the depth and stencil tests threw away. */
  depthRejectRate: number | null;
  /** Where `depthRejectRate` came from: the pass's own occlusion query, or the replay's per draw. */
  depthRejectSource: "counters" | "replay" | null;
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
    if (open) open.endIndex = cmd.index;
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
        computeRun.endIndex = cmd.index;
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
    if (p.overdraw !== null) p.overdrawSource = "counters";
    p.fragmentsPerPrimitive = ratio(fragments, primitives);
    // The counter counts fragments that survived the depth and stencil tests; the rest were
    // rejected, early or late.
    p.depthRejectRate = fragments !== null && passed !== null && fragments > 0 ? 1 - passed / fragments : null;
    if (p.depthRejectRate !== null) p.depthRejectSource = "counters";
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

  // Depth rejection from the replay's per-draw occlusion queries, for a pass whose own query the
  // layer could not run: a query cannot span vkCmdExecuteCommands, so a pass that records its draws
  // into secondary command buffers goes unmeasured, while the replay's queries sit inside the
  // secondary around one draw each (replay/src/draw_stats.cpp).
  if (data.drawStats?.length) {
    const sums = drawSumsByPass(data.drawStats);
    for (const p of passes) {
      if (p.compute || p.depthRejectRate !== null) continue;
      const s = sums.get(passSumKey(p.frame, p.commandBuffer, p.passIndex));
      if (!s?.sampled) continue;
      const fragments = counter(p.timing, "fragmentInvocations") ?? (s.counted ? s.fragmentInvocations : null);
      // More samples than shader runs means a multisampled target, where the two do not divide.
      if (fragments === null || fragments <= 0 || s.samplesPassed > fragments) continue;
      p.depthRejectRate = 1 - s.samplesPassed / fragments;
      p.depthRejectSource = "replay";
    }
  }

  // Measured overdraw, timed or not. Where the GPU's counters said nothing, the fragments that
  // passed the depth and stencil tests stand in for the shader invocations: what the pass shades
  // when its tests run before the fragment shader.
  if (data.overdraw?.length) {
    for (const p of passes) {
      if (p.compute) continue;
      const measured = data.overdrawForPass(p.frame, p.commandBuffer, p.passIndex).filter((o) => o.info.measured !== false);
      if (!measured.length) continue;
      const depthTested = measured.find((o) => o.info.depthTested)?.info ?? null;
      const rasterized = measured.find((o) => !o.info.depthTested)?.info ?? null;
      p.measuredOverdraw = { depthTested, rasterized };
      const pixels = depthTested ? depthTested.width * depthTested.height : 0;
      if (p.overdraw === null && depthTested && pixels > 0) {
        p.overdraw = depthTested.fragments / pixels;
        p.overdrawSource = "measured";
      }
    }
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
    commandIndex: cmd.index, endIndex: cmd.index, label: passLabel(cmd, passIndex), frame: cmd.frame ?? 0,
    commandBuffer: cb, passIndex, compute, draws: 0, vertices: 0,
    pixels: target?.pixels ?? 0, samples: target?.samples ?? 1,
    timing: null, durationMs: null, vertexMs: null, fragmentMs: null,
    overdraw: null, overdrawSource: null, measuredOverdraw: null, fragmentsPerPrimitive: null, depthRejectRate: null,
    depthRejectSource: null,
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

// ---------------------------------------------------------------------------------------------
// What the numbers mean. The GPU Bottlenecks report and the MCP server (src/mcp/) both say it,
// so it is written once, here.

export const BOUND_LABEL: Record<Bound, string> = {
  vertex: "Vertex bound",
  fragment: "Fragment bound",
  target: "Target write bound",
  balanced: "Balanced",
};

/** What to try first for a pass limited by each stage. */
export const BOUND_ADVICE: Record<Bound, string> = {
  vertex: "Cut vertices or vertex-stage work: mesh level of detail at distance, fewer or cheaper vertex attributes, and per-fragment rather than per-vertex evaluation of anything the fragment stage could do itself.",
  fragment: "Cut fragments or fragment-stage work: fewer overlapping surfaces, a smaller render target, cheaper texture sampling, and simpler shader maths.",
  target: "The pass spends its time writing the attachment rather than shading it. A smaller target, fewer targets, or a store action of DontCare on anything nothing reads afterwards.",
  balanced: "Neither stage dominates. The cheapest win is usually to remove work from the pass entirely: merge it with a neighbour, or skip it when nothing reads its output.",
};

export interface PassAdvice {
  severity: "high" | "medium" | "low";
  title: string;
  body: string;
}

/** The measured problems of one pass, worst first. */
export function passAdvice(p: PassMetrics): PassAdvice[] {
  const out: PassAdvice[] = [];
  if (p.overdraw !== null && p.overdraw > OVERDRAW_LIMIT) {
    out.push({
      severity: "high",
      title: `Each pixel is shaded ${formatRatio(p.overdraw)} times`,
      body: `A frame doing well sits near ${HEALTHY_OVERDRAW}. Overdraw this high is usually transparent surfaces stacking up, a full-screen effect drawn more than once, or opaque geometry drawn back to front so the depth test cannot reject anything.`,
    });
  }
  if (p.fragmentsPerPrimitive !== null && p.fragmentsPerPrimitive < MICROTRIANGLE_LIMIT) {
    out.push({
      severity: "high",
      title: `Triangles cover ${formatRatio(p.fragmentsPerPrimitive)} fragments each`,
      body: `The rasterizer shades in 2x2 quads, so a triangle covering fewer than ${MICROTRIANGLE_LIMIT} fragments wastes lanes it has already paid for. This is dense geometry drawn small: add mesh level of detail, or cull the meshes that are far enough away to be smaller than their own triangles.`,
    });
  }
  if (p.depthRejectRate !== null && p.overdraw !== null && p.overdraw > 1.5 && p.depthRejectRate < LOW_REJECTION_RATE) {
    out.push({
      severity: "medium",
      title: `The depth test rejects only ${formatPercent(p.depthRejectRate)} of shaded fragments`,
      body: "Fragments are being shaded and then thrown away by something later, or not thrown away at all. Drawing opaque geometry front to back lets the depth test reject work before the fragment shader runs; a depth prepass does the same for a scene that cannot be sorted.",
    });
  }
  if (p.bound === "target" && p.cycleShare) {
    out.push({
      severity: "medium",
      title: "Most of the pass is spent writing the render target",
      body: "Fewer or smaller attachments, or a store action of DontCare on the ones nothing reads afterwards. On a tile-based GPU a target that is only read by the pass that follows never has to reach memory at all.",
    });
  }
  return out;
}

/** The frame's verdict in one sentence, from the stage times summed over every timed pass. */
export function frameStageVerdict(m: FrameMetrics): string {
  const staged = m.vertexMs + m.fragmentMs;
  if (staged <= 0) return "The stage split is not available for this capture, so the frame's balance cannot be stated.";
  const fragmentShare = m.fragmentMs / staged;
  if (fragmentShare > 0.65) return `This frame is fragment bound: ${formatPercent(fragmentShare)} of stage time is fragment work.`;
  if (fragmentShare < 0.35) return `This frame is vertex bound: ${formatPercent(1 - fragmentShare)} of stage time is vertex work.`;
  return `Vertex and fragment work are close to balanced (${formatPercent(fragmentShare)} fragment).`;
}

/** "1.84x", "—". */
export function formatRatio(v: number | null, digits = 2): string {
  return v === null ? "—" : v.toFixed(digits);
}

/** "82%", "—". */
export function formatPercent(v: number | null): string {
  return v === null ? "—" : `${(100 * v).toFixed(0)}%`;
}
