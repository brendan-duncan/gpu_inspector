// Shader objects in captures (VK_EXT_shader_object): a draw bound with vkCmdBindShadersEXT has no
// pipeline, so its state (src/renderer/draw_state.ts) carries the shader objects and the dynamic
// state that stands in for a pipeline's, and the reports that group draws by pipeline group these by
// a program key (src/renderer/shader_cache.ts).
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const out = join(mkdtempSync(join(tmpdir(), "shader-objects-")), "entry.mjs");
buildSync({
  stdin: {
    contents: `
      export { CaptureData } from "./capture_data.ts";
      export { drawState, dynamicValue } from "./draw_state.ts";
      export { pipelineUses, programStages, shaderProgram } from "./shader_cache.ts";
      export { rasterStateOf } from "./shader_debug_setup.ts";
    `,
    resolveDir: join(here, "..", "src", "renderer"), loader: "ts",
  },
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const { CaptureData, drawState, dynamicValue, pipelineUses, programStages, shaderProgram, rasterStateOf } = await import(pathToFileURL(out).href);

const ref = (id, cls) => ({ __id: id, __class: cls });
const shaderObject = (id, stage, name) => ({
  id, type: "VkShaderEXT", name, blobs: [{ name: `${stage}:main` }],
  descriptor: { stage: `VK_SHADER_STAGE_${stage.toUpperCase()}_BIT`, pName: "main" },
});
const objects = new Map([
  [10, { id: 10, type: "VkPipeline", name: "Old pipeline", blobs: [{ name: "vertex:main" }, { name: "fragment:main" }],
         descriptor: { pStages: [{ stage: "VK_SHADER_STAGE_VERTEX_BIT", pName: "main" }, { stage: "VK_SHADER_STAGE_FRAGMENT_BIT", pName: "main" }],
                       pRasterizationState: { cullMode: "VK_CULL_MODE_NONE", frontFace: "VK_FRONT_FACE_CLOCKWISE" } } }],
  [20, shaderObject(20, "vertex", "Cube vertex shader")],
  [21, shaderObject(21, "fragment", "Cube fragment shader")],
  [22, shaderObject(22, "fragment", "Other fragment shader")],
]);
const db = { getObject: (id) => objects.get(id) ?? null };

const cb = ref(8, "VkCommandBuffer");
const commands = [
  ["vkCmdBindPipeline", { pipelineBindPoint: "VK_PIPELINE_BIND_POINT_GRAPHICS", pipeline: ref(10, "VkPipeline") }],
  ["vkCmdDraw", { vertexCount: 3 }],
  ["vkCmdBindShadersEXT", { stageCount: 2, pStages: ["VK_SHADER_STAGE_VERTEX_BIT", "VK_SHADER_STAGE_FRAGMENT_BIT"], pShaders: [ref(20, "VkShaderEXT"), ref(21, "VkShaderEXT")] }],
  ["vkCmdSetCullModeEXT", { cullMode: "VK_CULL_MODE_BACK_BIT" }],
  ["vkCmdSetFrontFaceEXT", { frontFace: "VK_FRONT_FACE_COUNTER_CLOCKWISE" }],
  ["vkCmdSetPrimitiveTopologyEXT", { primitiveTopology: "VK_PRIMITIVE_TOPOLOGY_LINE_LIST" }],
  ["vkCmdSetDepthTestEnableEXT", { depthTestEnable: true }],
  ["vkCmdSetDepthCompareOpEXT", { depthCompareOp: "VK_COMPARE_OP_GREATER" }],
  ["vkCmdDraw", { vertexCount: 36 }],
  ["vkCmdDraw", { vertexCount: 36 }],
  // Only the fragment stage changes: the vertex shader object stays bound.
  ["vkCmdBindShadersEXT", { stageCount: 1, pStages: ["VK_SHADER_STAGE_FRAGMENT_BIT"], pShaders: [ref(22, "VkShaderEXT")] }],
  ["vkCmdDraw", { vertexCount: 6 }],
  // And the same pair as before: the same program as the first two draws.
  ["vkCmdBindShadersEXT", { stageCount: 1, pStages: ["VK_SHADER_STAGE_FRAGMENT_BIT"], pShaders: [ref(21, "VkShaderEXT")] }],
  ["vkCmdDraw", { vertexCount: 36 }],
  // A pipeline bound again replaces the shader objects.
  ["vkCmdBindPipeline", { pipelineBindPoint: "VK_PIPELINE_BIND_POINT_GRAPHICS", pipeline: ref(10, "VkPipeline") }],
  ["vkCmdDraw", { vertexCount: 3 }],
];
const data = new CaptureData();
data.commands = commands.map(([method, args], index) => ({ index, frame: 0, slot: index, method, object: cb, args: { commandBuffer: cb, ...args } }));

test("a draw's state holds the shader objects bound in place of a pipeline", () => {
  const state = drawState(data, db, data.commands[8]);
  assert.equal(state.pipeline, null);
  assert.deepEqual(state.shaders.map((s) => s.id).sort(), [20, 21]);
  assert.equal(state.shadersCmd.index, 2);
  assert.equal(state.dynamic.topology, "VK_PRIMITIVE_TOPOLOGY_LINE_LIST");

  const mixed = drawState(data, db, data.commands[11]);
  assert.deepEqual(mixed.shaders.map((s) => s.id).sort(), [20, 22], "a later bind of one stage keeps the other");
  assert.equal(mixed.shadersCmd.index, 10);

  const before = drawState(data, db, data.commands[1]);
  assert.equal(before.pipeline?.id, 10);
  assert.deepEqual(before.shaders, []);
  const after = drawState(data, db, data.commands[15]);
  assert.equal(after.pipeline?.id, 10, "a pipeline bound after the shader objects replaces them");
  assert.deepEqual(after.shaders, []);
});

test("draws are grouped by program: one key per set of shader objects", () => {
  const uses = pipelineUses(data);
  assert.equal(uses.get(10), 2);
  const programs = [...uses.keys()].filter((k) => k < 0).map((k) => shaderProgram(data, db, k));
  const byName = Object.fromEntries(programs.map((p) => [p.name, uses.get(p.key)]));
  assert.deepEqual(byName, { "Cube vertex shader + Cube fragment shader": 3, "Cube vertex shader + Other fragment shader": 1 });
  const cube = programs.find((p) => p.name.includes("Cube fragment"));
  assert.deepEqual(programStages(cube, db).map((s) => [s.stage, s.object.id]), [["vertex", 20], ["fragment", 21]]);
});

test("dynamic state counts for shader objects, and for a pipeline only where it declares it dynamic", () => {
  const state = drawState(data, db, data.commands[8]);
  const raster = rasterStateOf(state);
  assert.equal(raster.cullBack, true);
  assert.equal(raster.ccwFront, true);
  assert.equal(raster.depthPrefers, "greater");

  // The same dynamic state before a pipeline that bakes its own in is ignored.
  const baked = { ...state, pipeline: objects.get(10), shaders: [] };
  assert.equal(dynamicValue(baked, "cullMode", "VK_CULL_MODE_NONE"), "VK_CULL_MODE_NONE");
  const declared = { ...baked, pipeline: { ...objects.get(10), descriptor: { ...objects.get(10).descriptor, pDynamicState: { pDynamicStates: ["VK_DYNAMIC_STATE_CULL_MODE"] } } } };
  assert.equal(dynamicValue(declared, "cullMode", "VK_CULL_MODE_NONE"), "VK_CULL_MODE_BACK_BIT");
});
