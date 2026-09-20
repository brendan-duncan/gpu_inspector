// A shader edited and run in the capture: the frame replayed with other code for some pipelines'
// stages (`vkinsp_replay --replace`, `dxinsp_replay --replace`), and what its render targets hold
// then against what the capture read back.
//
// Editing a shader in the running application (INSPECT, "Editing a shader") shows the edit in the
// next frame the application draws, which is the right place to look while it is running. A capture
// is what is left when it is not: a file somebody sent, a frame that took an hour to reach, a bug
// that shows once a day. There the only frame there is is the captured one, so that is the frame
// the edit runs in, and because every render target of it was read back, the answer is exact: which
// targets changed, in how many pixels, and what they look like now.
//
// No DOM here: the renderer and the tests both build requests and read results.

const REQUEST_MAGIC = "REPLACE 1\n";
const TARGETS_MAGIC = "TARGETS 1\n";

/** One stage of one pipeline given other code: SPIR-V for a Vulkan capture, a DXBC/DXIL container for a D3D12 one. */
export interface ShaderReplacement {
  pipeline: number;
  /** "vertex", "fragment", "compute", ...: the name the capture keeps the stage's code under. */
  stage: string;
  code: Uint8Array;
}

/** The request file the replay tools read (ReadReplaceRequest in their main.cpp). */
export function encodeReplaceRequest(replacements: ShaderReplacement[]): Uint8Array {
  let offset = 0;
  const manifest = {
    format: "gpu-inspector-shader-replacement",
    replacements: replacements.map((r) => {
      const payload = [offset, r.code.byteLength];
      offset += r.code.byteLength;
      return { pipeline: r.pipeline, stage: r.stage, payload };
    }),
  };
  const json = new TextEncoder().encode(JSON.stringify(manifest));
  const magic = new TextEncoder().encode(REQUEST_MAGIC);
  const out = new Uint8Array(magic.byteLength + 4 + json.byteLength + offset);
  out.set(magic, 0);
  new DataView(out.buffer).setUint32(magic.byteLength, json.byteLength, true);
  out.set(json, magic.byteLength + 4);
  let pos = magic.byteLength + 4 + json.byteLength;
  for (const r of replacements) {
    out.set(r.code, pos);
    pos += r.code.byteLength;
  }
  return out;
}

/** A render target of the replayed frame, against the capture's read-back of it. */
export interface ReplayedTarget {
  /** The keys a captured texture is found by (CaptureTextureInfo). */
  image: number;
  commandBuffer: number;
  frame: number;
  passIndex: number;
  attachment: number;
  aspect: string;
  format: string;
  width: number;
  height: number;
  /** False when the replay could not compare it; `note` says why. */
  compared: boolean;
  texels: number;
  differingTexels: number;
  maxByteDelta: number;
  note?: string;
  /** What it holds now, in the layout of the capture's own read-back; only for a target that differs. */
  pixels: Uint8Array | null;
}

export interface ReplayedTargets {
  device: string;
  targets: ReplayedTarget[];
  /** What the replay could not rebuild: a replaced pipeline that was refused is named here. */
  problems: string[];
}

/** Parses `--target-data` (WriteTargetData in the replay tools' main.cpp). */
export function parseReplayedTargets(bytes: Uint8Array): ReplayedTargets {
  const magic = new TextEncoder().encode(TARGETS_MAGIC);
  if (bytes.byteLength < magic.byteLength + 4 || !magic.every((b, i) => bytes[i] === b)) throw new Error("Not the render targets of a replay.");
  const length = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(magic.byteLength, true);
  const start = magic.byteLength + 4;
  if (start + length > bytes.byteLength) throw new Error("The replay's render targets are truncated.");
  let json: Record<string, unknown>;
  try {
    json = JSON.parse(new TextDecoder().decode(bytes.subarray(start, start + length))) as Record<string, unknown>;
  } catch (e) {
    throw new Error(`The replay's render targets are not valid JSON: ${(e as Error).message}`);
  }
  if (json.format !== "gpu-inspector-replayed-targets") throw new Error("Not the render targets of a replay.");
  const base = start + length;
  const num = (v: unknown): number => (typeof v === "number" ? v : 0);
  const str = (v: unknown): string => (typeof v === "string" ? v : "");
  const targets = (Array.isArray(json.targets) ? json.targets : []).map((raw): ReplayedTarget => {
    const t = raw as Record<string, unknown>;
    const payload = Array.isArray(t.payload) && t.payload.length === 2 ? [num(t.payload[0]), num(t.payload[1])] : null;
    const inside = payload && base + payload[0] + payload[1] <= bytes.byteLength;
    return {
      image: num(t.image), commandBuffer: num(t.commandBuffer), frame: num(t.frame), passIndex: num(t.passIndex), attachment: num(t.attachment),
      aspect: str(t.aspect), format: str(t.format), width: num(t.width), height: num(t.height), compared: t.compared === true,
      texels: num(t.texels), differingTexels: num(t.differingTexels), maxByteDelta: num(t.maxByteDelta),
      ...(typeof t.note === "string" ? { note: t.note } : {}),
      pixels: payload && inside ? bytes.slice(base + payload[0], base + payload[0] + payload[1]) : null,
    };
  });
  return {
    device: str(json.device), targets,
    problems: Array.isArray(json.problems) ? json.problems.filter((p): p is string => typeof p === "string") : [],
  };
}

/** The targets the edit changed, most changed first. */
export function changedTargets(result: ReplayedTargets): ReplayedTarget[] {
  return result.targets.filter((t) => t.compared && t.differingTexels > 0).sort((a, b) => b.differingTexels / Math.max(1, b.texels) - a.differingTexels / Math.max(1, a.texels));
}

/**
 * Problems that are about a replaced pipeline. A replacement the driver refuses (its inputs no
 * longer match the stage before it, its bindings no longer match the layout) leaves the pipeline
 * out of the replay, and then its draws draw nothing: the targets differ, but not because of what
 * the edit computes. That has to be said before anything is shown as the edit's effect.
 */
export function replacementProblems(result: ReplayedTargets, pipelines: number[]): string[] {
  return result.problems.filter((p) => pipelines.some((id) => new RegExp(`\\bpipeline ${id}\\b`, "i").test(p)));
}

/** One line for the status bar. */
export function replayedTargetsSummary(result: ReplayedTargets, pipelines: number[]): string {
  const refused = replacementProblems(result, pipelines);
  if (refused.length) return `the edited shader was not accepted: ${refused[0]}`;
  const compared = result.targets.filter((t) => t.compared);
  const changed = changedTargets(result);
  if (!compared.length) return "the frame replayed with the edit, but the capture holds no render targets to compare";
  if (!changed.length) return `the frame replayed with the edit, and all ${compared.length} render targets are as they were: nothing the frame shows depends on the change`;
  const most = changed[0];
  return `the edit changed ${changed.length} of ${compared.length} render targets, the most ${(100 * most.differingTexels / Math.max(1, most.texels)).toFixed(1)}% of its pixels (replayed on ${result.device || "this GPU"})`;
}
