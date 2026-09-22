// The Direct3D 11 plugin's backend (src/plugins/d3d11/ui/backend.ts) reading a capture the way its
// library records one (src/plugins/d3d11/src/capture.h): synthetic passes around what drew into one
// set of render targets, a state snapshot on every draw and dispatch, and a deferred context's
// commands inlined under the ExecuteCommandList that ran them.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "d3d11-plugin-"));
const out = join(dir, "entry.mjs");
buildSync({
  stdin: {
    contents: `
      export { backendFor, backendForObjectType, shortTypeName } from "./backend.ts";
      export { activatePlugin } from "./plugin_host.ts";
      export { labelNameOf } from "./command_sets.ts";
      export { CaptureData } from "./capture_data.ts";
      export { frameRenderGraph } from "./frame_graph.ts";
      export { drawState } from "./draw_state.ts";
      export { meshInput } from "./mesh_input.ts";
      export { VulkanObject } from "./vulkan/vulkan_object.ts";
      export { pipelineStages } from "./shader_cache.ts";
      export { hasD3D12Reflection, d3d12Reflection } from "./d3d12/reflection.ts";
      export * as d3d11 from "../../../plugins/d3d11/ui/backend.ts";
    `,
    resolveDir: join(here, "..", "src", "renderer"), loader: "ts",
  },
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const m = await import(pathToFileURL(out).href);

const pluginInfo = { id: "d3d11", name: "Direct3D 11", version: "0.1.0", api: "d3d11", dir: "/plugins/d3d11", backendUrl: null, error: null };
await m.activatePlugin(m.d3d11, pluginInfo, "mcp");

// ------------------------------------------------------------------------------------------
// A capture of the Direct3D 11 library: a deferred context draws an indexed cube into an offscreen
// target with a depth buffer, its depth discarded; the immediate context runs that list, then draws
// the offscreen color onto the back buffer and presents.

const DEVICE = 1, CTX = 2, DEFERRED = 3;
const ref = (id, cls) => ({ __id: id, __class: cls });
const ctx = ref(CTX, "ID3D11DeviceContext");
const addObject = (id, type, cmd, args, label = null, parent = DEVICE) => ({ action: "AddObject", id, parent, type, cmd, index: 0, handle: `0x${id}`, label, args });
const reflection = {
  vertex: {
    target: "vs_5_0", inputs: [{ name: "POSITION", semantic: "POSITION", index: 0, type: { kind: "vector", element: { kind: "scalar", base: "float", width: 32, size: 4 }, count: 3, size: 12 } }],
    outputs: [], resources: [{
      kind: "cbuffer", name: "Transform", register: 0, space: 0, count: 1, dimension: "buffer",
      type: { kind: "struct", name: "Transform", size: 80, members: [
        { name: "mvp", offset: 0, type: { kind: "matrix", element: { kind: "scalar", base: "float", width: 32, size: 4 }, columns: 4, rows: 4, stride: 16, rowMajor: false, size: 64 } },
        { name: "tint", offset: 64, type: { kind: "vector", element: { kind: "scalar", base: "float", width: 32, size: 4 }, count: 4, size: 16 } },
      ] },
    }],
  },
};
const objectMessages = [
  { action: "AddObject", id: DEVICE, parent: 0, type: "ID3D11Device", cmd: "D3D11CreateDevice", index: 0, handle: "0x1", label: null, args: { featureLevel: "D3D_FEATURE_LEVEL_11_0", adapter: { Description: "Test GPU" } } },
  addObject(CTX, "ID3D11DeviceContext", "GetImmediateContext", { type: "immediate" }),
  addObject(DEFERRED, "ID3D11DeviceContext", "CreateDeferredContext", { type: "deferred" }),
  addObject(4, "IDXGISwapChain", "CreateSwapChainForHwnd", { pDesc: { BufferDesc: { Width: 8, Height: 8, Format: "DXGI_FORMAT_R8G8B8A8_UNORM" }, BufferCount: 2 } }),
  addObject(5, "ID3D11VertexShader", "CreateVertexShader", { BytecodeLength: 512, stage: "vertex", target: "vs_5_0", reflection }, "cube vs"),
  addObject(6, "ID3D11PixelShader", "CreatePixelShader", { BytecodeLength: 400, stage: "fragment", target: "ps_5_0" }, "cube ps"),
  addObject(7, "ID3D11InputLayout", "CreateInputLayout", {
    pInputElementDescs: [{ SemanticName: "POSITION", Format: "DXGI_FORMAT_R32G32B32_FLOAT", InputSlot: 0 }, { SemanticName: "TEXCOORD", Format: "DXGI_FORMAT_R8G8_UNORM", InputSlot: 0 }],
    elements: [{ location: 0, name: "POSITION", slot: 0, offset: 0, format: "VK_FORMAT_R32G32B32_SFLOAT", perInstance: false, stepRate: 0 },
               { location: 1, name: "TEXCOORD", slot: 0, offset: 12, format: "VK_FORMAT_R8G8_UNORM", perInstance: false, stepRate: 0 }],
  }, "cube layout"),
  addObject(13, "ID3D11Buffer", "CreateBuffer", { pDesc: { ByteWidth: 96, Usage: "D3D11_USAGE_IMMUTABLE", BindFlags: "D3D11_BIND_VERTEX_BUFFER" } }, "vertices"),
  addObject(14, "ID3D11Buffer", "CreateBuffer", { pDesc: { ByteWidth: 12, Usage: "D3D11_USAGE_IMMUTABLE", BindFlags: "D3D11_BIND_INDEX_BUFFER" } }, "indices"),
  addObject(15, "ID3D11Buffer", "CreateBuffer", { pDesc: { ByteWidth: 80, Usage: "D3D11_USAGE_DYNAMIC", BindFlags: "D3D11_BIND_CONSTANT_BUFFER" } }, "transform"),
  addObject(18, "ID3D11Texture2D", "CreateTexture2D", { pDesc: { Width: 4, Height: 4, MipLevels: 1, ArraySize: 1, Format: "DXGI_FORMAT_BC1_UNORM", SampleDesc: { Count: 1 } }, format: "VK_FORMAT_BC1_RGBA_UNORM_BLOCK" }, "checker"),
  addObject(19, "ID3D11ShaderResourceView", "CreateShaderResourceView", { pResource: ref(18, "ID3D11Texture2D"), pDesc: { Format: "DXGI_FORMAT_BC1_UNORM", ViewDimension: "D3D11_SRV_DIMENSION_TEXTURE2D" } }),
  addObject(20, "ID3D11Texture2D", "CreateTexture2D", { pDesc: { Width: 8, Height: 8, MipLevels: 1, ArraySize: 1, Format: "DXGI_FORMAT_R8G8B8A8_UNORM", SampleDesc: { Count: 1 } }, format: "VK_FORMAT_R8G8B8A8_UNORM" }, "offscreen color"),
  addObject(21, "ID3D11Texture2D", "CreateTexture2D", { pDesc: { Width: 8, Height: 8, MipLevels: 1, ArraySize: 1, Format: "DXGI_FORMAT_D24_UNORM_S8_UINT", SampleDesc: { Count: 1 } }, format: "VK_FORMAT_D24_UNORM_S8_UINT" }, "offscreen depth"),
  addObject(22, "ID3D11RenderTargetView", "CreateRenderTargetView", { pResource: ref(20, "ID3D11Texture2D"), pDesc: { Format: "DXGI_FORMAT_R8G8B8A8_UNORM", ViewDimension: "D3D11_RTV_DIMENSION_TEXTURE2D" } }, "offscreen rtv"),
  addObject(23, "ID3D11DepthStencilView", "CreateDepthStencilView", { pResource: ref(21, "ID3D11Texture2D"), pDesc: { Format: "DXGI_FORMAT_D24_UNORM_S8_UINT", ViewDimension: "D3D11_DSV_DIMENSION_TEXTURE2D" } }),
  addObject(24, "ID3D11ShaderResourceView", "CreateShaderResourceView", { pResource: ref(20, "ID3D11Texture2D"), pDesc: { Format: "DXGI_FORMAT_R8G8B8A8_UNORM", ViewDimension: "D3D11_SRV_DIMENSION_TEXTURE2D" } }),
  addObject(25, "ID3D11Texture2D", "GetBuffer", { pDesc: { Width: 8, Height: 8, MipLevels: 1, ArraySize: 1, Format: "DXGI_FORMAT_R8G8B8A8_UNORM", SampleDesc: { Count: 1 } }, format: "VK_FORMAT_R8G8B8A8_UNORM", swapChain: ref(4, "IDXGISwapChain") }, "back buffer", 4),
  addObject(26, "ID3D11RenderTargetView", "CreateRenderTargetView", { pResource: ref(25, "ID3D11Texture2D"), pDesc: { Format: "DXGI_FORMAT_R8G8B8A8_UNORM", ViewDimension: "D3D11_RTV_DIMENSION_TEXTURE2D" } }, "back buffer rtv"),
  addObject(27, "ID3D11RasterizerState", "CreateRasterizerState", { pDesc: { FillMode: "D3D11_FILL_SOLID", CullMode: "D3D11_CULL_BACK", FrontCounterClockwise: false, ScissorEnable: false } }),
  addObject(28, "ID3D11DepthStencilState", "CreateDepthStencilState", { pDesc: { DepthEnable: true, DepthWriteMask: "D3D11_DEPTH_WRITE_MASK_ALL", DepthFunc: "D3D11_COMPARISON_LESS", StencilEnable: false } }),
  addObject(29, "ID3D11SamplerState", "CreateSamplerState", { pDesc: { Filter: "D3D11_FILTER_MIN_MAG_MIP_LINEAR", AddressU: "D3D11_TEXTURE_ADDRESS_WRAP" } }),
  addObject(30, "ID3D11CommandList", "FinishCommandList", { context: ref(DEFERRED, "ID3D11DeviceContext") }, "scene list", DEFERRED),
];
const objects = new Map(objectMessages.map((msg) => [msg.id, new m.VulkanObject(msg)]));
const db = { getObject: (id) => objects.get(id) ?? null };

const viewport = { TopLeftX: 0, TopLeftY: 0, Width: 8, Height: 8, MinDepth: 0, MaxDepth: 1 };
const cubeState = {
  passIndex: 0,
  draw: { indexed: true, count: 6, first: 0, baseVertex: 0, instances: 1, firstInstance: 0, indirect: false },
  topology: "D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST", inputLayout: ref(7, "ID3D11InputLayout"),
  attributes: [{ location: 0, name: "POSITION", slot: 0, offset: 0, format: "VK_FORMAT_R32G32B32_SFLOAT", perInstance: false, stepRate: 0 },
               { location: 1, name: "TEXCOORD", slot: 0, offset: 12, format: "VK_FORMAT_R8G8_UNORM", perInstance: false, stepRate: 0 }],
  vertexBuffers: [{ slot: 0, buffer: ref(13, "ID3D11Buffer"), stride: 20, offset: 0, data: 2 }],
  indexBuffer: { buffer: ref(14, "ID3D11Buffer"), format: "DXGI_FORMAT_R16_UINT", offset: 0, data: 1 },
  stages: {
    vertex: { shader: ref(5, "ID3D11VertexShader"), constantBuffers: [{ slot: 0, buffer: ref(15, "ID3D11Buffer"), offset: 0, size: 80, data: 4 }], resources: [], samplers: [] },
    fragment: { shader: ref(6, "ID3D11PixelShader"), constantBuffers: [], resources: [{ slot: 0, view: ref(19, "ID3D11ShaderResourceView"), resource: ref(18, "ID3D11Texture2D"), capture: 1 }], samplers: [{ slot: 0, sampler: ref(29, "ID3D11SamplerState") }] },
  },
  uavs: [],
  renderTargets: [{ slot: 0, view: ref(22, "ID3D11RenderTargetView"), resource: ref(20, "ID3D11Texture2D") }],
  depthStencil: { view: ref(23, "ID3D11DepthStencilView"), resource: ref(21, "ID3D11Texture2D") },
  rasterizerState: ref(27, "ID3D11RasterizerState"), viewports: [viewport], scissors: [],
  blend: { state: null, factor: [1, 1, 1, 1], sampleMask: 0xffffffff },
  depthStencilState: { state: ref(28, "ID3D11DepthStencilState"), stencilRef: 0 },
};
const quadState = {
  passIndex: 1,
  draw: { indexed: false, count: 3, first: 0, baseVertex: 0, instances: 1, firstInstance: 0, indirect: false },
  topology: "D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST", inputLayout: null, attributes: [], vertexBuffers: [],
  stages: {
    vertex: { shader: ref(5, "ID3D11VertexShader"), constantBuffers: [], resources: [], samplers: [] },
    fragment: { shader: ref(6, "ID3D11PixelShader"), constantBuffers: [], resources: [{ slot: 0, view: ref(24, "ID3D11ShaderResourceView"), resource: ref(20, "ID3D11Texture2D"), capture: 2 }], samplers: [] },
  },
  uavs: [],
  renderTargets: [{ slot: 0, view: ref(26, "ID3D11RenderTargetView"), resource: ref(25, "ID3D11Texture2D") }],
  rasterizerState: null, viewports: [viewport], scissors: [],
  blend: { state: null, factor: [1, 1, 1, 1], sampleMask: 0xffffffff },
  depthStencilState: { state: null, stencilRef: 0 },
};
const attachment = (attachment, aspect, view, resource, format, dxgiFormat) => ({ attachment, aspect, view, resource, mip: 0, firstSlice: 0, slices: 1, format, dxgiFormat, width: 8, height: 8 });
const child = (method, args, extra = {}) => ({ method, args, ...extra });
const listCommands = [
  child("BeginEvent", { Name: "Scene" }),
  child("OMSetRenderTargets", { NumViews: 1, ppRenderTargetViews: [ref(22, "ID3D11RenderTargetView")], pDepthStencilView: ref(23, "ID3D11DepthStencilView") }),
  child("BeginRenderPass", {
    passIndex: 0, synthetic: true, cleared: [0, "depth", "stencil"], discarded: ["depth", "stencil"],
    attachments: [
      attachment(0, "color", ref(22, "ID3D11RenderTargetView"), ref(20, "ID3D11Texture2D"), "VK_FORMAT_R8G8B8A8_UNORM", "DXGI_FORMAT_R8G8B8A8_UNORM"),
      attachment(1, "depth", ref(23, "ID3D11DepthStencilView"), ref(21, "ID3D11Texture2D"), "VK_FORMAT_D24_UNORM_S8_UINT", "DXGI_FORMAT_D24_UNORM_S8_UINT"),
    ],
  }),
  child("ClearRenderTargetView", { pRenderTargetView: ref(22, "ID3D11RenderTargetView"), ColorRGBA: [0, 0, 0, 1] }),
  child("ClearDepthStencilView", { pDepthStencilView: ref(23, "ID3D11DepthStencilView"), ClearFlags: "D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL", Depth: 1, Stencil: 0 }),
  child("VSSetShader", { pShader: ref(5, "ID3D11VertexShader"), NumClassInstances: 0 }),
  child("DrawIndexed", { IndexCount: 6, StartIndexLocation: 0, BaseVertexLocation: 0 }, { state: cubeState }),
  child("DiscardView", { pResourceView: ref(23, "ID3D11DepthStencilView") }),
  child("EndRenderPass", { passIndex: 0, synthetic: true }),
  child("EndEvent", {}),
  child("FinishCommandList", { RestoreDeferredContextState: false }),
];
const command = (method, args, extra = {}) => ({ frame: 0, method, object: ctx, args, ...extra });
const commands = [
  command("ExecuteCommandList", { pCommandList: ref(30, "ID3D11CommandList"), RestoreContextState: false }, { children: [{ commandBuffer: DEFERRED, commands: listCommands }] }),
  command("OMSetRenderTargets", { NumViews: 1, ppRenderTargetViews: [ref(26, "ID3D11RenderTargetView")], pDepthStencilView: null }),
  command("BeginRenderPass", {
    passIndex: 1, synthetic: true,
    attachments: [attachment(0, "color", ref(26, "ID3D11RenderTargetView"), ref(25, "ID3D11Texture2D"), "VK_FORMAT_R8G8B8A8_UNORM", "DXGI_FORMAT_R8G8B8A8_UNORM")],
  }),
  command("PSSetShaderResources", { StartSlot: 0, NumViews: 1, ppShaderResourceViews: [ref(24, "ID3D11ShaderResourceView")] }),
  command("Draw", { VertexCount: 3, StartVertexLocation: 0 }, { state: quadState }),
  command("EndRenderPass", { passIndex: 1, synthetic: true }),
  command("Present", { swapChain: ref(4, "IDXGISwapChain"), SyncInterval: 1, Flags: 0 }),
].map((c, index) => ({ index, ...c }));

const data = new m.CaptureData();
data.handleMessage({ action: "CaptureFrameResults", frame: 10, frames: 1, count: commands.length, batches: 1, api: "d3d11" });
data.handleMessage({ action: "CaptureFrameCommands", frame: 10, index: 0, commands });
// Index data: 0,1,2, 2,1,3. Vertices: four of 20 bytes, position floats then uv bytes.
const indexBytes = new Uint8Array(new Uint16Array([0, 1, 2, 2, 1, 3]).buffer);
const vertexBytes = new Uint8Array(80);
const view = new DataView(vertexBytes.buffer);
[[0, 0, 0], [1, 0, 0], [0, 1, 0], [1, 1, 0]].forEach((p, i) => p.forEach((v, k) => view.setFloat32(i * 20 + k * 4, v, true)));
data.handleMessage({
  action: "CaptureBuffers", count: 3, buffers: [
    { id: 1, buffer: 14, frame: 0, commandBuffer: DEFERRED, offset: 0, size: 12 },
    { id: 2, buffer: 13, frame: 0, commandBuffer: DEFERRED, offset: 0, size: 80 },
    { id: 4, buffer: 15, frame: 0, commandBuffer: DEFERRED, offset: 0, size: 80 },
  ],
});
data.handleMessage({ action: "CaptureBufferData", id: 1, size: 12, __binary: indexBytes });
data.handleMessage({ action: "CaptureBufferData", id: 2, size: 80, __binary: vertexBytes });

/** The flattened stream: the list's commands inlined after the ExecuteCommandList. */
const flat = data.commandsForFrame(0);
const drawIndexed = flat.find((c) => c.method === "DrawIndexed");
const draw = flat.find((c) => c.method === "Draw");

test("the plugin registers as the d3d11 API and owns the ID3D11 and swap chain types", () => {
  const b = m.backendFor("d3d11");
  assert.equal(b.displayName, "Direct3D 11");
  assert.equal(b.builtin, false);
  assert.equal(b.submitCall, "Present");
  assert.equal(m.backendForObjectType("ID3D11Buffer")?.id, "d3d11");
  assert.equal(m.backendForObjectType("IDXGISwapChain")?.id, "d3d11", "the swap chain type is the plugin's, not the D3D12 backend's");
  assert.equal(m.shortTypeName("ID3D11Texture2D"), "Texture2D");
});

test("the command sets classify the library's commands, name its passes and read its events", () => {
  const sets = data.sets;
  assert.ok(sets.DRAW.has("DrawIndexed") && sets.DRAW.has("Draw") && sets.DISPATCH.has("Dispatch"));
  assert.ok(sets.PASS_BEGIN.has("BeginRenderPass") && sets.PASS_END.has("EndRenderPass"));
  assert.ok(sets.SUBMIT.has("Present"), "a present bounds a context's pass numbering, as the library's does");
  assert.ok(sets.COMPUTE_PASS_END.has("OMSetRenderTargets") && sets.COMPUTE_PASS_END.has("Present"));
  assert.equal(m.labelNameOf(flat[1], sets), "Scene");
  const names = new Map([[20, "offscreen color"], [25, "back buffer"], [5, "cube vs"], [22, "offscreen rtv"]]);
  const nameOf = (v) => (v && typeof v.__id === "number" ? names.get(v.__id) ?? "" : "");
  assert.equal(sets.passLabel(flat[3], 0, nameOf), "Render Pass 0: offscreen color + depth");
  assert.equal(sets.passLabel(commands[2], 1, nameOf), "Render Pass 1: back buffer");
  assert.equal(sets.summarize(drawIndexed, nameOf), "6 idx");
  assert.equal(sets.summarize(draw, nameOf), "3 verts");
  assert.equal(sets.summarize(flat[6], nameOf), "cube vs");
  assert.equal(sets.summarize(flat[2], nameOf), "offscreen rtv + ");
  assert.deepEqual(sets.drawArgsOf(drawIndexed), { indexed: true, indexCount: 6, firstIndex: 0, vertexOffset: 0, instanceCount: 1 });
});

test("a draw inlined from a deferred context keeps its state, in the shape the mesh view reads", () => {
  assert.equal(drawIndexed.secondary, DEFERRED, "the inlined command names the deferred context");
  assert.equal(drawIndexed.object.__id, CTX, "and belongs to the immediate context's stream");
  const state = m.drawState(data, db, drawIndexed);
  assert.equal(state.pipeline, null, "Direct3D 11 has no pipeline object");
  assert.deepEqual(state.shaders.map((s) => s.id), [5, 6], "the bound shaders stand in for one");
  assert.equal(state.dynamic.topology, "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST");
  assert.equal(state.cullMode, "VK_CULL_MODE_BACK_BIT");
  assert.equal(state.frontFace, "VK_FRONT_FACE_CLOCKWISE");
  assert.equal(state.depthStencil?.id, 28);
  assert.deepEqual([...state.vertexBuffers.keys()], [0]);
  assert.equal(state.vertexBuffers.get(0).dataId, 2);
  assert.equal(state.indexBuffer.indexType, "VK_INDEX_TYPE_UINT16");
  assert.equal(state.vertexInput.pVertexBindingDescriptions[0].stride, 20, "the stride is the bound buffer's, not the layout's");
  const formats = state.vertexInput.pVertexAttributeDescriptions.map((a) => a.format);
  assert.deepEqual(formats, ["VK_FORMAT_R32G32B32_SFLOAT", "VK_FORMAT_R8G8_UNORM"]);
  assert.equal(state.stageBuffers.get("vertex:0").dataId, 4);
  assert.equal(state.stageTextures.get("fragment:0").dataId, 1);
  assert.equal(state.stageSamplers.get("fragment:0").sampler.__id, 29);
  assert.deepEqual(state.viewports, [{ x: 0, y: 0, width: 8, height: 8, minDepth: 0, maxDepth: 1 }]);
  const b = m.backendFor("d3d11");
  assert.deepEqual([...b.vertexInputNames(drawIndexed)], [[0, "POSITION"], [1, "TEXCOORD"]]);
  const mesh = m.meshInput(data, db, drawIndexed, new Map([[0, "position"]]));
  assert.deepEqual(mesh.indices, [0, 1, 2, 2, 1, 3]);
  assert.deepEqual(mesh.values(5, 0), [1, 1, 0], "the sixth index is vertex 3, read at 3 x stride");
});

test("a shader object's reflection is read the way a D3D12 pipeline's is", () => {
  const vs = objects.get(5);
  assert.ok(m.hasD3D12Reflection(vs));
  const r = m.d3d12Reflection(vs, "vertex");
  assert.equal(r.version, "dxbc");
  assert.equal(r.resources[0].name, "Transform");
  assert.equal(m.pipelineStages(vs, db).length, 0, "with no blob attached in this fixture there is no stage source");
});

test("the plugin's command details describe a draw section by section", () => {
  const b = m.backendFor("d3d11");
  const sections = b.commandDetails(drawIndexed, { data, db, nameOf: (id) => db.getObject(id)?.name ?? "" });
  const titles = sections.map((s) => s.title);
  assert.deepEqual(titles, ["Shaders", "Vertex Input", "Index Buffer", "Vertex Constant Buffers", "Pixel Shader Resources", "Pixel Samplers", "Rasterizer", "Depth and Stencil", "Blend", "Render Targets"]);
  const input = sections.find((s) => s.title === "Vertex Input");
  assert.deepEqual(input.table.rows[0][8], { buffer: 2 });
  assert.equal(input.table.rows[1][1], "TEXCOORD");
  const cbs = sections.find((s) => s.title === "Vertex Constant Buffers");
  const contents = cbs.table.rows[0][5];
  assert.equal(contents.buffer, 4);
  assert.equal(contents.blockName, "Transform");
  assert.deepEqual(contents.members[0], { name: "mvp", type: "float4x4", offset: 0 }, "the buffer is typed by the shader's reflection");
  assert.equal(cbs.table.rows[0][1], "Transform");
  assert.deepEqual(sections.find((s) => s.title === "Pixel Shader Resources").table.rows[0][4], { texture: 1 });
  assert.equal(sections.find((s) => s.title === "Depth and Stencil").rows[1][1], "COMPARISON_LESS");
  const pass = b.commandDetails(flat[3], { data, db, nameOf: () => "" });
  assert.equal(pass[0].title, "Render Pass");
  assert.equal(pass[0].table.rows.length, 2);
  assert.equal(pass[0].table.rows[1][6], "cleared, discarded");
});

test("the render graph sees the passes, what they sample, and what they clear and throw away", () => {
  const graph = m.frameRenderGraph(data, db);
  assert.equal(graph.nodes.length, 2);
  const [offscreen, present] = graph.nodes;
  assert.equal(offscreen.label, "offscreen color");
  assert.equal(present.label, "back buffer");
  const depth = offscreen.writes.find((u) => u.resource.objectId === 21);
  assert.equal(depth.dropped, true, "the discarded depth is thrown away");
  assert.equal(depth.discards, true, "and cleared before the pass drew");
  const color = offscreen.writes.find((u) => u.resource.objectId === 20);
  assert.equal(color.discards, true);
  assert.equal(color.dropped, false);
  assert.ok(offscreen.reads.some((u) => u.resource.objectId === 18 && u.usage === "sampled"));
  assert.ok(present.reads.some((u) => u.resource.objectId === 20 && u.usage === "sampled"));
  assert.ok(present.writes.some((u) => u.resource.objectId === 25 && u.resource.presented), "the back buffer is what the frame is for");
  assert.ok(graph.edges.some((e) => e.from === offscreen && e.to === present), "the offscreen pass feeds the present's");
  assert.equal(graph.unreadNodes.length, 0);
});

test("D3D11 objects are summarized by the plugin and their types read without the prefix", () => {
  assert.equal(objects.get(20).shortType, "Texture2D");
  assert.equal(objects.get(20).summary(db), "R8G8B8A8_UNORM 8x8");
  assert.equal(objects.get(25).summary(db), "R8G8B8A8_UNORM 8x8 (back buffer)");
  assert.equal(objects.get(15).summary(db), "80 B constant_buffer dynamic");
  assert.equal(objects.get(5).summary(db), "vs_5_0, 512 B");
  assert.equal(objects.get(7).summary(db), "POSITION, TEXCOORD");
  assert.equal(objects.get(DEVICE).summary(db), "feature level 11.0, Test GPU");
  assert.equal(objects.get(27).summary(db), "solid, cull back");
});
