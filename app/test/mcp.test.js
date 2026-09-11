// The MCP server (src/mcp/) over a capture file built here: that a .gpucap opens the way the UI
// opens one, that each tool reports what the capture holds (the analyses, the bound state at a
// draw, decoded buffers, vertices and images), that failures come back as tool errors, and that
// the bundled server speaks the protocol over stdio.
//
//     cd app && npm test
import { test } from "node:test";
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { mkdirSync, mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { createInterface } from "node:readline";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "mcp-"));
// Nothing from this machine's GPU Inspector settings or environment: the search paths start empty.
process.env.GPU_INSPECTOR_SETTINGS = join(dir, "settings.json");
delete process.env.GPU_INSPECTOR_SOURCE_ROOTS;
delete process.env.GPU_INSPECTOR_SYMBOL_DIRS;
const bundle = (entry, name) => {
  const out = join(dir, `${name}.mjs`);
  buildSync({ entryPoints: [join(here, "..", "src", entry)], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
  return out;
};
const { createServer } = await import(pathToFileURL(bundle("mcp/server.ts", "server")).href);
const { encodeCaptureFile } = await import(pathToFileURL(bundle("renderer/capture_format.ts", "capture_format")).href);

// ------------------------------------------------------------------------------------------
// A capture: one render pass drawing forty three-vertex triangles into a 4x4 target, profiled
// with pipeline statistics, with a validation message on the first draw.

const CB = 16;
const ref = (id, cls) => ({ __id: id, __class: cls });
const object = (id, type, cmd, args, label = null) => ({
  id, parent: 0, type, cmd, index: 0, handle: `0x${id.toString(16)}`, label, args, blobs: [], updates: {}, deleted: false,
});
const objects = [
  object(10, "VkImage", "vkCreateImage", { pCreateInfo: {
    imageType: "VK_IMAGE_TYPE_2D", format: "VK_FORMAT_R8G8B8A8_UNORM", extent: { width: 4, height: 4, depth: 1 }, mipLevels: 1, arrayLayers: 1,
    samples: "VK_SAMPLE_COUNT_1_BIT", usage: "VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT",
  } }, "Color"),
  object(11, "VkImageView", "vkCreateImageView", { pCreateInfo: {
    image: ref(10, "VkImage"), viewType: "VK_IMAGE_VIEW_TYPE_2D", format: "VK_FORMAT_R8G8B8A8_UNORM",
    subresourceRange: { baseMipLevel: 0, levelCount: 1, baseArrayLayer: 0, layerCount: 1 },
  } }),
  object(12, "VkRenderPass", "vkCreateRenderPass", { pCreateInfo: {
    attachmentCount: 1,
    pAttachments: [{ format: "VK_FORMAT_R8G8B8A8_UNORM", samples: "VK_SAMPLE_COUNT_1_BIT", loadOp: "VK_ATTACHMENT_LOAD_OP_CLEAR", storeOp: "VK_ATTACHMENT_STORE_OP_STORE", stencilLoadOp: "VK_ATTACHMENT_LOAD_OP_DONT_CARE", stencilStoreOp: "VK_ATTACHMENT_STORE_OP_DONT_CARE" }],
    subpassCount: 1, pSubpasses: [{ colorAttachmentCount: 1, pColorAttachments: [{ attachment: 0, layout: "VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL" }] }],
  } }, "Main Pass"),
  object(13, "VkFramebuffer", "vkCreateFramebuffer", { pCreateInfo: { renderPass: ref(12, "VkRenderPass"), attachmentCount: 1, pAttachments: [ref(11, "VkImageView")], width: 4, height: 4, layers: 1 } }),
  object(14, "VkPipeline", "vkCreateGraphicsPipelines", { pCreateInfos: [{
    stageCount: 1, pStages: [{ stage: "VK_SHADER_STAGE_VERTEX_BIT", pName: "main" }],
    pVertexInputState: {
      pVertexBindingDescriptions: [{ binding: 0, stride: 12, inputRate: "VK_VERTEX_INPUT_RATE_VERTEX" }],
      pVertexAttributeDescriptions: [{ location: 0, binding: 0, format: "VK_FORMAT_R32G32B32_SFLOAT", offset: 0 }],
    },
    pInputAssemblyState: { topology: "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST" },
    pRasterizationState: { polygonMode: "VK_POLYGON_MODE_FILL", cullMode: "VK_CULL_MODE_BACK_BIT" },
  }] }, "Opaque"),
  object(15, "VkBuffer", "vkCreateBuffer", { pCreateInfo: { size: 36, usage: "VK_BUFFER_USAGE_VERTEX_BUFFER_BIT" } }, "Triangle"),
  object(CB, "VkCommandBuffer", "vkAllocateCommandBuffers", { pAllocateInfo: { level: "VK_COMMAND_BUFFER_LEVEL_PRIMARY", commandBufferCount: 1 } }),
];

const commands = [];
const add = (method, args = {}, extra = {}) => commands.push({ index: commands.length, frame: 0, method, object: ref(CB, "VkCommandBuffer"), args, slot: commands.length, ...extra });
const bindPipeline = { pipelineBindPoint: "VK_PIPELINE_BIND_POINT_GRAPHICS", pipeline: ref(14, "VkPipeline") };
add("vkBeginCommandBuffer");
add("vkCmdBeginDebugUtilsLabelEXT", { pLabelInfo: { pLabelName: "Opaque" } });
add("vkCmdBeginRenderPass", { pRenderPassBegin: { renderPass: ref(12, "VkRenderPass"), framebuffer: ref(13, "VkFramebuffer"), renderArea: { offset: { x: 0, y: 0 }, extent: { width: 4, height: 4 } } } });
const PIPELINE_BIND = commands.length;
add("vkCmdBindPipeline", bindPipeline);
add("vkCmdBindPipeline", bindPipeline);
add("vkCmdBindVertexBuffers", { firstBinding: 0, bindingCount: 1, pBuffers: [ref(15, "VkBuffer")], pOffsets: [0] }, { bufferData: [1] });
const FIRST_DRAW = commands.length;
for (let i = 0; i < 40; i++) add("vkCmdDraw", { vertexCount: 3, instanceCount: 1, firstVertex: 0, firstInstance: 0 });
add("vkCmdEndRenderPass");
add("vkCmdEndDebugUtilsLabelEXT");
add("vkEndCommandBuffer");
commands.push({ index: commands.length, frame: 0, method: "vkQueueSubmit", object: ref(20, "VkQueue"), args: { submitCount: 1 } });

const pixels = new Uint8Array(64);
for (let i = 0; i < 16; i++) pixels.set([255, 0, 0, 255], i * 4);
const vertices = new Uint8Array(new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, NaN]).buffer);
// The pipeline's vertex stage: a named main that adds two constants, so the shader tools have an
// entry point to weigh, with line information naming shader.frag but not embedding its text.
const spirv = new Uint8Array(new Uint32Array([
  0x07230203, 0x00010300, 0, 9, 0,
  0x00020011, 1,                    // OpCapability Shader
  0x0005000f, 0, 4, 0x6e69616d, 0,  // OpEntryPoint Vertex %4 "main"
  0x00050007, 8, 0x64616873, 0x662e7265, 0x00676172,  // %8 = OpString "shader.frag"
  0x00040003, 2, 450, 8,            // OpSource GLSL 450 %8, without text
  0x00040005, 4, 0x6e69616d, 0,     // OpName %4 "main"
  0x00020013, 1,                    // %1 = OpTypeVoid
  0x00030021, 2, 1,                 // %2 = OpTypeFunction %1
  0x00030016, 3, 32,                // %3 = OpTypeFloat 32
  0x0004002b, 3, 5, 0x3f800000,     // %5 = OpConstant %3 1.0
  0x00050036, 1, 4, 0, 2,           // %4 = OpFunction %1 None %2
  0x000200f8, 6,                    // OpLabel
  0x00040008, 8, 3, 0,              // OpLine %8 3 0
  0x00050081, 3, 7, 5, 5,           // %7 = OpFAdd %3 %5 %5
  0x000100fd,                       // OpReturn
  0x00010038,                       // OpFunctionEnd
]).buffer);

/**
 * Writes the capture with the SPIR-V payload at a file offset that is not a multiple of four, as
 * payloads usually are, so word views of it need the parsers' aligned copy.
 */
function writeCapture(name, passMs) {
  const file = join(dir, name);
  for (let pad = 1; pad < 4; pad++) {
    const withShader = objects.map((o) => (o.id === 14 ? { ...o, blobs: [{ name: "vertex:main", size: spirv.byteLength, payload: [100 + pad, spirv.byteLength] }] } : o));
    const bytes = encodeCaptureFile(manifest(withShader, passMs), [pixels, vertices, new Uint8Array(pad), spirv]);
    const jsonLength = new DataView(bytes.buffer).getUint32(9, true);
    if ((13 + jsonLength + 100 + pad) % 4 === 0) continue;
    writeFileSync(file, bytes);
    return file;
  }
  throw new Error("no misaligned offset found");
}

function manifest(objectList, passMs) {
  return {
    format: "gpu-inspector-capture", version: 1, api: "vulkan", application: "GPU Inspector", savedAt: "2026-09-10T00:00:00.000Z",
    source: { name: "test.exe" }, frame: 7, frames: 1, frameTimeMs: 16.7, submitMs: 1.5, refreshMs: 16.667, refreshSource: "estimate", frameBoundary: "present",
    objects: objectList, commands,
    textures: [{ info: { id: 10, frame: 0, commandBuffer: CB, passIndex: 0, attachment: 0, format: "VK_FORMAT_R8G8B8A8_UNORM", aspect: "color", width: 4, height: 4, depth: 1, layers: 1, mip: 0, size: 64 }, payload: [0, 64] }],
    buffers: [{ info: { id: 1, buffer: 15, frame: 0, commandBuffer: CB, offset: 0, size: 36 }, payload: [64, 36] }],
    passTimings: [{ frame: 0, commandBuffer: CB, passIndex: 0, startMs: 0, durationMs: passMs, counters: { vertexInvocations: 120, fragmentInvocations: 64, clipperPrimitivesOut: 40 } }],
    validation: [{
      action: "ValidationMessage", key: 1, severity: "error", types: ["validation"], idName: "VUID-Test", idNumber: 1, message: "A test message.", frame: 7, count: 3,
      objects: [{ object: ref(14, "VkPipeline"), class: "VkPipeline", handle: "0xe" }], command: { commandBuffer: CB, slot: FIRST_DRAW },
    }],
  };
}
const before = writeCapture("before.gpucap", 2.5);
const after = writeCapture("after.gpucap", 1.0);

const server = createServer();
let nextId = 1;
async function call(name, args = {}) {
  const reply = await server.handle({ jsonrpc: "2.0", id: nextId++, method: "tools/call", params: { name, arguments: args } });
  const result = reply.result;
  const text = result.content.find((c) => c.type === "text")?.text ?? "";
  return { result, text, json: result.isError ? null : JSON.parse(text) };
}

test("initialize answers the client's protocol version, and every tool is listed with a schema", async () => {
  const init = await server.handle({ jsonrpc: "2.0", id: 0, method: "initialize", params: { protocolVersion: "2025-06-18", capabilities: {}, clientInfo: { name: "test", version: "1" } } });
  assert.equal(init.result.protocolVersion, "2025-06-18");
  assert.equal(init.result.serverInfo.name, "gpu-inspector");
  assert.equal(await server.handle({ jsonrpc: "2.0", method: "notifications/initialized" }), null, "notifications get no reply");
  const list = await server.handle({ jsonrpc: "2.0", id: 1, method: "tools/list" });
  const names = list.result.tools.map((t) => t.name);
  for (const name of ["open_capture", "get_capture_summary", "get_bottlenecks", "get_command", "read_texture", "read_vertices", "get_shader", "compare_captures"]) {
    assert.ok(names.includes(name), `${name} is listed`);
  }
  assert.ok(list.result.tools.every((t) => t.inputSchema.type === "object"));
});

test("a capture opens with its summary", async () => {
  const { json } = await call("open_capture", { path: before });
  assert.equal(json.capture, "cap-1");
  assert.equal(json.counts.draws, 40);
  assert.equal(json.counts.renderPasses, 1);
  assert.equal(json.timing.profiled, true);
  assert.match(json.timing.frameBound.verdict, /Vsync bound/);
  assert.equal(json.validation.errors, 1);
  const rules = json.issues.top.map((f) => f.rule);
  assert.ok(rules.includes("tiny-draws") && rules.includes("redundant-pipeline-bind"), rules.join(", "));
  assert.equal((await call("open_capture", { path: before })).json.reused, true, "an unchanged file is not opened twice");
});

test("frame issues name their command and pass", async () => {
  const { json } = await call("get_frame_issues", { rule: "tiny-draws" });
  assert.equal(json.total, 1);
  assert.equal(json.findings[0].command, FIRST_DRAW);
  assert.equal(json.findings[0].count, 40);
  assert.match(json.findings[0].passLabel, /Main Pass \[Opaque\]/);
});

test("the bottleneck report measures the pass from its counters", async () => {
  const { json } = await call("get_bottlenecks");
  const pass = json.passes[0];
  assert.equal(pass.ms, 2.5);
  assert.equal(pass.overdraw, 4, "64 fragment invocations over 16 pixels");
  assert.equal(pass.fragmentsPerPrimitive, 1.6);
  assert.ok(pass.problems.some((p) => /shaded 4.00 times/.test(p.title)));
});

test("the command list filters and pages", async () => {
  const { json } = await call("list_commands", { kind: "draw", limit: 5 });
  assert.equal(json.total, 40);
  assert.equal(json.commands.length, 5);
  assert.equal(json.nextOffset, 5);
  assert.equal(json.commands[0].i, FIRST_DRAW);
  assert.equal(json.commands[0].labels, "Opaque");
  assert.equal(json.commands[0].pass, 0);
  assert.equal(json.commands[0].validation, "error");
  assert.deepEqual((await call("list_commands", { method: "BindPipeline" })).json.commands.map((c) => c.i), [PIPELINE_BIND, PIPELINE_BIND + 1]);
});

test("a draw shows the state it read", async () => {
  const { json } = await call("get_command", { index: FIRST_DRAW });
  assert.equal(json.state.pipeline.pipeline, 'VkPipeline#14 "Opaque"');
  assert.equal(json.state.pipeline.boundAt, PIPELINE_BIND + 1);
  assert.equal(json.state.pipeline.fixedFunction.rasterization.cullMode, "VK_CULL_MODE_BACK_BIT");
  const vb = json.state.vertexBuffers[0];
  assert.equal(vb.stride, 12);
  assert.equal(vb.data, 1);
  assert.deepEqual(vb.values[1], { location0: [1, 0, 0] });
  assert.equal(json.state.renderTargets[0].texture, 0);
  assert.equal(json.validation[0].id, "VUID-Test");
  assert.equal(json.validation[0].command, FIRST_DRAW);
  assert.ok(json.issues.some((f) => f.rule === "tiny-draws"));
});

test("a render target comes back as an image with its numbers", async () => {
  const { result, json } = await call("read_texture", { texture: 0, texels: [[3, 3]] });
  assert.equal(result.content[0].type, "image");
  assert.equal(result.content[0].mimeType, "image/png");
  assert.equal(Buffer.from(result.content[0].data, "base64").subarray(1, 4).toString(), "PNG");
  assert.equal(json.uniform, true);
  assert.equal(json.stats[0].min, 1);
  assert.deepEqual(json.texels[0].value, [1, 0, 0, 1]);
});

test("vertices decode through the pipeline's layout, with bounds", async () => {
  const { json } = await call("read_vertices", { command: FIRST_DRAW, count: 3 });
  const binding = json.bindings[0];
  assert.equal(binding.capturedVertices, 3);
  assert.deepEqual(binding.vertices[1], { vertex: 1, location0: [1, 0, 0] });
  assert.deepEqual(binding.bounds.location0.max, [1, 1, 0]);
  assert.equal(binding.bounds.location0.nan, 1);
});

test("a buffer range reads as scalars or through a struct layout", async () => {
  const floats = (await call("read_buffer", { data: 1 })).json;
  assert.equal(floats.values.length, 9);
  assert.deepEqual(floats.boundBy, [FIRST_DRAW - 1]);
  const structs = (await call("read_buffer", { data: 1, layout: "struct V { float x; float y; float z; };", count: 2 })).json;
  assert.deepEqual(structs.values[1], { x: 1, y: 0, z: 0 });
});

test("objects, validation and the render graph", async () => {
  const pipeline = (await call("get_object", { id: 14 })).json;
  assert.equal(pipeline.fixedFunction.topology, "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST");
  assert.equal((await call("list_objects", {})).json.types.VkImage, 1);
  assert.equal((await call("get_validation")).json.messages[0].command, FIRST_DRAW);
  const graph = (await call("get_render_graph")).json;
  assert.equal(graph.nodes, 1);
  assert.equal((await call("get_render_graph", { node: 0 })).json.writes[0].object, 'VkImage#10 "Color"');
});

test("a shader payload reads at any offset in the file", async () => {
  const source = await call("get_shader", { object: 14, view: "source" });
  assert.equal(source.result.isError, undefined, source.text);
  assert.equal(source.json.stages[0].spirvBytes, spirv.byteLength);
  const analysis = await call("get_shader", { object: 14, view: "analysis" });
  assert.equal(analysis.result.isError, undefined, analysis.text);
  assert.equal((await call("get_shader", { object: 14 })).json.stages[0].spirvVersion, "1.3");
});

test("the shader flame graph spreads the measured pass over its stages", async () => {
  const { json, text } = await call("get_shader_flame_graph", { capture: before });
  assert.ok(json, text);
  assert.equal(json.units, "ms");
  assert.equal(json.total, 2.5);
  const pass = json.graph.children[0];
  assert.equal(pass.pass, 0);
  assert.match(pass.name, /Main Pass \[Opaque\]/);
  const item = pass.children[0];
  assert.equal(item.pipeline, 'VkPipeline#14 "Opaque"');
  assert.equal(item.command, FIRST_DRAW);
  const stage = item.children[0];
  assert.equal(stage.name, "vertex: main");
  assert.equal(stage.invocations, 120, "40 draws of 3 vertices");
  assert.equal(stage.invocationCount, "exact");
  assert.equal(stage.cost, 2.5);
  assert.deepEqual([json.hottestFunctions[0].function, json.hottestFunctions[0].stage, json.hottestFunctions[0].share], ["main", "vertex", 1]);
  assert.equal((await call("get_shader_flame_graph", { pass: 0, depth: 1 })).json.graph.children[0].hiddenChildren, 1);
  assert.equal((await call("get_shader_flame_graph", { pass: 3 })).result.isError, true);
});

test("a shader's source file named by its line information is found under the source roots", async () => {
  const missing = (await call("get_shader", { capture: before, object: 14, view: "source" })).json.stages[0];
  assert.match(missing.note, /names shader\.frag.*set_search_paths/);
  const roots = join(dir, "sources");
  mkdirSync(join(roots, "shaders"), { recursive: true });
  writeFileSync(join(roots, "shaders", "shader.frag"), "#version 450\nvoid main() {\n  float x = 1.0 + 1.0;\n}\n");
  const paths = (await call("set_search_paths", { sourceRoots: [roots] })).json;
  assert.deepEqual(paths.sourceRoots, { dirs: [roots], from: "set_search_paths" });

  const found = (await call("get_shader", { capture: before, object: 14, view: "source" })).json.stages[0];
  assert.deepEqual(found.foundOnThisMachine, ["shader.frag"]);
  assert.match(found.files[0].text, /float x = 1\.0 \+ 1\.0;/);
  const analysis = (await call("get_shader", { capture: before, object: 14, view: "analysis" })).json.stages[0];
  assert.equal(analysis.costliestLines[0].code, "float x = 1.0 + 1.0;");
  assert.equal((await call("get_shader_flame_graph", { capture: before })).json.hottestLines[0].code, "float x = 1.0 + 1.0;");
  assert.equal((await call("set_search_paths", { sourceRoots: [] })).json.sourceRoots.from, "none");
});

test("two captures compare pass by pass", async () => {
  const { json } = await call("compare_captures", { before, after });
  assert.equal(json.timing.gpuPassMs.change, -1.5);
  assert.equal(json.passes[0].ms.change, -1.5);
  assert.equal(json.onlyBefore.length, 0);
});

test("a Metal draw's argument buffer resolves to the buffers and textures it holds", async () => {
  const MCB = 34;
  const metalObjects = [
    object(30, "MTLBuffer", "newBufferWithLength:options:", { length: 64, gpuAddress: "0x100000" }, "Material params"),
    object(31, "MTLTexture", "newTextureWithDescriptor:", { width: 4, height: 4, gpuResourceID: "0x77" }, "Albedo"),
    object(32, "MTLBuffer", "newBufferWithLength:options:", { length: 16, gpuAddress: "0x200000" }, "Arguments"),
    object(33, "MTLRenderPipelineState", "newRenderPipelineStateWithDescriptor:error:", { reflection: { fragment: { buffers: [{
      index: 0, name: "material", access: "readOnly",
      type: { kind: "struct", name: "Material", size: 16, members: [
        { name: "albedo", offset: 0, type: { kind: "opaque", name: "texture2d<float>", metal: "texture" } },
        { name: "params", offset: 8, type: { kind: "opaque", name: "constant Params *", metal: "pointer" } },
      ] },
    }] } } }, "Lit"),
    object(MCB, "MTLCommandBuffer", "commandBuffer", {}),
  ];
  const metalCommands = [];
  const encode = (method, args = {}, extra = {}) => metalCommands.push({ index: metalCommands.length, frame: 0, method, object: ref(MCB, "MTLCommandBuffer"), args, ...extra });
  encode("renderCommandEncoderWithDescriptor:", { colorAttachments: [] });
  encode("setRenderPipelineState:", { pipeline: ref(33, "MTLRenderPipelineState") });
  encode("setFragmentBuffer:offset:atIndex:", { buffer: ref(32, "MTLBuffer"), offset: 0, index: 0 }, { bufferData: [5] });
  const draw = metalCommands.length;
  encode("drawPrimitives:vertexStart:vertexCount:", { primitiveType: "MTLPrimitiveTypeTriangle", vertexStart: 0, vertexCount: 3 });
  encode("endEncoding");
  encode("commit");
  // The argument buffer: the texture's resource id, then an address 16 bytes into the params buffer.
  const argumentBytes = new Uint8Array(16);
  new DataView(argumentBytes.buffer).setBigUint64(0, 0x77n, true);
  new DataView(argumentBytes.buffer).setBigUint64(8, 0x100010n, true);
  // The pass's overdraw, 2x2: counts 0, 1, 2, 3 passing depth, 1, 2, 3, 4 rasterized (u16 little endian).
  const counts = (values) => new Uint8Array(new Uint16Array(values).buffer);
  const measurement = (depthTested, fragments, histogram) => ({
    frame: 0, commandBuffer: MCB, passIndex: 0, depthTested, width: 2, height: 2, fragments, coveredPixels: depthTested ? 3 : 4,
    maxCount: depthTested ? 3 : 4, draws: 1, skippedDraws: 0, histogram, size: 8,
  });
  const file = join(dir, "metal.gpucap");
  writeFileSync(file, encodeCaptureFile({
    format: "gpu-inspector-capture", version: 1, api: "metal", application: "GPU Inspector", savedAt: "2026-09-10T00:00:00.000Z",
    source: { name: "metal.app" }, frame: 3, frames: 1, objects: metalObjects, commands: metalCommands, textures: [],
    buffers: [{ info: { id: 5, buffer: 32, frame: 0, commandBuffer: MCB, offset: 0, size: 16 }, payload: [0, 16] }],
    passTimings: [], validation: [],
    overdraw: [
      { info: measurement(true, 6, [1, 1, 1, 0, 0, 0, 0, 0]), payload: [16, 8] },
      { info: measurement(false, 10, [1, 1, 1, 1, 0, 0, 0, 0]), payload: [24, 8] },
    ],
  }, [argumentBytes, counts([0, 1, 2, 3]), counts([1, 2, 3, 4])]));

  const { json, text } = await call("get_command", { capture: file, index: draw });
  assert.ok(json, text);
  const slot = json.state.stageBuffers.slots[0];
  assert.equal(slot.name, "material");
  assert.deepEqual(slot.argumentBuffer, [
    { member: "albedo", kind: "texture", type: "texture2d<float>", resource: 'MTLTexture#31 "Albedo"' },
    { member: "params", kind: "pointer", type: "constant Params *", resource: 'MTLBuffer#30 "Material params"', offset: 16 },
  ]);

  // Measured overdraw: the list, one pass's heatmap with the counts asked for, and the pointer from get_bottlenecks.
  const list = (await call("get_overdraw", { capture: file })).json;
  assert.equal(list.passes.length, 1);
  assert.equal(list.passes[0].depthTested.perPixel, 1.5);
  assert.equal(list.passes[0].rasterized.perPixel, 2.5);
  assert.deepEqual(list.passes[0].depthTested.pixelsByCount, { 1: 1, 2: 1, 3: 1 });
  const heatmap = await call("get_overdraw", { capture: file, pass: 0, texels: [[1, 1], [0, 0]] });
  assert.equal(heatmap.result.content[0].type, "image");
  assert.deepEqual(heatmap.json.texels, [{ x: 1, y: 1, count: 3 }, { x: 0, y: 0, count: 0 }]);
  const rasterized = (await call("get_overdraw", { capture: file, pass: 0, depthTested: false, image: false, texels: [[0, 0]] })).json;
  assert.equal(rasterized.texels[0].count, 1);
  assert.match((await call("get_bottlenecks", { capture: file })).json.note, /get_overdraw/);
});

test("vkinsp_replay's overdraw file is read back with its counts", async () => {
  const { parseOverdrawFile, overdrawCount, OVERDRAW_LEGEND } = await import(pathToFileURL(bundle("renderer/overdraw.ts", "overdraw")).href);
  const info = {
    frame: 0, commandBuffer: 7, passIndex: 0, depthTested: true, measured: true, width: 2, height: 1, fragments: 3, coveredPixels: 2,
    maxCount: 2, draws: 1, skippedDraws: 0, histogram: [1, 1, 0, 0, 0, 0, 0, 0], size: 4, capturedFragments: 3, payload: [0, 4],
  };
  const json = new TextEncoder().encode(JSON.stringify({ format: "gpu-inspector-overdraw", version: 1, device: "Test GPU", passes: [info], problems: ["a problem"] }));
  const magic = new TextEncoder().encode("OVERDRAW 1\n");
  const bytes = new Uint8Array(magic.length + 4 + json.length + 4);
  bytes.set(magic, 0);
  new DataView(bytes.buffer).setUint32(magic.length, json.length, true);
  bytes.set(json, magic.length + 4);
  bytes.set([1, 0, 2, 0], magic.length + 4 + json.length);

  const file = parseOverdrawFile(bytes);
  assert.equal(file.device, "Test GPU");
  assert.deepEqual(file.problems, ["a problem"]);
  assert.equal(file.measurements.length, 1);
  const [m] = file.measurements;
  assert.equal(m.info.payload, undefined);
  assert.equal(m.info.capturedFragments, 3);
  assert.equal(overdrawCount(m, 0, 0), 1);
  assert.equal(overdrawCount(m, 1, 0), 2);
  assert.throws(() => parseOverdrawFile(bytes.subarray(0, bytes.length - 2)), /truncated/);
  assert.deepEqual(OVERDRAW_LEGEND.map((e) => e.label), ["0", "1", "2", "3", "4", "5-6", "7-10", "11-16", "17-32", "33+"]);
});

test("failures are tool errors the model reads, unknown tools protocol errors", async () => {
  const missing = await call("get_command", { index: 9999 });
  assert.equal(missing.result.isError, true);
  assert.match(missing.text, /No command 9999/);
  const unknown = await server.handle({ jsonrpc: "2.0", id: 99, method: "tools/call", params: { name: "no_such_tool" } });
  assert.equal(unknown.error.code, -32602);
});

test("the bundled server speaks MCP over stdio", async () => {
  const main = bundle("mcp/main.ts", "main");
  const child = spawn(process.execPath, [main], { stdio: ["pipe", "pipe", "inherit"] });
  const lines = createInterface({ input: child.stdout });
  const replies = [];
  const got = new Promise((resolve) => lines.on("line", (line) => {
    replies.push(JSON.parse(line));
    if (replies.length === 2) resolve();
  }));
  child.stdin.write(`${JSON.stringify({ jsonrpc: "2.0", id: 1, method: "initialize", params: { protocolVersion: "2024-11-05", capabilities: {} } })}\n`);
  child.stdin.write(`${JSON.stringify({ jsonrpc: "2.0", id: 2, method: "tools/call", params: { name: "open_capture", arguments: { path: after } } })}\n`);
  await got;
  child.stdin.end();
  assert.equal(replies[0].result.protocolVersion, "2024-11-05");
  assert.equal(JSON.parse(replies[1].result.content[0].text).counts.draws, 40);
});
