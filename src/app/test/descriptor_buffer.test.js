// Descriptor buffers in captures (VK_EXT_descriptor_buffer). A draw binds its sets by an offset
// into memory rather than by handle, and the layer reads that memory back and decodes it into the
// same snapshot a bound set produces (src/vulkan/src/descriptor_buffer.h). The render graph's
// resource source therefore sees an ordinary set — and must still notice the case the layer could
// not read, where the snapshot arrives with no bindings at all.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const out = join(mkdtempSync(join(tmpdir(), "descriptor-buffer-")), "entry.mjs");
buildSync({
  stdin: {
    contents: `export { VulkanResourceSource } from "./vulkan/frame_resources.ts";`,
    resolveDir: join(here, "..", "src", "renderer"), loader: "ts",
  },
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const { VulkanResourceSource } = await import(pathToFileURL(out).href);

const ref = (id, cls) => ({ __id: id, __class: cls });
const objects = new Map([
  [33, { id: 33, type: "VkBuffer", name: "Cube uniforms", descriptor: { size: 64 } }],
  [40, { id: 40, type: "VkImageView", name: "Texture view", descriptor: { image: ref(39, "VkImage") } }],
  [39, { id: 39, type: "VkImage", name: "Texture", descriptor: { extent: { width: 4, height: 4 }, format: "VK_FORMAT_R8G8B8A8_UNORM" } }],
]);
const db = { getObject: (id) => objects.get(id) ?? null };

/** What the layer attaches to vkCmdSetDescriptorBufferOffsetsEXT once it has read the memory. */
const decoded = {
  bindPoint: "VK_PIPELINE_BIND_POINT_GRAPHICS",
  sets: [{
    set: 0,
    descriptorSet: null,
    layout: ref(42, "VkDescriptorSetLayout"),
    bindings: [
      { binding: 0, type: "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER", stages: "VK_SHADER_STAGE_VERTEX_BIT",
        descriptors: [{ buffer: ref(33, "VkBuffer"), offset: 0, range: 64 }] },
      { binding: 1, type: "VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER", stages: "VK_SHADER_STAGE_FRAGMENT_BIT",
        descriptors: [{ imageView: ref(40, "VkImageView"), imageLayout: "VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL" }] },
    ],
  }],
};
/** And what it attaches when it could not: the set was bound, its contents are not knowable. */
const unread = { bindPoint: "VK_PIPELINE_BIND_POINT_GRAPHICS", sets: [{ set: 0, descriptorSet: null, bindings: [] }] };

const draw = { index: 9, method: "vkCmdDrawIndexed", args: { indexCount: 36 } };
const offsets = (descriptors) => ({ index: 8, method: "vkCmdSetDescriptorBufferOffsetsEXT", args: {}, descriptors });
const bindBuffers = { index: 7, method: "vkCmdBindDescriptorBuffersEXT", args: { bufferCount: 1 } };

test("a decoded descriptor buffer reads like a bound set", () => {
  const source = new VulkanResourceSource(db);
  source.observe(bindBuffers, "s");
  source.observe(offsets(decoded), "s");
  const { accesses, unresolved } = source.actionAccesses(draw, "s");
  assert.equal(unresolved, 0, "nothing is hidden once the memory has been read");
  const named = accesses.map((a) => a.resource.label ?? a.resource.name ?? String(a.resource.id));
  assert.ok(accesses.length >= 2, `both bindings reach the graph, got ${JSON.stringify(named)}`);
});

test("binding the descriptor buffers alone hides nothing", () => {
  // It names memory; it is the offsets that say which descriptors a draw actually uses.
  const source = new VulkanResourceSource(db);
  source.observe(bindBuffers, "s");
  assert.equal(source.actionAccesses(draw, "s").unresolved, 0);
});

test("a descriptor buffer the layer could not read is counted as hidden", () => {
  const source = new VulkanResourceSource(db);
  source.observe(bindBuffers, "s");
  source.observe(offsets(unread), "s");
  assert.ok(source.actionAccesses(draw, "s").unresolved > 0,
            "a set bound with no bindings is memory the graph cannot name");
});

test("one stream's descriptor buffer does not hide another's", () => {
  const source = new VulkanResourceSource(db);
  source.observe(offsets(unread), "a");
  source.observe(offsets(decoded), "b");
  assert.ok(source.actionAccesses(draw, "a").unresolved > 0);
  assert.equal(source.actionAccesses(draw, "b").unresolved, 0);
});
