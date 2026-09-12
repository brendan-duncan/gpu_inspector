// Per-draw timing and counters: every draw and dispatch of a frame measured by replaying it on this
// machine's GPU with a timestamp pair and a pipeline statistics query around each one
// (`vkinsp_replay --draws`, replay/src/draw_stats.cpp).
//
// The counters are exact — vertex and fragment shader invocations, primitives, compute invocations,
// indirect draws included, which nothing in the capture itself reports per draw. The times are not
// what a draw costs alone: the GPU pipelines consecutive draws, so their spans overlap and add up
// to more than the pass takes. They say what share of a pass a draw accounts for, which is what the
// Shader Flame Graph splits a pass's measured duration by (frame_cost_tree.ts).
import type { OverdrawPassKey } from "./overdraw.js";

/** One draw or dispatch, keyed by the command index it has in the capture. */
export interface DrawStat {
  command: number;
  frame: number;
  commandBuffer: number;
  /** The render pass it is in; absent for a dispatch outside one. */
  passIndex?: number;
  /** The replay timed it (a queue that writes timestamps). */
  timed: boolean;
  ms: number;
  /** The replay counted it (a device with pipeline statistics queries). */
  counted: boolean;
  vertexInvocations: number;
  primitives: number;
  fragmentInvocations: number;
  computeInvocations: number;
}

export interface DrawStatsFile {
  device: string;
  /** Why some measurements are missing (no timestamps, no statistics queries, too many draws). */
  note: string;
  draws: DrawStat[];
  /** What the replay could not rebuild (the first hundred). */
  problems: string[];
}

/** The sentinel the replay writes for a dispatch that is in no render pass. */
const NO_PASS = 0xffffffff;

/** Parses `vkinsp_replay --draw-data` (replay/src/main.cpp). */
export function parseDrawStats(input: Uint8Array | string): DrawStatsFile {
  const text = typeof input === "string" ? input : new TextDecoder().decode(input);
  let json: Record<string, unknown>;
  try {
    json = JSON.parse(text) as Record<string, unknown>;
  } catch (e) {
    throw new Error(`The draw measurements are not valid JSON: ${(e as Error).message}`);
  }
  if (json.format !== "gpu-inspector-draw-stats") throw new Error("Not draw measurements from vkinsp_replay.");
  const num = (v: unknown): number => (typeof v === "number" ? v : 0);
  const draws = (Array.isArray(json.draws) ? json.draws : []).map((raw): DrawStat => {
    const d = raw as Record<string, unknown>;
    const pass = num(d.passIndex);
    return {
      command: num(d.command), frame: num(d.frame), commandBuffer: num(d.commandBuffer),
      ...(pass === NO_PASS ? {} : { passIndex: pass }),
      timed: d.timed === true, ms: num(d.ms), counted: d.counted === true,
      vertexInvocations: num(d.vertexInvocations), primitives: num(d.primitives),
      fragmentInvocations: num(d.fragmentInvocations), computeInvocations: num(d.computeInvocations),
    };
  });
  return {
    device: typeof json.device === "string" ? json.device : "",
    note: typeof json.note === "string" ? json.note : "",
    draws,
    problems: Array.isArray(json.problems) ? json.problems.filter((p): p is string => typeof p === "string") : [],
  };
}

/** The measurements by command index, for the cost tree's lookups. */
export function drawStatsByCommand(draws: DrawStat[]): Map<number, DrawStat> {
  const out = new Map<number, DrawStat>();
  for (const d of draws) out.set(d.command, d);
  return out;
}

/** The measured draws of one render pass. */
export function drawStatsForPass(draws: DrawStat[], key: OverdrawPassKey): DrawStat[] {
  return draws.filter((d) => d.frame === key.frame && d.commandBuffer === key.commandBuffer && d.passIndex === key.passIndex);
}

/** One line: what the replay measured, for a status line or a note. */
export function drawStatsSummary(file: DrawStatsFile): string {
  const timed = file.draws.filter((d) => d.timed).length;
  const counted = file.draws.filter((d) => d.counted).length;
  const fragments = file.draws.reduce((sum, d) => sum + d.fragmentInvocations, 0);
  return `${file.draws.length} draws and dispatches measured (${timed} timed, ${counted} counted), `
    + `${fragments.toLocaleString()} fragment shader invocations${file.device ? `, replayed on ${file.device}` : ""}`;
}
