// Ray tracing pipelines in captures (src/renderer/shader_cache.ts): stages matched to the payloads the
// layer names with their index in pStages, the shader groups, and a trace command's shader binding
// table regions.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const out = join(mkdtempSync(join(tmpdir(), "rt-")), "shader_cache.mjs");
buildSync({ entryPoints: [join(here, "..", "src", "renderer", "shader_cache.ts")], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
const { pipelineStages, shaderGroups, bindingTableRegions } = await import(pathToFileURL(out).href);

// A pipeline with two miss shaders: the payloads carry the index, so each stage gets its own.
const stage = (flag) => ({ stage: `VK_SHADER_STAGE_${flag}_BIT_KHR`, pName: "main", module: null });
const pipeline = {
  descriptor: {
    pStages: [stage("RAYGEN"), stage("MISS"), stage("MISS"), stage("CLOSEST_HIT")],
    pGroups: [
      { type: "VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR", generalShader: 0, closestHitShader: 0xffffffff, anyHitShader: 0xffffffff, intersectionShader: 0xffffffff },
      { type: "VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR", generalShader: 1, closestHitShader: 0xffffffff, anyHitShader: 0xffffffff, intersectionShader: 0xffffffff },
      { type: "VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR", generalShader: 2, closestHitShader: 0xffffffff, anyHitShader: 0xffffffff, intersectionShader: 0xffffffff },
      { type: "VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR", generalShader: 0xffffffff, closestHitShader: 3, anyHitShader: 0xffffffff, intersectionShader: 0xffffffff },
    ],
    maxPipelineRayRecursionDepth: 1,
  },
  blobs: [{ name: "raygen:main#0" }, { name: "miss:main#1" }, { name: "miss:main#2" }, { name: "closest_hit:main#3" }],
};
const db = { getObject: () => null };

test("each ray tracing stage is matched to its own payload, by its index in pStages", () => {
  const stages = pipelineStages(pipeline, db);
  assert.deepEqual(stages.map((s) => [s.stage, s.blobIndex, s.stageIndex, s.entryPoint]),
    [["raygen", 0, 0, "main"], ["miss", 1, 1, "main"], ["miss", 2, 2, "main"], ["closest_hit", 3, 3, "main"]]);
});

test("shader groups name their stages, with unused ones left out", () => {
  const groups = shaderGroups(pipeline);
  assert.equal(groups.length, 4);
  assert.deepEqual(groups[2], { index: 2, type: "general", general: 2, closestHit: undefined, anyHit: undefined, intersection: undefined });
  assert.equal(groups[3].type, "triangles hit");
  assert.equal(groups[3].closestHit, 3);
  assert.deepEqual(shaderGroups({ descriptor: { pStages: [] }, blobs: [] }), [], "not a ray tracing pipeline");
});

test("a trace command's binding table regions are counted in records", () => {
  const regions = bindingTableRegions({
    pRaygenShaderBindingTable: { deviceAddress: 64, stride: 64, size: 64 },
    pMissShaderBindingTable: { deviceAddress: 128, stride: 64, size: 128 },
    pHitShaderBindingTable: { deviceAddress: 256, stride: 64, size: 64 },
    pCallableShaderBindingTable: { deviceAddress: 0, stride: 0, size: 0 },
  });
  assert.deepEqual(regions.map((r) => [r.region, r.records]), [["raygen", 1], ["miss", 2], ["hit", 1], ["callable", 0]]);
});
