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
const { parseHwCounters, counterValue, hwCountersByPass, formatCounter, hwCountersSummary, counterLabel } = await load("renderer/hw_counters.ts", "hw_counters");
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

test("non-counter JSON is rejected", () => {
  assert.throws(() => parseHwCounters(JSON.stringify({ format: "something-else" })), /Not hardware counters/);
});
