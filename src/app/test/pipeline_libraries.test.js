// Graphics pipeline libraries in captures (src/renderer/vulkan/vulkan_object.ts): a pipeline linked
// from libraries names none of their stages or state in its own create info, so its descriptor fills
// in what each library holds, by the parts its flags name.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const out = join(mkdtempSync(join(tmpdir(), "gpl-")), "vulkan_object.mjs");
buildSync({ entryPoints: [join(here, "..", "src", "renderer", "vulkan", "vulkan_object.ts")], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
const { VulkanObject } = await import(pathToFileURL(out).href);

const ref = (id) => ({ __id: id, __class: "VkPipeline" });
const pipeline = (id, info) => new VulkanObject({ action: "AddObject", id, parent: 1, type: "VkPipeline", cmd: "vkCreateGraphicsPipelines", index: 0, handle: "0x1", args: { pCreateInfos: [info] } });
const parts = (flags) => ({ sType: "VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT", flags });
const stage = (s) => ({ stage: `VK_SHADER_STAGE_${s}_BIT`, pName: "main", module: null });

const vertexLibrary = pipeline(10, {
  pNext: [parts("VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT | VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT")],
  flags: "VK_PIPELINE_CREATE_LIBRARY_BIT_KHR", stageCount: 1, pStages: [stage("VERTEX")],
  pInputAssemblyState: { topology: "VK_PRIMITIVE_TOPOLOGY_LINE_LIST" },
  pRasterizationState: { cullMode: "VK_CULL_MODE_BACK_BIT" },
  // Not a part this library holds: ignored, as the driver ignores it.
  pColorBlendState: { attachmentCount: 7 },
  pDynamicState: { pDynamicStates: ["VK_DYNAMIC_STATE_VIEWPORT"] },
  layout: { __id: 5, __class: "VkPipelineLayout" },
});
const fragmentLibrary = pipeline(11, {
  pNext: [parts("VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT")],
  flags: "VK_PIPELINE_CREATE_LIBRARY_BIT_KHR", stageCount: 1, pStages: [stage("FRAGMENT")],
  pDepthStencilState: { depthCompareOp: "VK_COMPARE_OP_GREATER" },
  pDynamicState: { pDynamicStates: ["VK_DYNAMIC_STATE_SCISSOR", "VK_DYNAMIC_STATE_VIEWPORT"] },
});
const outputLibrary = pipeline(12, {
  pNext: [parts("VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT")],
  flags: "VK_PIPELINE_CREATE_LIBRARY_BIT_KHR", stageCount: 0, pStages: null,
  pColorBlendState: { attachmentCount: 1 },
});
// The fragment side linked from two libraries first, then linked again with the vertex library.
const fragmentSide = pipeline(13, {
  pNext: [{ sType: "VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR", libraryCount: 2, pLibraries: [ref(11), ref(12)] }],
  flags: "VK_PIPELINE_CREATE_LIBRARY_BIT_KHR", stageCount: 0, pStages: null,
});
const linked = pipeline(14, {
  pNext: [{ sType: "VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR", libraryCount: 2, pLibraries: [ref(10), ref(13)] }],
  flags: "0", stageCount: 0, pStages: null, pInputAssemblyState: null, layout: null,
});
const objects = new Map([vertexLibrary, fragmentLibrary, outputLibrary, fragmentSide, linked].map((o) => [o.id, o]));
const db = { getObject: (id) => objects.get(id) ?? null };
for (const o of objects.values()) o.libraryLookup = db;

test("a linked pipeline's descriptor holds its libraries' stages and state", () => {
  const d = linked.descriptor;
  assert.deepEqual(d.pStages.map((s) => s.stage), ["VK_SHADER_STAGE_VERTEX_BIT", "VK_SHADER_STAGE_FRAGMENT_BIT"]);
  assert.equal(d.stageCount, 2);
  assert.equal(d.pInputAssemblyState.topology, "VK_PRIMITIVE_TOPOLOGY_LINE_LIST");
  assert.equal(d.pRasterizationState.cullMode, "VK_CULL_MODE_BACK_BIT");
  assert.equal(d.pDepthStencilState.depthCompareOp, "VK_COMPARE_OP_GREATER");
  assert.equal(d.pColorBlendState.attachmentCount, 1, "the blend state of the library holding the output part");
  assert.deepEqual(d.pDynamicState.pDynamicStates, ["VK_DYNAMIC_STATE_VIEWPORT", "VK_DYNAMIC_STATE_SCISSOR"]);
  assert.equal(d.layout.__id, 5);
  assert.equal(linked.args.pCreateInfos[0].stageCount, 0, "the captured create info is left as it was");
});

test("a pipeline without libraries, or with one missing, keeps its own create info", () => {
  assert.equal(vertexLibrary.descriptor, vertexLibrary.args.pCreateInfos[0]);
  const orphan = pipeline(15, { pNext: [{ sType: "VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR", pLibraries: [ref(99)] }], stageCount: 0 });
  orphan.libraryLookup = db;
  assert.equal(orphan.descriptor, orphan.args.pCreateInfos[0]);
});
