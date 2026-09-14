// Direct3D 12's command classification (src/renderer/d3d12/command_sets.ts): the vertex and
// index buffer views a command binds, as the capture library records them (BufferLocation
// resolved to {address, buffer, offset}), and the one-line summaries the command list shows.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const out = join(mkdtempSync(join(tmpdir(), "d3d12-sets-")), "entry.mjs");
buildSync({
  stdin: {
    contents: `
      export { setsFor, labelNameOf, boundPipelineOf } from "./command_sets.ts";
      export { D3D12_SETS } from "./d3d12/command_sets.ts";
      export { vkFormatOfDxgi, dxgiFormatBytes, dxgiFormatIsDepth, dxgiFormatIsBlockCompressed } from "./d3d12/dxgi_format.ts";
      export { d3d12InputElements, vkTopologyOfD3D } from "./d3d12/d3d12_object.ts";
    `,
    resolveDir: join(here, "..", "src", "renderer"), loader: "ts",
  },
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const { setsFor, labelNameOf, boundPipelineOf, D3D12_SETS, vkFormatOfDxgi, dxgiFormatBytes, dxgiFormatIsDepth, dxgiFormatIsBlockCompressed, d3d12InputElements, vkTopologyOfD3D } = await import(pathToFileURL(out).href);

const ref = (id, cls) => ({ __id: id, __class: cls });
const list = ref(5, "ID3D12GraphicsCommandList");
const names = new Map([[10, "Vertices"], [11, "Indices"], [20, "Scene PSO"], [30, "Backbuffer 0"], [31, "Depth"], [40, "Args"]]);
const nameOf = (v) => (v && typeof v.__id === "number" ? names.get(v.__id) ?? "" : "");
const command = (index, method, args, extra = {}) => ({ index, frame: 0, method, object: list, args, ...extra });

test("the capture's api selects the D3D12 tables", () => {
  assert.equal(setsFor("d3d12"), D3D12_SETS);
  assert.ok(D3D12_SETS.DRAW.has("DrawIndexedInstanced"));
  assert.ok(D3D12_SETS.PASS_BEGIN.has("OMSetRenderTargets"));
  assert.ok(D3D12_SETS.PASS_END.has("EndRenderTargets"), "the library's synthetic pass end closes an OMSetRenderTargets pass");
  assert.ok(D3D12_SETS.COMPUTE_PASS_END.has("ResourceBarrier"));
  assert.equal(D3D12_SETS.bindPointOf("Dispatch"), "compute");
  assert.equal(D3D12_SETS.bindPointOf("DrawInstanced"), "graphics");
  assert.equal(D3D12_SETS.pipelineBindPointOf("SetPipelineState", { bindPoint: "compute" }), "compute");
  assert.equal(D3D12_SETS.pipelineBindPointOf("SetPipelineState", { pipeline: ref(20, "ID3D12PipelineState") }), "graphics");
  assert.equal(D3D12_SETS.passIsCompute, undefined, "PASS_BEGIN is render-only");
  assert.deepEqual(boundPipelineOf({ pPipelineState: ref(20, "ID3D12PipelineState") }), ref(20, "ID3D12PipelineState"));
});

test("IASetVertexBuffers binds a view per slot from StartSlot, with the resolved buffer and the stride", () => {
  const cmd = command(3, "IASetVertexBuffers", {
    StartSlot: 1, NumViews: 2,
    pViews: [
      { BufferLocation: { address: "0x1000", buffer: ref(10, "ID3D12Resource"), offset: 64 }, SizeInBytes: 1200, StrideInBytes: 24 },
      { BufferLocation: { address: "0x2000", buffer: ref(10, "ID3D12Resource"), offset: 4096 }, SizeInBytes: 400, StrideInBytes: 8 },
    ],
  }, { bufferData: [7, 8] });
  const bound = D3D12_SETS.vertexBuffersOf(cmd);
  assert.equal(bound.length, 2);
  assert.deepEqual(bound.map((b) => [b.binding, b.buffer.__id, b.offset, b.size, b.stride, b.dataId]), [[1, 10, 64, 1200, 24, 7], [2, 10, 4096, 400, 8, 8]]);
  assert.equal(D3D12_SETS.vertexBuffersOf(command(4, "DrawInstanced", { VertexCountPerInstance: 3 })).length, 0);
});

test("IASetIndexBuffer declares the index buffer with its DXGI format as the index type", () => {
  const cmd = command(5, "IASetIndexBuffer", {
    pView: { BufferLocation: { address: "0x3000", buffer: ref(11, "ID3D12Resource"), offset: 0 }, SizeInBytes: 36, Format: "DXGI_FORMAT_R16_UINT" },
  }, { bufferData: [9] });
  const ib = D3D12_SETS.indexBufferOf(cmd);
  assert.equal(ib.buffer.__id, 11);
  assert.equal(ib.offset, 0);
  assert.equal(ib.indexType, "DXGI_FORMAT_R16_UINT");
  assert.equal(ib.dataId, 9);
  assert.equal(D3D12_SETS.indexBufferOf(command(6, "DrawIndexedInstanced", { IndexCountPerInstance: 6 })), null, "a draw declares no index buffer");
});

test("summaries name what matters at a glance", () => {
  const summary = (method, args) => D3D12_SETS.summarize(command(0, method, args), nameOf);
  assert.equal(summary("DrawInstanced", { VertexCountPerInstance: 36, InstanceCount: 4, StartVertexLocation: 0, StartInstanceLocation: 0 }), "36 verts x4");
  assert.equal(summary("DrawIndexedInstanced", { IndexCountPerInstance: 6, InstanceCount: 1 }), "6 idx x1");
  assert.equal(summary("Dispatch", { ThreadGroupCountX: 8, ThreadGroupCountY: 4, ThreadGroupCountZ: 1 }), "8x4x1 groups");
  assert.equal(summary("SetPipelineState", { pipeline: ref(20, "ID3D12PipelineState"), bindPoint: "graphics" }), "Scene PSO");
  assert.equal(summary("SetGraphicsRootConstantBufferView", { RootParameterIndex: 2, BufferLocation: { address: "0x100", buffer: ref(10, "ID3D12Resource"), offset: 256 } }), "[2] Vertices +256");
  assert.equal(summary("SetGraphicsRoot32BitConstants", { RootParameterIndex: 1, pValues: { base64: "" }, offset: 0, size: 16, stageFlags: "graphics" }), "[1] 16 bytes");
  assert.equal(summary("OMSetRenderTargets", {
    NumRenderTargetDescriptors: 2,
    pRenderTargetDescriptors: [{ heap: ref(50, "ID3D12DescriptorHeap"), index: 0, resource: ref(30, "ID3D12Resource"), view: null }, { heap: ref(50, "ID3D12DescriptorHeap"), index: 1, resource: ref(31, "ID3D12Resource"), view: null }],
    pDepthStencilDescriptor: { heap: ref(51, "ID3D12DescriptorHeap"), index: 0, resource: ref(31, "ID3D12Resource"), view: null },
  }), "Backbuffer 0 +1 + depth");
  assert.equal(summary("ClearRenderTargetView", { RenderTargetView: { heap: ref(50, "ID3D12DescriptorHeap"), index: 0, resource: ref(30, "ID3D12Resource") }, ColorRGBA: [0, 0, 0, 1] }), "Backbuffer 0");
  assert.equal(summary("ResourceBarrier", { NumBarriers: 3, pBarriers: [{}, {}, {}] }), "3 barriers");
  assert.equal(summary("ExecuteCommandLists", { NumCommandLists: 1, ppCommandLists: [list] }), "1 list");
  assert.equal(summary("BeginEvent", { label: "Shadows" }), "\"Shadows\"");
  assert.equal(summary("ExecuteIndirect", { pCommandSignature: ref(60, "ID3D12CommandSignature"), MaxCommandCount: 16, pArgumentBuffer: ref(40, "ID3D12Resource") }), "Args x16");
  // The generic fallback: scalars and enums.
  assert.equal(summary("OMSetStencilRef", { StencilRef: 1 }), "StencilRef 1");
  assert.equal(labelNameOf(command(0, "BeginEvent", { label: "Shadows" })), "Shadows");
});

test("DXGI formats map to the protocol's VK_FORMAT spellings", () => {
  assert.equal(vkFormatOfDxgi("DXGI_FORMAT_R32G32B32A32_FLOAT"), "VK_FORMAT_R32G32B32A32_SFLOAT");
  assert.equal(vkFormatOfDxgi("DXGI_FORMAT_B8G8R8A8_UNORM"), "VK_FORMAT_B8G8R8A8_UNORM");
  assert.equal(vkFormatOfDxgi("DXGI_FORMAT_R10G10B10A2_UNORM"), "VK_FORMAT_A2B10G10R10_UNORM_PACK32");
  assert.equal(vkFormatOfDxgi("DXGI_FORMAT_R11G11B10_FLOAT"), "VK_FORMAT_B10G11R11_UFLOAT_PACK32");
  assert.equal(vkFormatOfDxgi("DXGI_FORMAT_D24_UNORM_S8_UINT"), "VK_FORMAT_D24_UNORM_S8_UINT");
  assert.equal(vkFormatOfDxgi("DXGI_FORMAT_D32_FLOAT_S8X24_UINT"), "VK_FORMAT_D32_SFLOAT_S8_UINT");
  assert.equal(vkFormatOfDxgi("DXGI_FORMAT_BC7_UNORM_SRGB"), "VK_FORMAT_BC7_SRGB_BLOCK");
  assert.equal(vkFormatOfDxgi("DXGI_FORMAT_BC6H_SF16"), "VK_FORMAT_BC6H_SFLOAT_BLOCK");
  assert.equal(vkFormatOfDxgi("DXGI_FORMAT_R8G8B8A8_TYPELESS"), "VK_FORMAT_R8G8B8A8_UNORM", "a typeless format reads as its UNORM family");
  assert.equal(vkFormatOfDxgi("R32G32_FLOAT"), "VK_FORMAT_R32G32_SFLOAT", "the prefix is optional");
  assert.equal(vkFormatOfDxgi("DXGI_FORMAT_R1_UNORM"), null);
  assert.equal(dxgiFormatBytes("DXGI_FORMAT_R16G16B16A16_FLOAT"), 8);
  assert.equal(dxgiFormatBytes("DXGI_FORMAT_BC1_UNORM"), 8, "per 4x4 block");
  assert.ok(dxgiFormatIsDepth("DXGI_FORMAT_D32_FLOAT"));
  assert.ok(!dxgiFormatIsDepth("DXGI_FORMAT_R32_FLOAT"));
  assert.ok(dxgiFormatIsBlockCompressed("DXGI_FORMAT_BC3_UNORM"));
  assert.ok(!dxgiFormatIsBlockCompressed("DXGI_FORMAT_R8_UNORM"));
});

test("an input layout's elements resolve appended offsets per slot and name their attributes by semantic", () => {
  const pipeline = { type: "ID3D12PipelineState", descriptor: { InputLayout: { NumElements: 3, pInputElementDescs: [
    { SemanticName: "POSITION", SemanticIndex: 0, Format: "DXGI_FORMAT_R32G32B32_FLOAT", InputSlot: 0, AlignedByteOffset: 0, InputSlotClass: "D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA", InstanceDataStepRate: 0 },
    { SemanticName: "TEXCOORD", SemanticIndex: 0, Format: "DXGI_FORMAT_R32G32_FLOAT", InputSlot: 0, AlignedByteOffset: 0xffffffff, InputSlotClass: "D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA", InstanceDataStepRate: 0 },
    { SemanticName: "TEXCOORD", SemanticIndex: 1, Format: "DXGI_FORMAT_R32G32B32A32_FLOAT", InputSlot: 1, AlignedByteOffset: 0xffffffff, InputSlotClass: "D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA", InstanceDataStepRate: 1 },
  ] } } };
  const elements = d3d12InputElements(pipeline);
  assert.deepEqual(elements.map((e) => [e.location, e.name, e.slot, e.offset, e.perInstance]), [[0, "POSITION0", 0, 0, false], [1, "TEXCOORD0", 0, 12, false], [2, "TEXCOORD1", 1, 0, true]]);
  assert.equal(vkTopologyOfD3D("D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP"), "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP");
  assert.equal(vkTopologyOfD3D("D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE"), "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST");
});
