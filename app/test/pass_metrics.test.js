// The per-pass measurements the GPU Bottlenecks report and the counter rules are built on
// (renderer/metal/pass_metrics.ts): that passes are numbered the way the capture library numbers
// them, and that each derived figure is the division it claims to be.
//
//     cd app && npm test
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const out = join(mkdtempSync(join(tmpdir(), "passmetrics-")), "pass_metrics.mjs");
buildSync({
  entryPoints: [join(here, "..", "src", "renderer", "metal", "pass_metrics.ts")],
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const { collectPassMetrics } = await import(pathToFileURL(out).href);

const COMMAND_BUFFER = 7;
const COLOR_TEXTURE = 42;

/** A capture built from a list of [method, args] pairs, all in one command buffer and frame. */
function capture(commands, timings, api = "metal") {
  const list = commands.map(([method, args], index) => ({
    index, frame: 0, method, object: { __id: COMMAND_BUFFER, __class: "MTLCommandBuffer" }, args: args ?? null,
  }));
  const byKey = new Map();
  for (const t of timings) byKey.set(`0:${COMMAND_BUFFER}:${t.kind === "compute" ? "c" : ""}${t.passIndex}`, t);
  return {
    api, commands: list,
    passTiming: (frame, cb, passIndex, compute) => byKey.get(`${frame}:${cb}:${compute ? "c" : ""}${passIndex}`) ?? null,
  };
}

/** One 100x100 colour texture, so overdraw has a denominator of 10000. */
const db = {
  getObject: (id) => (id === COLOR_TEXTURE
    ? { descriptor: { width: 100, height: 100, sampleCount: 1, mipmapLevelCount: 1 } }
    : null),
};

const colorPass = { colorAttachments: [{ index: 0, texture: { __id: COLOR_TEXTURE, __class: "MTLTexture" } }] };

test("passes are numbered from one counter per command buffer, compute keyed apart", () => {
  const data = capture([
    ["renderCommandEncoderWithDescriptor:", colorPass],
    ["endEncoding", {}],
    ["computeCommandEncoder", {}],
    ["dispatchThreads:threadsPerThreadgroup:", {}],
    ["endEncoding", {}],
    ["blitCommandEncoder", {}],
    ["endEncoding", {}],
  ], [
    { frame: 0, commandBuffer: COMMAND_BUFFER, passIndex: 0, durationMs: 1, startMs: 0 },
    { frame: 0, commandBuffer: COMMAND_BUFFER, passIndex: 1, kind: "compute", durationMs: 2, startMs: 1 },
    { frame: 0, commandBuffer: COMMAND_BUFFER, passIndex: 2, durationMs: 3, startMs: 3 },
  ]);
  const m = collectPassMetrics(data, db);
  assert.equal(m.passes.length, 3);
  assert.deepEqual(m.passes.map((p) => p.passIndex), [0, 1, 2]);
  assert.deepEqual(m.passes.map((p) => p.compute), [false, true, false]);
  // Every one found its timing: the compute encoder shares the counter but is keyed as compute.
  assert.deepEqual(m.passes.map((p) => p.durationMs), [1, 2, 3]);
  assert.equal(m.timed, 3);
  assert.equal(m.gpuMs, 6);
  assert.match(m.passes[1].label, /^Compute 1/);
  assert.match(m.passes[2].label, /^Blit 2/);
  assert.equal(m.passes[1].draws, 1, "the dispatch counted against its compute pass");
});

test("the derived figures are the divisions they claim to be", () => {
  const data = capture([
    ["renderCommandEncoderWithDescriptor:", colorPass],
    ["drawPrimitives:vertexStart:vertexCount:", { vertexCount: 300, instanceCount: 2 }],
    ["endEncoding", {}],
  ], [{
    frame: 0, commandBuffer: COMMAND_BUFFER, passIndex: 0, durationMs: 4, startMs: 0,
    vertexMs: 1, fragmentMs: 3,
    counters: { vertexInvocations: 600, fragmentInvocations: 25000, clipperPrimitivesOut: 5000, fragmentsPassed: 5000 },
  }]);
  const p = collectPassMetrics(data, db).passes[0];
  assert.equal(p.draws, 1);
  assert.equal(p.vertices, 600, "instanced draws multiply the vertex count");
  assert.equal(p.pixels, 10000);
  assert.equal(p.overdraw, 2.5, "25000 fragment invocations over 10000 pixels");
  assert.equal(p.fragmentsPerPrimitive, 5, "25000 fragments over 5000 primitives");
  assert.equal(p.depthRejectRate, 0.8, "5000 of 25000 fragments survived");
  // 1 ms of vertex stage over 600 invocations, in nanoseconds.
  assert.ok(Math.abs(p.nsPerVertex - 1e6 / 600) < 1e-6);
  assert.equal(p.bound, "fragment", "3 ms of fragment against 1 ms of vertex");
});

test("a pass without the statistic counters keeps its timings and drops the rest", () => {
  const data = capture([
    ["renderCommandEncoderWithDescriptor:", colorPass],
    ["drawPrimitives:vertexStart:vertexCount:", { vertexCount: 3 }],
    ["endEncoding", {}],
  ], [{ frame: 0, commandBuffer: COMMAND_BUFFER, passIndex: 0, durationMs: 2, startMs: 0, vertexMs: 1.5, fragmentMs: 0.4 }]);
  const m = collectPassMetrics(data, db);
  const p = m.passes[0];
  assert.equal(p.durationMs, 2);
  assert.equal(p.overdraw, null);
  assert.equal(p.fragmentsPerPrimitive, null);
  assert.equal(p.depthRejectRate, null);
  assert.equal(m.withCounters, 0);
  assert.equal(m.totals, null);
  assert.equal(p.bound, "vertex", "the stage split alone still names the bound stage");
});

test("the stage-utilization cycles can name a target-write bound pass", () => {
  const data = capture([
    ["renderCommandEncoderWithDescriptor:", colorPass],
    ["endEncoding", {}],
  ], [{
    frame: 0, commandBuffer: COMMAND_BUFFER, passIndex: 0, durationMs: 1, startMs: 0,
    utilization: { totalCycles: 1000, vertexCycles: 100, fragmentCycles: 200, renderTargetCycles: 600 },
  }]);
  const p = collectPassMetrics(data, db).passes[0];
  assert.equal(p.bound, "target");
  assert.ok(Math.abs(p.cycleShare.target - 0.6) < 1e-9);
});

test("an untimed pass is listed without measurements, and a Vulkan capture is not usable", () => {
  const data = capture([["renderCommandEncoderWithDescriptor:", colorPass], ["endEncoding", {}]], []);
  const m = collectPassMetrics(data, db);
  assert.equal(m.passes.length, 1);
  assert.equal(m.passes[0].durationMs, null);
  assert.equal(m.timed, 0);
  assert.equal(m.usable, false);

  const vulkan = capture([["renderCommandEncoderWithDescriptor:", colorPass], ["endEncoding", {}]],
    [{ frame: 0, commandBuffer: COMMAND_BUFFER, passIndex: 0, durationMs: 1, startMs: 0 }], "vulkan");
  assert.equal(collectPassMetrics(vulkan, db).usable, false, "the report is Metal-only");
});
