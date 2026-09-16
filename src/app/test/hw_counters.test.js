// Hardware counters (src/renderer/hw_counters.ts): what `vkinsp_replay --counter-data` writes, and
// how the parser and formatter read it (src/replay/src/hw_counters.cpp, main.cpp WriteCounterData).
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "hwcounters-"));
const load = async (entry, name) => {
  const out = join(dir, `${name}.mjs`);
  buildSync({ entryPoints: [join(here, "..", "src", entry)], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
  return import(pathToFileURL(out).href);
};
const { parseHwCounters, counterValue, hwCountersByPass, formatCounter, hwCountersSummary, counterLabel, passLimiter, LIMITER_LABEL, heaviestStage, limiterAdvice } = await load("renderer/hw_counters.ts", "hw_counters");
const { encodeCaptureFile, parseCaptureFile, CAPTURE_FORMAT } = await load("renderer/capture_format.ts", "capture_format");

const file = {
  format: "gpu-inspector-hw-counters", version: 1, device: "NVIDIA GeForce RTX 4080", backend: "nvperf", chip: "AD103", rounds: 2,
  counters: [
    { name: "sm__throughput.avg.pct_of_peak_sustained_elapsed", description: "SM throughput", category: "sm", unit: "percent" },
    { name: "dram__bytes.sum", description: "VRAM bytes", category: "dram", unit: "bytes" },
  ],
  passes: [
    { command: 3, frame: 0, commandBuffer: 7, passIndex: 0, values: [82.5, 1048576] },
    // A dispatch pass has no render pass index.
    { command: 40, frame: 0, commandBuffer: 7, passIndex: 0xffffffff, values: [12.0, null] },
  ],
  draws: [
    { command: 17, frame: 0, commandBuffer: 7, passIndex: 0, values: [90.0, 524288] },
  ],
  available: [],
  notes: [],
  problems: [],
};

test("the counter file is parsed, with the no-pass sentinel dropped and null values kept", () => {
  const parsed = parseHwCounters(JSON.stringify(file));
  assert.equal(parsed.backend, "nvperf");
  assert.equal(parsed.chip, "AD103");
  assert.equal(parsed.rounds, 2);
  assert.equal(parsed.counters.length, 2);
  assert.equal(parsed.passes[0].passIndex, 0);
  assert.equal(parsed.passes[1].passIndex, undefined, "a dispatch outside a render pass has no pass index");
  assert.equal(parsed.passes[1].values[1], null, "a counter that did not evaluate stays null");
});

test("counterValue looks a named counter up by column", () => {
  const parsed = parseHwCounters(JSON.stringify(file));
  assert.equal(counterValue(parsed, parsed.passes[0], "sm__throughput.avg.pct_of_peak_sustained_elapsed"), 82.5);
  assert.equal(counterValue(parsed, parsed.passes[0], "dram__bytes.sum"), 1048576);
  assert.equal(counterValue(parsed, parsed.passes[0], "not_a_counter"), null);
});

test("passes are keyed by frame, command buffer and pass index", () => {
  const parsed = parseHwCounters(JSON.stringify(file));
  const byPass = hwCountersByPass(parsed);
  assert.ok(byPass.has("0:7:0"));
  assert.equal(byPass.size, 1, "the dispatch with no pass index is not keyed");
});

test("values are formatted by their unit", () => {
  assert.equal(formatCounter(82.5, "percent"), "82.5%");
  assert.equal(formatCounter(1048576, "bytes"), "1.0 MB");
  assert.equal(formatCounter(null, "percent"), "—");
  assert.equal(formatCounter(1500000, "ns"), "1.500 ms");
});

test("a summary names the backend and the collection passes", () => {
  const parsed = parseHwCounters(JSON.stringify(file));
  const s = hwCountersSummary(parsed);
  assert.match(s, /NVIDIA AD103/);
  assert.match(s, /2 collection passes/);
});

test("a file with no backend reports why", () => {
  const none = parseHwCounters(JSON.stringify({ format: "gpu-inspector-hw-counters", version: 1, device: "x", backend: "", chip: "", rounds: 0, counters: [], passes: [], draws: [], available: [], notes: ["no counters here"], problems: [] }));
  assert.equal(none.backend, "");
  assert.match(hwCountersSummary(none), /no hardware counters/);
});

test("column headings stay distinct for counters sharing a unit prefix", () => {
  // All four of these begin "sm", so a naive prefix would label every column the same.
  assert.equal(counterLabel("sm__throughput.avg.pct_of_peak_sustained_elapsed"), "sm throughput");
  assert.equal(counterLabel("sm__warps_active.avg.pct_of_peak_sustained_active"), "sm warps active");
  assert.equal(counterLabel("sm__pipe_alu_cycles_active.avg.pct_of_peak_sustained_active"), "sm pipe alu");
  assert.equal(counterLabel("sm__pipe_fma_cycles_active.avg.pct_of_peak_sustained_active"), "sm pipe fma");
  assert.equal(counterLabel("gpu__dram_throughput.avg.pct_of_peak_sustained_elapsed"), "gpu dram throughput");
  const labels = ["sm__throughput.avg.x", "sm__warps_active.avg.x", "sm__pipe_alu_cycles_active.avg.x"].map(counterLabel);
  assert.equal(new Set(labels).size, labels.length, "each column keeps its own heading");
});

test("counters survive being saved into a capture file and read back", () => {
  // Collecting them replays the frame dozens of times, so a saved capture has to keep them.
  const counters = parseHwCounters(JSON.stringify(file));
  const manifest = { format: CAPTURE_FORMAT, version: 1, frame: 0, frames: 1, api: "vulkan", objects: [], commands: [], textures: [], buffers: [], passTimings: [], hwCounters: counters };
  const round = parseCaptureFile(encodeCaptureFile(manifest, []));
  assert.ok(round.hwCounters, "the capture file carried the counters back");
  assert.equal(round.hwCounters.backend, "nvperf");
  assert.equal(round.hwCounters.chip, "AD103");
  assert.equal(round.hwCounters.counters.length, 2);
  assert.equal(round.hwCounters.passes[0].values[0], 82.5);
  assert.equal(round.hwCounters.passes[1].values[1], null, "a counter that did not evaluate stays null through the file");
});

test("a capture file without counters reads back as none", () => {
  const manifest = { format: CAPTURE_FORMAT, version: 1, frame: 0, frames: 1, api: "vulkan", objects: [], commands: [], textures: [], buffers: [], passTimings: [] };
  const round = parseCaptureFile(encodeCaptureFile(manifest, []));
  assert.equal(round.hwCounters, null);
});

// The verdict the counters give: which unit a pass saturates, measured, rather than which stage it
// is inferred to wait on (pass_metrics.ts reads this into PassMetrics.limiter).
const limiterFile = (values) => parseHwCounters(JSON.stringify({
  format: "gpu-inspector-hw-counters", version: 1, device: "d", backend: "nvperf", chip: "AD103", rounds: 1,
  counters: [
    { name: "sm__throughput.avg.pct_of_peak_sustained_elapsed", description: "", category: "sm", unit: "percent" },
    { name: "gpu__dram_throughput.avg.pct_of_peak_sustained_elapsed", description: "", category: "dram", unit: "percent" },
    { name: "lts__throughput.avg.pct_of_peak_sustained_elapsed", description: "", category: "lts", unit: "percent" },
    { name: "sm__warps_active.avg.pct_of_peak_sustained_active", description: "", category: "sm", unit: "percent" },
  ],
  passes: [{ command: 1, frame: 0, commandBuffer: 1, passIndex: 0, values }],
  draws: [], available: [], notes: [], problems: [],
}));

test("a unit at its limit is named as the bottleneck", () => {
  const f = limiterFile([78, 10, 12, 60]);
  const l = passLimiter(f, f.passes[0]);
  assert.equal(l.kind, "shader");
  assert.equal(l.saturated, true);
  assert.equal(Math.round(l.percent), 78);
  assert.equal(LIMITER_LABEL[l.kind], "Shader bound");
});

test("bandwidth is named when it is the unit at the limit, not the shader core", () => {
  // The case the inferred stage verdict cannot see: little shader work, memory saturated.
  const f = limiterFile([8, 82, 20, 55]);
  const l = passLimiter(f, f.passes[0]);
  assert.equal(l.kind, "memory");
  assert.equal(l.saturated, true);
  assert.equal(LIMITER_LABEL[l.kind], "Bandwidth bound");
});

test("the busiest unit is named even when nothing is saturated", () => {
  const f = limiterFile([12, 9, 41, 70]);
  const l = passLimiter(f, f.passes[0]);
  assert.equal(l.kind, "cache");
  assert.equal(l.saturated, false, "busy is not the same as at its limit");
});

test("a busy unit outranks low occupancy, since it is the thing to act on", () => {
  // Shader core doing real work and few warps in flight: the busy unit is the useful answer.
  const f = limiterFile([45, 10, 12, 20]);
  const l = passLimiter(f, f.passes[0]);
  assert.equal(l.kind, "shader");
  assert.equal(Math.round(l.percent), 45);
});

test("low occupancy with nothing busy reads as waiting, not working", () => {
  const f = limiterFile([9, 6, 7, 11]);
  const l = passLimiter(f, f.passes[0]);
  assert.equal(l.kind, "occupancy");
  assert.equal(LIMITER_LABEL[l.kind], "Latency bound");
});

test("a pass with healthy occupancy and no busy unit saturates nothing", () => {
  const f = limiterFile([10, 8, 9, 75]);
  const l = passLimiter(f, f.passes[0]);
  assert.equal(l.kind, "unsaturated");
});

test("counters that are not percentages of peak give no verdict", () => {
  const raw = parseHwCounters(JSON.stringify({
    format: "gpu-inspector-hw-counters", version: 1, device: "d", backend: "khr", chip: "", rounds: 1,
    counters: [{ name: "dram__bytes.sum", description: "", category: "dram", unit: "bytes" }],
    passes: [{ command: 1, frame: 0, commandBuffer: 1, passIndex: 0, values: [123456] }],
    draws: [], available: [], notes: [], problems: [],
  }));
  assert.equal(passLimiter(raw, raw.passes[0]), null, "a raw total says how much, not how close to the limit");
});

// The compiler statistics the layer attaches to a pipeline (shader_statistics.h), which say why
// occupancy is what it is rather than only that it is low.
const executables = (regs) => regs.map(([stage, count]) => ({
  name: stage.toUpperCase(), stages: [stage],
  statistics: [{ name: "Register Count", description: "", value: count }, { name: "Binary Size", description: "", value: 640 }],
}));

test("the heaviest stage by register count is the one reported", () => {
  const s = heaviestStage(executables([["vertex", 16], ["fragment", 40]]));
  assert.equal(s.stage, "fragment");
  assert.equal(s.count, 40);
});

test("a pipeline with no register statistic gives no stage", () => {
  assert.equal(heaviestStage([{ name: "VS", stages: ["vertex"], statistics: [{ name: "Binary Size", value: 640 }] }]), null);
  assert.equal(heaviestStage(undefined), null, "a capture taken without compiler statistics");
});

test("a register-heavy stage is named as what holds occupancy down", () => {
  const f = limiterFile([9, 6, 7, 11]);
  const l = { ...passLimiter(f, f.passes[0]), registers: { stage: "fragment", count: 40 } };
  assert.equal(l.kind, "occupancy");
  const advice = limiterAdvice(l);
  assert.match(advice, /fragment stage uses 40 registers/);
  assert.match(advice, /holding occupancy down/);
});

test("a light stage rules register pressure out instead of blaming it", () => {
  const f = limiterFile([9, 6, 7, 11]);
  const l = { ...passLimiter(f, f.passes[0]), registers: { stage: "vertex", count: 16 } };
  const advice = limiterAdvice(l);
  assert.match(advice, /only 16 registers/);
  assert.match(advice, /not the cause/);
});

test("without compiler statistics the advice is unchanged", () => {
  const f = limiterFile([9, 6, 7, 11]);
  const l = passLimiter(f, f.passes[0]);
  assert.equal(l.registers, undefined);
  assert.match(limiterAdvice(l), /waiting rather than working/);
});

test("registers only explain an occupancy verdict, not a saturated unit", () => {
  const f = limiterFile([78, 10, 12, 60]);
  const l = { ...passLimiter(f, f.passes[0]), registers: { stage: "fragment", count: 40 } };
  assert.equal(l.kind, "shader");
  assert.equal(limiterAdvice(l), limiterAdvice({ ...l, registers: undefined }), "a shader-bound pass says the same either way");
});

test("non-counter JSON is rejected", () => {
  assert.throws(() => parseHwCounters(JSON.stringify({ format: "something-else" })), /Not hardware counters/);
});
