// The HLSL stub generated from a D3D12 pipeline's reflection (src/renderer/d3d12/hlsl_stub.ts):
// with no source in the bytecode and no PDB to read, the shader editor starts from a stage written
// out of the reflection, which has to declare the same constant buffers at the same offsets, the
// same resources at the same registers and spaces, and the same entry signature, so that dxc
// compiles it into a binding-compatible replacement.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const out = join(mkdtempSync(join(tmpdir(), "hlsl-stub-")), "entry.mjs");
buildSync({
  stdin: {
    contents: `
      export { hlslStub } from "./d3d12/hlsl_stub.ts";
      export { d3d12Reflection } from "./d3d12/reflection.ts";
      export { VulkanObject } from "./vulkan/vulkan_object.ts";
    `,
    resolveDir: join(here, "..", "src", "renderer"), loader: "ts",
  },
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const { hlslStub, d3d12Reflection, VulkanObject } = await import(pathToFileURL(out).href);

const float = { kind: "scalar", base: "float", width: 32, size: 4 };
const uint = { kind: "scalar", base: "uint", width: 32, size: 4 };
const float2 = { kind: "vector", element: float, count: 2, size: 8 };
const float3 = { kind: "vector", element: float, count: 3, size: 12 };
const float4 = { kind: "vector", element: float, count: 4, size: 16 };
const float4x4 = { kind: "matrix", element: float, columns: 4, rows: 4, stride: 16, rowMajor: false, size: 64 };

// The shape the capture library attaches to a pipeline state's descriptor (shader_reflect.cpp).
const reflection = {
  vertex: {
    entryPoint: "VSMain", target: "vs_6_0",
    inputs: [
      { name: "POSITION", semantic: "POSITION", index: 0, type: float3 },
      { name: "TEXCOORD", semantic: "TEXCOORD", index: 0, type: float2 },
      { name: "SV_INSTANCEID", semantic: "SV_INSTANCEID", index: 0, systemValue: "D3D_NAME_INSTANCE_ID", type: uint },
    ],
    outputs: [
      { name: "SV_POSITION", semantic: "SV_POSITION", index: 0, systemValue: "D3D_NAME_POSITION", type: float4 },
      { name: "TEXCOORD", semantic: "TEXCOORD", index: 0, type: float2 },
    ],
    resources: [
      { kind: "cbuffer", name: "Cube", register: 0, space: 0, count: 1, dimension: "buffer",
        type: { kind: "struct", name: "Cube", size: 144, members: [
          { name: "viewProj", offset: 0, type: float4x4 },
          { name: "model", offset: 64, type: float4x4 },
          { name: "time", offset: 128, type: float },
          { name: "flags", offset: 132, type: uint },
          { name: "tint", offset: 144 - 8, type: float2 },
        ] } },
      // A second space, to prove the space is not dropped.
      { kind: "cbuffer", name: "$Globals", register: 2, space: 3, count: 1, dimension: "buffer",
        type: { kind: "struct", name: "$Globals", size: 16, members: [{ name: "scale", offset: 0, type: float4 }] } },
    ],
  },
  fragment: {
    entryPoint: "PSMain", target: "ps_6_0",
    inputs: [
      { name: "SV_POSITION", semantic: "SV_POSITION", index: 0, systemValue: "D3D_NAME_POSITION", type: float4 },
      { name: "TEXCOORD", semantic: "TEXCOORD", index: 0, type: float2 },
      { name: "SV_ISFRONTFACE", semantic: "SV_ISFRONTFACE", index: 0, systemValue: "D3D_NAME_IS_FRONT_FACE", type: uint },
    ],
    outputs: [{ name: "SV_TARGET", semantic: "SV_TARGET", index: 0, systemValue: "D3D_NAME_TARGET", type: float4 }],
    resources: [
      { kind: "srv", name: "checker", register: 0, space: 0, count: 1, dimension: "texture2d", returnType: "float4" },
      { kind: "srv", name: "shadowMaps", register: 1, space: 2, count: 4, dimension: "texture2darray", returnType: "float" },
      { kind: "srv", name: "lights", register: 5, space: 0, count: 1, dimension: "structured", stride: 32,
        type: { kind: "struct", name: "Light", size: 32, members: [{ name: "position", offset: 0, type: float4 }, { name: "color", offset: 16, type: float4 }] } },
      { kind: "srv", name: "indices", register: 0, space: 1, count: 1, dimension: "byteaddress" },
      { kind: "uav", name: "output", register: 3, space: 0, count: 1, dimension: "texture2d", returnType: "float4" },
      { kind: "uav", name: "counters", register: 4, space: 0, count: 1, dimension: "structured", stride: 4,
        type: { kind: "struct", name: "Counter", size: 4, members: [{ name: "n", offset: 0, type: uint }] } },
      { kind: "sampler", name: "pointSampler", register: 0, space: 0, count: 1, dimension: "" },
    ],
  },
  compute: {
    entryPoint: "CSMain", target: "cs_6_0", inputs: [], outputs: [], threadGroupSize: [16, 8, 2],
    resources: [{ kind: "uav", name: "waveOut", register: 0, space: 0, count: 1, dimension: "buffer", returnType: "float4" }],
  },
  // A stage whose entry point needs attributes the reflection does not carry.
  geometry: {
    entryPoint: "GSMain", target: "gs_6_0", inputs: [], outputs: [], resources: [],
  },
};

const pipeline = new VulkanObject({
  id: 7, type: "ID3D12PipelineState", parent: 1, cmd: "CreateGraphicsPipelineState", index: 0, handle: "0x7", label: "Cube PSO",
  args: { pDesc: { pRootSignature: { __id: 2, __class: "ID3D12RootSignature" }, VS: { __bytes: 900 }, PS: { __bytes: 1800 }, reflection } },
  blobs: [{ name: "vertex:VSMain", size: 900 }, { name: "fragment:PSMain", size: 1800 }],
});

function stubFor(stage) {
  const r = d3d12Reflection(pipeline, stage);
  assert.ok(r, `${stage} has reflection`);
  return hlslStub(r, stage, reflection[stage].entryPoint, { pipelineName: "Cube PSO", reason: "the container carries no HLSL" });
}

test("a vertex stub declares the constant buffers at their registers, spaces and offsets", () => {
  const hlsl = stubFor("vertex");
  assert.match(hlsl, /cbuffer Cube : register\(b0, space0\) \{/);
  assert.match(hlsl, /float4x4 viewProj : packoffset\(c0\);/);
  assert.match(hlsl, /float4x4 model : packoffset\(c4\);/);
  assert.match(hlsl, /float time : packoffset\(c8\);/);
  assert.match(hlsl, /uint flags : packoffset\(c8\.y\);/);
  assert.match(hlsl, /float2 tint : packoffset\(c8\.z\);/);
  // "$Globals" is not an identifier; the register and space are what must survive.
  assert.match(hlsl, /cbuffer _Globals : register\(b2, space3\) \{/);
  assert.match(hlsl, /float4 scale : packoffset\(c0\);/);
});

test("a vertex stub's entry point has the real signature and passes the position through", () => {
  const hlsl = stubFor("vertex");
  assert.match(hlsl, /struct VSMainInput \{/);
  assert.match(hlsl, /float3 POSITION0 : POSITION0;/);
  assert.match(hlsl, /float2 TEXCOORD0 : TEXCOORD0;/);
  // The signature reports SV_InstanceID as a uint, and that is what it must be declared as.
  assert.match(hlsl, /uint SV_INSTANCEID0 : SV_INSTANCEID0;/);
  assert.match(hlsl, /struct VSMainOutput \{/);
  assert.match(hlsl, /float4 SV_POSITION0 : SV_POSITION0;/);
  assert.match(hlsl, /VSMainOutput VSMain\(VSMainInput input\) \{/);
  assert.match(hlsl, /output\.SV_POSITION0 = float4\(input\.POSITION0, 1\.0\);/);
  assert.match(hlsl, /return output;/);
  assert.match(hlsl, /^\/\/ Replacement vertex shader for Cube PSO/);
  assert.match(hlsl, /the container carries no HLSL/);
});

test("a fragment stub declares every resource with its dimension, register and space", () => {
  const hlsl = stubFor("fragment");
  assert.match(hlsl, /Texture2D<float4> checker : register\(t0, space0\);/);
  assert.match(hlsl, /Texture2DArray<float> shadowMaps\[4\] : register\(t1, space2\);/);
  assert.match(hlsl, /struct Light \{\n    float4 position;\n    float4 color;\n\};/);
  assert.match(hlsl, /StructuredBuffer<Light> lights : register\(t5, space0\);/);
  assert.match(hlsl, /ByteAddressBuffer indices : register\(t0, space1\);/);
  assert.match(hlsl, /RWTexture2D<float4> output : register\(u3, space0\);/);
  assert.match(hlsl, /RWStructuredBuffer<Counter> counters : register\(u4, space0\);/);
  assert.match(hlsl, /SamplerState pointSampler : register\(s0, space0\);/);
});

test("a fragment stub returns a constant colour with the output signature's semantic", () => {
  const hlsl = stubFor("fragment");
  // SV_IsFrontFace comes back as a uint from the reflection and only compiles as a bool.
  assert.match(hlsl, /bool SV_ISFRONTFACE0 : SV_ISFRONTFACE0;/);
  assert.match(hlsl, /float4 PSMain\(PSMainInput input\) : SV_TARGET0 \{/);
  assert.match(hlsl, /return float4\(1\.0, 0\.0, 1\.0, 1\.0\);/);
});

test("a compute stub keeps the real thread group size and has an empty body", () => {
  const hlsl = stubFor("compute");
  assert.match(hlsl, /RWBuffer<float4> waveOut : register\(u0, space0\);/);
  assert.match(hlsl, /\[numthreads\(16, 8, 2\)\]\nvoid CSMain\(uint3 threadId : SV_DispatchThreadID\) \{\n\}/);
});

test("a stage whose attributes the reflection does not record says so", () => {
  const hlsl = stubFor("geometry");
  assert.match(hlsl, /maxvertexcount/);
  assert.match(hlsl, /void GSMain\(\) \{/);
});

test("without reflection the stub is still a compilable empty stage", () => {
  const hlsl = hlslStub(null, "fragment", "PSMain", { pipelineName: "Unknown PSO" });
  assert.match(hlsl, /no reflection for this stage/);
  assert.match(hlsl, /void PSMain\(\) \{\n\}/);
});
