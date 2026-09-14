// D3D12 shader reflection (src/renderer/d3d12/reflection.ts): the JSON the capture library attaches
// to a pipeline state's descriptor, read into the same ShaderReflection the SPIR-V path builds, and
// matched to a snapshot's bindings by register and space.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const out = join(mkdtempSync(join(tmpdir(), "d3d12-reflection-")), "entry.mjs");
buildSync({
  stdin: {
    contents: `
      export { d3d12Reflection, d3d12StageReflections, hasD3D12Reflection, findD3D12Resource } from "./d3d12/reflection.ts";
      export { findBoundResource, pipelineStages, ShaderReflectionCache } from "./shader_cache.ts";
      export { VulkanObject } from "./vulkan/vulkan_object.ts";
    `,
    resolveDir: join(here, "..", "src", "renderer"), loader: "ts",
  },
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const { d3d12Reflection, d3d12StageReflections, hasD3D12Reflection, findD3D12Resource, findBoundResource, pipelineStages, ShaderReflectionCache, VulkanObject } = await import(pathToFileURL(out).href);

const float = { kind: "scalar", base: "float", width: 32, size: 4 };
const float4 = { kind: "vector", element: float, count: 4, size: 16 };
const uint = { kind: "scalar", base: "uint", width: 32, size: 4 };
const reflection = {
  vertex: {
    entryPoint: "VSMain", target: "vs_6_0",
    inputs: [{ name: "position", semantic: "POSITION", index: 0, type: { kind: "vector", element: float, count: 3, size: 12 } }, { name: "uv", semantic: "TEXCOORD", index: 0, type: { kind: "vector", element: float, count: 2, size: 8 } }],
    outputs: [{ name: "position", semantic: "SV_Position", index: 0, type: float4 }],
    resources: [
      { kind: "cbuffer", name: "Constants", register: 0, space: 0, count: 1, dimension: "", type: { kind: "struct", name: "Constants", members: [{ name: "mvp", offset: 0, type: { kind: "matrix", element: float, columns: 4, rows: 4, stride: 16, rowMajor: false, size: 64 } }], size: 64 } },
    ],
  },
  fragment: {
    entryPoint: "PSMain", target: "ps_6_0",
    inputs: [{ name: "position", semantic: "SV_Position", index: 0, type: float4 }],
    outputs: [{ name: "color", semantic: "SV_Target", index: 0, type: float4 }],
    resources: [
      { kind: "cbuffer", name: "Constants", register: 0, space: 0, count: 1, dimension: "", type: { kind: "struct", name: "Constants", members: [], size: 64 } },
      { kind: "srv", name: "albedo", register: 0, space: 0, count: 1, dimension: "texture2d", returnType: "float4" },
      { kind: "srv", name: "shadowMaps", register: 1, space: 0, count: 4, dimension: "texture2darray", returnType: "float" },
      { kind: "srv", name: "lights", register: 5, space: 0, count: 1, dimension: "structured", stride: 32, type: { kind: "struct", name: "Light", members: [{ name: "position", offset: 0, type: float4 }], size: 32 } },
      { kind: "srv", name: "indices", register: 0, space: 1, count: 1, dimension: "byteaddress" },
      { kind: "srv", name: "colors", register: 6, space: 0, count: 1, dimension: "buffer", returnType: "float4" },
      { kind: "srv", name: "scene", register: 7, space: 0, count: 1, dimension: "accelerationStructure" },
      { kind: "uav", name: "output", register: 0, space: 0, count: 1, dimension: "texture2d", returnType: "float4" },
      { kind: "uav", name: "counters", register: 1, space: 0, count: 1, dimension: "structured", stride: 4, type: { kind: "struct", name: "Counter", members: [{ name: "n", offset: 0, type: uint }], size: 4 } },
      { kind: "uav", name: "histogram", register: 2, space: 0, count: 1, dimension: "buffer", returnType: "uint" },
      { kind: "sampler", name: "linearSampler", register: 0, space: 0, count: 1, dimension: "" },
    ],
  },
  compute: { entryPoint: "CSMain", target: "cs_6_0", inputs: [], outputs: [], threadGroupSize: [8, 8, 1], resources: [] },
};
const pipeline = new VulkanObject({
  id: 20, type: "ID3D12PipelineState", parent: 1, cmd: "CreateGraphicsPipelineState", index: 0, handle: "0x20", label: "Scene PSO",
  args: { pDesc: { pRootSignature: { __id: 2, __class: "ID3D12RootSignature" }, VS: { __bytes: 1200 }, PS: { __bytes: 2400 }, reflection } },
  blobs: [{ name: "vertex:VSMain", size: 1200 }, { name: "fragment:PSMain", size: 2400 }],
});

test("a pipeline's stage reflection reads into ShaderReflection with the kinds keyed by register and space", () => {
  assert.ok(hasD3D12Reflection(pipeline));
  const fs = d3d12Reflection(pipeline, "fragment");
  assert.ok(fs);
  assert.equal(fs.version, "dxil");
  assert.equal(fs.entryPoints.length, 1);
  assert.equal(fs.entryPoints[0].name, "PSMain");
  assert.equal(fs.entryPoints[0].stage, "fragment");
  const kinds = Object.fromEntries(fs.resources.map((r) => [r.name, [r.kind, r.set, r.binding, r.count, r.readOnly]]));
  assert.deepEqual(kinds, {
    Constants: ["uniform", 0, 0, 1, false],
    albedo: ["sampledImage", 0, 0, 1, false],
    shadowMaps: ["sampledImage", 0, 1, 4, false],
    lights: ["storage", 0, 5, 1, true],
    indices: ["storage", 1, 0, 1, true],
    colors: ["uniformTexelBuffer", 0, 6, 1, true],
    scene: ["accelerationStructure", 0, 7, 1, false],
    output: ["storageImage", 0, 0, 1, false],
    counters: ["storage", 0, 1, 1, false],
    histogram: ["storageTexelBuffer", 0, 2, 1, false],
    linearSampler: ["sampler", 0, 0, 1, false],
  });
  const lights = fs.resources.find((r) => r.name === "lights");
  assert.equal(lights.type.kind, "struct");
  assert.equal(lights.typeName, "StructuredBuffer<Light>");
  const albedo = fs.resources.find((r) => r.name === "albedo");
  assert.equal(albedo.typeName, "Texture2D<float4>");
  assert.equal(fs.resources.find((r) => r.name === "counters").typeName, "RWStructuredBuffer<Counter>");
  assert.equal(fs.findResource(0, 5), lights, "the SPIR-V path's lookup keeps working on set (space) and binding (register)");

  const vs = d3d12Reflection(pipeline, "vertex");
  assert.deepEqual(vs.entryPoints[0].inputs.map((i) => [i.location, i.name, i.typeName]), [[0, "POSITION0", "vec3"], [1, "TEXCOORD0", "vec2"]]);
  assert.equal(vs.resources[0].type.members[0].name, "mvp");

  const cs = d3d12Reflection(pipeline, "compute");
  assert.deepEqual(cs.entryPoints[0].workgroupSize, [8, 8, 1]);
  assert.equal(d3d12Reflection(pipeline, "geometry"), null);
  assert.deepEqual([...d3d12StageReflections(pipeline).keys()], ["vertex", "fragment", "compute"]);
});

test("a snapshot's binding finds the declaration covering its register, of the kind its range type holds", () => {
  const fs = d3d12Reflection(pipeline, "fragment");
  const srv = (register, space = 0) => ({ binding: 0, type: "D3D12_DESCRIPTOR_RANGE_TYPE_SRV", register, space, descriptors: [] });
  assert.equal(findD3D12Resource(fs, srv(0)).name, "albedo", "an SRV at t0 is the texture, not the UAV at u0");
  assert.equal(findD3D12Resource(fs, srv(1), 2).name, "shadowMaps", "the third descriptor of a range starting at t1 is inside the array at t1..t4");
  assert.equal(findD3D12Resource(fs, srv(1), 4).name, "lights", "the fifth is t5");
  assert.equal(findD3D12Resource(fs, srv(0, 1)).name, "indices", "space 1");
  assert.equal(findD3D12Resource(fs, srv(9)), null);
  assert.equal(findD3D12Resource(fs, { binding: 0, type: "D3D12_DESCRIPTOR_RANGE_TYPE_UAV", register: 0, space: 0, descriptors: [] }).name, "output");
  assert.equal(findD3D12Resource(fs, { binding: 0, type: "D3D12_ROOT_PARAMETER_TYPE_UAV", register: 1, space: 0, descriptors: [] }).name, "counters", "a root UAV finds a writable storage declaration");
  assert.equal(findD3D12Resource(fs, { binding: 0, type: "D3D12_ROOT_PARAMETER_TYPE_CBV", register: 0, space: 0, descriptors: [] }).name, "Constants");
  assert.equal(findD3D12Resource(fs, { binding: 0, type: "D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER", register: 0, space: 0, descriptors: [] }).name, "linearSampler");
  // The shared helper: a Vulkan binding goes through set and binding, a D3D12 one through register and space.
  assert.equal(findBoundResource(fs, 3, srv(5)).name, "lights");
  assert.equal(findBoundResource(fs, 0, { binding: 5, type: "VK_DESCRIPTOR_TYPE_STORAGE_BUFFER", descriptors: [] }).name, "lights");
});

test("the pipeline's stages come from its payloads and the reflection cache answers without a fetch", async () => {
  const db = { getObject: (id) => (id === 20 ? pipeline : null) };
  const stages = pipelineStages(pipeline, db);
  assert.deepEqual(stages.map((s) => [s.stage, s.stageFlag, s.entryPoint, s.blobIndex]), [["vertex", "vertex", "VSMain", 0], ["fragment", "fragment", "PSMain", 1]]);
  let sent = 0;
  const cache = new ShaderReflectionCache({ onObjectBlob: { addListener() {} }, onReset: { addListener() {} } }, async () => { sent++; return true; });
  const r = await cache.get(pipeline, 1);
  assert.equal(sent, 0, "no RequestBlob: the reflection came with the object");
  assert.equal(r.entryPoints[0].name, "PSMain");
  assert.equal(r.resources.length, 11);
});
