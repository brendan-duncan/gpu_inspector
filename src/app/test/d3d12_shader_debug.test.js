// The shader debugger on a Direct3D 12 capture (src/renderer/d3d12/shader_debug.ts and
// src/main/shader_tools.ts compileHlslForDebugging): a stage's HLSL compiled to SPIR-V by dxc and
// stepped in the SPIR-V interpreter, its registers found in the draw's root parameters, its vertex
// inputs matched to the input layout and a pixel's inputs to the vertex shader's outputs by
// semantic, and D3D's rasterizer conventions. The mapping helpers run everywhere; the sessions need
// dxc and dxinsp_shader.exe (the D3D12 library's build), and are skipped without them.
import { test } from "node:test";
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "d3d12dbg-"));
const out = join(dir, "d3d12dbg.mjs");
buildSync({
  stdin: {
    contents: `
      export { CaptureData } from "./renderer/capture_data.js";
      export { drawState } from "./renderer/draw_state.js";
      export { VulkanObject } from "./renderer/vulkan/vulkan_object.js";
      export { prepareDebugSession, rasterStateOf, coveredPixel, vertexOutputsOf, pixelRasterState } from "./renderer/shader_debug_setup.js";
      export { d3d12Filter, d3d12Sampler, normalizeSemantic, isD3D12Pipeline } from "./renderer/d3d12/shader_debug.js";
      export { hlslRegisterOf, hlslBindingName, HLSL_SHIFT_ARGS } from "./shared/hlsl_debug.js";
      export { DebugController } from "./renderer/shader_debugger.js";
      export { scalars } from "./renderer/debug/values.js";
      export { compileDxil, compileHlslForDebugging, findShaderTool, findTool } from "./main/shader_tools.js";
    `,
    resolveDir: join(here, "..", "src"), loader: "ts",
  },
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const {
  CaptureData, drawState, VulkanObject, prepareDebugSession, rasterStateOf, coveredPixel, vertexOutputsOf, pixelRasterState,
  d3d12Filter, d3d12Sampler, normalizeSemantic, isD3D12Pipeline, hlslRegisterOf, hlslBindingName, HLSL_SHIFT_ARGS,
  DebugController, scalars, compileDxil, compileHlslForDebugging, findShaderTool, findTool,
} = await import(pathToFileURL(out).href);

// The shader tool is looked for under the checkout's build tree (main/d3d12.ts d3d12ToolDirs).
process.env.GPU_INSPECTOR_ROOT ??= resolve(here, "..", "..", "..");

// --------------------------------------------------------------------------------------------
// The mappings

test("a binding of the translated module names one register: the class from its range, the space from its set", () => {
  assert.deepEqual(hlslRegisterOf(0, 0), { kind: "b", register: 0, space: 0 });
  assert.deepEqual(hlslRegisterOf(0, 65536), { kind: "t", register: 0, space: 0 });
  assert.deepEqual(hlslRegisterOf(2, 131072 + 3), { kind: "s", register: 3, space: 2 });
  assert.deepEqual(hlslRegisterOf(0, 196608 + 7), { kind: "u", register: 7, space: 0 });
  assert.equal(hlslBindingName(0, 65536 + 1), "t1");
  assert.equal(hlslBindingName(1, 5), "b5 space 1");
  // What dxc is told, so the two sides agree by construction.
  assert.deepEqual(HLSL_SHIFT_ARGS, ["-fvk-t-shift", "65536", "all", "-fvk-s-shift", "131072", "all", "-fvk-u-shift", "196608", "all"]);
});

test("semantics are paired case-insensitively, with the index a bare name leaves out", () => {
  assert.equal(normalizeSemantic("TEXCOORD"), "TEXCOORD0");
  assert.equal(normalizeSemantic("texcoord1"), "TEXCOORD1");
  assert.equal(normalizeSemantic("SV_Position"), "SV_POSITION0");
});

test("D3D12_FILTER names say which stages are linear", () => {
  assert.deepEqual(d3d12Filter("D3D12_FILTER_MIN_MAG_MIP_POINT"), { min: "nearest", mag: "nearest", mip: "nearest", comparison: false });
  assert.deepEqual(d3d12Filter("D3D12_FILTER_MIN_MAG_MIP_LINEAR"), { min: "linear", mag: "linear", mip: "linear", comparison: false });
  assert.deepEqual(d3d12Filter("D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT"), { min: "nearest", mag: "linear", mip: "nearest", comparison: false });
  assert.deepEqual(d3d12Filter("D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT"), { min: "linear", mag: "linear", mip: "nearest", comparison: false });
  assert.deepEqual(d3d12Filter("D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR"), { min: "linear", mag: "linear", mip: "linear", comparison: true });
  assert.deepEqual(d3d12Filter("D3D12_FILTER_ANISOTROPIC"), { min: "linear", mag: "linear", mip: "linear", comparison: false });
});

test("a sampler descriptor and a static sampler both read as the interpreter's sampler", () => {
  const dynamic = d3d12Sampler({
    Filter: "D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT", AddressU: "D3D12_TEXTURE_ADDRESS_MODE_WRAP", AddressV: "D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE",
    AddressW: "D3D12_TEXTURE_ADDRESS_MODE_BORDER", MipLODBias: 0.5, MaxAnisotropy: 1, ComparisonFunc: "D3D12_COMPARISON_FUNC_LESS_EQUAL",
    BorderColor: [1, 0, 0, 1], MinLOD: 1, MaxLOD: 3.402823466e38,
  });
  assert.deepEqual(dynamic, {
    magFilter: "linear", minFilter: "linear", mipmapMode: "nearest", address: ["repeat", "mirrorClamp", "border"], border: [1, 0, 0, 1],
    compareOp: "VK_COMPARE_OP_LESS_OR_EQUAL", minLod: 1, maxLod: 1000, lodBias: 0.5, unnormalized: false,
  });
  const fixed = d3d12Sampler({
    Filter: "D3D12_FILTER_MIN_MAG_MIP_POINT", AddressU: "D3D12_TEXTURE_ADDRESS_MODE_CLAMP", AddressV: "D3D12_TEXTURE_ADDRESS_MODE_CLAMP", AddressW: "D3D12_TEXTURE_ADDRESS_MODE_CLAMP",
    MipLODBias: 0, MaxAnisotropy: 0, ComparisonFunc: "D3D12_COMPARISON_FUNC_NEVER", BorderColor: "D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE", MinLOD: 0, MaxLOD: 3.402823466e38,
    ShaderRegister: 0, RegisterSpace: 0, ShaderVisibility: "D3D12_SHADER_VISIBILITY_PIXEL",
  });
  assert.equal(fixed.compareOp, null, "a filter without COMPARISON_ does not compare, whatever ComparisonFunc says");
  assert.deepEqual(fixed.border, [1, 1, 1, 1]);
  assert.deepEqual(fixed.address, ["clamp", "clamp", "clamp"]);
  assert.equal(d3d12Sampler(null), null);
});

// --------------------------------------------------------------------------------------------
// A D3D12 draw and dispatch, as the capture library records them (test/d3d12_frame_resources.test.js
// has the same shapes for the render graph): the cube's shaders of test/d3d12_triangle.

const ref = (id, cls) => ({ __id: id, __class: cls });
const object = (id, type, cmd, args, label, blobs = [], parent = 1) => new VulkanObject({ id, type, parent, cmd, index: 0, handle: `0x${id.toString(16)}`, label, args, blobs });
const buffer = (id, label, size) => object(id, "ID3D12Resource", "CreateCommittedResource", { pDesc: {
  Dimension: "D3D12_RESOURCE_DIMENSION_BUFFER", Alignment: 0, Width: size, Height: 1, DepthOrArraySize: 1, MipLevels: 1, Format: "DXGI_FORMAT_UNKNOWN",
  SampleDesc: { Count: 1, Quality: 0 }, Layout: "D3D12_TEXTURE_LAYOUT_ROW_MAJOR", Flags: "D3D12_RESOURCE_FLAG_NONE",
} }, label);

const list = ref(5, "ID3D12GraphicsCommandList");
const queue = ref(6, "ID3D12CommandQueue");
const heap = ref(50, "ID3D12DescriptorHeap");
const base64 = (bytes) => Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength).toString("base64");
const floats = (values) => new Uint8Array(new Float32Array(values).buffer);

/** cube.hlsl's root signature: a table (CBV b0, SRV t0), root constants at b1, and a static point sampler at s0. */
const graphicsRoot = {
  Version: "D3D_ROOT_SIGNATURE_VERSION_1_1",
  Desc_1_1: {
    NumParameters: 2,
    pParameters: [
      { ParameterType: "D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE", DescriptorTable: { NumDescriptorRanges: 2, pDescriptorRanges: [
        { RangeType: "D3D12_DESCRIPTOR_RANGE_TYPE_CBV", NumDescriptors: 1, BaseShaderRegister: 0, RegisterSpace: 0 },
        { RangeType: "D3D12_DESCRIPTOR_RANGE_TYPE_SRV", NumDescriptors: 1, BaseShaderRegister: 0, RegisterSpace: 0 },
      ] }, ShaderVisibility: "D3D12_SHADER_VISIBILITY_ALL" },
      { ParameterType: "D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS", Constants: { ShaderRegister: 1, RegisterSpace: 0, Num32BitValues: 2 }, ShaderVisibility: "D3D12_SHADER_VISIBILITY_ALL" },
    ],
    NumStaticSamplers: 1,
    pStaticSamplers: [{
      Filter: "D3D12_FILTER_MIN_MAG_MIP_POINT", AddressU: "D3D12_TEXTURE_ADDRESS_MODE_WRAP", AddressV: "D3D12_TEXTURE_ADDRESS_MODE_WRAP", AddressW: "D3D12_TEXTURE_ADDRESS_MODE_WRAP",
      MipLODBias: 0, MaxAnisotropy: 0, ComparisonFunc: "D3D12_COMPARISON_FUNC_NEVER", BorderColor: "D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK", MinLOD: 0, MaxLOD: 3.402823466e38,
      ShaderRegister: 0, RegisterSpace: 0, ShaderVisibility: "D3D12_SHADER_VISIBILITY_PIXEL",
    }],
    Flags: "D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT",
  },
};

/** wave.hlsl's: a UAV table at u0 and root constants at b0. */
const computeRoot = {
  Version: "D3D_ROOT_SIGNATURE_VERSION_1_1",
  Desc_1_1: {
    NumParameters: 2,
    pParameters: [
      { ParameterType: "D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE", DescriptorTable: { NumDescriptorRanges: 1, pDescriptorRanges: [
        { RangeType: "D3D12_DESCRIPTOR_RANGE_TYPE_UAV", NumDescriptors: 1, BaseShaderRegister: 0, RegisterSpace: 0 },
      ] }, ShaderVisibility: "D3D12_SHADER_VISIBILITY_ALL" },
      { ParameterType: "D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS", Constants: { ShaderRegister: 0, RegisterSpace: 0, Num32BitValues: 2 }, ShaderVisibility: "D3D12_SHADER_VISIBILITY_ALL" },
    ],
    NumStaticSamplers: 0, pStaticSamplers: [], Flags: "D3D12_ROOT_SIGNATURE_FLAG_NONE",
  },
};

const graphicsPipeline = (rasterizer = {}, depth = {}) => object(21, "ID3D12PipelineState", "CreateGraphicsPipelineState", { pDesc: {
  pRootSignature: ref(2, "ID3D12RootSignature"), VS: { __bytes: 1200 }, PS: { __bytes: 2400 },
  InputLayout: { NumElements: 3, pInputElementDescs: [
    { SemanticName: "POSITION", SemanticIndex: 0, Format: "DXGI_FORMAT_R32G32B32_FLOAT", InputSlot: 0, AlignedByteOffset: 0, InputSlotClass: "D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA", InstanceDataStepRate: 0 },
    { SemanticName: "COLOR", SemanticIndex: 0, Format: "DXGI_FORMAT_R32G32B32_FLOAT", InputSlot: 0, AlignedByteOffset: 12, InputSlotClass: "D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA", InstanceDataStepRate: 0 },
    { SemanticName: "TEXCOORD", SemanticIndex: 0, Format: "DXGI_FORMAT_R32G32_FLOAT", InputSlot: 0, AlignedByteOffset: 24, InputSlotClass: "D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA", InstanceDataStepRate: 0 },
  ] },
  RasterizerState: { FillMode: "D3D12_FILL_MODE_SOLID", CullMode: "D3D12_CULL_MODE_NONE", FrontCounterClockwise: false, ...rasterizer },
  DepthStencilState: { DepthEnable: false, DepthFunc: "D3D12_COMPARISON_FUNC_LESS", ...depth },
  PrimitiveTopologyType: "D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE", NumRenderTargets: 1, RTVFormats: ["DXGI_FORMAT_R8G8B8A8_UNORM"],
  reflection: {
    vertex: { entryPoint: "VSMain", target: "vs_6_0", inputs: [], outputs: [], resources: [] },
    fragment: { entryPoint: "PSMain", target: "ps_6_0", inputs: [], outputs: [], resources: [] },
  },
} }, "Cube PSO", [{ name: "vertex:VSMain", size: 1200 }, { name: "fragment:PSMain", size: 2400 }]);

test("a D3D12 draw's rasterizer state comes off its pipeline state, with D3D's conventions", () => {
  const objects = new Map([[21, graphicsPipeline({ CullMode: "D3D12_CULL_MODE_BACK", FrontCounterClockwise: true }, { DepthEnable: true, DepthFunc: "D3D12_COMPARISON_FUNC_GREATER_EQUAL" })]]);
  const db = { getObject: (id) => objects.get(id) ?? null, allObjects: objects, blobData: new Map() };
  const data = new CaptureData();
  data.api = "d3d12";
  data.commands = [
    ["SetPipelineState", { pipeline: ref(21, "ID3D12PipelineState"), bindPoint: "graphics" }],
    ["RSSetViewports", { NumViewports: 1, pViewports: [{ TopLeftX: 8, TopLeftY: 16, Width: 640, Height: 480, MinDepth: 0, MaxDepth: 1 }] }],
    ["DrawInstanced", { VertexCountPerInstance: 3, InstanceCount: 1, StartVertexLocation: 0, StartInstanceLocation: 0 }],
  ].map(([method, args], i) => ({ index: i, frame: 0, slot: i, method, object: list, args }));
  const state = drawState(data, db, data.commands[2]);
  assert.ok(isD3D12Pipeline(state.pipeline));
  const raster = rasterStateOf(state);
  assert.deepEqual(raster.viewport, { x: 8, y: 16, width: 640, height: 480, minDepth: 0, maxDepth: 1 }, "D3D12_VIEWPORT is spelled TopLeftX / Width");
  assert.equal(raster.cullBack, true);
  assert.equal(raster.cullFront, false);
  assert.equal(raster.ccwFront, true, "FrontCounterClockwise");
  assert.equal(raster.depthPrefers, "greater");
  assert.equal(raster.yUp, true, "clip +Y is the top of a D3D render target");
  const plain = rasterStateOf(drawState(data, { ...db, getObject: () => graphicsPipeline() }, data.commands[2]));
  assert.equal(plain.ccwFront, false, "clockwise is D3D's default front face");
  assert.equal(plain.depthPrefers, "none", "no depth test, no preference");
});

// --------------------------------------------------------------------------------------------
// Sessions: the real thing, with dxc compiling the cube's HLSL to DXIL (as the sample's build does,
// -Zi so the container carries its source) and again to SPIR-V for the debugger.

function available(tool) {
  try {
    execFileSync(findTool(tool), ["--help"], { stdio: "ignore" });
    return true;
  } catch (e) {
    return e.code !== "ENOENT";
  }
}
const skip = !available("dxc") ? "dxc is not installed" : !findShaderTool() ? "dxinsp_shader.exe is not built (src/d3d12/README.md)" : false;

const hlsl = (name) => readFileSync(join(here, "..", "..", "..", "test", "d3d12_triangle", name), "utf8");

/** The cube's three stages as DXIL containers with their HLSL embedded. */
async function containers() {
  const cube = hlsl("cube.hlsl");
  const wave = hlsl("wave.hlsl");
  const out = {};
  for (const [key, source, stage, entry] of [["vs", cube, "vertex", "VSMain"], ["ps", cube, "fragment", "PSMain"], ["cs", wave, "compute", "CSMain"]]) {
    const r = await compileDxil(source, stage, entry, "6_0");
    assert.ok(r.ok, `${entry} did not compile to DXIL: ${r.log}`);
    out[key] = r.spirv;
  }
  return out;
}

/** The capture: a full-screen triangle drawn twice (the cube's instancing) over a 64 x 64 target, and the wave dispatch before it. */
function capture(dxil) {
  const objects = new Map();
  const add = (o) => objects.set(o.id, o);
  add(object(2, "ID3D12RootSignature", "CreateRootSignature", { pDesc: graphicsRoot }, "Cube root signature"));
  add(object(3, "ID3D12RootSignature", "CreateRootSignature", { pDesc: computeRoot }, "Wave root signature"));
  add(buffer(12, "Cube constants", 256));
  add(buffer(13, "Vertices", 3 * 32));
  add(buffer(14, "Wave", 16));
  add(object(10, "ID3D12Resource", "CreateCommittedResource", { pDesc: {
    Dimension: "D3D12_RESOURCE_DIMENSION_TEXTURE2D", Alignment: 0, Width: 1, Height: 1, DepthOrArraySize: 1, MipLevels: 1, Format: "DXGI_FORMAT_R8G8B8A8_UNORM",
    SampleDesc: { Count: 1, Quality: 0 }, Layout: "D3D12_TEXTURE_LAYOUT_UNKNOWN", Flags: "D3D12_RESOURCE_FLAG_NONE",
  } }, "Checker"));
  add(object(4, "ID3D12Resource", "GetBuffer", { Buffer: 0 }, "Back buffer 0"));
  add(graphicsPipeline());
  add(object(20, "ID3D12PipelineState", "CreateComputePipelineState", { pDesc: {
    pRootSignature: ref(3, "ID3D12RootSignature"), CS: { __bytes: 800 },
    reflection: { compute: { entryPoint: "CSMain", target: "cs_6_0", threadGroupSize: [64, 1, 1], inputs: [], outputs: [], resources: [] } },
  } }, "Wave CS", [{ name: "compute:CSMain", size: 800 }]));
  add(object(50, "ID3D12DescriptorHeap", "CreateDescriptorHeap", { pDesc: { Type: "D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV", NumDescriptors: 64 } }, "Heap"));
  const blobData = new Map([["21:0", dxil.vs], ["21:1", dxil.ps], ["20:0", dxil.cs]]);
  const db = { getObject: (id) => objects.get(id) ?? null, allObjects: objects, blobData };

  const table = (bindPoint, set, layout, bindings) => ({ bindPoint, sets: [{ set, descriptorSet: heap, layout, bindings }] });
  const range = (binding, type, register, descriptors) => ({ binding, type, register, space: 0, stages: "D3D12_SHADER_VISIBILITY_ALL", descriptors });
  const srvView = { ViewDimension: "D3D12_SRV_DIMENSION_TEXTURE2D", Format: "DXGI_FORMAT_R8G8B8A8_UNORM", Shader4ComponentMapping: 5768, Texture2D: { MostDetailedMip: 0, MipLevels: 1 } };
  const commands = [
    ["SetComputeRootSignature", { pRootSignature: ref(3, "ID3D12RootSignature") }],
    ["SetPipelineState", { pipeline: ref(20, "ID3D12PipelineState"), bindPoint: "compute" }],
    ["SetComputeRootDescriptorTable", { RootParameterIndex: 0, BaseDescriptor: { heap, index: 2 } },
      { descriptors: table("compute", 0, ref(3, "ID3D12RootSignature"), [range(0, "D3D12_DESCRIPTOR_RANGE_TYPE_UAV", 0, [{ buffer: ref(14, "ID3D12Resource"), offset: 0, range: 16, data: 3 }])]) }],
    // Params: time 0.25, count 4.
    ["SetComputeRoot32BitConstants", { RootParameterIndex: 1, pValues: { base64: base64(new Uint8Array([...floats([0.25]), 4, 0, 0, 0])) }, offset: 0, size: 8, stageFlags: "compute" }],
    ["Dispatch", { ThreadGroupCountX: 1, ThreadGroupCountY: 1, ThreadGroupCountZ: 1 }],
    ["OMSetRenderTargets", { NumRenderTargetDescriptors: 1, pRenderTargetDescriptors: [{ heap, index: 0, resource: ref(4, "ID3D12Resource"), view: { ViewDimension: "D3D12_RTV_DIMENSION_TEXTURE2D", Format: "DXGI_FORMAT_R8G8B8A8_UNORM" } }], RTsSingleHandleToDescriptorRange: false, pDepthStencilDescriptor: null }],
    ["SetGraphicsRootSignature", { pRootSignature: ref(2, "ID3D12RootSignature") }],
    ["SetPipelineState", { pipeline: ref(21, "ID3D12PipelineState"), bindPoint: "graphics" }],
    ["SetGraphicsRootDescriptorTable", { RootParameterIndex: 0, BaseDescriptor: { heap, index: 0 } },
      { descriptors: table("graphics", 0, ref(2, "ID3D12RootSignature"), [
        range(0, "D3D12_DESCRIPTOR_RANGE_TYPE_CBV", 0, [{ buffer: ref(12, "ID3D12Resource"), offset: 0, range: 256, data: 1 }]),
        range(1, "D3D12_DESCRIPTOR_RANGE_TYPE_SRV", 0, [{ resource: ref(10, "ID3D12Resource"), view: srvView, data: 7 }]),
      ]) }],
    // Frame: time 0 (so the pulse is 0.75), flags 1 (swap red and blue).
    ["SetGraphicsRoot32BitConstants", { RootParameterIndex: 1, pValues: { base64: base64(new Uint8Array([...floats([0]), 1, 0, 0, 0])) }, offset: 0, size: 8, stageFlags: "graphics" }],
    ["RSSetViewports", { NumViewports: 1, pViewports: [{ TopLeftX: 0, TopLeftY: 0, Width: 64, Height: 64, MinDepth: 0, MaxDepth: 1 }] }],
    ["IASetPrimitiveTopology", { PrimitiveTopology: "D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST" }],
    ["IASetVertexBuffers", { StartSlot: 0, NumViews: 1, pViews: [{ BufferLocation: { address: "0x1000", buffer: ref(13, "ID3D12Resource"), offset: 0 }, SizeInBytes: 96, StrideInBytes: 32 }] }, { bufferData: [2] }],
    ["DrawInstanced", { VertexCountPerInstance: 3, InstanceCount: 2, StartVertexLocation: 0, StartInstanceLocation: 0 }],
    ["EndRenderTargets", null],
    ["Close", {}],
  ];
  const data = new CaptureData();
  data.api = "d3d12";
  data.commands = [
    { index: 0, frame: 0, method: "ExecuteCommandLists", object: queue, args: { NumCommandLists: 1, ppCommandLists: [list] } },
    ...commands.map(([method, args, extra = {}], i) => ({ index: i + 1, frame: 0, slot: i, method, object: list, args, ...extra })),
  ];
  // Identity view-projection and model matrices, so a vertex lands where its position says (plus the instance offset).
  const identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
  data.buffers.set(1, { info: { id: 1, buffer: 12, frame: 0, commandBuffer: 5, offset: 0, size: 128 }, data: floats([...identity, ...identity]) });
  // A triangle covering the whole viewport whichever way the instance offset moves it, one color and one uv at every corner.
  const vertices = [[-3, -3, 0], [5, -3, 0], [-3, 5, 0]].flatMap((p) => [...p, 1, 0.5, 0.25, 0.5, 0.5]);
  data.buffers.set(2, { info: { id: 2, buffer: 13, frame: 0, commandBuffer: 5, offset: 0, size: 96 }, data: floats(vertices) });
  data.buffers.set(3, { info: { id: 3, buffer: 14, frame: 0, commandBuffer: 5, offset: 0, size: 16 }, data: floats([0, 0, 0, 0]) });
  // The checker texture: one white texel.
  data.textures.push({ info: { id: 10, frame: 0, commandBuffer: 5, passIndex: 0, attachment: 0, format: "VK_FORMAT_R8G8B8A8_UNORM", aspect: "color", width: 1, height: 1, depth: 1, layers: 1, mip: 0, mips: 1, size: 4, kind: "sampled", capture: 7 }, data: new Uint8Array([255, 255, 255, 255]), canvas: null });
  // The render target after the pass: what the pixel shader computes, everywhere (texel * color * 0.75, red and blue swapped).
  const expected = [0.25 * 0.75, 0.5 * 0.75, 1 * 0.75, 1];
  const target = new Uint8Array(64 * 64 * 4);
  for (let i = 0; i < 64 * 64; i++) expected.forEach((c, k) => { target[i * 4 + k] = Math.round(c * 255); });
  data.textures.push({ info: { id: 4, frame: 0, commandBuffer: 5, passIndex: 0, attachment: 0, format: "VK_FORMAT_R8G8B8A8_UNORM", aspect: "color", width: 64, height: 64, depth: 1, layers: 1, mip: 0, size: target.byteLength }, data: target, canvas: null });
  const draw = data.commands.find((c) => c.method === "DrawInstanced");
  const dispatch = data.commands.find((c) => c.method === "Dispatch");
  const inputNames = new Map([[0, "POSITION0"], [1, "COLOR0"], [2, "TEXCOORD0"]]);
  const ctx = {
    data, db, inputNames,
    compileHlsl: async (bytes, source, target) => {
      const r = await compileHlslForDebugging(bytes, source.stage, source.entryPoint, { target });
      if (!r.ok) throw new Error(r.log);
      return r.spirv;
    },
  };
  return { data, db, ctx, draw, dispatch, expected };
}

const near = (a, b, eps = 1e-4) => a.length === b.length && a.every((x, i) => Math.abs(x - b[i]) <= eps);

test("compileHlslForDebugging: the container's HLSL comes back as SPIR-V with its source, lines and semantics", { skip }, async () => {
  const dxil = await containers();
  const r = await compileHlslForDebugging(dxil.ps, "fragment", "PSMain", { target: "ps_6_0" });
  assert.ok(r.ok, r.log);
  assert.ok(r.spirv.byteLength > 20 && new DataView(r.spirv.buffer, r.spirv.byteOffset).getUint32(0, true) === 0x07230203, "SPIR-V");
  assert.ok(r.source.includes("float4 PSMain(PSInput input) : SV_Target"), "the main file's text");
  const words = new Uint32Array(r.spirv.buffer.slice(r.spirv.byteOffset, r.spirv.byteOffset + r.spirv.byteLength));
  const text = new TextDecoder().decode(new Uint8Array(words.buffer));
  assert.ok(text.includes("SPV_GOOGLE_hlsl_functionality1"), "the semantics are kept (-fspv-reflect)");
  assert.ok(text.includes("checker.Sample(pointSampler, input.uv)"), "the source is embedded");
  assert.match(text, /\.hlsl/, "the file is named as the build named it (compileDxil's temporary here)");
  // A stage whose profile is DXBC's (fxc) still compiles, to the 6.0 profile.
  const old = await compileHlslForDebugging(dxil.vs, "vertex", "VSMain", { target: "vs_5_1" });
  assert.ok(old.ok, old.log);
  // The wrong entry point is dxc's error, reported.
  const bad = await compileHlslForDebugging(dxil.vs, "vertex", "NoSuchEntry", { target: "vs_6_0" });
  assert.equal(bad.ok, false);
  assert.match(bad.log, /NoSuchEntry|entry/i);
});

test("a vertex: its attributes by semantic, SV_InstanceID without the start instance, and the outputs the source computes", { skip }, async () => {
  const { ctx, draw } = capture(await containers());
  const session = await prepareDebugSession(ctx, { stage: "vertex", command: draw.index, vertex: 1, instance: 1 });
  assert.equal(session.program.kind, "spirv");
  assert.equal(session.program.languageName, "HLSL");
  assert.ok(session.program.hasSourceText(), "the HLSL is what is stepped");
  assert.deepEqual(session.limits, { vertices: 3, instances: 2 });
  assert.ok(session.notes.some((n) => n.includes("compiled to SPIR-V by dxc")), session.notes.join("; "));
  assert.ok(!session.notes.some((n) => n.includes("no element")), `every input found its element: ${session.notes.join("; ")}`);
  const ctl = new DebugController(session);
  assert.equal(ctl.mode, "source");
  // Stepping starts inside VSMain, not in dxc's wrapper around it.
  assert.equal(ctl.invocation.callStack()[0].name, "src.VSMain");
  const first = ctl.location(ctl.invocation.current)?.line;
  assert.ok(first >= 31 && first <= 34, `the first line stepped is VSMain's first statement: ${first}`);
  ctl.advance("continue");
  assert.equal(ctl.invocation.status, "returned", ctl.invocation.error);
  assert.deepEqual([...ctl.invocation.warnings], [], "every register was found");
  const outputs = ctl.invocation.outputs();
  const position = scalars(outputs.find((o) => o.builtin === 0).value);
  // Vertex 1 is (5, -3, 0); instance 1 moves it by +0.9 in x.
  assert.ok(near(position, [5.9, -3, 0, 1]), `SV_Position ${position}`);
  const color = scalars(outputs.find((o) => o.location !== undefined && o.name.includes("COLOR")).value);
  assert.ok(near(color, [1, 0.5, 0.25]), `COLOR ${color}`);
  const where = session.bindings;
  assert.equal(session.program.variableWhere({ set: 0, binding: 65536 }), "t0", "resources are named by register");
  assert.ok(where.buffer(0, 1, 0), "the root constants at b1 read as the Frame cbuffer");
});

test("a pixel: the vertex shader interpreted over the draw, the pixel's inputs from the covering triangle, and the target to compare with", { skip }, async () => {
  const { ctx, data, db, draw, expected } = capture(await containers());
  const state = drawState(data, db, draw);
  const mesh = await vertexOutputsOf(ctx, draw, state);
  assert.ok(mesh.measured);
  assert.equal(mesh.vertices, 6, "both instances, three vertices each");
  assert.ok(mesh.outputs.some((o) => o.builtin === "Position") && mesh.outputs.some((o) => o.name === "COLOR0") && mesh.outputs.some((o) => o.name === "TEXCOORD0"), JSON.stringify(mesh.outputs));
  const pixel = coveredPixel(state, mesh, pixelRasterState(ctx, draw, state));
  assert.ok(pixel, "the triangle covers the viewport");
  const session = await prepareDebugSession(ctx, { stage: "fragment", command: draw.index, x: pixel.x, y: pixel.y });
  assert.match(session.description, /whose vertices the interpreter ran the vertex shader for/);
  assert.ok(session.targetPixel, "the render target's pixel after the pass");
  const stepper = session.start();
  assert.equal(stepper.run(), "returned", stepper.invocation.error);
  assert.deepEqual([...stepper.invocation.warnings], [], "the texture through t0 and the static sampler at s0 were both found");
  const color = scalars(stepper.invocation.outputs().find((o) => o.location === 0).value);
  assert.ok(near(color, expected, 1e-3), `SV_Target ${color}, expected ${expected}`);
  assert.ok(near(session.targetPixel.value, expected, 1 / 255), `the target holds ${session.targetPixel.value}`);
  // The same pixel asked for the other way around lands on the same triangle.
  const again = await prepareDebugSession(ctx, { stage: "fragment", command: draw.index, x: 63 - pixel.x, y: 63 - pixel.y });
  assert.equal(again.start().run(), "returned");
});

test("a compute thread: SV_DispatchThreadID from the dispatch, the UAV at u0 written, the root constants at b0 read", { skip }, async () => {
  const { ctx, dispatch } = capture(await containers());
  const session = await prepareDebugSession(ctx, { stage: "compute", command: dispatch.index, invocation: [1, 0, 0] });
  assert.deepEqual(session.limits, { groups: [1, 1, 1], localSize: [64, 1, 1] });
  assert.match(session.description, /thread \(1, 0, 0\)/);
  const stepper = session.start();
  assert.equal(stepper.invocation.callStack()[0].name, "src.CSMain");
  assert.equal(stepper.run(), "returned", stepper.invocation.error);
  assert.deepEqual([...stepper.invocation.warnings], []);
  const wave = stepper.invocation.resourceVariables().find((v) => v.name === "wave");
  assert.ok(wave, "the RWStructuredBuffer is a resource");
  assert.equal(session.program.variableWhere(wave), "u0");
  const values = scalars(wave.value);
  assert.ok(Math.abs(values[1] - Math.sin(0.25 + 0.05)) < 1e-5, `wave[1] = sin(time + 1 * 0.05): ${values}`);
  assert.equal(values[0], 0, "the other elements keep the captured contents");
  // A thread past `count` writes nothing.
  const idle = await prepareDebugSession(ctx, { stage: "compute", command: dispatch.index, invocation: [9, 0, 0] });
  const run = idle.start();
  run.run();
  assert.equal(scalars(run.invocation.resourceVariables().find((v) => v.name === "wave").value)[9 % 4], 0);
});

test("without a compiler, or without the HLSL, the session says why", { skip }, async () => {
  const dxil = await containers();
  const { ctx, draw } = capture(dxil);
  await assert.rejects(prepareDebugSession({ ...ctx, compileHlsl: undefined }, { stage: "vertex", command: draw.index, vertex: 0, instance: 0 }), /no compiler is available/);
  // A container built without -Zi carries no source (compileDxil always embeds it, so dxc is asked directly).
  const bare = join(dir, "bare.dxil");
  execFileSync(findTool("dxc"), ["-T", "cs_6_0", "-E", "CSMain", "-Fo", bare, join(here, "..", "..", "..", "test", "d3d12_triangle", "wave.hlsl")], { stdio: "ignore" });
  const r = await compileHlslForDebugging(new Uint8Array(readFileSync(bare)), "compute", "CSMain", { target: "cs_6_0" });
  assert.equal(r.ok, false);
  assert.match(r.log, /-Zi|no HLSL source|PDB/);
});
