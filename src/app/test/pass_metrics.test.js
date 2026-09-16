// The per-pass measurements the GPU Bottlenecks report and the counter rules are built on
// (renderer/pass_metrics.ts): that passes are numbered the way the capture library numbers
// them, and that each derived figure is the division it claims to be.
//
//     cd src/app && npm test
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "passmetrics-"));
const bundle = (name) => {
  const out = join(dir, `${name}.mjs`);
  buildSync({
    entryPoints: [join(here, "..", "src", "renderer", `${name}.ts`)],
    bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
  });
  return import(pathToFileURL(out).href);
};
const { collectPassMetrics } = await bundle("pass_metrics");
const { setsFor } = await bundle("command_sets");

const COMMAND_BUFFER = 7;
const COLOR_TEXTURE = 42;

/** A capture built from a list of [method, args] pairs, all in one command buffer and frame. */
function capture(commands, timings, api = "metal") {
  const list = commands.map(([method, args], index) => ({
    index, frame: 0, method, object: { __id: COMMAND_BUFFER, __class: "CommandBuffer" }, args: args ?? null,
  }));
  const byKey = new Map();
  for (const t of timings) byKey.set(`0:${COMMAND_BUFFER}:${t.kind === "compute" ? "c" : ""}${t.passIndex}`, t);
  return {
    api, commands: list, sets: setsFor(api),
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

test("an untimed pass is listed without measurements", () => {
  const data = capture([["renderCommandEncoderWithDescriptor:", colorPass], ["endEncoding", {}]], []);
  const m = collectPassMetrics(data, db);
  assert.equal(m.passes.length, 1);
  assert.equal(m.passes[0].durationMs, null);
  assert.equal(m.timed, 0);
  assert.equal(m.usable, false);
});

// ------------------------------------------------------------------------------------------
// Vulkan: the layer numbers render passes and runs of dispatches separately, states the render
// area outright, and its pipeline statistics carry the invocation counts but not the fragments
// that survived the depth test.

const vulkanPass = { pRenderPassBegin: { renderArea: { extent: { width: 200, height: 50 } } } };

test("a Vulkan capture measures overdraw from the render area", () => {
  const data = capture([
    ["vkCmdBeginRenderPass", vulkanPass],
    ["vkCmdDraw", { vertexCount: 6, instanceCount: 1 }],
    ["vkCmdEndRenderPass", {}],
  ], [{
    frame: 0, commandBuffer: COMMAND_BUFFER, passIndex: 0, durationMs: 2, startMs: 0,
    counters: { vertexInvocations: 6, fragmentInvocations: 20000, clipperPrimitivesOut: 2000 },
  }], "vulkan");
  const m = collectPassMetrics(data, db);
  assert.equal(m.usable, true, "a timed Vulkan capture is usable");
  const p = m.passes[0];
  assert.equal(p.pixels, 10000, "200 x 50 of render area");
  assert.equal(p.overdraw, 2, "20000 fragment invocations over 10000 pixels");
  assert.equal(p.fragmentsPerPrimitive, 10);
  assert.equal(p.depthRejectRate, null, "no fragmentsPassed: the pass had no occlusion query");
  assert.equal(p.vertexMs, null, "Vulkan has no per-stage split");
  assert.equal(p.bound, null);
  assert.equal(m.withCounters, 1);
  assert.match(p.label, /^Render Pass 0/);
});

test("a Vulkan pass with the layer's occlusion query has a depth rejection rate", () => {
  // The layer runs an occlusion query around each pass (src/vulkan/src/capture.cpp), which counts the
  // samples that passed its depth and stencil tests: `fragmentsPassed`, the same name Metal uses.
  const data = capture([
    ["vkCmdBeginRenderPass", { pRenderPassBegin: { renderArea: { offset: { x: 0, y: 0 }, extent: { width: 200, height: 50 } } } }],
    ["vkCmdDraw", { vertexCount: 6, instanceCount: 1 }],
    ["vkCmdEndRenderPass", {}],
  ], [{
    frame: 0, commandBuffer: COMMAND_BUFFER, passIndex: 0, durationMs: 2, startMs: 0,
    counters: { vertexInvocations: 6, fragmentInvocations: 20000, clipperPrimitivesOut: 2000, fragmentsPassed: 4000 },
  }], "vulkan");
  const p = collectPassMetrics(data, db).passes[0];
  assert.equal(p.depthRejectRate, 0.8, "4000 of 20000 fragments survived the depth test");
});

test("the replay's per-draw occlusion queries give a pass the layer could not measure a rejection rate", () => {
  // A pass that records its draws into secondary command buffers gets no occlusion query from the
  // layer (a query cannot span vkCmdExecuteCommands), so the replay measures one per draw
  // (`vkinsp_replay --draws`, src/replay/src/draw_stats.cpp).
  const data = capture([
    ["vkCmdBeginRenderPass", vulkanPass],
    ["vkCmdExecuteCommands", {}],
    ["vkCmdEndRenderPass", {}],
  ], [{
    frame: 0, commandBuffer: COMMAND_BUFFER, passIndex: 0, durationMs: 2, startMs: 0,
    counters: { vertexInvocations: 6, fragmentInvocations: 20000, clipperPrimitivesOut: 2000 },
  }], "vulkan");
  const draw = { frame: 0, commandBuffer: COMMAND_BUFFER, passIndex: 0, timed: true, ms: 1, counted: true,
                 vertexInvocations: 3, primitives: 1000, fragmentInvocations: 10000, computeInvocations: 0,
                 sampled: true, samplesPassed: 2000 };
  data.drawStats = [{ ...draw, command: 10 }, { ...draw, command: 11 }];
  const p = collectPassMetrics(data, db).passes[0];
  assert.equal(p.depthRejectRate, 0.8, "4000 of the pass's 20000 fragments survived, summed over its two draws");
  assert.equal(p.depthRejectSource, "replay");

  // A draw the replay could not sample leaves the pass unmeasured rather than half measured.
  data.drawStats = [{ ...draw, command: 10 }, { ...draw, command: 11, sampled: false, samplesPassed: 0 }];
  assert.equal(collectPassMetrics(data, db).passes[0].depthRejectRate, null);
});

test("a pass with its own occlusion query keeps that count over the replay's", () => {
  const data = capture([
    ["vkCmdBeginRenderPass", vulkanPass],
    ["vkCmdDraw", { vertexCount: 6, instanceCount: 1 }],
    ["vkCmdEndRenderPass", {}],
  ], [{
    frame: 0, commandBuffer: COMMAND_BUFFER, passIndex: 0, durationMs: 2, startMs: 0,
    counters: { fragmentInvocations: 20000, fragmentsPassed: 4000 },
  }], "vulkan");
  data.drawStats = [{ command: 1, frame: 0, commandBuffer: COMMAND_BUFFER, passIndex: 0, timed: true, ms: 1, counted: true,
                      vertexInvocations: 6, primitives: 2000, fragmentInvocations: 20000, computeInvocations: 0,
                      sampled: true, samplesPassed: 10000 }];
  const p = collectPassMetrics(data, db).passes[0];
  assert.equal(p.depthRejectRate, 0.8);
  assert.equal(p.depthRejectSource, "counters");
});

test("a Vulkan run of dispatches is its own pass, numbered apart from render passes", () => {
  const data = capture([
    ["vkCmdBeginRenderPass", vulkanPass],
    ["vkCmdDraw", { vertexCount: 3 }],
    ["vkCmdEndRenderPass", {}],
    ["vkCmdDispatch", { groupCountX: 4, groupCountY: 1, groupCountZ: 1 }],
    ["vkCmdDispatch", { groupCountX: 4, groupCountY: 1, groupCountZ: 1 }],
    ["vkCmdPipelineBarrier", {}],
    ["vkCmdDispatch", { groupCountX: 4, groupCountY: 1, groupCountZ: 1 }],
  ], [
    { frame: 0, commandBuffer: COMMAND_BUFFER, passIndex: 0, durationMs: 1, startMs: 0 },
    { frame: 0, commandBuffer: COMMAND_BUFFER, passIndex: 0, kind: "compute", durationMs: 2, startMs: 1 },
    { frame: 0, commandBuffer: COMMAND_BUFFER, passIndex: 1, kind: "compute", durationMs: 3, startMs: 3 },
  ], "vulkan");
  const m = collectPassMetrics(data, db);
  assert.equal(m.passes.length, 3, "one render pass and two runs of dispatches");
  assert.deepEqual(m.passes.map((p) => p.compute), [false, true, true]);
  // The render pass and the first compute run are both index 0: two separate counters.
  assert.deepEqual(m.passes.map((p) => p.passIndex), [0, 0, 1]);
  assert.deepEqual(m.passes.map((p) => p.durationMs), [1, 2, 3]);
  assert.deepEqual(m.passes.map((p) => p.draws), [1, 2, 1], "the barrier ended the first run");
});

// ---------------------------------------------------------------------------------------------
// Numbering across recordings. A capture library restarts a command buffer's pass numbering with
// each recording of it (the Vulkan layer at vkBeginCommandBuffer, the D3D12 library at a list's
// Reset), so this has to restart with them. Counting straight through instead shifts every index
// from a buffer's second recording onwards, and a shifted index matches no timing at all — the pass
// then shows no GPU time anywhere, which is what a multi-frame capture used to do to most of its
// passes.

/** A capture of several frames: commands are [frame, commandBuffer, method, args]. */
function frames(commands, timings, api) {
  const list = commands.map(([frame, cb, method, args], index) => ({
    index, frame, method, args: args ?? null,
    object: { __id: cb, __class: api === "d3d12" ? "ID3D12GraphicsCommandList" : "VkCommandBuffer" },
  }));
  const byKey = new Map();
  for (const t of timings) byKey.set(`${t.frame}:${t.commandBuffer}:${t.kind === "compute" ? "c" : ""}${t.passIndex}`, t);
  return {
    api, commands: list, sets: setsFor(api),
    passTiming: (frame, cb, passIndex, compute) => byKey.get(`${frame}:${cb}:${compute ? "c" : ""}${passIndex}`) ?? null,
  };
}

const CB = 8;
const vkPass = (frame, cb) => [
  [frame, cb, "vkCmdBeginRenderPass", {}],
  [frame, cb, "vkCmdDraw", { vertexCount: 3 }],
  [frame, cb, "vkCmdEndRenderPass", {}],
];
const timing = (frame, commandBuffer, passIndex, durationMs) => ({ frame, commandBuffer, passIndex, durationMs, startMs: 0 });

test("a command buffer recorded again numbers its passes from zero again", () => {
  // Two frames, the same buffer re-recorded: the layer reports passIndex 0 for both.
  const data = frames([
    [0, CB, "vkBeginCommandBuffer", {}], ...vkPass(0, CB), [0, CB, "vkEndCommandBuffer", {}],
    [1, CB, "vkBeginCommandBuffer", {}], ...vkPass(1, CB), [1, CB, "vkEndCommandBuffer", {}],
  ], [timing(0, CB, 0, 1), timing(1, CB, 0, 2)], "vulkan");
  const m = collectPassMetrics(data, db);
  assert.equal(m.passes.length, 2);
  assert.deepEqual(m.passes.map((p) => p.passIndex), [0, 0], "not [0, 1]: the second recording restarts");
  assert.deepEqual(m.passes.map((p) => p.durationMs), [1, 2], "both found their timing");
  assert.equal(m.timed, 2);
});

test("four frames of one buffer keep every pass's timing", () => {
  const commands = [];
  const timings = [];
  for (let f = 0; f < 4; f++) {
    commands.push([f, CB, "vkBeginCommandBuffer", {}], ...vkPass(f, CB), [f, CB, "vkEndCommandBuffer", {}]);
    timings.push(timing(f, CB, 0, f + 1));
  }
  const m = collectPassMetrics(frames(commands, timings, "vulkan"), db);
  assert.equal(m.timed, 4, "every frame's pass, not just the first");
  assert.equal(m.gpuMs, 10);
});

test("a queue command between recordings does not disturb the numbering", () => {
  // vkQueueSubmit carries the queue as its object, not a command buffer; reading it as the buffer
  // resuming would restart the count in the middle of a recording.
  const QUEUE = 5;
  const data = frames([
    [0, QUEUE, "vkQueueSubmit", {}],
    [0, CB, "vkBeginCommandBuffer", {}], ...vkPass(0, CB), ...vkPass(0, CB), [0, CB, "vkEndCommandBuffer", {}],
  ], [timing(0, CB, 0, 1), timing(0, CB, 1, 2)], "vulkan");
  const m = collectPassMetrics(data, db);
  assert.deepEqual(m.passes.map((p) => p.passIndex), [0, 1], "two passes in one recording still count up");
  assert.equal(m.timed, 2);
});

test("two buffers alternating keep their own counts", () => {
  // Double buffering: the engine alternates, so each buffer is re-recorded every other frame.
  const A = 7;
  const B = 8;
  const commands = [];
  const timings = [];
  for (let f = 0; f < 4; f++) {
    const cb = f % 2 === 0 ? A : B;
    commands.push([f, cb, "vkBeginCommandBuffer", {}], ...vkPass(f, cb), [f, cb, "vkEndCommandBuffer", {}]);
    timings.push(timing(f, cb, 0, 1));
  }
  const m = collectPassMetrics(frames(commands, timings, "vulkan"), db);
  assert.deepEqual(m.passes.map((p) => p.passIndex), [0, 0, 0, 0]);
  assert.equal(m.timed, 4);
});

test("the same recording submitted again restarts, even with no begin marker", () => {
  // A stream that does not carry the recording's start: the guard in collectPassMetrics.
  const data = frames([
    [0, CB, "vkBeginCommandBuffer", {}], ...vkPass(0, CB), [0, CB, "vkEndCommandBuffer", {}],
    ...vkPass(1, CB),
  ], [timing(0, CB, 0, 1), timing(1, CB, 0, 2)], "vulkan");
  const m = collectPassMetrics(data, db);
  assert.deepEqual(m.passes.map((p) => p.passIndex), [0, 0]);
  assert.equal(m.timed, 2);
});

test("a D3D12 command list restarts at Reset", () => {
  const LIST = 9;
  const pass = (frame) => [
    [frame, LIST, "OMSetRenderTargets", {}],
    [frame, LIST, "DrawInstanced", { VertexCountPerInstance: 3 }],
    [frame, LIST, "EndRenderTargets", {}],
  ];
  const data = frames([
    [0, LIST, "Reset", {}], ...pass(0), [0, LIST, "Close", {}],
    [1, LIST, "Reset", {}], ...pass(1), [1, LIST, "Close", {}],
  ], [timing(0, LIST, 0, 1), timing(1, LIST, 0, 2)], "d3d12");
  const m = collectPassMetrics(data, db);
  assert.deepEqual(m.passes.map((p) => p.passIndex), [0, 0]);
  assert.deepEqual(m.passes.map((p) => p.durationMs), [1, 2]);
});

test("Metal keeps counting up within a command buffer, which is used once", () => {
  // No recording markers: a Metal command buffer's encoders share one rising counter, and the next
  // frame's command buffer is a different object with a counter of its own.
  const A = 10;
  const B = 11;
  const enc = (frame, cb) => [
    [frame, cb, "renderCommandEncoderWithDescriptor:", colorPass],
    [frame, cb, "endEncoding", {}],
  ];
  const data = frames([
    ...enc(0, A), ...enc(0, A),
    ...enc(1, B),
  ], [timing(0, A, 0, 1), timing(0, A, 1, 2), timing(1, B, 0, 3)], "metal");
  const m = collectPassMetrics(data, db);
  assert.deepEqual(m.passes.map((p) => p.passIndex), [0, 1, 0]);
  assert.equal(m.timed, 3, "unchanged by the restart rule, which Metal has no markers for");
});
