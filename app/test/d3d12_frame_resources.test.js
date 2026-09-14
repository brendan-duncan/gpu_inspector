// The render graph of a D3D12 capture (src/renderer/d3d12/frame_resources.ts through
// src/renderer/frame_graph.ts): a compute pass writing a texture through a UAV, a barrier, and a
// render pass on the back buffer sampling it through an SRV table. Plus the per-command rules
// (src/renderer/d3d12/frame_analysis.ts) and the draw state on the same stream.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const out = join(mkdtempSync(join(tmpdir(), "d3d12-graph-")), "entry.mjs");
buildSync({
  stdin: {
    contents: `
      export { CaptureData } from "./capture_data.ts";
      export { frameRenderGraph } from "./frame_graph.ts";
      export { analyzeD3D12Frame } from "./d3d12/frame_analysis.ts";
      export { drawState, vertexLayout } from "./draw_state.ts";
      export { meshInput } from "./mesh_input.ts";
      export { VulkanObject, objectMemoryBytes } from "./vulkan/vulkan_object.ts";
    `,
    resolveDir: join(here, "..", "src", "renderer"), loader: "ts",
  },
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const { CaptureData, frameRenderGraph, analyzeD3D12Frame, drawState, vertexLayout, meshInput, VulkanObject, objectMemoryBytes } = await import(pathToFileURL(out).href);

const ref = (id, cls) => ({ __id: id, __class: cls });
const object = (id, type, cmd, args, label, parent = 1) => new VulkanObject({ id, type, parent, cmd, index: 0, handle: `0x${id.toString(16)}`, label, args, blobs: [] });
const texture = (id, label, width, height, format, flags = "D3D12_RESOURCE_FLAG_NONE") => object(id, "ID3D12Resource", "CreateCommittedResource", { pDesc: {
  Dimension: "D3D12_RESOURCE_DIMENSION_TEXTURE2D", Alignment: 0, Width: width, Height: height, DepthOrArraySize: 1, MipLevels: 1, Format: format,
  SampleDesc: { Count: 1, Quality: 0 }, Layout: "D3D12_TEXTURE_LAYOUT_UNKNOWN", Flags: flags,
} }, label);
const buffer = (id, label, size) => object(id, "ID3D12Resource", "CreateCommittedResource", { pDesc: {
  Dimension: "D3D12_RESOURCE_DIMENSION_BUFFER", Alignment: 0, Width: size, Height: 1, DepthOrArraySize: 1, MipLevels: 1, Format: "DXGI_FORMAT_UNKNOWN",
  SampleDesc: { Count: 1, Quality: 0 }, Layout: "D3D12_TEXTURE_LAYOUT_ROW_MAJOR", Flags: "D3D12_RESOURCE_FLAG_NONE",
} }, label);

const objects = new Map();
const add = (o) => objects.set(o.id, o);
add(object(3, "IDXGISwapChain", "CreateSwapChainForHwnd", { pDesc: { Width: 1280, Height: 720, Format: "DXGI_FORMAT_R8G8B8A8_UNORM", BufferCount: 2, SampleDesc: { Count: 1, Quality: 0 } } }, "Swap chain"));
add(object(4, "ID3D12Resource", "GetBuffer", { Buffer: 0 }, "Back buffer 0", 3));
add(texture(10, "Noise", 256, 256, "DXGI_FORMAT_R8G8B8A8_UNORM", "D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS"));
add(texture(11, "Depth", 1280, 720, "DXGI_FORMAT_D32_FLOAT", "D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL"));
add(buffer(12, "Vertices", 36 * 20));
add(buffer(13, "Indices", 36 * 2));
add(object(20, "ID3D12PipelineState", "CreateComputePipelineState", { pDesc: { pRootSignature: ref(2, "ID3D12RootSignature"), CS: { __bytes: 800 }, reflection: { compute: { entryPoint: "CSMain", target: "cs_6_0", threadGroupSize: [8, 8, 1], inputs: [], outputs: [], resources: [{ kind: "uav", name: "output", register: 0, space: 0, count: 1, dimension: "texture2d", returnType: "float4" }] } } } }, "Noise CS"));
add(object(21, "ID3D12PipelineState", "CreateGraphicsPipelineState", { pDesc: {
  pRootSignature: ref(2, "ID3D12RootSignature"), VS: { __bytes: 1200 }, PS: { __bytes: 2400 },
  InputLayout: { NumElements: 2, pInputElementDescs: [
    { SemanticName: "POSITION", SemanticIndex: 0, Format: "DXGI_FORMAT_R32G32B32_FLOAT", InputSlot: 0, AlignedByteOffset: 0, InputSlotClass: "D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA", InstanceDataStepRate: 0 },
    { SemanticName: "TEXCOORD", SemanticIndex: 0, Format: "DXGI_FORMAT_R32G32_FLOAT", InputSlot: 0, AlignedByteOffset: 0xffffffff, InputSlotClass: "D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA", InstanceDataStepRate: 0 },
  ] },
  PrimitiveTopologyType: "D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE", NumRenderTargets: 1, RTVFormats: ["DXGI_FORMAT_R8G8B8A8_UNORM"], DSVFormat: "DXGI_FORMAT_D32_FLOAT",
} }, "Scene PSO"));
add(object(50, "ID3D12DescriptorHeap", "CreateDescriptorHeap", { pDesc: { Type: "D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV", NumDescriptors: 64, Flags: "D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE" } }, "Heap"));
add(object(2, "ID3D12RootSignature", "CreateRootSignature", {}, "Root signature"));
const db = { getObject: (id) => objects.get(id) ?? null, allObjects: objects };

const list = ref(5, "ID3D12GraphicsCommandList");
const queue = ref(6, "ID3D12CommandQueue");
const handle = (index, resource, view) => ({ heap: ref(50, "ID3D12DescriptorHeap"), index, resource: ref(resource, "ID3D12Resource"), view });
const table = (bindPoint, set, type, register, descriptors) => ({ bindPoint, sets: [{ set, descriptorSet: ref(50, "ID3D12DescriptorHeap"), layout: ref(2, "ID3D12RootSignature"),
  bindings: [{ binding: 0, type, register, space: 0, stages: "D3D12_SHADER_VISIBILITY_ALL", descriptors }] }] });
const commands = [
  ["SetComputeRootSignature", { pRootSignature: ref(2, "ID3D12RootSignature") }],
  ["SetPipelineState", { pipeline: ref(20, "ID3D12PipelineState"), bindPoint: "compute" }],
  ["SetComputeRootDescriptorTable", { RootParameterIndex: 0, BaseDescriptor: { heap: ref(50, "ID3D12DescriptorHeap"), index: 0 } },
    { descriptors: table("compute", 0, "D3D12_DESCRIPTOR_RANGE_TYPE_UAV", 0, [{ resource: ref(10, "ID3D12Resource"), view: { ViewDimension: "D3D12_UAV_DIMENSION_TEXTURE2D", Format: "DXGI_FORMAT_R8G8B8A8_UNORM", Texture2D: { MipSlice: 0, PlaneSlice: 0 } } }]) }],
  ["Dispatch", { ThreadGroupCountX: 32, ThreadGroupCountY: 32, ThreadGroupCountZ: 1 }],
  ["ResourceBarrier", { NumBarriers: 1, pBarriers: [{ Type: "D3D12_RESOURCE_BARRIER_TYPE_TRANSITION", Flags: "D3D12_RESOURCE_BARRIER_FLAG_NONE",
    Transition: { pResource: ref(10, "ID3D12Resource"), Subresource: 0xffffffff, StateBefore: "D3D12_RESOURCE_STATE_UNORDERED_ACCESS", StateAfter: "D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE" } }] }],
  ["OMSetRenderTargets", { NumRenderTargetDescriptors: 1, pRenderTargetDescriptors: [handle(0, 4, { ViewDimension: "D3D12_RTV_DIMENSION_TEXTURE2D", Format: "DXGI_FORMAT_R8G8B8A8_UNORM", Texture2D: { MipSlice: 0, PlaneSlice: 0 } })],
    RTsSingleHandleToDescriptorRange: false, pDepthStencilDescriptor: handle(0, 11, { ViewDimension: "D3D12_DSV_DIMENSION_TEXTURE2D", Format: "DXGI_FORMAT_D32_FLOAT", Texture2D: { MipSlice: 0 } }) }],
  ["ClearRenderTargetView", { RenderTargetView: handle(0, 4, null), ColorRGBA: [0, 0, 0, 1], NumRects: 0, pRects: [] }],
  ["SetGraphicsRootSignature", { pRootSignature: ref(2, "ID3D12RootSignature") }],
  ["SetPipelineState", { pipeline: ref(21, "ID3D12PipelineState"), bindPoint: "graphics" }],
  ["SetPipelineState", { pipeline: ref(21, "ID3D12PipelineState"), bindPoint: "graphics" }],   // redundant
  ["SetGraphicsRootDescriptorTable", { RootParameterIndex: 1, BaseDescriptor: { heap: ref(50, "ID3D12DescriptorHeap"), index: 1 } },
    { descriptors: table("graphics", 1, "D3D12_DESCRIPTOR_RANGE_TYPE_SRV", 0, [{ resource: ref(10, "ID3D12Resource"), view: { ViewDimension: "D3D12_SRV_DIMENSION_TEXTURE2D", Format: "DXGI_FORMAT_R8G8B8A8_UNORM", Texture2D: { MostDetailedMip: 0, MipLevels: 1 } } }]) }],
  ["IASetPrimitiveTopology", { PrimitiveTopology: "D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST" }],
  ["IASetVertexBuffers", { StartSlot: 0, NumViews: 1, pViews: [{ BufferLocation: { address: "0x1000", buffer: ref(12, "ID3D12Resource"), offset: 0 }, SizeInBytes: 720, StrideInBytes: 20 }] }, { bufferData: [1] }],
  ["IASetIndexBuffer", { pView: { BufferLocation: { address: "0x2000", buffer: ref(13, "ID3D12Resource"), offset: 0 }, SizeInBytes: 72, Format: "DXGI_FORMAT_R16_UINT" } }, { bufferData: [2] }],
  ["SetGraphicsRoot32BitConstants", { RootParameterIndex: 2, pValues: { base64: "AACAPwAAAEAAAEBAAACAQA==" }, offset: 0, size: 16, stageFlags: "graphics" }],
  ["DrawIndexedInstanced", { IndexCountPerInstance: 36, InstanceCount: 1, StartIndexLocation: 0, BaseVertexLocation: 0, StartInstanceLocation: 0 }],
  ["EndRenderTargets", null],
  ["Close", {}],
];
const data = new CaptureData();
data.api = "d3d12";
data.commands = [
  { index: 0, frame: 0, method: "ExecuteCommandLists", object: queue, args: { NumCommandLists: 1, ppCommandLists: [list] } },
  ...commands.map(([method, args, extra = {}], i) => ({ index: i + 1, frame: 0, slot: i, method, object: list, args, ...extra })),
];
// The captured vertex and index buffers: a triangle's worth, so the mesh view has something to decode.
const vertices = new Float32Array(36 * 5).map((_, i) => i);
const indices = new Uint16Array(36).map((_, i) => i);
data.buffers.set(1, { info: { id: 1, buffer: 12, frame: 0, commandBuffer: 5, offset: 0, size: vertices.byteLength }, data: new Uint8Array(vertices.buffer) });
data.buffers.set(2, { info: { id: 2, buffer: 13, frame: 0, commandBuffer: 5, offset: 0, size: indices.byteLength }, data: new Uint8Array(indices.buffer) });

test("the render graph has the compute pass and the render pass, joined by the UAV write read as an SRV", () => {
  const graph = frameRenderGraph(data, db);
  assert.deepEqual(graph.nodes.map((n) => [n.kind, n.draws]), [["compute", 1], ["render", 1]]);
  assert.equal(graph.api, "d3d12");
  const [compute, render] = graph.nodes;
  assert.equal(render.label, "Pass 0: Back buffer 0");
  assert.ok(compute.writes.some((w) => w.resource.label === "Noise" && w.usage.includes("unordered access")), "the dispatch writes the noise texture through its UAV");
  const edge = graph.edges.find((e) => e.from === compute && e.to === render);
  assert.ok(edge, "an edge from the compute pass to the render pass");
  assert.equal(edge.version.resource.label, "Noise");
  assert.ok(edge.usage.includes("shader resource"));
  // The pass keeps what the targets held (no load op) and stores them; the clear inside it is folded in.
  const target = render.writes.find((w) => w.resource.label === "Back buffer 0");
  assert.ok(target && target.resource.presented, "the back buffer is the frame's output");
  assert.ok(render.writes.some((w) => w.resource.label === "Depth"));
  assert.ok(render.reads.some((r) => r.usage.includes("vertex buffer")) && render.reads.some((r) => r.usage.includes("index buffer")));
  // The barrier names the noise texture and sits between the two passes.
  assert.equal(graph.syncPoints.length, 1);
  assert.deepEqual(graph.syncPoints[0].resources, ["image:10:m0:l0"]);
  assert.equal(graph.syncPoints[0].after, 0);
  // A state transition is required whatever the data does (D3D12's layout change), so the
  // oversynchronized-barrier rule leaves it alone; only a UAV barrier is questioned.
  assert.ok(graph.syncPoints[0].structural);
  assert.equal(graph.nodes.reduce((n, p) => n + p.unresolvedReads, 0), 0);
});

test("a table the library could not read counts as unresolved", () => {
  const blind = new CaptureData();
  blind.api = "d3d12";
  blind.commands = data.commands.map((c) => (c.method === "SetGraphicsRootDescriptorTable"
    ? { ...c, descriptors: { bindPoint: "graphics", sets: [{ set: 1, descriptorSet: ref(50, "ID3D12DescriptorHeap"), layout: ref(2, "ID3D12RootSignature"), bindings: [] }] } }
    : c));
  const graph = frameRenderGraph(blind, db);
  assert.equal(graph.nodes[1].unresolvedReads, 1);
  assert.equal(graph.edges.length, 0);
});

test("the per-command rules find the redundant pipeline bind and nothing else on this stream", () => {
  const { findings, byCommand } = analyzeD3D12Frame(data, db);
  assert.deepEqual(findings.map((f) => [f.rule, f.count]), [["redundant-pipeline-bind", 1]]);
  assert.ok(byCommand.has(10), "attached to the second SetPipelineState");

  // BeginRenderPass: a PRESERVE of a target the previous pass discarded, and a clear the pass then discards.
  const rp = (begin, end) => ({ cpuDescriptor: handle(0, 4, null), BeginningAccess: { Type: `D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_${begin}` }, EndingAccess: { Type: `D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_${end}` } });
  const passes = new CaptureData();
  passes.api = "d3d12";
  passes.commands = [
    ["ClearRenderTargetView", { RenderTargetView: handle(0, 4, null), ColorRGBA: [0, 0, 0, 1], NumRects: 0 }],
    ["BeginRenderPass", { NumRenderTargets: 1, pRenderTargets: [rp("DISCARD", "DISCARD")], pDepthStencil: null, Flags: "D3D12_RENDER_PASS_FLAG_NONE" }],
    ["DrawInstanced", { VertexCountPerInstance: 3, InstanceCount: 1 }],
    ["EndRenderPass", {}],
    ["BeginRenderPass", { NumRenderTargets: 1, pRenderTargets: [rp("PRESERVE", "PRESERVE")], pDepthStencil: null, Flags: "D3D12_RENDER_PASS_FLAG_NONE" }],
    ["EndRenderPass", {}],
  ].map(([method, args], i) => ({ index: i, frame: 0, method, object: list, args }));
  const rules = analyzeD3D12Frame(passes, db).findings.map((f) => f.rule).sort();
  assert.deepEqual(rules, ["clear-then-discard", "empty-pass", "undefined-load"]);
  const graph = frameRenderGraph(passes, db);
  // The clear outside any pass is a transfer node of its own; the first render pass discards on both ends.
  assert.deepEqual(graph.nodes.map((n) => n.kind), ["transfer", "render", "render"]);
  const first = graph.nodes[1].writes.find((w) => w.resource.label === "Back buffer 0");
  assert.ok(first.discards && first.dropped, "DISCARD / DISCARD");
  const second = graph.nodes[2].writes.find((w) => w.resource.label === "Back buffer 0");
  assert.ok(!second.discards && !second.dropped, "PRESERVE / PRESERVE");
});

test("the draw's state reconstructs the D3D12 bindings, layout and root constants", () => {
  const draw = data.commands.find((c) => c.method === "DrawIndexedInstanced");
  const state = drawState(data, db, draw);
  assert.equal(state.pipeline.id, 21);
  assert.equal(state.bindPoint, "graphics");
  assert.deepEqual([...state.sets.keys()], [1], "the graphics table, not the compute one");
  assert.equal(state.indexBuffer.indexType, "DXGI_FORMAT_R16_UINT");
  assert.equal(state.pushConstants.length, 1);
  assert.equal(state.pushConstants[0].size, 16);
  const vb = state.vertexBuffers.get(0);
  const layout = vertexLayout(state, 0, vb);
  assert.equal(layout.stride, 20);
  assert.deepEqual(layout.attributes.map((a) => [a.location, a.format, a.offset]), [[0, "VK_FORMAT_R32G32B32_SFLOAT", 0], [1, "VK_FORMAT_R32G32_SFLOAT", 12]]);
  const mesh = meshInput(data, db, draw, new Map([[0, "POSITION0"], [1, "TEXCOORD0"]]));
  assert.equal(mesh.topology, "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST");
  assert.equal(mesh.ids.length, 36);
  assert.equal(mesh.position, 0);
  assert.deepEqual(mesh.values(1, 0), [5, 6, 7], "vertex 1's position");
  assert.deepEqual(mesh.notes, []);
});

test("resources report their sizes from their descriptions", () => {
  assert.equal(objectMemoryBytes(objects.get(10), db), 256 * 256 * 4);
  assert.equal(objectMemoryBytes(objects.get(12), db), 720);
  assert.equal(objectMemoryBytes(objects.get(4), db), 1280 * 720 * 4, "a back buffer takes its swap chain's shape");
  assert.equal(objects.get(10).summary(db), "R8G8B8A8_UNORM 256x256");
  assert.equal(objects.get(4).summary(db), "back buffer R8G8B8A8_UNORM 1280x720");
  assert.equal(objects.get(21).summary(db), "graphics", "no payloads or reflection on this one: its kind");
  assert.equal(objects.get(20).summary(db), "compute", "the stages the reflection names");
});
