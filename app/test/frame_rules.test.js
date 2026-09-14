// Frame Issues rules over pass structure and barriers (src/renderer/vulkan/frame_analysis.ts and
// src/renderer/render_graph_analysis.ts), on a capture built here and read through the MCP server:
//
//   oversized-attachment   the blur target is 8x8 but every pass draws only 4x4 of it
//   redundant-transition   the blur target goes to SHADER_READ_ONLY and then to COLOR_ATTACHMENT
//                          with nothing using it in between; the scene is "transitioned" to the
//                          layout it is already in
//   subpass-candidate      the blur pass reads only the scene the pass before it rendered, through a
//                          fragment shader that reads it once (vectors/ablation/reuse.frag.spv)
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "rules-"));
process.env.GPU_INSPECTOR_SETTINGS = join(dir, "settings.json");
const bundle = (entry, name) => {
  const out = join(dir, `${name}.mjs`);
  buildSync({ entryPoints: [join(here, "..", "src", entry)], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
  return out;
};
const { createServer } = await import(pathToFileURL(bundle("mcp/server.ts", "server")).href);
const { encodeCaptureFile } = await import(pathToFileURL(bundle("renderer/capture_format.ts", "capture_format")).href);

const CB = 30;
const ref = (id, cls) => ({ __id: id, __class: cls });
const object = (id, type, cmd, args, label = null) => ({
  id, parent: 0, type, cmd, index: 0, handle: `0x${id.toString(16)}`, label, args, blobs: [], updates: {}, deleted: false,
});
const image = (id, label, size) => object(id, "VkImage", "vkCreateImage", { pCreateInfo: {
  imageType: "VK_IMAGE_TYPE_2D", format: "VK_FORMAT_R8G8B8A8_UNORM", extent: { width: size, height: size, depth: 1 }, mipLevels: 1, arrayLayers: 1,
  samples: "VK_SAMPLE_COUNT_1_BIT", usage: "VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT",
} }, label);
const view = (id, imageId) => object(id, "VkImageView", "vkCreateImageView", { pCreateInfo: {
  image: ref(imageId, "VkImage"), viewType: "VK_IMAGE_VIEW_TYPE_2D", format: "VK_FORMAT_R8G8B8A8_UNORM",
  subresourceRange: { aspectMask: "VK_IMAGE_ASPECT_COLOR_BIT", baseMipLevel: 0, levelCount: 1, baseArrayLayer: 0, layerCount: 1 },
} });
const framebuffer = (id, viewId) => object(id, "VkFramebuffer", "vkCreateFramebuffer", { pCreateInfo: { renderPass: ref(14, "VkRenderPass"), attachmentCount: 1, pAttachments: [ref(viewId, "VkImageView")], width: 8, height: 8, layers: 1 } });
const spirv = new Uint8Array(readFileSync(join(here, "vectors", "ablation", "reuse.frag.spv")));

const pipeline = object(17, "VkPipeline", "vkCreateGraphicsPipelines", { pCreateInfos: [{
  stageCount: 1, pStages: [{ stage: "VK_SHADER_STAGE_FRAGMENT_BIT", pName: "main" }],
  pInputAssemblyState: { topology: "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST" },
}] }, "Blur");
pipeline.blobs = [{ name: "fragment:main", size: spirv.byteLength, payload: [0, spirv.byteLength] }];
const objects = [
  image(10, "Scene", 8), view(11, 10),
  image(12, "Blur target", 8), view(13, 12),
  object(14, "VkRenderPass", "vkCreateRenderPass", { pCreateInfo: {
    attachmentCount: 1,
    pAttachments: [{ format: "VK_FORMAT_R8G8B8A8_UNORM", samples: "VK_SAMPLE_COUNT_1_BIT", loadOp: "VK_ATTACHMENT_LOAD_OP_CLEAR", storeOp: "VK_ATTACHMENT_STORE_OP_STORE", stencilLoadOp: "VK_ATTACHMENT_LOAD_OP_DONT_CARE", stencilStoreOp: "VK_ATTACHMENT_STORE_OP_DONT_CARE" }],
    subpassCount: 1, pSubpasses: [{ colorAttachmentCount: 1, pColorAttachments: [{ attachment: 0, layout: "VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL" }] }],
  } }),
  framebuffer(15, 11), framebuffer(16, 13), pipeline,
  object(18, "VkDescriptorSet", "vkAllocateDescriptorSets", {}),
  object(CB, "VkCommandBuffer", "vkAllocateCommandBuffers", { pAllocateInfo: { level: "VK_COMMAND_BUFFER_LEVEL_PRIMARY", commandBufferCount: 1 } }),
];

const commands = [];
const add = (method, args = {}, extra = {}) => { commands.push({ index: commands.length, frame: 0, method, object: ref(CB, "VkCommandBuffer"), args, slot: commands.length, ...extra }); return commands.length - 1; };
const barrier = (imageId, oldLayout, newLayout, srcAccessMask = "0") => add("vkCmdPipelineBarrier", {
  srcStageMask: "VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT", dstStageMask: "VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT", dependencyFlags: "0",
  memoryBarrierCount: 0, pMemoryBarriers: null, bufferMemoryBarrierCount: 0, pBufferMemoryBarriers: null, imageMemoryBarrierCount: 1,
  pImageMemoryBarriers: [{ srcAccessMask, dstAccessMask: "VK_ACCESS_SHADER_READ_BIT", oldLayout, newLayout, srcQueueFamilyIndex: 4294967295, dstQueueFamilyIndex: 4294967295,
    image: ref(imageId, "VkImage"), subresourceRange: { aspectMask: "VK_IMAGE_ASPECT_COLOR_BIT", baseMipLevel: 0, levelCount: 1, baseArrayLayer: 0, layerCount: 1 } }],
});
const beginPass = (fb, size) => add("vkCmdBeginRenderPass", { pRenderPassBegin: { renderPass: ref(14, "VkRenderPass"), framebuffer: ref(fb, "VkFramebuffer"), renderArea: { offset: { x: 0, y: 0 }, extent: { width: size, height: size } } } });
const draw = () => add("vkCmdDraw", { vertexCount: 3, instanceCount: 1, firstVertex: 0, firstInstance: 0 });

add("vkBeginCommandBuffer");
const WASTED = barrier(12, "VK_IMAGE_LAYOUT_UNDEFINED", "VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL");
barrier(12, "VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL", "VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL");
beginPass(15, 8);
draw();
add("vkCmdEndRenderPass");
const NOOP = barrier(10, "VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL", "VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL");
const BLUR_PASS = beginPass(16, 4);
add("vkCmdBindPipeline", { pipelineBindPoint: "VK_PIPELINE_BIND_POINT_GRAPHICS", pipeline: ref(17, "VkPipeline") });
add("vkCmdBindDescriptorSets", { pipelineBindPoint: "VK_PIPELINE_BIND_POINT_GRAPHICS", firstSet: 0, descriptorSetCount: 1, pDescriptorSets: [ref(18, "VkDescriptorSet")] }, {
  descriptors: { bindPoint: "VK_PIPELINE_BIND_POINT_GRAPHICS", sets: [{ set: 0, descriptorSet: ref(18, "VkDescriptorSet"), bindings: [
    { binding: 0, type: "VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER", descriptors: [{ imageView: ref(11, "VkImageView"), imageLayout: "VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL" }] },
  ] }] },
});
draw();
add("vkCmdEndRenderPass");
add("vkEndCommandBuffer");
commands.push({ index: commands.length, frame: 0, method: "vkQueueSubmit", object: ref(40, "VkQueue"), args: { submitCount: 1 } });

const file = join(dir, "rules.gpucap");
writeFileSync(file, encodeCaptureFile({
  format: "gpu-inspector-capture", version: 1, api: "vulkan", application: "GPU Inspector", savedAt: "2026-09-13T00:00:00.000Z",
  source: { name: "test.exe" }, frame: 1, frames: 1, objects, commands, textures: [], buffers: [],
}, [spirv]));

const server = createServer();
let nextId = 1;
async function issues(rule) {
  const reply = await server.handle({ jsonrpc: "2.0", id: nextId++, method: "tools/call", params: { name: "get_frame_issues", arguments: { capture: file, rule } } });
  const text = reply.result.content.find((c) => c.type === "text")?.text ?? "";
  assert.ok(!reply.result.isError, text);
  return JSON.parse(text).findings;
}

test("an attachment drawn into only in part of its area is oversized", async () => {
  const [f, ...rest] = await issues("oversized-attachment");
  assert.equal(rest.length, 0);
  assert.equal(f.command, BLUR_PASS);
  assert.match(f.message, /Blur target is 8x8 but drawn only in 4x4/);
});

test("a transition nothing uses before the next, and a barrier that changes nothing, are redundant", async () => {
  const found = await issues("redundant-transition");
  assert.deepEqual(found.map((f) => f.command).sort((a, b) => a - b), [WASTED, NOOP]);
  assert.ok(found.some((f) => /nothing uses before a later barrier/.test(f.message)));
  assert.ok(found.some((f) => /leaves an image in the layout it was in/.test(f.message)));
});

test("a pass reading only the previous pass's output, once per pixel, could be its subpass", async () => {
  const [f, ...rest] = await issues("subpass-candidate");
  assert.equal(rest.length, 0);
  assert.equal(f.command, BLUR_PASS);
  assert.equal(f.confidence, "medium", "the shader was seen to read the scene once");
  assert.match(f.message, /reads Scene/);
  assert.match(f.message, /input attachments/);
});
