// Hardware counters per pass and per draw: the GPU vendor's own counters, read by replaying the
// capture on this machine's GPU (`vkinsp_replay --counters`, src/replay/src/hw_counters.cpp). They
// are what docs/PROFILING.md calls "the limiters" — which unit inside the shader core a pass
// saturates (SM throughput, memory bandwidth, cache, occupancy) — and the counterpart of what
// Nsight Graphics shows, for a Vulkan capture. Two backends supply them: NVIDIA's Nsight Perf SDK
// (per pass and per draw) and VK_KHR_performance_query (per draw). This parses the JSON the tool
// writes; GPU Bottlenecks shows it (bottleneck_report.ts, pass_metrics.ts).

/** One counter's identity and what it measures. */
export interface HwCounterInfo {
  name: string;
  description: string;
  /** The hardware unit it belongs to (NvPerf) or its category (KHR): "sm", "dram", "l1tex"... */
  category: string;
  /** How to format its value: "percent", "ns", "bytes", "bytes/s", "count", "ratio", "cycles", "hertz", "watts". */
  unit: string;
}

/** The counters of one range (a render pass, or a draw/dispatch), aligned with HwCounters.counters. */
export interface HwCounterRange {
  command: number;
  frame: number;
  commandBuffer: number;
  /** The render pass the range is in; absent for a dispatch outside one. */
  passIndex?: number;
  /** One value per counter; null where that counter did not evaluate for this range. */
  values: (number | null)[];
}

export interface HwCounters {
  device: string;
  /** "nvperf" or "khr". */
  backend: string;
  /** NvPerf: the chip the counters are for ("AD103"). */
  chip: string;
  /** Times the frame was replayed to collect them all. */
  rounds: number;
  counters: HwCounterInfo[];
  passes: HwCounterRange[];
  draws: HwCounterRange[];
  /** With --list-counters: every counter the device offers (no values). */
  available: HwCounterInfo[];
  notes: string[];
  problems: string[];
}

/** The sentinel the replay writes for a range in no render pass. */
const NO_PASS = 0xffffffff;

function counterInfo(raw: unknown): HwCounterInfo {
  const r = (raw ?? {}) as Record<string, unknown>;
  return {
    name: typeof r.name === "string" ? r.name : "",
    description: typeof r.description === "string" ? r.description : "",
    category: typeof r.category === "string" ? r.category : "",
    unit: typeof r.unit === "string" ? r.unit : "count",
  };
}

function counterRange(raw: unknown): HwCounterRange {
  const r = (raw ?? {}) as Record<string, unknown>;
  const num = (v: unknown): number => (typeof v === "number" ? v : 0);
  const pass = num(r.passIndex);
  return {
    command: num(r.command), frame: num(r.frame), commandBuffer: num(r.commandBuffer),
    ...(pass === NO_PASS ? {} : { passIndex: pass }),
    values: Array.isArray(r.values) ? r.values.map((v) => (typeof v === "number" ? v : null)) : [],
  };
}

/** Parses `vkinsp_replay --counter-data` (WriteCounterData in src/replay/src/main.cpp). */
export function parseHwCounters(input: Uint8Array | string): HwCounters {
  const text = typeof input === "string" ? input : new TextDecoder().decode(input);
  let json: Record<string, unknown>;
  try {
    json = JSON.parse(text) as Record<string, unknown>;
  } catch (e) {
    throw new Error(`The hardware counters are not valid JSON: ${(e as Error).message}`);
  }
  if (json.format !== "gpu-inspector-hw-counters") throw new Error("Not hardware counters from vkinsp_replay.");
  const str = (v: unknown): string => (typeof v === "string" ? v : "");
  const list = (v: unknown): unknown[] => (Array.isArray(v) ? v : []);
  return {
    device: str(json.device), backend: str(json.backend), chip: str(json.chip),
    rounds: typeof json.rounds === "number" ? json.rounds : 0,
    counters: list(json.counters).map(counterInfo),
    passes: list(json.passes).map(counterRange),
    draws: list(json.draws).map(counterRange),
    available: list(json.available).map(counterInfo),
    notes: list(json.notes).filter((n): n is string => typeof n === "string"),
    problems: list(json.problems).filter((p): p is string => typeof p === "string"),
  };
}

/** The value of a named counter for a range, or null when it is absent or did not evaluate. */
export function counterValue(file: HwCounters, range: HwCounterRange, name: string): number | null {
  const i = file.counters.findIndex((c) => c.name === name);
  if (i < 0 || i >= range.values.length) return null;
  return range.values[i];
}

/**
 * The counters by the render pass they measured, keyed as pass_metrics.ts keys passes. A compute
 * pass carries the same (frame, command buffer, pass index) triple as the render pass beside it, so
 * a caller must look up only render passes; counter ranges wrap render passes alone.
 */
export function hwCountersByPass(file: HwCounters): Map<string, HwCounterRange> {
  const out = new Map<string, HwCounterRange>();
  for (const r of file.passes) {
    if (r.passIndex === undefined) continue;
    out.set(`${r.frame}:${r.commandBuffer}:${r.passIndex}`, r);
  }
  return out;
}

/**
 * A short column heading for a counter. The vendor's names are long and share prefixes
 * ("sm__throughput", "sm__warps_active", "sm__pipe_alu_cycles_active" would all shorten to "sm"),
 * so this keeps the unit and what it measures and drops the submetric and the "cycles active"
 * tail: "sm throughput", "sm warps active", "sm pipe alu".
 */
export function counterLabel(name: string): string {
  const base = (name.split(".")[0] || name).replace(/_cycles_active$/, "");
  return base.replace(/__/g, " ").replace(/_/g, " ").trim() || name;
}

/** A value formatted with its unit, for a table cell or tooltip. */
export function formatCounter(value: number | null, unit: string): string {
  if (value === null) return "—";
  switch (unit) {
    case "percent": return `${value.toFixed(1)}%`;
    case "ns": return value >= 1e6 ? `${(value / 1e6).toFixed(3)} ms` : value >= 1e3 ? `${(value / 1e3).toFixed(2)} µs` : `${value.toFixed(0)} ns`;
    case "bytes": return formatBytes(value);
    case "bytes/s": return `${formatBytes(value)}/s`;
    case "cycles": return value.toLocaleString(undefined, { maximumFractionDigits: 0 });
    case "ratio": return value.toFixed(3);
    default: return value.toLocaleString(undefined, { maximumFractionDigits: value < 10 ? 2 : 0 });
  }
}

function formatBytes(v: number): string {
  const units = ["B", "KB", "MB", "GB", "TB"];
  let u = 0;
  while (v >= 1024 && u < units.length - 1) { v /= 1024; u++; }
  return `${v.toFixed(u === 0 ? 0 : 1)} ${units[u]}`;
}

// ---------------------------------------------------------------------------------------------
// What the counters say limits a pass. The rest of GPU Bottlenecks answers "which stage", inferred
// from overdraw and triangle size; these answer "which unit", measured. A throughput counter is a
// percentage of what that unit can sustain, so the highest one is the unit closest to its limit.

/** How hard a unit has to be working before it is called the limit. */
export const SATURATED_PERCENT = 60;
/** Below `SATURATED_PERCENT` but still the busiest unit worth naming. */
export const BUSY_PERCENT = 30;
/** Occupancy under this, with nothing else busy, means the pass is waiting rather than working. */
export const LOW_OCCUPANCY_PERCENT = 30;

export type LimiterKind = "shader" | "memory" | "cache" | "occupancy" | "unsaturated";

/** What the counters concluded about one pass. */
export interface PassLimiter {
  kind: LimiterKind;
  /** The unit in words: "the shader core", "memory bandwidth", "the L2 cache". */
  label: string;
  /** The counter it was read from, and its value; empty and 0 for "unsaturated". */
  counter: string;
  percent: number;
  /** Whether the unit is at or near its sustainable limit, rather than merely the busiest. */
  saturated: boolean;
  /**
   * The heaviest shader stage of the pass by register count, where a capture carries the driver's
   * compiler statistics (src/vulkan/src/shader_statistics.h). Registers are what usually holds
   * occupancy down, so a latency-bound pass can say not just that few warps ran but why.
   */
  registers?: { stage: string; count: number };
}

/** Registers a pass's heaviest stage uses, above which occupancy is worth blaming on them. */
export const HIGH_REGISTER_COUNT = 32;

/** Which unit a counter measures, and how to say it. */
function unitOfCounter(name: string): { kind: LimiterKind; label: string } {
  if (/warps_active/.test(name)) return { kind: "occupancy", label: "occupancy" };
  if (/dram/.test(name)) return { kind: "memory", label: "memory bandwidth" };
  if (/^lts__/.test(name)) return { kind: "cache", label: "the L2 cache" };
  if (/^l1tex__/.test(name)) return { kind: "cache", label: "the L1 and texture cache" };
  const pipe = /^sm__pipe_([a-z0-9]+)_/.exec(name);
  if (pipe) return { kind: "shader", label: `the ${pipe[1].toUpperCase()} pipe` };
  if (/^sm__|^smsp__/.test(name)) return { kind: "shader", label: "the shader core" };
  return { kind: "shader", label: counterLabel(name) };
}

/**
 * What the counters say limits one pass, or null when none of them is a percentage of peak (a
 * counter set of raw totals says how much happened, not how close to the limit it came).
 */
export function passLimiter(file: HwCounters, range: HwCounterRange): PassLimiter | null {
  const percents = file.counters
    .map((c, i) => ({ c, value: range.values[i] }))
    .filter((x): x is { c: HwCounterInfo; value: number } => x.c.unit === "percent" && typeof x.value === "number");
  if (!percents.length) return null;

  const occupancy = percents.find((x) => /warps_active/.test(x.c.name));
  const units = percents.filter((x) => !/warps_active/.test(x.c.name));
  if (!units.length) return null;
  const top = units.reduce((a, b) => (b.value > a.value ? b : a));
  const { kind, label } = unitOfCounter(top.c.name);

  if (top.value >= SATURATED_PERCENT) return { kind, label, counter: top.c.name, percent: top.value, saturated: true };
  // A unit doing real work is the more useful answer than low occupancy, so it is checked first:
  // a pass can be both meaningfully busy somewhere and short of warps, and the busy unit is what
  // there is to act on.
  if (top.value >= BUSY_PERCENT) return { kind, label, counter: top.c.name, percent: top.value, saturated: false };
  // Nothing even moderately busy: a pass with few warps in flight is waiting, not working.
  if (occupancy && occupancy.value < LOW_OCCUPANCY_PERCENT) {
    return { kind: "occupancy", label: "occupancy", counter: occupancy.c.name, percent: occupancy.value, saturated: false };
  }
  return { kind: "unsaturated", label: "no unit", counter: "", percent: top.value, saturated: false };
}

/**
 * The heaviest stage of a pipeline by register count, from the driver's compiler statistics as the
 * layer attached them (`updates.executables`). Null when the capture was taken without them, or the
 * driver reported no register count — the names are the driver's, so this matches loosely.
 */
export function heaviestStage(executables: unknown): { stage: string; count: number } | null {
  if (!Array.isArray(executables)) return null;
  let best: { stage: string; count: number } | null = null;
  for (const raw of executables) {
    const e = raw as Record<string, unknown>;
    const stats = Array.isArray(e.statistics) ? e.statistics : [];
    for (const s of stats) {
      const st = s as Record<string, unknown>;
      if (typeof st.name !== "string" || !/register/i.test(st.name)) continue;
      const count = typeof st.value === "number" ? st.value : Number.NaN;
      if (!Number.isFinite(count)) continue;
      const stage = Array.isArray(e.stages) && e.stages.length ? String(e.stages[0]) : String(e.name ?? "stage");
      if (!best || count > best.count) best = { stage, count };
    }
  }
  return best;
}

/** The verdict in words, for a table cell or a card. */
export const LIMITER_LABEL: Record<LimiterKind, string> = {
  shader: "Shader bound",
  memory: "Bandwidth bound",
  cache: "Cache bound",
  occupancy: "Latency bound",
  unsaturated: "Nothing saturated",
};

/** What to try first for a pass each unit limits. */
export const LIMITER_ADVICE: Record<LimiterKind, string> = {
  shader: "The shader core is the limit, so the work per invocation is what to cut: simpler maths, fewer instructions on the hot path, and anything that can move to a cheaper stage or be precomputed.",
  memory: "The pass is moving more data than the memory system can feed it. Smaller or better compressed textures, fewer or narrower render targets, and fewer full-resolution passes over memory.",
  cache: "The pass is limited by cache traffic rather than by arithmetic. Sampling that stays local (mips, smaller textures, better texture layout) and fewer scattered reads help more than cheaper shader maths.",
  occupancy: "No unit is near its limit and few warps are in flight, so the pass is waiting rather than working: long dependency chains, register pressure limiting occupancy, or too little work to fill the GPU.",
  // Filled in per pass by limiterAdvice when the compiler statistics name the stage responsible.
  unsaturated: "No unit measured is close to its limit, so the pass is probably too small to fill the GPU, or is waiting on something outside it. Merging it with a neighbour usually beats optimising it.",
};

/**
 * What to try for a pass, with the register count folded in where it explains the verdict. A pass
 * short of warps whose shader is register-heavy has a cause, not just a symptom, and that changes
 * the advice from "find more work" to "cut the registers this stage holds".
 */
export function limiterAdvice(limiter: PassLimiter): string {
  const base = LIMITER_ADVICE[limiter.kind];
  const r = limiter.registers;
  if (!r) return base;
  if (limiter.kind === "occupancy" && r.count >= HIGH_REGISTER_COUNT) {
    return `Its ${r.stage} stage uses ${r.count} registers, which is what is holding occupancy down: `
      + "the more registers a stage holds, the fewer of its threads the GPU can keep in flight. Shorter live "
      + "ranges, fewer variables held across a long computation, and less aggressive unrolling all free registers up. "
      + base;
  }
  if (limiter.kind === "occupancy") {
    return `${base} Its heaviest stage (${r.stage}) uses only ${r.count} registers, so register pressure is not the `
      + "cause: look at dependency chains and at whether the pass has enough work to fill the GPU.";
  }
  return base;
}

/** One line summarising what was collected, for a status line. */
export function hwCountersSummary(file: HwCounters): string {
  if (!file.backend) return `no hardware counters (${file.notes[0] ?? "none available"})`;
  const where = file.backend === "nvperf" ? `NVIDIA ${file.chip}` : "VK_KHR_performance_query";
  return `${file.counters.length} hardware counters over ${file.rounds} collection pass${file.rounds === 1 ? "" : "es"} `
    + `(${where}), ${file.passes.length} passes and ${file.draws.length} draws measured`;
}
