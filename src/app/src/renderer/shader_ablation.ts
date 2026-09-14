// What the parts of a shader cost, measured by ablation: vulkan/spirv_ablate.ts writes variants of a
// stage with one part taken out, `vkinsp_replay --ablate` times the draw with each (src/replay/src/
// ablation.cpp), and a part's cost is the time the draw saved without it. The Shader Flame Graph
// (frame_cost_tree.ts) sizes a measured stage's functions and lines by these instead of by the model.
//
// No DOM here: GPU Inspector's renderer and the MCP server both build requests and read results.
import type { AblationPart, AblationPlan } from "./vulkan/spirv_ablate.js";
import type { ShaderStage } from "./vulkan/spirv_reflect.js";

const MAGIC = "ABLATE 1\n";

/** One draw or dispatch to time with variants of one of its stages. */
export interface AblationTarget {
  command: number;
  stage: ShaderStage;
  /** Draws issued between one pair of timestamps, so a cheap draw is long enough to time (results are per draw). */
  repeat?: number;
  variants: { name: string; spirv: Uint8Array }[];
}

/** The request file `vkinsp_replay --ablate` reads (ReadAblationRequest in src/replay/src/main.cpp). */
export function encodeAblationRequest(targets: AblationTarget[], rounds: number): Uint8Array {
  const payloads: Uint8Array[] = [];
  let offset = 0;
  const manifest = {
    format: "gpu-inspector-ablation-request", rounds,
    targets: targets.map((t) => ({
      command: t.command, stage: t.stage, repeat: Math.max(1, Math.round(t.repeat ?? 1)),
      variants: t.variants.map((v) => {
        const payload = [offset, v.spirv.byteLength];
        payloads.push(v.spirv);
        offset += v.spirv.byteLength;
        return { name: v.name, payload };
      }),
    })),
  };
  const json = new TextEncoder().encode(JSON.stringify(manifest));
  const magic = new TextEncoder().encode(MAGIC);
  const out = new Uint8Array(magic.byteLength + 4 + json.byteLength + offset);
  out.set(magic, 0);
  new DataView(out.buffer).setUint32(magic.byteLength, json.byteLength, true);
  out.set(json, magic.byteLength + 4);
  let pos = magic.byteLength + 4 + json.byteLength;
  for (const p of payloads) {
    out.set(p, pos);
    pos += p.byteLength;
  }
  return out;
}

/** One pipeline's times at a target: the median of the rounds, and each round. */
export interface AblationTiming {
  name: string;
  measured: boolean;
  ms: number;
  samples: number[];
  note?: string;
}

export interface AblationTargetResult {
  command: number;
  stage: string;
  pipeline: number;
  frame: number;
  commandBuffer: number;
  passIndex: number;
  rounds: number;
  baseline: AblationTiming;
  variants: AblationTiming[];
  note?: string;
}

export interface AblationResultFile {
  device: string;
  targets: AblationTargetResult[];
  problems: string[];
}

/** Parses `vkinsp_replay --ablate-data` (WriteAblationData in src/replay/src/main.cpp). */
export function parseAblationResult(input: Uint8Array | string): AblationResultFile {
  const text = typeof input === "string" ? input : new TextDecoder().decode(input);
  let json: Record<string, unknown>;
  try {
    json = JSON.parse(text) as Record<string, unknown>;
  } catch (e) {
    throw new Error(`The ablation timings are not valid JSON: ${(e as Error).message}`);
  }
  if (json.format !== "gpu-inspector-ablation") throw new Error("Not ablation timings from vkinsp_replay.");
  const num = (v: unknown): number => (typeof v === "number" ? v : 0);
  const timing = (raw: unknown): AblationTiming => {
    const t = (raw ?? {}) as Record<string, unknown>;
    return {
      name: typeof t.name === "string" ? t.name : "", measured: t.measured === true, ms: num(t.ms),
      samples: Array.isArray(t.samples) ? t.samples.map(num) : [], ...(typeof t.note === "string" ? { note: t.note } : {}),
    };
  };
  const targets = (Array.isArray(json.targets) ? json.targets : []).map((raw): AblationTargetResult => {
    const t = raw as Record<string, unknown>;
    return {
      command: num(t.command), stage: typeof t.stage === "string" ? t.stage : "", pipeline: num(t.pipeline), frame: num(t.frame),
      commandBuffer: num(t.commandBuffer), passIndex: num(t.passIndex), rounds: num(t.rounds), baseline: timing(t.baseline),
      variants: (Array.isArray(t.variants) ? t.variants : []).map(timing), ...(typeof t.note === "string" ? { note: t.note } : {}),
    };
  });
  return {
    device: typeof json.device === "string" ? json.device : "", targets,
    problems: Array.isArray(json.problems) ? json.problems.filter((p): p is string => typeof p === "string") : [],
  };
}

/** A part of a stage with what taking it out saved. */
export interface MeasuredPart extends AblationPart {
  /** Milliseconds per draw the draw took less without the part; null when its variant was not timed. */
  savedMs: number | null;
  /**
   * What the part does itself: what it saved beyond the most any part feeding it saved (a line that
   * gathers the results of expensive lines saves their time too). A function's is what it saved.
   */
  ownMs: number | null;
  note?: string;
}

/** A shader stage of a pipeline measured by ablation at one of its draws, as a capture keeps it. */
export interface ShaderAblation {
  pipeline: number;
  stage: ShaderStage;
  entryPoint: string;
  /** The draw or dispatch it was measured at. */
  command: number;
  device: string;
  rounds: number;
  /** Draws issued in each timed span. */
  repeat?: number;
  /** The draw's time with the shader as captured (median of the rounds). */
  baselineMs: number;
  /** How far apart the baseline's rounds were (the median absolute deviation): savings below it are noise. */
  noiseMs: number;
  /** What the stage's own work took: the baseline less the draw without the stage's outputs; null when not measured. */
  stageMs: number | null;
  parts: MeasuredPart[];
  /** Parts that have no variant, and why. */
  skipped: (AblationPart & { reason: string })[];
  note?: string;
  /** What the measurement could not check (no spirv-val, an invalid captured module). */
  notes?: string[];
}

/** What a view asks to measure: a stage of the pipeline bound at a draw or dispatch. */
export interface ShaderMeasureTarget {
  command: number;
  pipeline: number;
  stage: ShaderStage;
  entryPoint: string;
  /** The stage's SPIR-V as the capture holds it. */
  spirv: Uint8Array;
}

/** The key a measured stage is found by. */
export function ablationKey(pipeline: number, stage: string, entryPoint: string): string {
  return `${pipeline}|${stage}|${entryPoint}`;
}

function medianAbsoluteDeviation(samples: number[]): number {
  if (samples.length < 2) return 0;
  const sorted = samples.slice().sort((a, b) => a - b);
  const median = sorted[Math.floor(sorted.length / 2)];
  const deviations = samples.map((s) => Math.abs(s - median)).sort((a, b) => a - b);
  return deviations[Math.floor(deviations.length / 2)];
}

/** Combines a plan with the replay's timings of it. */
export function measuredAblation(pipeline: number, stage: ShaderStage, entryPoint: string, plan: AblationPlan, result: AblationTargetResult,
                                 device: string): ShaderAblation {
  const baseline = result.baseline;
  const out: ShaderAblation = {
    pipeline, stage, entryPoint, command: result.command, device, rounds: result.rounds, baselineMs: baseline.ms,
    noiseMs: medianAbsoluteDeviation(baseline.samples), stageMs: null, parts: [], skipped: plan.skipped,
    ...(result.note ? { note: result.note } : {}),
  };
  const saved = plan.variants.map((_, i) => {
    const timing = result.variants[i];
    return baseline.measured && timing?.measured ? baseline.ms - timing.ms : null;
  });
  plan.variants.forEach((variant, i) => {
    const timing = result.variants[i];
    const { spirv: _spirv, edits: _edits, upstream, ...part } = variant;
    if (variant.kind === "stage") {
      out.stageMs = saved[i];
      return;
    }
    let own = saved[i];
    if (own !== null && variant.kind === "line") {
      const fed = Math.max(0, ...upstream.map((k) => saved[k] ?? 0));
      own = Math.max(0, own - fed);
    }
    out.parts.push({ ...part, savedMs: saved[i], ownMs: own, ...(timing?.note ? { note: timing.note } : {}) });
  });
  return out;
}

/**
 * A part's share of its stage, clamped to [0, 1]: a function's by what taking it out saved, a line's by
 * what it does itself (ownMs), over what taking the stage out saved.
 */
export function partShare(a: ShaderAblation, part: MeasuredPart): number | null {
  const ms = part.kind === "line" ? part.ownMs : part.savedMs;
  if (ms === null || a.stageMs === null || a.stageMs <= 0) return null;
  return Math.min(1, Math.max(0, ms / a.stageMs));
}
