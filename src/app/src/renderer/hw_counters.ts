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

/** One line summarising what was collected, for a status line. */
export function hwCountersSummary(file: HwCounters): string {
  if (!file.backend) return `no hardware counters (${file.notes[0] ?? "none available"})`;
  const where = file.backend === "nvperf" ? `NVIDIA ${file.chip}` : "VK_KHR_performance_query";
  return `${file.counters.length} hardware counters over ${file.rounds} collection pass${file.rounds === 1 ? "" : "es"} `
    + `(${where}), ${file.passes.length} passes and ${file.draws.length} draws measured`;
}
