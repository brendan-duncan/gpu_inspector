// Per-draw timings and counters (src/renderer/draw_stats.ts): what `vkinsp_replay --draw-data`
// writes, and what the Shader Flame Graph does with it (src/renderer/frame_cost_tree.ts).
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "drawstats-"));
const load = async (entry, name) => {
  const out = join(dir, `${name}.mjs`);
  buildSync({ entryPoints: [join(here, "..", "src", entry)], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
  return import(pathToFileURL(out).href);
};
const { parseDrawStats, drawStatsByCommand, drawStatsSummary, drawSumsByPass, passSumKey } = await load("renderer/draw_stats.ts", "draw_stats");

const file = {
  format: "gpu-inspector-draw-stats", version: 1, device: "Test GPU", note: "",
  draws: [
    // A dispatch outside any render pass: the replay writes UINT32_MAX for the pass.
    { command: 5, frame: 0, commandBuffer: 7, passIndex: 0xffffffff, timed: true, ms: 0.008, counted: true,
      vertexInvocations: 0, primitives: 0, fragmentInvocations: 0, computeInvocations: 1024 },
    { command: 17, frame: 0, commandBuffer: 7, passIndex: 0, timed: true, ms: 0.005, counted: true,
      vertexInvocations: 24, primitives: 12, fragmentInvocations: 51204, computeInvocations: 0,
      sampled: true, samplesPassed: 48000 },
  ],
  problems: [],
};

test("the replay's draw measurements are read back, with the no-pass sentinel dropped", () => {
  const parsed = parseDrawStats(JSON.stringify(file));
  assert.equal(parsed.device, "Test GPU");
  assert.equal(parsed.draws.length, 2);
  assert.equal(parsed.draws[0].passIndex, undefined, "a dispatch outside a render pass has no pass");
  assert.equal(parsed.draws[0].computeInvocations, 1024);
  assert.equal(parsed.draws[1].passIndex, 0);
  assert.equal(parsed.draws[1].fragmentInvocations, 51204);

  const byCommand = drawStatsByCommand(parsed.draws);
  assert.equal(byCommand.get(17).primitives, 12);
  assert.match(drawStatsSummary(parsed), /2 draws and dispatches measured \(2 timed, 2 counted\), 51,204 fragment/);
  assert.throws(() => parseDrawStats("{}"), /Not draw measurements/);

  // The per-pass sums the depth rejection rate comes from; the dispatch is in no pass, so it is out.
  const sums = drawSumsByPass(parsed.draws);
  assert.equal(sums.size, 1);
  const pass = sums.get(passSumKey(0, 7, 0));
  assert.deepEqual(pass, { draws: 1, counted: true, fragmentInvocations: 51204, sampled: true, samplesPassed: 48000 });
});
