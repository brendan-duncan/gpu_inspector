// The shader binding table rule (src/renderer/d3d12/frame_analysis.ts, bindingTableProblems).
//
// DXR asks two things of a table that are easy to get wrong together: every table starts on a
// 64-byte boundary, and every record stride is a multiple of 32. Laying the tables out back to back
// at the record stride satisfies the second and not the first, and the runtime then drops the trace
// without a word unless the debug layer is on. test/path_tracer/d3d12 did exactly that.
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
const out = join(mkdtempSync(join(tmpdir(), "sbt-")), "frame_analysis.mjs");
buildSync({ entryPoints: [join(here, "..", "src", "renderer", "d3d12", "frame_analysis.ts")], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
const { bindingTableProblems } = await import(pathToFileURL(out).href);

/** A trace's arguments as the capture library writes them: an address is a hex string. */
const trace = (base, missOffset, hitOffset, stride = 32) => ({
  pDesc: {
    RayGenerationShaderRecord: { StartAddress: { address: `0x${base.toString(16)}` }, SizeInBytes: stride },
    MissShaderTable: { StartAddress: { address: `0x${(base + missOffset).toString(16)}` }, SizeInBytes: stride, StrideInBytes: stride },
    HitGroupTable: { StartAddress: { address: `0x${(base + hitOffset).toString(16)}` }, SizeInBytes: 3 * stride, StrideInBytes: stride },
    CallableShaderTable: { StartAddress: null, SizeInBytes: 0, StrideInBytes: 0 },
    Width: 640, Height: 480, Depth: 1,
  },
});

test("tables laid out back to back at the record stride are not 64-byte aligned", () => {
  // What test/path_tracer/d3d12 did: [raygen 32][miss 32][hit 32*3] from the buffer's start.
  const problems = bindingTableProblems(trace(0x10000, 32, 64));
  assert.equal(problems.length, 1, problems.join("; "));
  assert.match(problems[0], /the miss table starts at 0x10020, which is not a multiple of 64/);
});

test("tables each on a 64-byte boundary raise nothing", () => {
  assert.deepEqual(bindingTableProblems(trace(0x10000, 64, 128)), []);
});

test("a stride that is not a multiple of the record alignment is caught", () => {
  const args = trace(0x10000, 64, 128);
  args.pDesc.MissShaderTable.StrideInBytes = 48;
  args.pDesc.MissShaderTable.SizeInBytes = 96;
  const problems = bindingTableProblems(args);
  assert.equal(problems.length, 1, problems.join("; "));
  assert.match(problems[0], /the miss table has a stride of 48, which is not a multiple of 32/);
});

test("a table its stride does not divide holds a partial record", () => {
  const args = trace(0x10000, 64, 128);
  args.pDesc.HitGroupTable.SizeInBytes = 80;   // two 32-byte records and half of a third
  const problems = bindingTableProblems(args);
  assert.equal(problems.length, 1, problems.join("; "));
  assert.match(problems[0], /the hit group table is 80 bytes, which its stride of 32 does not divide/);
});

test("a ray generation record shorter than an identifier names no shader", () => {
  const args = trace(0x10000, 64, 128);
  args.pDesc.RayGenerationShaderRecord.SizeInBytes = 16;
  const problems = bindingTableProblems(args);
  assert.equal(problems.length, 1, problems.join("; "));
  assert.match(problems[0], /the ray generation record is 16 bytes, shorter than a shader identifier/);
});

test("an unused region raises nothing, and neither does a command that is not a trace", () => {
  // The callable table is null and zero-sized in every trace that has no callables.
  assert.deepEqual(bindingTableProblems(trace(0x10000, 64, 128)), []);
  assert.deepEqual(bindingTableProblems(null), []);
  assert.deepEqual(bindingTableProblems({ ThreadGroupCountX: 1 }), []);
});

test("every misalignment of a table is reported, not just the first", () => {
  const args = trace(0x10000, 32, 48);
  const problems = bindingTableProblems(args);
  assert.equal(problems.length, 2, problems.join("; "));
  assert.match(problems[0], /miss table/);
  assert.match(problems[1], /hit group table/);
});
