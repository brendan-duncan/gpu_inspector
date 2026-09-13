// GPU Inspector's MCP server, built from app/src/mcp by app/build.mjs: edit the sources, not this file.

// src/mcp/main.ts
import process2 from "node:process";

// src/mcp/capture_store.ts
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

// src/renderer/utils/float.ts
function float16ToFloat32(float16) {
  const s = (float16 & 32768) >> 15;
  const e = (float16 & 31744) >> 10;
  const f = float16 & 1023;
  if (e == 0) {
    return (s ? -1 : 1) * Math.pow(2, -14) * (f / Math.pow(2, 10));
  } else if (e == 31) {
    return f ? NaN : (s ? -1 : 1) * Infinity;
  }
  return (s ? -1 : 1) * Math.pow(2, e - 15) * (1 + f / Math.pow(2, 10));
}
var uint32 = new Uint32Array(1);
var uint32ToFloat32 = new Float32Array(uint32.buffer, 0, 1);
function float11ToFloat32(f11) {
  const u322 = (f11 >> 6 & 31) + (127 - 15) << 23 | (f11 & 63) << 17;
  uint32[0] = u322;
  return uint32ToFloat32[0];
}
function float10ToFloat32(f10) {
  const u322 = (f10 >> 5 & 31) + (127 - 15) << 23 | (f10 & 31) << 18;
  uint32[0] = u322;
  return uint32ToFloat32[0];
}

// src/renderer/vulkan/vk_format.ts
var cache = /* @__PURE__ */ new Map();
var blockCache = /* @__PURE__ */ new Map();
var FIXED_BLOCKS = {
  VK_FORMAT_D16_UNORM: 2,
  VK_FORMAT_X8_D24_UNORM_PACK32: 4,
  VK_FORMAT_D32_SFLOAT: 4,
  VK_FORMAT_S8_UINT: 1,
  VK_FORMAT_D16_UNORM_S8_UINT: 4,
  VK_FORMAT_D24_UNORM_S8_UINT: 4,
  VK_FORMAT_D32_SFLOAT_S8_UINT: 8,
  VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: 4,
  VK_FORMAT_B10G11R11_UFLOAT_PACK32: 4
};
function formatBlock(name) {
  const hit = blockCache.get(name);
  if (hit !== void 0) return hit;
  const b = parseBlock(name);
  blockCache.set(name, b);
  return b;
}
function parseBlock(name) {
  const fixed = FIXED_BLOCKS[name];
  if (fixed) return { bytes: fixed, width: 1, height: 1 };
  let m = /^VK_FORMAT_BC(\d)/.exec(name);
  if (m) return { bytes: m[1] === "1" || m[1] === "4" ? 8 : 16, width: 4, height: 4 };
  if (/^VK_FORMAT_ETC2_R8G8B8A8|^VK_FORMAT_EAC_R11G11/.test(name)) return { bytes: 16, width: 4, height: 4 };
  if (/^VK_FORMAT_ETC2_|^VK_FORMAT_EAC_/.test(name)) return { bytes: 8, width: 4, height: 4 };
  m = /^VK_FORMAT_ASTC_(\d+)x(\d+)/.exec(name);
  if (m) return { bytes: 16, width: Number(m[1]), height: Number(m[2]) };
  m = /^VK_FORMAT_PVRTC\d_(\d)BPP/.exec(name);
  if (m) return { bytes: 8, width: m[1] === "2" ? 8 : 4, height: 4 };
  m = /_PACK(8|16|32)$/.exec(name);
  if (m) return { bytes: Number(m[1]) / 8, width: 1, height: 1 };
  m = /^VK_FORMAT_((?:[RGBAEXDS]\d+)+)_[A-Z0-9_]+$/.exec(name);
  if (m) {
    let bits = 0;
    for (const c2 of m[1].matchAll(/[RGBAEXDS](\d+)/g)) bits += Number(c2[1]);
    return bits ? { bytes: bits / 8, width: 1, height: 1 } : null;
  }
  return null;
}
function estimateImageBytes(format, width, height, depth, mips, layers, samples) {
  const block = formatBlock(format);
  if (!block) return 0;
  let total = 0;
  for (let m = 0; m < Math.max(1, mips); m++) {
    const w = Math.max(1, width >> m);
    const h = Math.max(1, height >> m);
    const d = Math.max(1, depth >> m);
    total += Math.ceil(w / block.width) * Math.ceil(h / block.height) * d * block.bytes;
  }
  return total * Math.max(1, layers) * Math.max(1, samples);
}
function convert(raw, bits, numeric) {
  const maxU = Math.pow(2, bits) - 1;
  switch (numeric) {
    case "UNORM":
    case "SRGB":
      return raw / maxU;
    case "SNORM":
      return Math.max(-1, toSigned(raw, bits) / (Math.pow(2, bits - 1) - 1));
    case "SINT":
    case "SSCALED":
      return toSigned(raw, bits);
    case "UFLOAT":
      return bits === 11 ? float11ToFloat32(raw) : bits === 10 ? float10ToFloat32(raw) : raw;
    default:
      return raw;
  }
}
function toSigned(raw, bits) {
  return raw >= Math.pow(2, bits - 1) ? raw - Math.pow(2, bits) : raw;
}
var ORDER = { R: 0, G: 1, B: 2, A: 3 };
function vertexFormat(name) {
  const hit = cache.get(name);
  if (hit !== void 0) return hit;
  const f = parse(name);
  cache.set(name, f);
  return f;
}
function parse(name) {
  const m = /^VK_FORMAT_((?:[RGBA]\d+)+)_([A-Z]+)(?:_PACK(8|16|32))?$/.exec(name);
  if (!m) return null;
  const channels = [];
  for (const c2 of m[1].matchAll(/([RGBA])(\d+)/g)) channels.push({ name: c2[1], bits: Number(c2[2]) });
  const numeric = m[2];
  const pack = m[3] ? Number(m[3]) : 0;
  const integer = numeric === "UINT" || numeric === "SINT" || numeric === "USCALED" || numeric === "SSCALED";
  const outputOrder = channels.map((c2, i) => ({ c: c2, i })).sort((a, b) => (ORDER[a.c.name] ?? 9) - (ORDER[b.c.name] ?? 9));
  const outChannels = outputOrder.map((o) => o.c.name);
  if (pack) {
    const size3 = pack / 8;
    const total = channels.reduce((s, c2) => s + c2.bits, 0);
    if (total !== pack) return null;
    const read2 = (view, offset) => {
      const v = pack === 8 ? view.getUint8(offset) : pack === 16 ? view.getUint16(offset, true) : view.getUint32(offset, true);
      const raw = [];
      let shift = pack;
      for (const c2 of channels) {
        shift -= c2.bits;
        raw.push(Math.floor(v / Math.pow(2, shift)) % Math.pow(2, c2.bits));
      }
      return outputOrder.map((o) => convert(raw[o.i], o.c.bits, numeric));
    };
    return { size: size3, channels: outChannels, integer, read: read2 };
  }
  const size2 = channels.reduce((s, c2) => s + c2.bits / 8, 0);
  const readers = [];
  let byteOffset = 0;
  for (const c2 of channels) {
    const o = byteOffset;
    const bits = c2.bits;
    byteOffset += bits / 8;
    if (numeric === "SFLOAT") {
      if (bits === 16) readers.push((v, off) => float16ToFloat32(v.getUint16(off + o, true)));
      else if (bits === 32) readers.push((v, off) => v.getFloat32(off + o, true));
      else if (bits === 64) readers.push((v, off) => v.getFloat64(off + o, true));
      else return null;
    } else if (bits === 8) {
      readers.push((v, off) => convert(v.getUint8(off + o), 8, numeric));
    } else if (bits === 16) {
      readers.push((v, off) => convert(v.getUint16(off + o, true), 16, numeric));
    } else if (bits === 32) {
      readers.push((v, off) => numeric === "SINT" || numeric === "SSCALED" ? v.getInt32(off + o, true) : convert(v.getUint32(off + o, true), 32, numeric));
    } else if (bits === 64) {
      readers.push((v, off) => numeric === "SINT" || numeric === "SSCALED" ? Number(v.getBigInt64(off + o, true)) : Number(v.getBigUint64(off + o, true)));
    } else {
      return null;
    }
  }
  const read = (view, offset) => outputOrder.map((o) => readers[o.i](view, offset));
  return { size: size2, channels: outChannels, integer, read };
}

// src/renderer/vulkan/vulkan_object.ts
var VulkanObject = class {
  id;
  type;
  // "VkImage"
  parentId;
  // id of owning object (device, pool, swapchain, ...)
  cmd;
  // "vkCreateImage"
  index;
  handle;
  // "0x..." raw handle for correlation with app logs
  label;
  args;
  blobs;
  // binary payloads available from the layer
  updates;
  // fields from ObjectUpdate messages (memory, memoryOffset, ...)
  dependencies = /* @__PURE__ */ new Set();
  // objects referenced by args
  dependents = /* @__PURE__ */ new Set();
  // objects whose args reference this one
  invalidReason = null;
  widget = null;
  // list entry in the inspect panel
  isDeleted = false;
  /** A shader of this pipeline / this module has been replaced by the shader editor. */
  edited = false;
  constructor(msg) {
    this.id = msg.id;
    this.type = msg.type;
    this.parentId = msg.parent;
    this.cmd = msg.cmd;
    this.index = msg.index ?? 0;
    this.handle = msg.handle;
    this.label = msg.label ?? "";
    this.args = msg.args ?? null;
    this.blobs = msg.blobs ?? [];
    this.updates = {};
  }
  get shortType() {
    return this.type.startsWith("Vk") ? this.type.substring(2) : this.type;
  }
  get name() {
    if (this.label) return this.label;
    const own = this.args?.name;
    if (typeof own === "string" && own && this.type.startsWith("MTL")) {
      const kind = this.type === "MTLFunction" ? this.args?.functionType : void 0;
      return typeof kind === "string" && kind ? `${own} (${kind})` : own;
    }
    return `${this.shortType} ${this.id}`;
  }
  get isInvalid() {
    return !!this.invalidReason;
  }
  /**
   * The create-info struct for this object, when the creating call has one. A Metal object's
   * arguments are its descriptor (metal/src/tracker.h): the library flattens the creating
   * call's descriptor into `args`, reflection included.
   */
  get descriptor() {
    const a = this.args;
    if (!a) return null;
    if (this.type.startsWith("MTL")) return a;
    if (isObject(a.pCreateInfo)) return a.pCreateInfo;
    if (isObject(a.pAllocateInfo)) return a.pAllocateInfo;
    if (Array.isArray(a.pCreateInfos)) {
      const d = a.pCreateInfos[this.index];
      return isObject(d) ? d : null;
    }
    if (isObject(a.pBeginInfo)) return a.pBeginInfo;
    return null;
  }
  /** One-line summary shown next to the name in the object list. */
  summary(db) {
    const d = this.descriptor;
    const a = this.args ?? {};
    switch (this.type) {
      case "VkDevice": {
        const props = a.properties;
        return isObject(props) ? str(props.deviceName) : "";
      }
      case "VkPhysicalDevice": {
        const props = this.updates.properties;
        return isObject(props) ? `${str(props.deviceName)}  ${fmt(props.deviceType).replace(/^DEVICE_TYPE_/, "").replace(/_GPU$/, "").toLowerCase()}` : "";
      }
      case "VkImage": {
        if (this.cmd === "vkGetSwapchainImagesKHR") {
          const sd = db?.getObject(this.parentId)?.descriptor;
          const ext = sd && isObject(sd.imageExtent) ? sd.imageExtent : null;
          return sd ? `swapchain ${fmt(sd.imageFormat)} ${num(ext?.width)}x${num(ext?.height)}` : "swapchain image";
        }
        if (!d) return "";
        const e = isObject(d.extent) ? d.extent : {};
        const dims = d.imageType === "VK_IMAGE_TYPE_3D" ? `${num(e.width)}x${num(e.height)}x${num(e.depth)}` : `${num(e.width)}x${num(e.height)}`;
        const layers = num(d.arrayLayers) > 1 ? ` [${num(d.arrayLayers)}]` : "";
        const mips = num(d.mipLevels) > 1 ? ` ${num(d.mipLevels)} mips` : "";
        const samples = d.samples && d.samples !== "VK_SAMPLE_COUNT_1_BIT" ? ` ${fmt(d.samples)}` : "";
        return `${fmt(d.format)} ${dims}${layers}${mips}${samples}`;
      }
      case "VkImageView": {
        const img = db?.getObject(refId(d?.image));
        return `${fmt(d?.viewType)} ${fmt(d?.format)}${img ? " of " + img.name : ""}`;
      }
      case "VkBuffer":
        return d ? `${formatBytes(num(d.size))} ${fmtFlags(d.usage)}` : "";
      case "VkDeviceMemory":
        return d ? `${formatBytes(num(d.allocationSize))} type ${num(d.memoryTypeIndex)}` : "";
      case "VkShaderModule": {
        const stage = str(this.updates.stage);
        return `${stage ? `${stage} shader, ` : ""}${d ? formatBytes(num(d.codeSize)) : ""} SPIR-V`;
      }
      case "VkPipeline": {
        if (!d) return "";
        if (Array.isArray(d.pStages)) {
          const names = d.pStages.map((s) => isObject(s) ? stageWord(str(s.stage)) : "").filter((s) => s);
          return names.length ? names.join(" + ") : `graphics ${d.pStages.length} stages`;
        }
        if (isObject(d.stage)) return `${stageWord(str(d.stage.stage))} (compute)`;
        return "";
      }
      case "VkRenderPass":
        return d ? `${num(d.attachmentCount)} attachments, ${num(d.subpassCount)} subpasses` : "";
      case "VkFramebuffer":
        return d ? `${num(d.width)}x${num(d.height)} ${num(d.attachmentCount)} attachments` : "";
      case "VkDescriptorSetLayout":
        return d ? `${num(d.bindingCount)} bindings` : "";
      case "VkDescriptorPool":
        return d ? `${num(d.maxSets)} sets` : "";
      case "VkPipelineLayout":
        return d ? `${num(d.setLayoutCount)} set layouts, ${num(d.pushConstantRangeCount)} push ranges` : "";
      case "VkSampler":
        return d ? `${fmt(d.magFilter)}/${fmt(d.minFilter)} ${fmt(d.addressModeU)}` : "";
      case "VkSwapchainKHR": {
        const ext = d && isObject(d.imageExtent) ? d.imageExtent : null;
        return d ? `${fmt(d.imageFormat)} ${num(ext?.width)}x${num(ext?.height)} x${num(d.minImageCount)}` : "";
      }
      case "VkCommandBuffer":
        return d ? fmt(d.level) : "";
      case "VkCommandPool":
        return d ? `queue family ${num(d.queueFamilyIndex)}` : "";
      case "VkQueue":
        return a.queueFamilyIndex !== void 0 ? `family ${num(a.queueFamilyIndex)} index ${num(a.queueIndex)}` : "";
      case "VkQueryPool":
        return d ? `${fmt(d.queryType)} x${num(d.queryCount)}` : "";
      default:
        return "";
    }
  }
};
function objectMemoryBytes(o, db) {
  const d = o.descriptor;
  switch (o.type) {
    case "MTLHeap":
      return num(o.args?.allocatedSize) || num(o.args?.size);
    case "MTLBuffer":
      return num(o.args?.allocatedSize) || num(o.args?.length);
    case "MTLTexture":
      return o.cmd.startsWith("buffer ") ? 0 : num(o.args?.allocatedSize);
    case "VkDeviceMemory":
      return num(d?.allocationSize);
    case "VkBuffer":
      return num(d?.size);
    case "VkImage": {
      if (o.cmd === "vkGetSwapchainImagesKHR") {
        const sd = db?.getObject(o.parentId)?.descriptor;
        const e2 = sd && isObject(sd.imageExtent) ? sd.imageExtent : null;
        return sd ? estimateImageBytes(str(sd.imageFormat), num(e2?.width), num(e2?.height), 1, 1, num(sd.imageArrayLayers) || 1, 1) : 0;
      }
      if (!d) return 0;
      const e = isObject(d.extent) ? d.extent : null;
      const samples = Number(/VK_SAMPLE_COUNT_(\d+)_BIT/.exec(str(d.samples))?.[1] ?? 1);
      return estimateImageBytes(str(d.format), num(e?.width), num(e?.height), num(e?.depth) || 1, num(d.mipLevels) || 1, num(d.arrayLayers) || 1, samples);
    }
    default:
      return 0;
  }
}
function stageWord(flag) {
  const m = /^VK_SHADER_STAGE_(.+?)_BIT/.exec(flag);
  if (!m) return flag;
  return m[1].toLowerCase().replace("tessellation_control", "tess control").replace("tessellation_evaluation", "tess eval").replace(/_/g, " ");
}
function isObject(v) {
  return typeof v === "object" && v !== null && !Array.isArray(v);
}
function isHandleRef(v) {
  return isObject(v) && typeof v.__id === "number";
}
function refId(v) {
  return isHandleRef(v) ? v.__id : null;
}
function num(v) {
  return typeof v === "number" ? v : typeof v === "string" ? Number(v) || 0 : 0;
}
function str(v) {
  return typeof v === "string" ? v : v === void 0 || v === null ? "" : String(v);
}
function fmt(v) {
  if (typeof v !== "string") return v === void 0 || v === null ? "" : String(v);
  if (v.startsWith("VK_SAMPLE_COUNT_")) return v.replace("VK_SAMPLE_COUNT_", "").replace("_BIT", "x");
  const prefixes = [
    "VK_FORMAT_",
    "VK_IMAGE_VIEW_TYPE_",
    "VK_IMAGE_TYPE_",
    "VK_SHADER_STAGE_",
    "VK_COMMAND_BUFFER_LEVEL_",
    "VK_FILTER_",
    "VK_SAMPLER_ADDRESS_MODE_",
    "VK_QUERY_TYPE_",
    "VK_DESCRIPTOR_TYPE_",
    "VK_IMAGE_LAYOUT_",
    "VK_PIPELINE_BIND_POINT_",
    "VK_INDEX_TYPE_",
    "VK_PRIMITIVE_TOPOLOGY_",
    "VK_ATTACHMENT_LOAD_OP_",
    "VK_ATTACHMENT_STORE_OP_",
    "VK_COMPARE_OP_",
    "VK_CULL_MODE_",
    "VK_FRONT_FACE_",
    "VK_POLYGON_MODE_",
    "VK_BLEND_FACTOR_",
    "VK_BLEND_OP_",
    "VK_STRUCTURE_TYPE_"
  ];
  for (const p of prefixes) if (v.startsWith(p)) return v.substring(p.length).replace(/_BIT$/, "");
  const m = /^VK_[A-Z0-9]+_(.+)$/.exec(v);
  return m ? m[1] : v;
}
function fmtFlags(v) {
  if (typeof v !== "string") return v === void 0 ? "" : String(v);
  return v.split(" | ").map((s) => s.replace(/^VK_[A-Z0-9]+?_(USAGE_|CREATE_|STAGE_|ACCESS_|ASPECT_)?/, "").replace(/_BIT(_[A-Z]+)?$/, "$1")).join(" | ");
}
function formatBytes(bytes) {
  if (!isFinite(bytes)) return "";
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  if (bytes < 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
  return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} GB`;
}

// src/renderer/metal/command_sets.ts
var DRAW = /* @__PURE__ */ new Set([
  "drawPrimitives:vertexStart:vertexCount:",
  "drawPrimitives:vertexStart:vertexCount:instanceCount:",
  "drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:",
  "drawPrimitives:indirectBuffer:indirectBufferOffset:",
  "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:",
  "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:",
  "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:",
  "drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:indirectBuffer:indirectBufferOffset:",
  "drawPatches:patchStart:patchCount:patchIndexBuffer:patchIndexBufferOffset:instanceCount:baseInstance:",
  "drawPatches:patchIndexBuffer:patchIndexBufferOffset:indirectBuffer:indirectBufferOffset:",
  "drawIndexedPatches:patchStart:patchCount:patchIndexBuffer:patchIndexBufferOffset:controlPointIndexBuffer:controlPointIndexBufferOffset:instanceCount:baseInstance:",
  "drawIndexedPatches:patchIndexBuffer:patchIndexBufferOffset:controlPointIndexBuffer:controlPointIndexBufferOffset:indirectBuffer:indirectBufferOffset:",
  "drawMeshThreadgroups:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
  "drawMeshThreads:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
  "drawMeshThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
  // An indirect command buffer executed in a render pass is a batch of draws.
  "executeCommandsInBuffer:withRange:",
  "executeCommandsInBuffer:indirectBuffer:indirectBufferOffset:"
]);
var DISPATCH = /* @__PURE__ */ new Set([
  "dispatchThreads:threadsPerThreadgroup:",
  "dispatchThreadgroups:threadsPerThreadgroup:",
  "dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerThreadgroup:",
  "dispatchThreadsPerTile:"
]);
var INDIRECT = /* @__PURE__ */ new Set([
  "drawPrimitives:indirectBuffer:indirectBufferOffset:",
  "drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:indirectBuffer:indirectBufferOffset:",
  "drawPatches:patchIndexBuffer:patchIndexBufferOffset:indirectBuffer:indirectBufferOffset:",
  "drawIndexedPatches:patchIndexBuffer:patchIndexBufferOffset:controlPointIndexBuffer:controlPointIndexBufferOffset:indirectBuffer:indirectBufferOffset:",
  "drawMeshThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
  "dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerThreadgroup:",
  "executeCommandsInBuffer:withRange:",
  "executeCommandsInBuffer:indirectBuffer:indirectBufferOffset:"
]);
var PASS_BEGIN = /* @__PURE__ */ new Set([
  "renderCommandEncoderWithDescriptor:",
  "parallelRenderCommandEncoderWithDescriptor:",
  "computeCommandEncoder",
  "computeCommandEncoderWithDescriptor:",
  "computeCommandEncoderWithDispatchType:",
  "blitCommandEncoder",
  "blitCommandEncoderWithDescriptor:",
  "resourceStateCommandEncoder",
  "resourceStateCommandEncoderWithDescriptor:",
  "accelerationStructureCommandEncoder",
  "accelerationStructureCommandEncoderWithDescriptor:"
]);
var PASS_END = /* @__PURE__ */ new Set(["endEncoding"]);
var STAGE_BUFFER_METHODS = {
  "setVertexBuffer:offset:atIndex:": { stage: "vertex", kind: "one" },
  "setVertexBuffers:offsets:withRange:": { stage: "vertex", kind: "many" },
  "setVertexBytes:length:atIndex:": { stage: "vertex", kind: "bytes" },
  "setFragmentBuffer:offset:atIndex:": { stage: "fragment", kind: "one" },
  "setFragmentBuffers:offsets:withRange:": { stage: "fragment", kind: "many" },
  "setFragmentBytes:length:atIndex:": { stage: "fragment", kind: "bytes" },
  "setBuffer:offset:atIndex:": { stage: "compute", kind: "one" },
  "setBuffers:offsets:withRange:": { stage: "compute", kind: "many" },
  "setBytes:length:atIndex:": { stage: "compute", kind: "bytes" },
  "setObjectBuffer:offset:atIndex:": { stage: "object", kind: "one" },
  "setObjectBytes:length:atIndex:": { stage: "object", kind: "bytes" },
  "setMeshBuffer:offset:atIndex:": { stage: "mesh", kind: "one" },
  "setMeshBytes:length:atIndex:": { stage: "mesh", kind: "bytes" },
  "setTileBuffer:offset:atIndex:": { stage: "tile", kind: "one" },
  "setTileBytes:length:atIndex:": { stage: "tile", kind: "bytes" }
};
var BIND_STAGE_BUFFER = new Set(Object.keys(STAGE_BUFFER_METHODS));
function enumShort(key, v) {
  if (typeof v !== "string") return v === void 0 || v === null ? "" : String(v);
  const prefix = `MTL${key.charAt(0).toUpperCase()}${key.slice(1)}`;
  if (v.startsWith(prefix) && v.length > prefix.length) return v.slice(prefix.length);
  return v.startsWith("MTL") ? v.slice(3) : v;
}
function size(v) {
  return isObject(v) ? `${num(v.width)}x${num(v.height)}x${num(v.depth)}` : "";
}
function summarize(cmd, nameOf) {
  const a = cmd.args;
  const m = cmd.method;
  if (!a) return "";
  const quoted = (v) => typeof v === "string" && v ? `"${v}"` : "";
  const slot = (what) => `[${num(a.index)}] ${what}`;
  switch (m) {
    case "setLabel:":
    case "pushDebugGroup:":
    case "insertDebugSignpost:":
      return quoted(a.label);
    case "drawPrimitives:vertexStart:vertexCount:":
    case "drawPrimitives:vertexStart:vertexCount:instanceCount:":
    case "drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:":
      return `${enumShort("primitiveType", a.primitiveType)} ${num(a.vertexCount)} verts x${num(a.instanceCount)}`;
    case "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:":
    case "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:":
    case "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:":
      return `${enumShort("primitiveType", a.primitiveType)} ${num(a.indexCount)} idx x${num(a.instanceCount)}`;
    case "drawPrimitives:indirectBuffer:indirectBufferOffset:":
    case "drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:indirectBuffer:indirectBufferOffset:":
      return `${enumShort("primitiveType", a.primitiveType)} indirect ${nameOf(a.indirectBuffer)}`;
    case "dispatchThreads:threadsPerThreadgroup:":
      return `${size(a.threadsPerGrid)} threads, ${size(a.threadsPerThreadgroup)} per group`;
    case "dispatchThreadgroups:threadsPerThreadgroup:":
      return `${size(a.threadgroupsPerGrid)} groups of ${size(a.threadsPerThreadgroup)}`;
    case "dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerThreadgroup:":
      return `indirect ${nameOf(a.indirectBuffer)}, ${size(a.threadsPerThreadgroup)} per group`;
    case "setRenderPipelineState:":
    case "setComputePipelineState:":
      return nameOf(a.pipeline);
    case "setDepthStencilState:":
      return nameOf(a.depthStencilState);
    case "setViewport:":
      return isObject(a.viewport) ? `${num(a.viewport.width)}x${num(a.viewport.height)}` : "";
    case "setScissorRect:":
      return isObject(a.rect) ? `${num(a.rect.width)}x${num(a.rect.height)} at ${num(a.rect.x)},${num(a.rect.y)}` : "";
    case "renderCommandEncoderWithDescriptor:":
    case "parallelRenderCommandEncoderWithDescriptor:": {
      const colors = Array.isArray(a.colorAttachments) ? a.colorAttachments : [];
      const first = colors.find((c2) => isObject(c2) && c2.texture !== null);
      const target = isObject(first) ? nameOf(first.texture) : "";
      const more = colors.length > 1 ? ` +${colors.length - 1}` : "";
      const depth = isObject(a.depthAttachment) ? " + depth" : "";
      return `${target || `${colors.length} attachment${colors.length === 1 ? "" : "s"}`}${more}${depth}`;
    }
    case "presentDrawable:":
    case "presentDrawable:atTime:":
    case "presentDrawable:afterMinimumDuration:":
      return nameOf(a.texture);
    case "present":
      return "";
    default:
      break;
  }
  if (a.buffer !== void 0 && a.index !== void 0) {
    return slot(`${nameOf(a.buffer) || "(none)"}${num(a.offset) ? ` +${num(a.offset)}` : ""}`);
  }
  if (a.pValues !== void 0 && a.index !== void 0) return slot(`${num(a.size)} bytes`);
  if (a.texture !== void 0 && a.index !== void 0) return slot(nameOf(a.texture) || "(none)");
  if (a.sampler !== void 0 && a.index !== void 0) return slot(nameOf(a.sampler) || "(none)");
  if (Array.isArray(a.buffers) && isObject(a.range)) return `[${num(a.range.location)}] +${num(a.range.length)}`;
  const parts2 = [];
  for (const [key, value] of Object.entries(a)) {
    if (parts2.length >= 4) break;
    if (value === null || value === void 0) continue;
    if (typeof value === "number") parts2.push(`${key} ${value}`);
    else if (typeof value === "boolean") parts2.push(`${key} ${value}`);
    else if (typeof value === "string") parts2.push(value.startsWith("MTL") ? enumShort(key, value) : `${key} ${str(value)}`);
    else if (isObject(value) && typeof value.__id === "number") {
      const n = nameOf(value);
      if (n) parts2.push(n);
    } else if (isObject(value) && value.width !== void 0 && value.height !== void 0) parts2.push(`${key} ${size(value)}`);
  }
  return parts2.join(", ");
}
var METAL_SETS = {
  DRAW,
  DISPATCH,
  TRACE: /* @__PURE__ */ new Set(),
  PASS_BEGIN,
  PASS_END,
  LABEL_BEGIN: /* @__PURE__ */ new Set(["pushDebugGroup:"]),
  LABEL_END: /* @__PURE__ */ new Set(["popDebugGroup"]),
  // `commit` hands the command buffer to the GPU and `presentDrawable:` schedules the frame:
  // between them they are what vkQueueSubmit and vkQueuePresentKHR are in a Vulkan capture.
  // `present` is the marker the library records when the frame ends through the drawable's own
  // present rather than through the command buffer (metal/README.md, "Frame boundaries").
  SUBMIT: /* @__PURE__ */ new Set([
    "commit",
    "presentDrawable:",
    "presentDrawable:atTime:",
    "presentDrawable:afterMinimumDuration:",
    "present"
  ]),
  // Metal binds resources to an encoder directly rather than through a descriptor set object;
  // argument buffers are the closest thing and are not captured yet.
  BIND_DESCRIPTOR: /* @__PURE__ */ new Set(),
  BIND_VERTEX: /* @__PURE__ */ new Set([
    "setVertexBuffer:offset:atIndex:",
    "setVertexBuffers:offsets:withRange:"
  ]),
  // Metal has no separate index-buffer binding: the index buffer is an argument of the draw.
  BIND_INDEX: /* @__PURE__ */ new Set(),
  // Inline constant blocks, on every stage that has them.
  PUSH_CONSTANT: /* @__PURE__ */ new Set([
    "setVertexBytes:length:atIndex:",
    "setFragmentBytes:length:atIndex:",
    "setBytes:length:atIndex:",
    "setObjectBytes:length:atIndex:",
    "setMeshBytes:length:atIndex:",
    "setTileBytes:length:atIndex:"
  ]),
  INDIRECT,
  COMPUTE_PASS_END: /* @__PURE__ */ new Set(),
  // Every encoder is a pass and they share one counter per command buffer, but the library times
  // a compute encoder under the compute kind (PassKind::Compute in metal/src/capture.mm), which
  // is a separate key. A blit or resource-state encoder is timed as a render pass.
  passIsCompute(method) {
    return method.startsWith("computeCommandEncoder");
  },
  bindPointOf(method) {
    return DISPATCH.has(method) ? "compute" : "render";
  },
  BIND_PIPELINE: /* @__PURE__ */ new Set(["setRenderPipelineState:", "setComputePipelineState:"]),
  pipelineBindPointOf(method) {
    return method === "setComputePipelineState:" ? "compute" : "render";
  },
  graphicsBindPoint: "render",
  vertexBuffersOf(cmd) {
    const a = cmd.args;
    if (!a) return [];
    if (a.buffer !== void 0) {
      return [{
        cmd,
        binding: num(a.index),
        buffer: a.buffer,
        offset: num(a.offset),
        size: null,
        stride: null,
        dataId: cmd.bufferData?.[0] ?? 0
      }];
    }
    if (Array.isArray(a.buffers)) {
      const first = isObject(a.range) ? num(a.range.location) : 0;
      const offsets = Array.isArray(a.offsets) ? a.offsets : [];
      return a.buffers.map((buffer, i) => ({
        cmd,
        binding: first + i,
        buffer,
        offset: num(offsets[i]),
        size: null,
        stride: null,
        dataId: cmd.bufferData?.[i] ?? 0
      }));
    }
    return [];
  },
  BIND_STAGE_BUFFER,
  summarize,
  stageBuffersOf(cmd) {
    const entry = STAGE_BUFFER_METHODS[cmd.method];
    const a = cmd.args;
    if (!entry || !a) return [];
    if (entry.kind === "bytes") {
      return [{ cmd, stage: entry.stage, index: num(a.index), buffer: null, offset: 0, dataId: cmd.bufferData?.[0] ?? 0, inline: true }];
    }
    if (entry.kind === "one") {
      return [{ cmd, stage: entry.stage, index: num(a.index), buffer: a.buffer ?? null, offset: num(a.offset), dataId: cmd.bufferData?.[0] ?? 0, inline: false }];
    }
    if (!Array.isArray(a.buffers)) return [];
    const first = isObject(a.range) ? num(a.range.location) : 0;
    const offsets = Array.isArray(a.offsets) ? a.offsets : [];
    return a.buffers.map((buffer, i) => ({
      cmd,
      stage: entry.stage,
      index: first + i,
      buffer,
      offset: num(offsets[i]),
      dataId: cmd.bufferData?.[i] ?? 0,
      inline: false
    }));
  },
  indexBufferOf(cmd) {
    const a = cmd.args;
    if (!a || a.indexBuffer === void 0) return null;
    return {
      cmd,
      buffer: a.indexBuffer,
      offset: num(a.indexBufferOffset),
      // MTLIndexType: 0 = UInt16, 1 = UInt32.
      indexType: num(a.indexType) === 1 ? "MTLIndexTypeUInt32" : "MTLIndexTypeUInt16",
      // bufferData is [vertex..., index] per command; an indexed draw captures only its index buffer.
      dataId: cmd.bufferData?.[0] ?? 0
    };
  }
};

// src/renderer/vulkan/command_sets.ts
var DRAW_METHODS = /* @__PURE__ */ new Set([
  "vkCmdDraw",
  "vkCmdDrawIndexed",
  "vkCmdDrawIndirect",
  "vkCmdDrawIndexedIndirect",
  "vkCmdDrawIndirectCount",
  "vkCmdDrawIndexedIndirectCount",
  "vkCmdDrawMeshTasksEXT",
  "vkCmdDrawMeshTasksIndirectEXT",
  "vkCmdDrawMeshTasksNV",
  "vkCmdDrawMultiEXT",
  "vkCmdDrawMultiIndexedEXT"
]);
var DISPATCH_METHODS = /* @__PURE__ */ new Set(["vkCmdDispatch", "vkCmdDispatchIndirect", "vkCmdDispatchBase"]);
var TRACE_METHODS = /* @__PURE__ */ new Set(["vkCmdTraceRaysKHR", "vkCmdTraceRaysIndirectKHR", "vkCmdTraceRaysIndirect2KHR"]);
var PASS_BEGIN2 = /* @__PURE__ */ new Set(["vkCmdBeginRenderPass", "vkCmdBeginRenderPass2", "vkCmdBeginRenderPass2KHR", "vkCmdBeginRendering", "vkCmdBeginRenderingKHR"]);
var PASS_END2 = /* @__PURE__ */ new Set(["vkCmdEndRenderPass", "vkCmdEndRenderPass2", "vkCmdEndRenderPass2KHR", "vkCmdEndRendering", "vkCmdEndRenderingKHR"]);
var LABEL_BEGIN = /* @__PURE__ */ new Set(["vkCmdBeginDebugUtilsLabelEXT", "vkCmdDebugMarkerBeginEXT"]);
var LABEL_END = /* @__PURE__ */ new Set(["vkCmdEndDebugUtilsLabelEXT", "vkCmdDebugMarkerEndEXT"]);
var SUBMIT_METHODS = /* @__PURE__ */ new Set(["vkQueueSubmit", "vkQueueSubmit2", "vkQueueSubmit2KHR", "vkQueuePresentKHR", "vkQueueBindSparse"]);
var BIND_DESCRIPTOR_METHODS = /* @__PURE__ */ new Set([
  "vkCmdBindDescriptorSets",
  "vkCmdBindDescriptorSets2",
  "vkCmdBindDescriptorSets2KHR",
  "vkCmdPushDescriptorSet",
  "vkCmdPushDescriptorSetKHR",
  "vkCmdPushDescriptorSet2",
  "vkCmdPushDescriptorSet2KHR"
]);
var BIND_VERTEX_METHODS = /* @__PURE__ */ new Set(["vkCmdBindVertexBuffers", "vkCmdBindVertexBuffers2", "vkCmdBindVertexBuffers2EXT"]);
var BIND_INDEX_METHODS = /* @__PURE__ */ new Set(["vkCmdBindIndexBuffer", "vkCmdBindIndexBuffer2", "vkCmdBindIndexBuffer2KHR"]);
var PUSH_CONSTANT_METHODS = /* @__PURE__ */ new Set(["vkCmdPushConstants", "vkCmdPushConstants2", "vkCmdPushConstants2KHR"]);
var INDIRECT_METHODS = /* @__PURE__ */ new Set(["vkCmdDrawIndirect", "vkCmdDrawIndexedIndirect", "vkCmdDispatchIndirect"]);
var COMPUTE_PASS_END = /* @__PURE__ */ new Set([
  "vkCmdPipelineBarrier",
  "vkCmdPipelineBarrier2",
  "vkCmdPipelineBarrier2KHR",
  "vkCmdWaitEvents",
  "vkCmdWaitEvents2",
  "vkCmdWaitEvents2KHR",
  "vkCmdExecuteCommands"
]);
function bindPointOf(method) {
  if (DISPATCH_METHODS.has(method)) return "VK_PIPELINE_BIND_POINT_COMPUTE";
  if (TRACE_METHODS.has(method)) return "VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR";
  return "VK_PIPELINE_BIND_POINT_GRAPHICS";
}
var VULKAN_SETS = {
  DRAW: DRAW_METHODS,
  DISPATCH: DISPATCH_METHODS,
  TRACE: TRACE_METHODS,
  PASS_BEGIN: PASS_BEGIN2,
  PASS_END: PASS_END2,
  LABEL_BEGIN,
  LABEL_END,
  SUBMIT: SUBMIT_METHODS,
  BIND_DESCRIPTOR: BIND_DESCRIPTOR_METHODS,
  BIND_VERTEX: BIND_VERTEX_METHODS,
  BIND_INDEX: BIND_INDEX_METHODS,
  PUSH_CONSTANT: PUSH_CONSTANT_METHODS,
  INDIRECT: INDIRECT_METHODS,
  COMPUTE_PASS_END,
  bindPointOf,
  BIND_PIPELINE: /* @__PURE__ */ new Set(["vkCmdBindPipeline"]),
  pipelineBindPointOf: (_method, args) => typeof args?.pipelineBindPoint === "string" ? args.pipelineBindPoint : "",
  graphicsBindPoint: "VK_PIPELINE_BIND_POINT_GRAPHICS",
  vertexBuffersOf(cmd) {
    const a = cmd.args;
    if (!a || !Array.isArray(a.pBuffers)) return [];
    const first = num(a.firstBinding);
    return a.pBuffers.map((buffer, k) => ({
      cmd,
      binding: first + k,
      buffer,
      offset: Array.isArray(a.pOffsets) ? num(a.pOffsets[k]) : 0,
      size: Array.isArray(a.pSizes) && a.pSizes[k] ? num(a.pSizes[k]) : null,
      stride: Array.isArray(a.pStrides) ? num(a.pStrides[k]) : null,
      dataId: cmd.bufferData?.[k] ?? 0
    }));
  },
  indexBufferOf(cmd) {
    if (!BIND_INDEX_METHODS.has(cmd.method)) return null;
    const a = cmd.args;
    if (!a) return null;
    return {
      cmd,
      buffer: a.buffer,
      offset: num(a.offset),
      indexType: str(a.indexType),
      dataId: cmd.bufferData?.[0] ?? 0
    };
  },
  summarize(cmd, name) {
    const a = cmd.args;
    if (!a) return "";
    switch (cmd.method) {
      case "vkCmdDraw":
        return `${num(a.vertexCount)} verts x${num(a.instanceCount)}`;
      case "vkCmdDrawIndexed":
        return `${num(a.indexCount)} idx x${num(a.instanceCount)}`;
      case "vkCmdDrawIndirect":
      case "vkCmdDrawIndexedIndirect":
        return `${name(a.buffer)} x${num(a.drawCount)}`;
      case "vkCmdDispatch":
        return `${num(a.groupCountX)}x${num(a.groupCountY)}x${num(a.groupCountZ)}`;
      case "vkCmdBindPipeline":
        return `${fmt(a.pipelineBindPoint)} ${name(a.pipeline)}`;
      case "vkCmdBindDescriptorSets":
        return `set ${num(a.firstSet)} +${num(a.descriptorSetCount)}`;
      case "vkCmdPushDescriptorSet":
      case "vkCmdPushDescriptorSetKHR":
        return `set ${num(a.set)}: ${num(a.descriptorWriteCount)} writes`;
      case "vkCmdBindVertexBuffers":
      case "vkCmdBindVertexBuffers2":
      case "vkCmdBindVertexBuffers2EXT":
        return `binding ${num(a.firstBinding)} +${num(a.bindingCount)}`;
      case "vkCmdBindIndexBuffer":
      case "vkCmdBindIndexBuffer2":
      case "vkCmdBindIndexBuffer2KHR":
        return `${name(a.buffer)} ${fmt(a.indexType)}`;
      case "vkCmdPushConstants":
        return `${fmt(a.stageFlags)} ${num(a.size)} bytes`;
      case "vkCmdPipelineBarrier":
        return `${num(a.memoryBarrierCount)}m ${num(a.bufferMemoryBarrierCount)}b ${num(a.imageMemoryBarrierCount)}i`;
      case "vkCmdCopyBufferToImage":
        return `${name(a.srcBuffer)} -> ${name(a.dstImage)}`;
      case "vkCmdCopyImage":
        return `${name(a.srcImage)} -> ${name(a.dstImage)}`;
      case "vkCmdCopyBuffer":
        return `${name(a.srcBuffer)} -> ${name(a.dstBuffer)}`;
      case "vkCmdSetViewport": {
        const v = Array.isArray(a.pViewports) && isObject(a.pViewports[0]) ? a.pViewports[0] : null;
        return v ? `${num(v.width)}x${num(v.height)}` : "";
      }
      case "vkCmdSetScissor": {
        const s = Array.isArray(a.pScissors) && isObject(a.pScissors[0]) && isObject(a.pScissors[0].extent) ? a.pScissors[0].extent : null;
        return s ? `${num(s.width)}x${num(s.height)}` : "";
      }
      case "vkQueueSubmit":
        return `${Array.isArray(a.pSubmits) ? a.pSubmits.length : 0} submit(s)`;
      default:
        return "";
    }
  }
};

// src/renderer/command_sets.ts
function isAction(sets, method) {
  return sets.DRAW.has(method) || sets.DISPATCH.has(method) || sets.TRACE.has(method);
}
function labelNameOf(cmd) {
  const a = cmd.args;
  const info = a && (isObject(a.pLabelInfo) ? a.pLabelInfo : isObject(a.pMarkerInfo) ? a.pMarkerInfo : null);
  return info ? str(info.pLabelName ?? info.pMarkerName) : a && a.label !== void 0 ? str(a.label) : cmd.method;
}
function setsFor(api) {
  return api === "metal" ? METAL_SETS : VULKAN_SETS;
}

// src/renderer/utils/signal.ts
var Signal = class _Signal {
  static _disableSignals = 0;
  _lastSlotId;
  slots;
  name;
  /**
   * @param name Optional name for the signal, usually used for debugging purposes.
   */
  constructor(name) {
    this._lastSlotId = 0;
    this.slots = /* @__PURE__ */ new Map();
    if (name) {
      this.name = name;
    }
  }
  /**
   * Returns true if signals are allowed to be emitted. If false,
   * calling the Signal's emit method will do nothing.
   */
  static get enabled() {
    return _Signal._disableSignals == 0;
  }
  /**
   * Returns true if signals are disabled from being emitted. If true,
   * calling the Signal's emit method will do nothing.
   */
  static get disabled() {
    return _Signal._disableSignals > 0;
  }
  /**
   * Disables all signals from being emitted. This can be called multiple times, but an equal
   * number of calls to enable should be used to re-enable signals. This is often used to disable
   * any callbacks while doing heavy operations, like file loading, so a single signal will be
   * emitted at the end.
   */
  static disable() {
    return _Signal._disableSignals++;
  }
  /**
   * Enable signals to be emitted, having been previously disabled.
   * @param force If true, signals will be forced to the enabled state,
   * even if there were an unbalanced number of calls to disable..
   */
  static enable(force) {
    if (force) {
      _Signal._disableSignals = 0;
      return 0;
    }
    return _Signal._disableSignals > 0 ? _Signal._disableSignals-- : 0;
  }
  /**
   * Disconnect the listener from all signals of the given object.
   * @param object The object to disconnect from.
   * @param callback The listener to disconnect
   * @param instance The optional listener instance that owns callback.
   */
  static disconnect(object, callback, instance) {
    const record = object;
    for (const i in record) {
      const p = record[i];
      if (p.constructor === _Signal) {
        p.disconnect(callback, instance);
      }
    }
  }
  /**
   * Return all signals that belong to the object.
   * @param object The object to get the signals from.
   * @param out Optional storage for the results. A new array will be created if null.
   * @return The list of signals that belong to the object.
   */
  static getSignals(object, out) {
    out = out || [];
    const record = object;
    for (const i in record) {
      const p = record[i];
      if (p.constructor === _Signal) {
        out.push(p);
      }
    }
    return out;
  }
  /**
   * True if this signal has at least one listener.
   */
  get hasListeners() {
    return this.slots.size > 0;
  }
  /**
   * Emit a signal, calling all listeners.
   * @param args Optional arguments to call the listeners with.
   * @returns The first truthy value returned by a listener (which stops emission), else null.
   */
  emit(...args) {
    if (_Signal.disabled) {
      return null;
    }
    for (const k of this.slots) {
      const s = k[1][0];
      const o = k[1][1] || s;
      if (!s) {
        continue;
      }
      if (s.constructor === _Signal) {
        s.emit.apply(o, args);
      } else {
        const res = s.apply(o, args);
        if (res) {
          return res;
        }
      }
    }
    return null;
  }
  /**
   * Connect a listener to the signal. This can be a function, object method,
   * class static method, or another signal. There is no type-checking to
   * ensure the listener function can successfully receive the arguments that
   * will be emitted by the signal, which will result in an exception if you
   * connect an incompatible listener and emit the signal.
   * To have an object method listen to a signal, pass in the object, too.
   * @returns A handle that can be used to disconnect the listener. Returns -1 if the listener was already connected.
   * @example
   * listen(Function)
   * listen(Signal)
   * listen(method, object)
   */
  addListener(callback, object) {
    if (this.isListening(callback, object)) {
      return -1;
    }
    this.slots.set(this._lastSlotId++, [callback, object]);
    return this._lastSlotId - 1;
  }
  /**
   * Checks if there is a binded listener that matches the criteria.
   * @example
   * isListening(Signal)
   * isListening(callback)
   * isListening(object)
   * isListening(method, object)
   */
  isListening(callback, object) {
    for (const slot of this.slots) {
      const slotInfo = slot[1];
      if (callback && !object) {
        if (slotInfo[0] === callback || slotInfo[1] === callback) {
          return true;
        }
      } else if (!callback && object) {
        if (slotInfo[1] === object) {
          return true;
        }
      } else {
        if (slotInfo[0] === callback && slotInfo[1] === object) {
          return true;
        }
      }
    }
    return false;
  }
  /**
   * Disconnect a listener from the signal.
   * @example
   * disconnect(Object) -- Disconnect all method listeners of the given object.
   * disconnect(Function) -- Disconnect the function listener.
   * disconnect(Signal) -- Disconnect the signal listener.
   * disconnect(method, object) -- Disconnect the method listener.
   * disconnect(handle) -- Disconnect the listener registered under the numeric handle.
   * disconnect() -- Disconnect all listeners from the signal.
   */
  disconnect(callback, object) {
    if ((callback === null || callback === void 0) && (object === null || object === void 0)) {
      this.slots.clear();
      return true;
    }
    if (typeof callback === "number") {
      const handle = callback;
      if (!this.slots.has(handle)) {
        return false;
      }
      this.slots.delete(handle);
      return true;
    }
    let found = false;
    for (const slot of this.slots) {
      const slotHandle = slot[0];
      const slotInfo = slot[1];
      if (callback && !object) {
        if (slotInfo[0] === callback || slotInfo[1] === callback) {
          this.slots.delete(slotHandle);
          found = true;
        }
      } else if (!callback && object) {
        if (slotInfo[1] === object) {
          this.slots.delete(slotHandle);
          found = true;
        }
      } else {
        if (slotInfo[0] === callback && slotInfo[1] === object) {
          this.slots.delete(slotHandle);
          found = true;
        }
      }
    }
    return found;
  }
  /**
   * Alias of disconnect(callback, object); the name the TypeScript Signal contract uses.
   */
  removeListener(callback, object) {
    this.disconnect(callback, object);
  }
};

// src/renderer/capture_data.ts
function flattenSecondaries(commands) {
  if (!commands.some((c2) => c2 && c2.children && c2.children.length)) return commands;
  const out = [];
  for (const c2 of commands) {
    if (!c2) continue;
    out.push(c2);
    for (const child of c2.children ?? []) {
      for (const cc of child.commands) {
        out.push({
          index: 0,
          frame: c2.frame,
          method: cc.method,
          object: c2.object,
          args: cc.args,
          secondary: child.commandBuffer,
          children: cc.children,
          descriptors: cc.descriptors,
          bufferData: cc.bufferData,
          slot: cc.slot,
          stack: cc.stack
        });
      }
    }
  }
  out.forEach((c2, i) => {
    c2.index = i;
  });
  return out;
}
function passKey(frame, commandBufferId, passIndex, compute = false) {
  return `${frame}:${commandBufferId}:${compute ? "c" : ""}${passIndex}`;
}
var CaptureData = class {
  /** Frame number of the first captured frame, and how many frames the capture spans. */
  frame = 0;
  frames = 1;
  /** Which API produced it. Captures made before the field existed are Vulkan. */
  api = "vulkan";
  commands = [];
  textures = [];
  buffers = /* @__PURE__ */ new Map();
  /** GPU pass timings (Profile passes), keyed "frame:commandBuffer:passIndex". */
  passTimings = /* @__PURE__ */ new Map();
  /** Overdraw measurements (a Metal capture with "Overdraw"): two per render pass. */
  overdraw = [];
  /** The pixel a Metal capture with "pixelHistory" followed, as it sent it (renderer/pixel_history.ts parses it). */
  pixelHistory = null;
  /** Per-draw timings and counters from a replay of the capture (renderer/draw_stats.ts). */
  drawStats = null;
  /** Draw-call overlays replayed so far, by command index (renderer/draw_overlay.ts); not kept in capture files. */
  drawOverlays = /* @__PURE__ */ new Map();
  _expectedCommands = 0;
  _pendingBuffers = 0;
  onCaptureStatus = new Signal();
  onCommandsComplete = new Signal();
  onTextureLoaded = new Signal();
  onTexturesAnnounced = new Signal();
  onBuffersAnnounced = new Signal();
  onBufferLoaded = new Signal();
  /** Every announced buffer's data has arrived (or failed). */
  onBuffersComplete = new Signal();
  onPassTimings = new Signal();
  /** Overdraw measurements were announced, or one's per-pixel counts arrived. */
  onOverdraw = new Signal();
  /** A Metal capture's pixel history arrived. */
  onPixelHistory = new Signal();
  /** Per-draw measurements arrived (a replay finished, or a capture file carried them). */
  onDrawStats = new Signal();
  /** Draw-call overlays arrived from a replay. */
  onDrawOverlays = new Signal();
  /** The command classification for this capture's API (see ../command_sets.ts). */
  get sets() {
    return setsFor(this.api);
  }
  reset() {
    this.frame = 0;
    this.frames = 1;
    this.api = "vulkan";
    this.commands = [];
    this.textures = [];
    this.buffers = /* @__PURE__ */ new Map();
    this.passTimings = /* @__PURE__ */ new Map();
    this.overdraw = [];
    this.pixelHistory = null;
    this.drawStats = null;
    this.drawOverlays = /* @__PURE__ */ new Map();
    this._expectedCommands = 0;
    this._pendingBuffers = 0;
  }
  /** A render pass's overdraw measurements: the depth-tested one first. */
  overdrawForPass(frame, commandBufferId, passIndex) {
    return this.overdraw.filter((o) => o.info.frame === frame && o.info.commandBuffer === commandBufferId && o.info.passIndex === passIndex).sort((a, b) => Number(b.info.depthTested) - Number(a.info.depthTested));
  }
  passTiming(frame, commandBufferId, passIndex, compute = false) {
    return this.passTimings.get(passKey(frame, commandBufferId, passIndex, compute)) ?? null;
  }
  texturesForPass(frame, commandBufferId, passIndex) {
    return this.textures.filter((t) => t.info.kind !== "sampled" && t.info.frame === frame && t.info.commandBuffer === commandBufferId && t.info.passIndex === passIndex).sort((a, b) => a.info.attachment - b.info.attachment);
  }
  /** A sampled / storage image read back for a descriptor, by the id the descriptor carries in `data`. */
  capturedImage(captureId) {
    if (!captureId) return null;
    return this.textures.find((t) => t.info.kind === "sampled" && t.info.capture === captureId) ?? null;
  }
  /** Any captured contents of an image (a sampled read-back or a render target), with data. */
  imageContents(imageId) {
    return this.textures.find((t) => t.info.id === imageId && t.data && !t.info.error) ?? null;
  }
  /** Sampled image read-backs: [captured, failed]. */
  get sampledImageCounts() {
    let ok = 0;
    let failed = 0;
    for (const t of this.textures) {
      if (t.info.kind !== "sampled") continue;
      if (t.info.error) failed++;
      else ok++;
    }
    return [ok, failed];
  }
  /** The commands of one captured frame (indices stay those of the full list). */
  commandsForFrame(frame) {
    return this.commands.filter((c2) => c2.frame === frame);
  }
  /** The captured contents of a bound buffer range, by the id the binding command carries. */
  buffer(id) {
    if (!id) return null;
    return this.buffers.get(id) ?? null;
  }
  get buffersLoading() {
    return this._pendingBuffers > 0;
  }
  /** Takes over a capture file's contents, emitting the signals a streamed capture would. */
  load(c2) {
    this.reset();
    this.frame = c2.manifest.frame;
    this.frames = Math.max(1, c2.manifest.frames ?? 1);
    this.api = c2.api;
    this.commands = c2.commands;
    this.textures = c2.textures;
    this.buffers = c2.buffers;
    this.passTimings = c2.passTimings;
    this.overdraw = c2.overdraw;
    this.pixelHistory = c2.pixelHistory;
    this.drawStats = c2.drawStats;
    this.onCaptureStatus.emit(`${this.commands.length} commands`);
    this.onCommandsComplete.emit();
    this.onTexturesAnnounced.emit();
    for (const t of this.textures) if (t.data) this.onTextureLoaded.emit(t);
    this.onBuffersAnnounced.emit();
    this.onBuffersComplete.emit();
    if (this.passTimings.size) this.onPassTimings.emit();
    if (this.overdraw.length) this.onOverdraw.emit();
    if (this.pixelHistory) this.onPixelHistory.emit();
    if (this.drawStats) this.onDrawStats.emit();
  }
  handleMessage(msg) {
    switch (msg.action) {
      case "CaptureFrameResults":
        this.reset();
        this.frame = msg.frame;
        this.frames = Math.max(1, msg.frames ?? 1);
        this.api = msg.api ?? "vulkan";
        this._expectedCommands = msg.count;
        this.onCaptureStatus.emit(`receiving ${msg.count} commands...`);
        if (msg.count === 0) this.onCommandsComplete.emit();
        break;
      case "CaptureFrameCommands":
        for (const c2 of msg.commands) this.commands[c2.index] = c2;
        if (this.commands.length >= this._expectedCommands) {
          this.commands = flattenSecondaries(this.commands);
          this.onCaptureStatus.emit(`${this.commands.length} commands`);
          this.onCommandsComplete.emit();
        }
        break;
      case "CaptureTextureFrames":
        this.textures = msg.textures.map((info) => ({ info, data: null, canvas: null }));
        this.onTexturesAnnounced.emit();
        break;
      case "CaptureTextureData": {
        const tex = msg.capture ? this.textures.find((t) => t.info.kind === "sampled" && t.info.capture === msg.capture) : this.textures.find((t) => t.info.kind !== "sampled" && t.info.frame === (msg.frame ?? 0) && t.info.commandBuffer === msg.commandBuffer && t.info.passIndex === msg.passIndex && t.info.attachment === msg.attachment);
        if (tex) {
          tex.data = msg.__binary ?? null;
          this.onTextureLoaded.emit(tex);
        }
        break;
      }
      case "CaptureBuffers":
        this.buffers = /* @__PURE__ */ new Map();
        this._pendingBuffers = 0;
        for (const info of msg.buffers ?? []) {
          this.buffers.set(info.id, { info, data: null });
          if (!info.error && info.size > 0) this._pendingBuffers++;
        }
        this.onBuffersAnnounced.emit();
        if (this._pendingBuffers === 0) this.onBuffersComplete.emit();
        break;
      case "CapturePassTimings":
        this.passTimings = /* @__PURE__ */ new Map();
        for (const p of msg.passes ?? []) this.passTimings.set(passKey(p.frame, p.commandBuffer, p.passIndex, p.kind === "compute"), p);
        this.onPassTimings.emit();
        break;
      case "CaptureOverdraw":
        this.overdraw = (msg.passes ?? []).map((info) => ({ info, data: null }));
        this.onOverdraw.emit();
        break;
      case "CapturePixelHistory":
        this.pixelHistory = msg.history ?? null;
        this.onPixelHistory.emit();
        break;
      case "CaptureOverdrawData": {
        const o = this.overdraw.find((m) => m.info.frame === msg.frame && m.info.commandBuffer === msg.commandBuffer && m.info.passIndex === msg.passIndex && m.info.depthTested === msg.depthTested);
        if (o) {
          o.data = msg.__binary ?? null;
          this.onOverdraw.emit();
        }
        break;
      }
      case "CaptureBufferData": {
        const buf = this.buffers.get(msg.id);
        if (buf) {
          if (!buf.data) this._pendingBuffers = Math.max(0, this._pendingBuffers - 1);
          buf.data = msg.__binary ?? new Uint8Array(0);
          this.onBufferLoaded.emit(buf);
          if (this._pendingBuffers === 0) this.onBuffersComplete.emit();
        }
        break;
      }
      default:
        break;
    }
  }
};

// src/renderer/capture_format.ts
var CAPTURE_FILE_EXTENSION = "gpucap";
var MAGIC = "GPUCAP 1\n";
var CAPTURE_FORMAT = "gpu-inspector-capture";
var CAPTURE_VERSION = 1;
function captureFileName(source, frame, frames) {
  const base = source.replace(/\.[^.]+$/, "").replace(/[^\w.-]+/g, "_").replace(/^_+|_+$/g, "") || "capture";
  return `${base}_frame_${frame}${frames > 1 ? `-${frame + frames - 1}` : ""}.${CAPTURE_FILE_EXTENSION}`;
}
function encodeCaptureFile(manifest, payloads) {
  const json = new TextEncoder().encode(JSON.stringify(manifest));
  const magic = new TextEncoder().encode(MAGIC);
  const payloadBytes = payloads.reduce((n, p) => n + p.byteLength, 0);
  const out = new Uint8Array(magic.byteLength + 4 + json.byteLength + payloadBytes);
  let pos = 0;
  out.set(magic, pos);
  pos += magic.byteLength;
  new DataView(out.buffer).setUint32(pos, json.byteLength, true);
  pos += 4;
  out.set(json, pos);
  pos += json.byteLength;
  for (const p of payloads) {
    out.set(p, pos);
    pos += p.byteLength;
  }
  return out;
}
function parseCaptureFile(bytes) {
  const magic = new TextEncoder().encode(MAGIC);
  if (bytes.byteLength < magic.byteLength + 4) throw new Error("The file is too short to be a capture.");
  for (let i = 0; i < magic.byteLength; i++) {
    if (bytes[i] !== magic[i]) throw new Error("Not a GPU Inspector capture file (bad header).");
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const jsonLength = view.getUint32(magic.byteLength, true);
  const jsonStart = magic.byteLength + 4;
  const base = jsonStart + jsonLength;
  if (base > bytes.byteLength) throw new Error("The capture file is truncated.");
  let manifest;
  try {
    manifest = JSON.parse(new TextDecoder().decode(bytes.subarray(jsonStart, base)));
  } catch (e) {
    throw new Error(`The capture's manifest is not valid JSON: ${e.message}`);
  }
  if (manifest.format !== CAPTURE_FORMAT) throw new Error("Not a GPU Inspector capture file (unknown format).");
  if (manifest.version > CAPTURE_VERSION) throw new Error(`The capture was saved by a newer GPU Inspector (format version ${manifest.version}); this build reads version ${CAPTURE_VERSION}.`);
  const payload = (p) => {
    if (!p) return null;
    const [offset, length] = p;
    if (offset < 0 || length < 0 || base + offset + length > bytes.byteLength) throw new Error("The capture file is truncated (payload out of range).");
    return bytes.subarray(base + offset, base + offset + length);
  };
  const blobs = /* @__PURE__ */ new Map();
  for (const o of manifest.objects ?? []) {
    (o.blobs ?? []).forEach((b, i) => {
      const data = payload(b.payload);
      if (data) blobs.set(`${o.id}:${i}`, data);
    });
  }
  const textures = (manifest.textures ?? []).map((t) => ({ info: t.info, data: payload(t.payload), canvas: null }));
  const buffers = /* @__PURE__ */ new Map();
  for (const b of manifest.buffers ?? []) buffers.set(b.info.id, { info: b.info, data: payload(b.payload) });
  const passTimings = /* @__PURE__ */ new Map();
  for (const p of manifest.passTimings ?? []) passTimings.set(passKey(p.frame, p.commandBuffer, p.passIndex, p.kind === "compute"), p);
  const overdraw = (manifest.overdraw ?? []).map((o) => ({ info: o.info, data: payload(o.payload) }));
  const commands = (manifest.commands ?? []).map((c2, i) => ({ ...c2, index: i }));
  return {
    manifest,
    validation: manifest.validation ?? [],
    objects: manifest.objects ?? [],
    blobs,
    commands,
    textures,
    buffers,
    passTimings,
    overdraw,
    pixelHistory: manifest.pixelHistory ?? null,
    drawStats: manifest.drawStats ?? null,
    api: manifest.api ?? "vulkan"
  };
}

// src/renderer/capture_statistics.ts
var COPY_METHODS = /* @__PURE__ */ new Set([
  "vkCmdCopyBuffer",
  "vkCmdCopyBuffer2",
  "vkCmdCopyBuffer2KHR",
  "vkCmdCopyImage",
  "vkCmdCopyImage2",
  "vkCmdCopyImage2KHR",
  "vkCmdCopyBufferToImage",
  "vkCmdCopyBufferToImage2",
  "vkCmdCopyBufferToImage2KHR",
  "vkCmdCopyImageToBuffer",
  "vkCmdCopyImageToBuffer2",
  "vkCmdCopyImageToBuffer2KHR",
  "vkCmdBlitImage",
  "vkCmdBlitImage2",
  "vkCmdBlitImage2KHR",
  "vkCmdResolveImage",
  "vkCmdResolveImage2",
  "vkCmdResolveImage2KHR",
  "vkCmdUpdateBuffer",
  "vkCmdFillBuffer",
  "vkCmdClearColorImage",
  "vkCmdClearDepthStencilImage",
  "vkCmdClearAttachments"
]);
var BARRIER_METHODS = /* @__PURE__ */ new Set(["vkCmdPipelineBarrier", "vkCmdPipelineBarrier2", "vkCmdPipelineBarrier2KHR", "vkCmdSetEvent", "vkCmdSetEvent2", "vkCmdWaitEvents", "vkCmdWaitEvents2"]);
var CaptureStatistics = class {
  frames = 0;
  apiCalls = 0;
  submits = 0;
  commandBuffers = 0;
  secondaryCommandBuffers = 0;
  draws = 0;
  indexedDraws = 0;
  indirectDraws = 0;
  meshDraws = 0;
  dispatches = 0;
  traceRays = 0;
  copyCommands = 0;
  barriers = 0;
  debugLabels = 0;
  renderPasses = 0;
  /** Runs of dispatches outside render passes (what Profile passes times as compute passes). */
  computePasses = 0;
  colorAttachments = 0;
  depthStencilAttachments = 0;
  renderTargetsCaptured = 0;
  bindPipeline = 0;
  graphicsPipelinesBound = 0;
  computePipelinesBound = 0;
  uniquePipelines = 0;
  vertexStages = 0;
  fragmentStages = 0;
  computeStages = 0;
  bindDescriptorSets = 0;
  descriptorSetsBound = 0;
  uniqueDescriptorSets = 0;
  uniformBuffers = 0;
  storageBuffers = 0;
  images = 0;
  samplers = 0;
  texelBuffers = 0;
  bindVertexBuffers = 0;
  bindIndexBuffer = 0;
  pushConstants = 0;
  pushConstantBytes = 0;
  updateBuffer = 0;
  updateBufferBytes = 0;
  fillBufferBytes = 0;
  bufferCopyBytes = 0;
  capturedBufferBytes = 0;
  capturedBuffers = 0;
  totalInstances = 0;
  totalVertices = 0;
  totalTriangles = 0;
  totalLines = 0;
  totalPoints = 0;
  totalPatches = 0;
  compute(data, db) {
    const cmdSets = data.sets;
    this.frames = data.frames;
    const pipelines = /* @__PURE__ */ new Set();
    const sets = /* @__PURE__ */ new Set();
    const commandBuffers = /* @__PURE__ */ new Set();
    const secondaries = /* @__PURE__ */ new Set();
    const boundPipeline = /* @__PURE__ */ new Map();
    const inRenderPass = /* @__PURE__ */ new Map();
    const computeOpen = /* @__PURE__ */ new Map();
    for (const cmd of data.commands) {
      if (!cmd) continue;
      this.apiCalls++;
      const method = cmd.method;
      const a = cmd.args;
      const stream = `${cmd.object?.__id ?? 0}:${cmd.secondary ?? 0}`;
      if (cmdSets.SUBMIT.has(method)) {
        this.submits++;
        continue;
      }
      if (cmdSets.COMPUTE_PASS_END.has(method) || cmdSets.PASS_BEGIN.has(method) || cmdSets.LABEL_BEGIN.has(method) || cmdSets.LABEL_END.has(method) || method === "vkEndCommandBuffer") computeOpen.set(stream, false);
      if (cmdSets.PASS_BEGIN.has(method)) inRenderPass.set(stream, true);
      if (cmdSets.PASS_END.has(method)) inRenderPass.set(stream, false);
      if (cmd.object) commandBuffers.add(cmd.object.__id);
      if (cmd.secondary) secondaries.add(cmd.secondary);
      if (cmdSets.DRAW.has(method)) {
        this.draws++;
        if (method.includes("Indexed")) this.indexedDraws++;
        if (method.includes("MeshTasks")) this.meshDraws++;
        const pipeline = db.getObject(boundPipeline.get(stream));
        this._geometry(cmd, data, pipeline);
      } else if (cmdSets.DISPATCH.has(method)) {
        this.dispatches++;
        if (!inRenderPass.get(stream) && !computeOpen.get(stream)) {
          this.computePasses++;
          computeOpen.set(stream, true);
        }
      } else if (cmdSets.TRACE.has(method)) {
        this.traceRays++;
      } else if (COPY_METHODS.has(method)) {
        this.copyCommands++;
        this._memory(cmd, db);
      } else if (BARRIER_METHODS.has(method)) {
        this.barriers++;
      } else if (cmdSets.LABEL_BEGIN.has(method)) {
        this.debugLabels++;
      } else if (cmdSets.PASS_BEGIN.has(method)) {
        this.renderPasses++;
        this._attachments(cmd, db);
      } else if (cmdSets.BIND_PIPELINE.has(method) && a) {
        this.bindPipeline++;
        const id = refId(a.pipeline);
        if (id !== null) pipelines.add(id);
        const bindPoint = cmdSets.pipelineBindPointOf(method, a);
        if (bindPoint === "VK_PIPELINE_BIND_POINT_GRAPHICS") {
          this.graphicsPipelinesBound++;
          boundPipeline.set(stream, id ?? 0);
        } else if (bindPoint === "VK_PIPELINE_BIND_POINT_COMPUTE") {
          this.computePipelinesBound++;
        }
        const d = db.getObject(id)?.descriptor;
        const stages = d ? Array.isArray(d.pStages) ? d.pStages : isObject(d.stage) ? [d.stage] : [] : [];
        for (const s of stages) {
          if (!isObject(s)) continue;
          const stage = str(s.stage);
          if (stage === "VK_SHADER_STAGE_VERTEX_BIT") this.vertexStages++;
          else if (stage === "VK_SHADER_STAGE_FRAGMENT_BIT") this.fragmentStages++;
          else if (stage === "VK_SHADER_STAGE_COMPUTE_BIT") this.computeStages++;
        }
      } else if (method === "vkCmdBindVertexBuffers" || method === "vkCmdBindVertexBuffers2" || method === "vkCmdBindVertexBuffers2EXT") {
        this.bindVertexBuffers++;
      } else if (method === "vkCmdBindIndexBuffer" || method === "vkCmdBindIndexBuffer2" || method === "vkCmdBindIndexBuffer2KHR") {
        this.bindIndexBuffer++;
      } else if (method === "vkCmdPushConstants" || method === "vkCmdPushConstants2" || method === "vkCmdPushConstants2KHR") {
        this.pushConstants++;
        const info = a && isObject(a.pPushConstantsInfo) ? a.pPushConstantsInfo : a;
        this.pushConstantBytes += num(info?.size);
      }
      if (cmd.descriptors) {
        this.bindDescriptorSets++;
        for (const set of cmd.descriptors.sets) {
          this.descriptorSetsBound++;
          const id = refId(set.descriptorSet);
          if (id !== null) sets.add(id);
          for (const b of set.bindings) {
            const written = b.descriptors.filter((d) => d).length;
            const t = b.type;
            if (t.includes("UNIFORM_BUFFER")) this.uniformBuffers += written;
            else if (t.includes("STORAGE_BUFFER")) this.storageBuffers += written;
            else if (t.includes("TEXEL_BUFFER")) this.texelBuffers += written;
            else if (t === "VK_DESCRIPTOR_TYPE_SAMPLER") this.samplers += written;
            else if (t === "VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER") {
              this.images += written;
              this.samplers += written;
            } else if (t.includes("IMAGE") || t.includes("INPUT_ATTACHMENT")) this.images += written;
          }
        }
      }
    }
    this.commandBuffers = commandBuffers.size;
    this.secondaryCommandBuffers = secondaries.size;
    this.uniquePipelines = pipelines.size;
    this.uniqueDescriptorSets = sets.size;
    this.renderTargetsCaptured = data.textures.filter((t) => !t.info.error).length;
    for (const b of data.buffers.values()) {
      if (b.info.error) continue;
      this.capturedBuffers++;
      this.capturedBufferBytes += b.info.size;
    }
    return this;
  }
  _geometry(cmd, data, pipeline) {
    const a = cmd.args;
    if (!a) return;
    let vertices = 0;
    let instances = 0;
    switch (cmd.method) {
      case "vkCmdDraw":
        vertices = num(a.vertexCount) * Math.max(1, num(a.instanceCount));
        instances = num(a.instanceCount);
        break;
      case "vkCmdDrawIndexed":
        vertices = num(a.indexCount) * Math.max(1, num(a.instanceCount));
        instances = num(a.instanceCount);
        break;
      case "vkCmdDrawMultiEXT":
      case "vkCmdDrawMultiIndexedEXT": {
        const infos = Array.isArray(a.pVertexInfo) ? a.pVertexInfo : Array.isArray(a.pIndexInfo) ? a.pIndexInfo : [];
        let count2 = 0;
        for (const i of infos) if (isObject(i)) count2 += num(i.vertexCount ?? i.indexCount);
        vertices = count2 * Math.max(1, num(a.instanceCount));
        instances = num(a.instanceCount) * infos.length;
        break;
      }
      case "vkCmdDrawIndirect":
      case "vkCmdDrawIndexedIndirect": {
        this.indirectDraws++;
        const buf = data.buffer(cmd.bufferData?.[0]);
        if (!buf?.data) return;
        const view = new DataView(buf.data.buffer, buf.data.byteOffset, buf.data.byteLength);
        const stride = Math.max(16, num(a.stride));
        const count2 = num(a.drawCount);
        for (let i = 0; i < count2; i++) {
          const at = i * stride;
          if (at + 8 > view.byteLength) break;
          const n2 = view.getUint32(at, true);
          const inst = view.getUint32(at + 4, true);
          vertices += n2 * Math.max(1, inst);
          instances += inst;
        }
        break;
      }
      default:
        if (cmd.method.includes("Indirect")) this.indirectDraws++;
        return;
    }
    this.totalInstances += instances;
    this.totalVertices += vertices;
    const d = pipeline?.descriptor;
    const ia = d && isObject(d.pInputAssemblyState) ? d.pInputAssemblyState : null;
    const topology = ia ? str(ia.topology) : "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST";
    const perInstance = instances > 0 ? vertices / instances : vertices;
    const n = Math.max(0, perInstance);
    const scale = Math.max(1, instances);
    switch (topology) {
      case "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST":
        this.totalTriangles += Math.floor(n / 3) * scale;
        break;
      case "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP":
      case "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN":
        this.totalTriangles += Math.max(0, n - 2) * scale;
        break;
      case "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY":
        this.totalTriangles += Math.floor(n / 6) * scale;
        break;
      case "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY":
        this.totalTriangles += Math.max(0, Math.floor((n - 4) / 2)) * scale;
        break;
      case "VK_PRIMITIVE_TOPOLOGY_LINE_LIST":
        this.totalLines += Math.floor(n / 2) * scale;
        break;
      case "VK_PRIMITIVE_TOPOLOGY_LINE_STRIP":
        this.totalLines += Math.max(0, n - 1) * scale;
        break;
      case "VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY":
        this.totalLines += Math.floor(n / 4) * scale;
        break;
      case "VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY":
        this.totalLines += Math.max(0, n - 3) * scale;
        break;
      case "VK_PRIMITIVE_TOPOLOGY_POINT_LIST":
        this.totalPoints += n * scale;
        break;
      case "VK_PRIMITIVE_TOPOLOGY_PATCH_LIST": {
        const ts = d && isObject(d.pTessellationState) ? d.pTessellationState : null;
        const points = Math.max(1, num(ts?.patchControlPoints ?? 3));
        this.totalPatches += Math.floor(n / points) * scale;
        break;
      }
      default:
        break;
    }
  }
  _attachments(cmd, db) {
    const a = cmd.args;
    if (!a) return;
    if (isObject(a.pRenderingInfo)) {
      const r = a.pRenderingInfo;
      this.colorAttachments += Array.isArray(r.pColorAttachments) ? r.pColorAttachments.length : 0;
      if (isObject(r.pDepthAttachment) || isObject(r.pStencilAttachment)) this.depthStencilAttachments++;
      return;
    }
    if (isObject(a.pRenderPassBegin)) {
      const rp = db.getObject(refId(a.pRenderPassBegin.renderPass))?.descriptor;
      const atts = rp && Array.isArray(rp.pAttachments) ? rp.pAttachments : [];
      for (const att of atts) {
        if (!isObject(att)) continue;
        if (/_D\d+_|_D\d+$|_S8_UINT/.test(fmt(att.format))) this.depthStencilAttachments++;
        else this.colorAttachments++;
      }
    }
  }
  _memory(cmd, db) {
    const a = cmd.args;
    if (!a) return;
    switch (cmd.method) {
      case "vkCmdUpdateBuffer":
        this.updateBuffer++;
        this.updateBufferBytes += num(a.dataSize);
        break;
      case "vkCmdFillBuffer": {
        let size2 = num(a.size);
        if (size2 === 0 || size2 > 1e15) {
          const buf = db.getObject(refId(a.dstBuffer))?.descriptor;
          size2 = Math.max(0, num(buf?.size) - num(a.offset));
        }
        this.fillBufferBytes += size2;
        break;
      }
      case "vkCmdCopyBuffer":
      case "vkCmdCopyBuffer2":
      case "vkCmdCopyBuffer2KHR": {
        const info = isObject(a.pCopyBufferInfo) ? a.pCopyBufferInfo : a;
        const regions = Array.isArray(info.pRegions) ? info.pRegions : [];
        for (const r of regions) if (isObject(r)) this.bufferCopyBytes += num(r.size);
        break;
      }
      default:
        break;
    }
  }
  /** The non-empty sections, in WebGPU Inspector's order. */
  sections() {
    const s = (title, rows) => rows.some((r) => r.value) ? { title, rows } : null;
    const out = [
      s("API Activity", [
        { label: "Frames", value: this.frames },
        { label: "Commands", value: this.apiCalls },
        { label: "Queue submits", value: this.submits },
        { label: "Command buffers", value: this.commandBuffers },
        { label: "Secondary command buffers", value: this.secondaryCommandBuffers },
        { label: "Draws", value: this.draws },
        { label: "Indexed draws", value: this.indexedDraws },
        { label: "Indirect draws", value: this.indirectDraws },
        { label: "Mesh draws", value: this.meshDraws },
        { label: "Dispatches", value: this.dispatches },
        { label: "Ray tracing launches", value: this.traceRays },
        { label: "Copy / clear commands", value: this.copyCommands },
        { label: "Barriers and events", value: this.barriers },
        { label: "Debug labels", value: this.debugLabels }
      ]),
      s("Passes", [
        { label: "Render passes", value: this.renderPasses },
        { label: "Compute passes", value: this.computePasses },
        { label: "Color attachments", value: this.colorAttachments },
        { label: "Depth / stencil attachments", value: this.depthStencilAttachments },
        { label: "Render targets read back", value: this.renderTargetsCaptured }
      ]),
      s("Pipeline", [
        { label: "Bind pipeline calls", value: this.bindPipeline },
        { label: "Graphics pipelines bound", value: this.graphicsPipelinesBound },
        { label: "Compute pipelines bound", value: this.computePipelinesBound },
        { label: "Distinct pipelines", value: this.uniquePipelines },
        { label: "Vertex stages", value: this.vertexStages },
        { label: "Fragment stages", value: this.fragmentStages },
        { label: "Compute stages", value: this.computeStages }
      ]),
      s("Bindings", [
        { label: "Bind descriptor set calls", value: this.bindDescriptorSets },
        { label: "Descriptor sets bound", value: this.descriptorSetsBound },
        { label: "Distinct descriptor sets", value: this.uniqueDescriptorSets },
        { label: "Uniform buffers", value: this.uniformBuffers },
        { label: "Storage buffers", value: this.storageBuffers },
        { label: "Texel buffers", value: this.texelBuffers },
        { label: "Images", value: this.images },
        { label: "Samplers", value: this.samplers },
        { label: "Bind vertex buffers", value: this.bindVertexBuffers },
        { label: "Bind index buffer", value: this.bindIndexBuffer },
        { label: "Push constant updates", value: this.pushConstants },
        { label: "Push constant bytes", value: this.pushConstantBytes, bytes: true }
      ]),
      s("Memory", [
        { label: "Update buffer calls", value: this.updateBuffer },
        { label: "Update buffer bytes", value: this.updateBufferBytes, bytes: true },
        { label: "Fill buffer bytes", value: this.fillBufferBytes, bytes: true },
        { label: "Buffer copy bytes", value: this.bufferCopyBytes, bytes: true },
        { label: "Buffers read back", value: this.capturedBuffers },
        { label: "Buffer bytes read back", value: this.capturedBufferBytes, bytes: true }
      ]),
      s("Geometry", [
        { label: "Instances", value: this.totalInstances },
        { label: "Vertices", value: this.totalVertices },
        { label: "Triangles", value: this.totalTriangles },
        { label: "Lines", value: this.totalLines },
        { label: "Points", value: this.totalPoints },
        { label: "Patches", value: this.totalPatches }
      ])
    ];
    return out.filter((x) => x !== null);
  }
};
var REFRESH_SOURCE_NOTE = {
  present_timing: "reported by the driver through VK_EXT_present_timing",
  display_timing: "reported by the driver through VK_GOOGLE_display_timing",
  monitor: "the current mode of the monitor showing the application",
  estimate: "estimated from the frame intervals while vsync is on"
};
function frameBound(t) {
  const vsync = t.refreshMs > 0;
  const budget = vsync ? t.refreshMs : t.frameMs > 0 ? t.frameMs : Math.max(t.gpuSpanMs, t.submitMs);
  if (!(budget > 0)) return null;
  const gpu = t.frames > 1 ? t.gpuSpanMs / t.frames : t.gpuSpanMs;
  let verdict;
  let kind;
  if (gpu / budget > 0.8) {
    verdict = "GPU bound";
    kind = "gpu";
  } else if (t.submitMs / budget > 0.8) {
    verdict = "CPU bound (submit)";
    kind = "cpu";
  } else if (vsync && t.frameMs >= budget * 0.9 && t.frameMs <= budget * 1.1) {
    verdict = "Vsync bound: the frame waits for the display; GPU and CPU have headroom";
    kind = "idle";
  } else if (vsync && t.frameMs > budget * 1.1) {
    verdict = "Missing the refresh: the frame takes longer than the display period, but neither the GPU passes nor the submit fill it (CPU work outside submission, or waits)";
    kind = "cpu";
  } else {
    verdict = "Present / CPU bound outside submit: the GPU has headroom";
    kind = "idle";
  }
  return { verdict, kind, budgetMs: budget, gpuMs: gpu, vsync };
}

// src/renderer/render_graph.ts
function buildRenderGraph(passes, options = {}) {
  const resources = /* @__PURE__ */ new Map();
  const nodes = [];
  const edges = [];
  const current = /* @__PURE__ */ new Map();
  const resourceOf = (raw) => {
    let r = resources.get(raw.key);
    if (!r) {
      r = { ...raw, versions: [], uses: [], first: Infinity, last: -1, externalInput: false };
      r.versions.push({ resource: r, index: 0, producer: null, readers: [], dropped: false });
      resources.set(raw.key, r);
      current.set(raw.key, r.versions[0]);
    }
    return r;
  };
  for (const pass of passes) {
    const node2 = {
      ordinal: nodes.length,
      kind: pass.kind,
      label: pass.label,
      commandIndex: pass.commandIndex,
      passKey: pass.passKey,
      frame: pass.frame,
      draws: pass.draws,
      reads: [],
      writes: [],
      inputs: [],
      outputs: [],
      durationMs: pass.passKey && options.durationOf ? options.durationOf(pass.passKey) : null,
      unresolvedReads: pass.unresolvedReads ?? 0,
      unread: false,
      pathMs: 0
    };
    nodes.push(node2);
    for (const access of pass.accesses) {
      if (access.mode === "write" && access.discards) continue;
      const resource2 = resourceOf(access.resource);
      const version = current.get(resource2.key);
      const isRead = access.mode !== "write";
      const use = { node: node2, resource: resource2, mode: access.mode, usage: access.usage, version, discards: !!access.discards, dropped: !!access.dropped, resolved: !!access.resolved };
      if (isRead) {
        node2.reads.push(use);
        resource2.uses.push(use);
        touch(resource2, node2);
      }
      if (!version.readers.includes(node2)) version.readers.push(node2);
      if (version.producer && version.producer !== node2) {
        const edge = { from: version.producer, to: node2, version, usage: access.usage };
        edges.push(edge);
        version.producer.outputs.push(edge);
        node2.inputs.push(edge);
      } else if (!version.producer) {
        resource2.externalInput = true;
      }
    }
    for (const access of pass.accesses) {
      if (access.mode === "read") continue;
      const resource2 = resourceOf(access.resource);
      const version = {
        resource: resource2,
        index: resource2.versions.length,
        producer: node2,
        readers: [],
        dropped: !!access.dropped
      };
      resource2.versions.push(version);
      current.set(resource2.key, version);
      const use = { node: node2, resource: resource2, mode: access.mode, usage: access.usage, version, discards: !!access.discards, dropped: !!access.dropped, resolved: !!access.resolved };
      node2.writes.push(use);
      resource2.uses.push(use);
      touch(resource2, node2);
    }
  }
  for (const node2 of nodes) {
    node2.unread = node2.writes.length > 0 && node2.writes.every((w) => w.version.readers.length === 0 && !w.resource.presented);
  }
  const list = [...resources.values()];
  const graph = {
    api: options.api ?? "vulkan",
    nodes,
    resources: list,
    edges,
    syncPoints: options.syncPoints ?? [],
    externalInputs: list.filter((r) => r.externalInput),
    unreadNodes: nodes.filter((n) => n.unread),
    criticalPath: [],
    criticalPathMs: 0,
    warnings: []
  };
  computeCriticalPath(graph);
  if (nodes.some((n) => [...n.reads, ...n.writes].some((u) => u.usage.includes("storage")))) {
    graph.warnings.push("Storage buffers and storage images are counted as read-write: without shader reflection the capture cannot tell a binding the shader only reads from one it writes, so some write edges may be dependencies that are not really there.");
  }
  const unresolved = nodes.reduce((n, p) => n + p.unresolvedReads, 0);
  if (unresolved) {
    graph.warnings.push(`${unresolved} binding${unresolved === 1 ? "" : "s"} could not be resolved to a resource (bindless / descriptor buffers), so some read edges are missing: the graph is a lower bound on the frame's dependencies.`);
  }
  if (!nodes.some((n) => n.durationMs !== null)) {
    graph.warnings.push('The capture has no GPU pass timings, so there is no critical path. Capture with "Profile passes" to get one.');
  }
  return graph;
}
function touch(resource2, node2) {
  resource2.first = Math.min(resource2.first, node2.ordinal);
  resource2.last = Math.max(resource2.last, node2.ordinal);
}
function computeCriticalPath(graph) {
  const next = /* @__PURE__ */ new Map();
  let head = null;
  let best = 0;
  for (let i = graph.nodes.length - 1; i >= 0; i--) {
    const node2 = graph.nodes[i];
    let bestChild = null;
    let bestChildMs = 0;
    for (const edge of node2.outputs) {
      if (edge.to.ordinal <= node2.ordinal) continue;
      if (edge.to.pathMs > bestChildMs) {
        bestChildMs = edge.to.pathMs;
        bestChild = edge.to;
      }
    }
    node2.pathMs = (node2.durationMs ?? 0) + bestChildMs;
    next.set(node2, bestChild);
    if (node2.pathMs > best) {
      best = node2.pathMs;
      head = node2;
    }
  }
  if (!head || best <= 0) return;
  const path11 = [];
  for (let n = head; n; n = next.get(n) ?? null) path11.push(n);
  graph.criticalPath = path11;
  graph.criticalPathMs = best;
}
function usageClass(usage) {
  if (usage.endsWith(" src") || usage.endsWith(" dst")) return "transfer";
  if (usage.startsWith("color") || usage.startsWith("depth") || usage.startsWith("stencil") || usage.startsWith("resolve")) return "attachment";
  if (usage.startsWith("copy") || usage.startsWith("clear") || usage.startsWith("blit")) return "transfer";
  if (usage.startsWith("storage")) return "storage";
  if (usage.startsWith("sampled")) return "sampled";
  if (usage.startsWith("vertex") || usage.startsWith("index") || usage.startsWith("indirect") || usage.startsWith("uniform")) return "input";
  return "other";
}

// src/renderer/metal/frame_resources.ts
var PASS_KINDS = {
  "renderCommandEncoderWithDescriptor:": "render",
  "parallelRenderCommandEncoderWithDescriptor:": "render",
  computeCommandEncoder: "compute",
  "computeCommandEncoderWithDescriptor:": "compute",
  "computeCommandEncoderWithDispatchType:": "compute",
  blitCommandEncoder: "transfer",
  "blitCommandEncoderWithDescriptor:": "transfer",
  resourceStateCommandEncoder: "transfer",
  "resourceStateCommandEncoderWithDescriptor:": "transfer",
  accelerationStructureCommandEncoder: "compute",
  "accelerationStructureCommandEncoderWithDescriptor:": "compute"
};
var BLITS = {
  "copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:": { verb: "copy", read: "sourceTexture", write: "destinationTexture" },
  "copyFromTexture:toTexture:": { verb: "copy", read: "sourceTexture", write: "destinationTexture", full: true },
  "copyFromTexture:sourceSlice:sourceLevel:toTexture:destinationSlice:destinationLevel:sliceCount:levelCount:": { verb: "copy", read: "sourceTexture", write: "destinationTexture", full: true },
  "copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:": { verb: "copy", read: "sourceTexture", write: "destinationBuffer" },
  "copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:options:": { verb: "copy", read: "sourceTexture", write: "destinationBuffer" },
  "copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:": { verb: "copy", read: "sourceBuffer", write: "destinationTexture" },
  "copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:options:": { verb: "copy", read: "sourceBuffer", write: "destinationTexture" },
  "copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:": { verb: "copy", read: "sourceBuffer", write: "destinationBuffer" },
  "generateMipmapsForTexture:": { verb: "generate mipmaps", read: "texture", write: "texture", full: true },
  "fillBuffer:range:value:": { verb: "fill", write: "buffer", full: true }
};
var TEXTURE_BINDS = {
  "setVertexTexture:atIndex:": "vertex",
  "setFragmentTexture:atIndex:": "fragment",
  "setTexture:atIndex:": "compute",
  "setObjectTexture:atIndex:": "object",
  "setMeshTexture:atIndex:": "mesh",
  "setTileTexture:atIndex:": "tile"
};
var TEXTURE_BINDS_MANY = {
  "setVertexTextures:withRange:": "vertex",
  "setFragmentTextures:withRange:": "fragment",
  "setTextures:withRange:": "compute",
  "setObjectTextures:withRange:": "object",
  "setMeshTextures:withRange:": "mesh",
  "setTileTextures:withRange:": "tile"
};
var ARGUMENT_BUFFER_METHODS = /* @__PURE__ */ new Set([
  "useResource:usage:",
  "useResource:usage:stages:",
  "useResources:count:usage:",
  "useResources:count:usage:stages:",
  "useHeap:",
  "useHeaps:count:",
  "useHeap:stages:",
  "useHeaps:count:stages:"
]);
var MetalResourceSource = class {
  _db;
  /** Bound state per encoder; a command's `encoder` says which one it belongs to. */
  _encoders = /* @__PURE__ */ new Map();
  _resources = /* @__PURE__ */ new Map();
  constructor(db) {
    this._db = db;
  }
  observe(cmd, _stream) {
    const a = cmd.args;
    if (!a) return;
    const state = this._state(cmd);
    const single = TEXTURE_BINDS[cmd.method];
    if (single) {
      const id = refId(a.texture);
      if (id !== null) state.textures.set(`${single}:${num(a.index)}`, id);
      return;
    }
    const many = TEXTURE_BINDS_MANY[cmd.method];
    if (many && Array.isArray(a.textures)) {
      const first = isObject(a.range) ? num(a.range.location) : 0;
      a.textures.forEach((t, i) => {
        const id = refId(t);
        if (id !== null) state.textures.set(`${many}:${first + i}`, id);
      });
      return;
    }
    if (ARGUMENT_BUFFER_METHODS.has(cmd.method)) {
      state.opaque++;
      return;
    }
    if (METAL_SETS.BIND_STAGE_BUFFER?.has(cmd.method) && METAL_SETS.stageBuffersOf) {
      for (const b of METAL_SETS.stageBuffersOf(cmd)) {
        const id = refId(b.buffer);
        if (id !== null) state.buffers.set(`${b.stage}:${b.index}`, id);
      }
    }
  }
  passAccesses(cmd, ordinal) {
    const kind = PASS_KINDS[cmd.method] ?? "render";
    const a = cmd.args;
    const accesses = [];
    const targets = [];
    if (kind === "render" && a) {
      const colors = Array.isArray(a.colorAttachments) ? a.colorAttachments.filter(isObject) : [];
      for (const c2 of colors) this._attachment(c2, "color", accesses, targets);
      for (const key of ["depthAttachment", "stencilAttachment"]) {
        const d = a[key];
        if (isObject(d)) this._attachment(d, key === "depthAttachment" ? "depth" : "stencil", accesses, targets);
      }
    }
    const encoder = this._db.getObject(cmd.encoder?.__id ?? null);
    const name = encoder?.label || targets.join(", ");
    const what = kind === "render" ? "Pass" : kind === "compute" ? "Compute" : "Blit";
    return { kind, label: `${what} ${ordinal}${name ? `: ${name}` : ""}`, accesses };
  }
  actionAccesses(cmd, _stream) {
    const state = this._state(cmd);
    const accesses = [];
    for (const [key, id] of state.textures) {
      const resource2 = this._textureResource(id, 0, 0);
      if (resource2) accesses.push({ resource: resource2, mode: "read", usage: `${key.split(":")[0]} texture` });
    }
    for (const [key, id] of state.buffers) {
      const resource2 = this._bufferResource(id);
      const stage = key.split(":")[0];
      if (resource2) accesses.push({ resource: resource2, mode: "read", usage: `${stage} buffer` });
    }
    const index = METAL_SETS.indexBufferOf(cmd);
    if (index) {
      const resource2 = this._bufferResource(refId(index.buffer));
      if (resource2) accesses.push({ resource: resource2, mode: "read", usage: "index buffer" });
    }
    for (const key of ["indirectBuffer", "patchIndexBuffer", "controlPointIndexBuffer"]) {
      const resource2 = this._bufferResource(refId(cmd.args?.[key]));
      if (resource2) accesses.push({ resource: resource2, mode: "read", usage: "indirect buffer" });
    }
    return { accesses, unresolved: state.opaque };
  }
  transferAccesses(cmd) {
    const spec = BLITS[cmd.method];
    const a = cmd.args;
    if (!spec || !a) return null;
    const accesses = [];
    const names = [];
    const add = (field2, mode) => {
      const id = refId(a[field2]);
      if (id === null) return;
      const isTexture = field2.toLowerCase().includes("texture");
      const level = num(a[mode === "read" ? "sourceLevel" : "destinationLevel"]);
      const slice = num(a[mode === "read" ? "sourceSlice" : "destinationSlice"]);
      const resource2 = isTexture ? this._textureResource(id, level, slice) : this._bufferResource(id);
      if (!resource2) return;
      accesses.push({
        resource: resource2,
        mode,
        usage: `${spec.verb} ${mode === "read" ? "src" : "dst"}`,
        discards: mode === "write" && !!spec.full
      });
      if (mode === "write") names.push(resource2.label);
    };
    if (spec.read) add(spec.read, "read");
    if (spec.write) add(spec.write, "write");
    const verb = spec.verb.charAt(0).toUpperCase() + spec.verb.slice(1);
    return { label: `${verb} \u2192 ${names.join(", ") || cmd.method}`, accesses };
  }
  computePassLabel(ordinal) {
    return `Compute ${ordinal}`;
  }
  // ------------------------------------------------------------------------------- resources
  _state(cmd) {
    const id = cmd.encoder?.__id ?? 0;
    let state = this._encoders.get(id);
    if (!state) this._encoders.set(id, state = { textures: /* @__PURE__ */ new Map(), buffers: /* @__PURE__ */ new Map(), opaque: 0 });
    return state;
  }
  _attachment(att, kind, accesses, targets) {
    const id = refId(att.texture);
    if (id === null) return;
    const resource2 = this._textureResource(id, num(att.level), num(att.slice));
    if (!resource2) return;
    const load = str(att.loadAction).replace("MTLLoadAction", "");
    const store = str(att.storeAction).replace("MTLStoreAction", "");
    const stores = store === "Store" || store === "StoreAndMultisampleResolve" || store === "CustomSampleDepthStore";
    accesses.push({
      resource: resource2,
      mode: "write",
      usage: `${kind} attachment (${load.toLowerCase() || "unknown"}/${store.toLowerCase() || "unknown"})`,
      discards: load !== "Load",
      dropped: !stores && store !== "MultisampleResolve",
      resolved: store.includes("MultisampleResolve")
    });
    if (kind === "color") targets.push(resource2.label);
    const resolveId = refId(att.resolveTexture);
    if (resolveId !== null && store.includes("MultisampleResolve")) {
      const target = this._textureResource(resolveId, num(att.resolveLevel), num(att.resolveSlice));
      if (target) accesses.push({ resource: target, mode: "write", usage: "resolve target (discard/store)", discards: true });
    }
  }
  _textureResource(textureId, level, slice) {
    if (textureId === null) return null;
    const object = this._db.getObject(textureId);
    if (!object) return null;
    const key = `image:${textureId}:m${level}:l${slice}`;
    const cached = this._resources.get(key);
    if (cached) return cached;
    const d = object.args ?? {};
    const mips = num(d.mipmapLevelCount) || 1;
    const layers = num(d.arrayLength) || 1;
    const width = Math.max(1, num(d.width) >> level);
    const height = Math.max(1, num(d.height) >> level);
    const format = str(d.pixelFormat).replace("MTLPixelFormat", "");
    const sub = [mips > 1 ? `level ${level}` : "", layers > 1 ? `slice ${slice}` : ""].filter(Boolean).join(" ");
    const resource2 = {
      key,
      objectId: textureId,
      type: "image",
      label: sub ? `${object.name} ${sub}` : object.name,
      detail: [width && height ? `${width}x${height}` : "", format].filter(Boolean).join("  "),
      bytes: width * height * bytesPerPixel(format),
      presented: isDrawableTexture(object)
    };
    this._resources.set(key, resource2);
    return resource2;
  }
  _bufferResource(bufferId) {
    if (bufferId === null) return null;
    const object = this._db.getObject(bufferId);
    if (!object) return null;
    const key = `buffer:${bufferId}`;
    const cached = this._resources.get(key);
    if (cached) return cached;
    const size2 = num(object.args?.length);
    const resource2 = {
      key,
      objectId: bufferId,
      type: "buffer",
      label: object.name,
      detail: size2 ? formatBytes2(size2) : "",
      bytes: size2,
      presented: false
    };
    this._resources.set(key, resource2);
    return resource2;
  }
};
function isDrawableTexture(object) {
  return object.cmd.includes("nextDrawable");
}
function bytesPerPixel(format) {
  const bits = [...format.matchAll(/[RGBADS](\d+)/g)].reduce((sum, m) => sum + Number(m[1]), 0);
  if (bits) return bits / 8;
  if (/BC|ETC|ASTC|EAC|PVRTC/.test(format)) return 1;
  return 4;
}
function formatBytes2(bytes) {
  if (bytes >= 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${bytes} B`;
}

// src/renderer/vulkan/pass_info.ts
function loadStoreOp(v) {
  return str(v).replace("VK_ATTACHMENT_LOAD_OP_", "").replace("VK_ATTACHMENT_STORE_OP_", "");
}
function sampleCount(v) {
  const m = /VK_SAMPLE_COUNT_(\d+)_BIT/.exec(str(v));
  return m ? Number(m[1]) : 1;
}
function pNextChain(o) {
  const chain = o?.pNext;
  return Array.isArray(chain) ? chain.filter(isObject) : [];
}
function imageOfView(db, viewId) {
  const view = db.getObject(viewId);
  return view ? refId(view.descriptor?.image) : null;
}
function viewSubresource(db, viewId) {
  const view = db.getObject(viewId)?.descriptor ?? null;
  const range = isObject(view?.subresourceRange) ? view.subresourceRange : null;
  const layers = num(range?.layerCount);
  return {
    mipLevel: num(range?.baseMipLevel),
    baseLayer: num(range?.baseArrayLayer),
    // VK_REMAINING_ARRAY_LAYERS reads back as the raw 0xffffffff; report it as "all remaining" (0).
    layerCount: layers === 4294967295 ? 0 : layers
  };
}
function decodePass(cmd, db) {
  const a = cmd.args;
  if (!a) return null;
  const pass = { width: 0, height: 0, viewMask: 0, attachments: [] };
  if (cmd.method.startsWith("vkCmdBeginRendering")) {
    const info = isObject(a.pRenderingInfo) ? a.pRenderingInfo : null;
    if (!info) return pass;
    const extent2 = isObject(info.renderArea) && isObject(info.renderArea.extent) ? info.renderArea.extent : null;
    pass.width = num(extent2?.width);
    pass.height = num(extent2?.height);
    pass.viewMask = num(info.viewMask);
    const colors = Array.isArray(info.pColorAttachments) ? info.pColorAttachments.filter(isObject) : [];
    colors.forEach((c2, i) => pass.attachments.push(dynamicAttachment(db, c2, "color", i)));
    for (const key of ["pDepthAttachment", "pStencilAttachment"]) {
      const d = info[key];
      if (isObject(d) && refId(d.imageView) !== null) pass.attachments.push(dynamicAttachment(db, d, "depth", colors.length));
    }
    return pass;
  }
  const begin = isObject(a.pRenderPassBegin) ? a.pRenderPassBegin : null;
  if (!begin) return pass;
  const rp = db.getObject(refId(begin.renderPass))?.descriptor ?? null;
  const fb = db.getObject(refId(begin.framebuffer))?.descriptor ?? null;
  const extent = isObject(begin.renderArea) && isObject(begin.renderArea.extent) ? begin.renderArea.extent : null;
  pass.width = num(extent?.width) || num(fb?.width);
  pass.height = num(extent?.height) || num(fb?.height);
  if (!rp) return pass;
  const subpasses = Array.isArray(rp.pSubpasses) ? rp.pSubpasses.filter(isObject) : [];
  const mv = pNextChain(rp).find((s) => str(s.sType) === "VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO");
  const masks = mv && Array.isArray(mv.pViewMasks) ? mv.pViewMasks.map(num) : subpasses.map((s) => num(s.viewMask));
  pass.viewMask = masks.reduce((m, v) => m | v, 0);
  let views = Array.isArray(fb?.pAttachments) ? fb.pAttachments : [];
  const imageless = pNextChain(begin).find((s) => str(s.sType) === "VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO");
  if (imageless && Array.isArray(imageless.pAttachments)) views = imageless.pAttachments;
  const descs = Array.isArray(rp.pAttachments) ? rp.pAttachments : [];
  const kinds = /* @__PURE__ */ new Map();
  const resolvedColors = /* @__PURE__ */ new Set();
  for (const s of subpasses) {
    const refs = (key) => (Array.isArray(s[key]) ? s[key] : []).filter(isObject).map((r) => num(r.attachment)).filter((i) => i < 4294967295);
    const colors = refs("pColorAttachments");
    const resolves = refs("pResolveAttachments");
    colors.forEach((c2, i) => {
      kinds.set(c2, "color");
      if (resolves[i] !== void 0) resolvedColors.add(c2);
    });
    for (const r of resolves) kinds.set(r, "resolve");
    const ds = isObject(s.pDepthStencilAttachment) ? num(s.pDepthStencilAttachment.attachment) : 4294967295;
    if (ds < 4294967295) kinds.set(ds, "depth");
  }
  descs.forEach((d, i) => {
    if (!isObject(d)) return;
    const kind = kinds.get(i);
    if (!kind) return;
    const viewId = refId(views[i]);
    pass.attachments.push({
      kind,
      viewId,
      imageId: imageOfView(db, viewId),
      ...viewSubresource(db, viewId),
      format: str(d.format),
      samples: sampleCount(d.samples),
      loadOp: loadStoreOp(d.loadOp),
      storeOp: loadStoreOp(d.storeOp),
      stencilLoadOp: loadStoreOp(d.stencilLoadOp),
      stencilStoreOp: loadStoreOp(d.stencilStoreOp),
      usage: "",
      resolved: resolvedColors.has(i),
      resolveViewId: null,
      index: i
    });
  });
  return pass;
}
function dynamicAttachment(db, att, kind, index) {
  const viewId = refId(att.imageView);
  const imageId = imageOfView(db, viewId);
  const view = db.getObject(viewId)?.descriptor ?? null;
  const image = db.getObject(imageId)?.descriptor ?? null;
  return {
    kind,
    viewId,
    imageId,
    ...viewSubresource(db, viewId),
    format: str(view?.format ?? image?.format),
    samples: sampleCount(image?.samples),
    loadOp: loadStoreOp(att.loadOp),
    storeOp: loadStoreOp(att.storeOp),
    stencilLoadOp: loadStoreOp(att.loadOp),
    stencilStoreOp: loadStoreOp(att.storeOp),
    usage: "",
    resolved: refId(att.resolveImageView) !== null,
    resolveViewId: refId(att.resolveImageView),
    index
  };
}

// src/renderer/vulkan/frame_resources.ts
var TRANSFERS = {
  vkCmdCopyBuffer: { read: ["srcBuffer"], write: ["dstBuffer"], verb: "copy" },
  vkCmdCopyBuffer2: { read: ["srcBuffer"], write: ["dstBuffer"], nested: "pCopyBufferInfo", verb: "copy" },
  vkCmdCopyBuffer2KHR: { read: ["srcBuffer"], write: ["dstBuffer"], nested: "pCopyBufferInfo", verb: "copy" },
  vkCmdCopyImage: { read: ["srcImage"], write: ["dstImage"], verb: "copy" },
  vkCmdCopyImage2: { read: ["srcImage"], write: ["dstImage"], nested: "pCopyImageInfo", verb: "copy" },
  vkCmdCopyImage2KHR: { read: ["srcImage"], write: ["dstImage"], nested: "pCopyImageInfo", verb: "copy" },
  vkCmdCopyBufferToImage: { read: ["srcBuffer"], write: ["dstImage"], verb: "copy" },
  vkCmdCopyBufferToImage2: { read: ["srcBuffer"], write: ["dstImage"], nested: "pCopyBufferToImageInfo", verb: "copy" },
  vkCmdCopyBufferToImage2KHR: { read: ["srcBuffer"], write: ["dstImage"], nested: "pCopyBufferToImageInfo", verb: "copy" },
  vkCmdCopyImageToBuffer: { read: ["srcImage"], write: ["dstBuffer"], verb: "copy" },
  vkCmdCopyImageToBuffer2: { read: ["srcImage"], write: ["dstBuffer"], nested: "pCopyImageToBufferInfo", verb: "copy" },
  vkCmdCopyImageToBuffer2KHR: { read: ["srcImage"], write: ["dstBuffer"], nested: "pCopyImageToBufferInfo", verb: "copy" },
  vkCmdBlitImage: { read: ["srcImage"], write: ["dstImage"], verb: "blit" },
  vkCmdBlitImage2: { read: ["srcImage"], write: ["dstImage"], nested: "pBlitImageInfo", verb: "blit" },
  vkCmdBlitImage2KHR: { read: ["srcImage"], write: ["dstImage"], nested: "pBlitImageInfo", verb: "blit" },
  vkCmdResolveImage: { read: ["srcImage"], write: ["dstImage"], verb: "resolve copy" },
  vkCmdResolveImage2: { read: ["srcImage"], write: ["dstImage"], nested: "pResolveImageInfo", verb: "resolve copy" },
  vkCmdResolveImage2KHR: { read: ["srcImage"], write: ["dstImage"], nested: "pResolveImageInfo", verb: "resolve copy" },
  vkCmdUpdateBuffer: { read: [], write: ["dstBuffer"], verb: "update" },
  vkCmdFillBuffer: { read: [], write: ["dstBuffer"], verb: "fill" },
  vkCmdClearColorImage: { read: [], write: ["image"], verb: "clear" },
  vkCmdClearDepthStencilImage: { read: [], write: ["image"], verb: "clear" },
  vkCmdCopyQueryPoolResults: { read: [], write: ["dstBuffer"], verb: "query results" }
};
var BARRIER_METHODS2 = /* @__PURE__ */ new Set([
  "vkCmdPipelineBarrier",
  "vkCmdPipelineBarrier2",
  "vkCmdPipelineBarrier2KHR",
  "vkCmdWaitEvents",
  "vkCmdWaitEvents2",
  "vkCmdWaitEvents2KHR"
]);
var FULL_WRITE_VERBS = /* @__PURE__ */ new Set(["clear", "fill", "update"]);
var DESCRIPTOR_BUFFER_METHODS = /* @__PURE__ */ new Set([
  "vkCmdBindDescriptorBuffersEXT",
  "vkCmdSetDescriptorBufferOffsetsEXT",
  "vkCmdSetDescriptorBufferOffsets2EXT",
  "vkCmdBindDescriptorBufferEmbeddedSamplersEXT"
]);
var VulkanResourceSource = class {
  _db;
  _bound = /* @__PURE__ */ new Map();
  /** Vertex and index buffers bound per stream: stream -> "v<binding>" / "index" -> buffer id. */
  _buffers = /* @__PURE__ */ new Map();
  /** Streams that bound a descriptor buffer, whose contents the capture cannot see. */
  _descriptorBuffers = /* @__PURE__ */ new Set();
  _resources = /* @__PURE__ */ new Map();
  constructor(db) {
    this._db = db;
  }
  observe(cmd, stream) {
    if (cmd.descriptors) {
      let byPoint = this._bound.get(stream);
      if (!byPoint) this._bound.set(stream, byPoint = /* @__PURE__ */ new Map());
      let sets = byPoint.get(cmd.descriptors.bindPoint);
      if (!sets) byPoint.set(cmd.descriptors.bindPoint, sets = /* @__PURE__ */ new Map());
      for (const s of cmd.descriptors.sets) sets.set(s.set, s);
      return;
    }
    const a = cmd.args;
    if (!a) return;
    if (DESCRIPTOR_BUFFER_METHODS.has(cmd.method)) {
      this._descriptorBuffers.add(stream);
    } else if (BIND_VERTEX_METHODS.has(cmd.method) && Array.isArray(a.pBuffers)) {
      const first = num(a.firstBinding);
      a.pBuffers.forEach((b, i) => {
        const id = refId(b);
        if (id !== null) this._streamBuffers(stream).set(`v${first + i}`, id);
      });
    } else if (BIND_INDEX_METHODS.has(cmd.method)) {
      const id = refId(a.buffer);
      if (id !== null) this._streamBuffers(stream).set("index", id);
    }
  }
  passAccesses(cmd, ordinal) {
    const decoded = decodePass(cmd, this._db);
    if (!decoded) return null;
    const accesses = [];
    for (const att of decoded.attachments) {
      const resource2 = this._imageResource(att.imageId, att.mipLevel, att.baseLayer);
      if (!resource2) continue;
      const loads = att.loadOp === "LOAD" || att.kind === "depth" && att.stencilLoadOp === "LOAD";
      const stores = att.storeOp === "STORE" || att.kind === "depth" && att.stencilStoreOp === "STORE";
      const kind = att.kind === "resolve" ? "resolve target" : `${att.kind} attachment`;
      accesses.push({
        resource: resource2,
        mode: "write",
        usage: `${kind} (${loads ? "load" : att.loadOp === "CLEAR" ? "clear" : "discard"}/${stores ? "store" : "discard"})`,
        discards: !loads,
        dropped: !stores,
        resolved: att.resolved
      });
      const resolve = this._imageResource(imageOfView(this._db, att.resolveViewId), 0, 0);
      if (resolve) accesses.push({ resource: resolve, mode: "write", usage: "resolve target (discard/store)", discards: true });
    }
    return { kind: "render", label: this._passLabel(cmd, ordinal), accesses };
  }
  actionAccesses(cmd, stream) {
    const accesses = [];
    let unresolved = 0;
    const sets = this._bound.get(stream)?.get(bindPointOf(cmd.method));
    if (sets) {
      for (const set of sets.values()) {
        for (const binding of set.bindings) {
          const usage = descriptorUsage(binding.type);
          if (!usage) continue;
          for (const d of binding.descriptors) {
            if (!d) continue;
            const bufferId = refId(d.buffer);
            if (bufferId !== null) {
              const resource2 = this._bufferResource(bufferId);
              if (resource2) accesses.push({ resource: resource2, mode: usage.write ? "readwrite" : "read", usage: usage.name });
              continue;
            }
            const viewId = refId(d.imageView);
            if (viewId !== null) {
              const resource2 = this._viewResource(viewId);
              if (resource2) accesses.push({ resource: resource2, mode: usage.write ? "readwrite" : "read", usage: usage.name });
              continue;
            }
            if (!d.immutable && !d.sampler && !d.bufferView) unresolved++;
          }
        }
      }
    }
    if (this._descriptorBuffers.has(stream)) unresolved++;
    if (!DISPATCH_METHODS.has(cmd.method) && !TRACE_METHODS.has(cmd.method)) {
      for (const [binding, id] of this._streamBuffers(stream)) {
        const resource2 = this._bufferResource(id);
        if (resource2) accesses.push({ resource: resource2, mode: "read", usage: binding === "index" ? "index buffer" : "vertex buffer" });
      }
    }
    if (INDIRECT_METHODS.has(cmd.method) || cmd.method.includes("Indirect")) {
      for (const key of ["buffer", "countBuffer"]) {
        const resource2 = this._bufferResource(refId(cmd.args?.[key]));
        if (resource2) accesses.push({ resource: resource2, mode: "read", usage: "indirect buffer" });
      }
    }
    return { accesses, unresolved };
  }
  transferAccesses(cmd) {
    const spec = TRANSFERS[cmd.method];
    if (!spec || !cmd.args) return null;
    const nested = spec.nested ? cmd.args[spec.nested] : null;
    const a = isObject(nested) ? nested : cmd.args;
    const accesses = [];
    const names = [];
    const add = (field2, mode) => {
      const value = a[field2];
      const id = refId(value);
      if (id === null) return;
      const isImage = field2.toLowerCase().includes("image");
      const sub = isImage ? copySubresource(a, mode) : null;
      const resource2 = sub ? this._imageResource(id, sub.mip, sub.layer) : this._bufferResource(id);
      if (!resource2) return;
      accesses.push({
        resource: resource2,
        mode,
        usage: `${spec.verb} ${mode === "read" ? "src" : "dst"}`,
        // A clear, fill or update replaces everything it touches; a copy region may not, so it is
        // reported as preserving what was there and depending on the previous writer.
        discards: mode === "write" && FULL_WRITE_VERBS.has(spec.verb)
      });
      if (mode === "write") names.push(resource2.label);
    };
    for (const f of spec.read) add(f, "read");
    for (const f of spec.write) add(f, "write");
    const verb = spec.verb.charAt(0).toUpperCase() + spec.verb.slice(1);
    return { label: `${verb} \u2192 ${names.join(", ") || cmd.method}`, accesses };
  }
  computePassLabel(ordinal) {
    return `Compute ${ordinal}`;
  }
  /**
   * The subresources a pipeline barrier or event wait names. A barrier that also transitions an
   * image layout or moves a resource between queue families is marked structural: those are
   * required by the API whatever the frame's data dependencies are, so no rule may question them.
   */
  syncPoint(cmd) {
    if (!BARRIER_METHODS2.has(cmd.method) || !cmd.args) return null;
    const a = cmd.args;
    const groups = isObject(a.pDependencyInfo) ? [a.pDependencyInfo] : [a];
    const resources = [];
    let structural = false;
    for (const g of groups) {
      for (const b of arrayOf(g.pImageMemoryBarriers)) {
        if (str(b.oldLayout) !== str(b.newLayout)) structural = true;
        if (queueTransfer(b)) structural = true;
        const range = isObject(b.subresourceRange) ? b.subresourceRange : null;
        const resource2 = this._imageResource(refId(b.image), num(range?.baseMipLevel), num(range?.baseArrayLayer));
        if (resource2) resources.push(resource2.key);
      }
      for (const b of arrayOf(g.pBufferMemoryBarriers)) {
        if (queueTransfer(b)) structural = true;
        const resource2 = this._bufferResource(refId(b.buffer));
        if (resource2) resources.push(resource2.key);
      }
    }
    return { commandIndex: cmd.index, method: cmd.method, resources, structural };
  }
  _streamBuffers(stream) {
    let m = this._buffers.get(stream);
    if (!m) this._buffers.set(stream, m = /* @__PURE__ */ new Map());
    return m;
  }
  // ------------------------------------------------------------------------------- resources
  _viewResource(viewId) {
    const view = this._db.getObject(viewId)?.descriptor ?? null;
    const range = isObject(view?.subresourceRange) ? view.subresourceRange : null;
    return this._imageResource(imageOfView(this._db, viewId), num(range?.baseMipLevel), num(range?.baseArrayLayer));
  }
  _imageResource(imageId, mip, layer) {
    if (imageId === null) return null;
    const object = this._db.getObject(imageId);
    if (!object) return null;
    const key = `image:${imageId}:m${mip}:l${layer}`;
    const cached = this._resources.get(key);
    if (cached) return cached;
    const swapchain = isSwapchainImage(object) ? this._db.getObject(object.parentId)?.descriptor ?? null : null;
    const d = object.descriptor;
    const extent = isObject(d?.extent) ? d.extent : isObject(swapchain?.imageExtent) ? swapchain.imageExtent : null;
    const mips = num(d?.mipLevels) || 1;
    const layers = num(d?.arrayLayers) || num(swapchain?.imageArrayLayers) || 1;
    const width = Math.max(1, num(extent?.width) >> mip);
    const height = Math.max(1, num(extent?.height) >> mip);
    const format = str(d?.format ?? swapchain?.imageFormat).replace("VK_FORMAT_", "");
    const sub = [mips > 1 ? `mip ${mip}` : "", layers > 1 ? `layer ${layer}` : ""].filter(Boolean).join(" ");
    const resource2 = {
      key,
      objectId: imageId,
      type: "image",
      label: sub ? `${object.name} ${sub}` : object.name,
      detail: [width && height ? `${width}x${height}` : "", format].filter(Boolean).join("  "),
      bytes: width * height * bytesPerPixel2(format),
      presented: isSwapchainImage(object)
    };
    this._resources.set(key, resource2);
    return resource2;
  }
  _bufferResource(bufferId) {
    if (bufferId === null) return null;
    const object = this._db.getObject(bufferId);
    if (!object) return null;
    const key = `buffer:${bufferId}`;
    const cached = this._resources.get(key);
    if (cached) return cached;
    const size2 = num(object.descriptor?.size);
    const resource2 = {
      key,
      objectId: bufferId,
      type: "buffer",
      label: object.name,
      detail: size2 ? formatBytes3(size2) : "",
      bytes: size2,
      presented: false
    };
    this._resources.set(key, resource2);
    return resource2;
  }
  _passLabel(cmd, ordinal) {
    const a = cmd.args;
    const begin = a && isObject(a.pRenderPassBegin) ? a.pRenderPassBegin : null;
    if (begin) {
      const rp = this._db.getObject(refId(begin.renderPass));
      const fb = this._db.getObject(refId(begin.framebuffer));
      return `Pass ${ordinal}: ${rp?.label || rp?.name || "?"}${fb?.label ? ` (${fb.label})` : ""}`;
    }
    const info = a && isObject(a.pRenderingInfo) ? a.pRenderingInfo : null;
    const colors = info && Array.isArray(info.pColorAttachments) ? info.pColorAttachments.length : 0;
    return `Pass ${ordinal}: rendering, ${colors} color attachment${colors === 1 ? "" : "s"}`;
  }
};
function descriptorUsage(type) {
  if (type.includes("STORAGE_IMAGE")) return { name: "storage image", write: true };
  if (type.includes("STORAGE_BUFFER")) return { name: "storage buffer", write: true };
  if (type.includes("STORAGE_TEXEL_BUFFER")) return { name: "storage texel buffer", write: true };
  if (type.includes("SAMPLED_IMAGE") || type.includes("COMBINED_IMAGE_SAMPLER")) return { name: "sampled", write: false };
  if (type.includes("INPUT_ATTACHMENT")) return { name: "input attachment", write: false };
  if (type.includes("UNIFORM_BUFFER") || type.includes("UNIFORM_TEXEL_BUFFER")) return { name: "uniform buffer", write: false };
  return null;
}
function copySubresource(a, mode) {
  const regions = firstArray(a, ["pRegions", "pImageBlits", "pBlits", "pImageCopies", "pBufferImageCopies", "pImageResolves"]);
  const region = regions && isObject(regions[0]) ? regions[0] : null;
  if (region) {
    const sub = region[mode === "read" ? "srcSubresource" : "dstSubresource"] ?? region.imageSubresource;
    if (isObject(sub)) return { mip: num(sub.mipLevel), layer: num(sub.baseArrayLayer) };
  }
  const ranges = firstArray(a, ["pRanges"]);
  const range = ranges && isObject(ranges[0]) ? ranges[0] : null;
  if (range) return { mip: num(range.baseMipLevel), layer: num(range.baseArrayLayer) };
  return { mip: 0, layer: 0 };
}
function arrayOf(v) {
  return Array.isArray(v) ? v.filter(isObject) : [];
}
function queueTransfer(b) {
  const src = str(b.srcQueueFamilyIndex);
  const dst = str(b.dstQueueFamilyIndex);
  return src !== dst && src !== "" && dst !== "";
}
function firstArray(a, keys) {
  for (const key of keys) {
    const v = a[key];
    if (Array.isArray(v) && v.length) return v;
  }
  return null;
}
function isSwapchainImage(object) {
  return object.cmd === "vkGetSwapchainImagesKHR";
}
function bytesPerPixel2(format) {
  const bits = [...format.matchAll(/[RGBADSEX](\d+)/g)].reduce((sum, m) => sum + Number(m[1]), 0);
  if (bits) return bits / 8;
  if (format.includes("BC") || format.includes("ETC") || format.includes("ASTC")) return 1;
  return 4;
}
function formatBytes3(bytes) {
  if (bytes >= 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${bytes} B`;
}

// src/renderer/frame_graph.ts
var AccessSet = class {
  _byKey = /* @__PURE__ */ new Map();
  _usages = /* @__PURE__ */ new Map();
  add(access) {
    const key = access.resource.key;
    const existing = this._byKey.get(key);
    let usages = this._usages.get(key);
    if (!usages) this._usages.set(key, usages = /* @__PURE__ */ new Set());
    usages.add(access.usage);
    if (!existing) {
      this._byKey.set(key, { ...access });
      return;
    }
    if (existing.mode !== access.mode) existing.mode = "readwrite";
    if (!access.discards) existing.discards = false;
    if (!access.dropped) existing.dropped = false;
  }
  list() {
    const out = [];
    for (const [key, access] of this._byKey) {
      const usages = [...this._usages.get(key) ?? []].sort();
      out.push({ ...access, usage: usages.join(", ") });
    }
    return out;
  }
};
function collectPasses(data, sets, source) {
  const passes = [];
  const syncPoints = [];
  const renderCounters = /* @__PURE__ */ new Map();
  const computeCounters = /* @__PURE__ */ new Map();
  const state = { open: null };
  let inPass = false;
  let stream = "";
  let currentCb = -1;
  const finish2 = () => {
    if (!state.open) return;
    state.open.pass.accesses = state.open.accesses.list();
    state.open = null;
  };
  const begin = (pass, accesses) => {
    passes.push(pass);
    state.open = { pass, accesses };
  };
  let currentFrame = -1;
  for (const cmd of data.commands) {
    const objId = cmd.object?.__id ?? 0;
    if (cmd.frame !== currentFrame) {
      currentFrame = cmd.frame;
      renderCounters.clear();
      computeCounters.clear();
    }
    if (sets.SUBMIT.has(cmd.method)) {
      finish2();
      inPass = false;
      currentCb = -1;
      continue;
    }
    if (objId !== currentCb) {
      finish2();
      inPass = false;
      currentCb = objId;
    }
    stream = `${objId}:${cmd.secondary ?? 0}`;
    source.observe(cmd, stream);
    const sync = source.syncPoint?.(cmd);
    if (sync) syncPoints.push({ ...sync, after: passes.length - 1 });
    if (sets.PASS_BEGIN.has(cmd.method)) {
      finish2();
      const ordinal = renderCounters.get(objId) ?? 0;
      renderCounters.set(objId, ordinal + 1);
      const decoded = source.passAccesses(cmd, ordinal);
      inPass = true;
      const accesses = new AccessSet();
      for (const a of decoded?.accesses ?? []) accesses.add(a);
      begin({
        kind: decoded?.kind ?? "render",
        label: decoded?.label ?? `Pass ${ordinal}`,
        commandIndex: cmd.index,
        passKey: passKey(cmd.frame, objId, ordinal),
        frame: cmd.frame,
        draws: 0,
        accesses: [],
        unresolvedReads: 0
      }, accesses);
      continue;
    }
    if (sets.PASS_END.has(cmd.method)) {
      finish2();
      inPass = false;
      continue;
    }
    if (!inPass && state.open?.pass.kind === "compute" && (sets.COMPUTE_PASS_END.has(cmd.method) || cmd.method === "vkEndCommandBuffer" || sets.LABEL_BEGIN.has(cmd.method) || sets.LABEL_END.has(cmd.method))) {
      finish2();
    }
    if (sets.DISPATCH.has(cmd.method) && !inPass && !state.open) {
      const cbKey = cmd.secondary || objId;
      const ordinal = computeCounters.get(cbKey) ?? 0;
      computeCounters.set(cbKey, ordinal + 1);
      begin({
        kind: "compute",
        label: source.computePassLabel(ordinal),
        commandIndex: cmd.index,
        passKey: passKey(cmd.frame, cbKey, ordinal, true),
        frame: cmd.frame,
        draws: 0,
        accesses: [],
        unresolvedReads: 0
      }, new AccessSet());
    }
    if (isAction(sets, cmd.method)) {
      const open = state.open;
      if (!open) continue;
      open.pass.draws++;
      const { accesses, unresolved } = source.actionAccesses(cmd, stream);
      for (const a of accesses) open.accesses.add(a);
      open.pass.unresolvedReads = (open.pass.unresolvedReads ?? 0) + unresolved;
      continue;
    }
    const transfer = source.transferAccesses(cmd);
    if (transfer && transfer.accesses.length) {
      if (state.open && inPass) {
        for (const a of transfer.accesses) state.open.accesses.add(a);
      } else {
        passes.push({
          kind: "transfer",
          label: transfer.label,
          commandIndex: cmd.index,
          passKey: null,
          frame: cmd.frame,
          draws: 0,
          accesses: transfer.accesses,
          unresolvedReads: 0
        });
      }
    }
  }
  finish2();
  const kept = passes.filter((p) => p.accesses.length > 0 || p.draws > 0);
  if (kept.length !== passes.length) {
    const ordinals = /* @__PURE__ */ new Map();
    kept.forEach((p, i) => ordinals.set(p, i));
    for (const sync of syncPoints) {
      let after = -1;
      for (let i = sync.after; i >= 0; i--) {
        const ordinal = ordinals.get(passes[i]);
        if (ordinal !== void 0) {
          after = ordinal;
          break;
        }
      }
      sync.after = after;
    }
  }
  return { passes: kept, syncPoints };
}
function buildFrameGraph(data, sets, source) {
  const { passes, syncPoints } = collectPasses(data, sets, source);
  return buildRenderGraph(passes, {
    api: data.api,
    durationOf: (key) => data.passTimings.get(key)?.durationMs ?? null,
    syncPoints
  });
}
function frameRenderGraph(data, db) {
  const source = data.api === "metal" ? new MetalResourceSource(db) : new VulkanResourceSource(db);
  return buildFrameGraph(data, data.sets, source);
}

// src/renderer/draw_stats.ts
var NO_PASS = 4294967295;
function parseDrawStats(input) {
  const text = typeof input === "string" ? input : new TextDecoder().decode(input);
  let json;
  try {
    json = JSON.parse(text);
  } catch (e) {
    throw new Error(`The draw measurements are not valid JSON: ${e.message}`);
  }
  if (json.format !== "gpu-inspector-draw-stats") throw new Error("Not draw measurements from vkinsp_replay.");
  const num2 = (v) => typeof v === "number" ? v : 0;
  const draws = (Array.isArray(json.draws) ? json.draws : []).map((raw) => {
    const d = raw;
    const pass = num2(d.passIndex);
    return {
      command: num2(d.command),
      frame: num2(d.frame),
      commandBuffer: num2(d.commandBuffer),
      ...pass === NO_PASS ? {} : { passIndex: pass },
      timed: d.timed === true,
      ms: num2(d.ms),
      counted: d.counted === true,
      vertexInvocations: num2(d.vertexInvocations),
      primitives: num2(d.primitives),
      fragmentInvocations: num2(d.fragmentInvocations),
      computeInvocations: num2(d.computeInvocations),
      sampled: d.sampled === true,
      samplesPassed: num2(d.samplesPassed)
    };
  });
  return {
    device: typeof json.device === "string" ? json.device : "",
    note: typeof json.note === "string" ? json.note : "",
    draws,
    problems: Array.isArray(json.problems) ? json.problems.filter((p) => typeof p === "string") : []
  };
}
function drawStatsByCommand(draws) {
  const out = /* @__PURE__ */ new Map();
  for (const d of draws) out.set(d.command, d);
  return out;
}
function passSumKey(frame, commandBuffer, passIndex) {
  return `${frame}:${commandBuffer}:${passIndex}`;
}
function drawSumsByPass(draws) {
  const out = /* @__PURE__ */ new Map();
  for (const d of draws) {
    if (d.passIndex === void 0) continue;
    const key = passSumKey(d.frame, d.commandBuffer, d.passIndex);
    let s = out.get(key);
    if (!s) out.set(key, s = { draws: 0, counted: true, fragmentInvocations: 0, sampled: true, samplesPassed: 0 });
    s.draws++;
    s.counted = s.counted && d.counted;
    s.fragmentInvocations += d.fragmentInvocations;
    s.sampled = s.sampled && d.sampled;
    s.samplesPassed += d.samplesPassed;
  }
  return out;
}
function drawStatsSummary(file) {
  const timed = file.draws.filter((d) => d.timed).length;
  const counted = file.draws.filter((d) => d.counted).length;
  const fragments = file.draws.reduce((sum, d) => sum + d.fragmentInvocations, 0);
  return `${file.draws.length} draws and dispatches measured (${timed} timed, ${counted} counted), ${fragments.toLocaleString()} fragment shader invocations${file.device ? `, replayed on ${file.device}` : ""}`;
}

// src/renderer/pass_metrics.ts
var HEALTHY_OVERDRAW = 1.2;
var OVERDRAW_LIMIT = 2;
var MICROTRIANGLE_LIMIT = 4;
var LOW_REJECTION_RATE = 0.25;
function counter(t, name) {
  const v = t?.counters?.[name];
  return typeof v === "number" ? v : null;
}
function ratio(top, bottom) {
  if (top === null || bottom === null || bottom <= 0) return null;
  return top / bottom;
}
function drawVertices(a) {
  const instances = Math.max(1, num(a.instanceCount));
  const vertices = num(a.indexCount) || num(a.vertexCount);
  return vertices * instances;
}
function collectPassMetrics(data, db) {
  const sets = data.sets;
  const passes = [];
  const passIndexOf = /* @__PURE__ */ new Map();
  const computeIndexOf = /* @__PURE__ */ new Map();
  let open = null;
  let computeRun = null;
  let inPass = false;
  let currentCb = -1;
  let currentSecondary = 0;
  const closeComputeRun = () => {
    computeRun = null;
  };
  for (const cmd of data.commands) {
    const m = cmd.method;
    const cb = cmd.object?.__id ?? 0;
    const a = cmd.args;
    if (cb !== currentCb || (cmd.secondary ?? 0) !== currentSecondary) {
      closeComputeRun();
      currentCb = cb;
      currentSecondary = cmd.secondary ?? 0;
    }
    if (sets.PASS_BEGIN.has(m)) {
      closeComputeRun();
      inPass = true;
      const index = passIndexOf.get(cb) ?? 0;
      passIndexOf.set(cb, index + 1);
      open = blank(cmd, index, sets.passIsCompute?.(m) ?? false, cb, targetOf(cmd, db));
      passes.push(open);
      continue;
    }
    if (open) open.endIndex = cmd.index;
    if (sets.PASS_END.has(m)) {
      inPass = false;
      open = null;
      continue;
    }
    if (sets.SUBMIT.has(m) || m === "vkEndCommandBuffer" || sets.COMPUTE_PASS_END.has(m) || sets.LABEL_BEGIN.has(m) || sets.LABEL_END.has(m)) {
      closeComputeRun();
    }
    if (!a) continue;
    if (sets.DRAW.has(m)) {
      if (open) {
        open.draws++;
        open.vertices += drawVertices(a);
      }
      continue;
    }
    if (sets.DISPATCH.has(m)) {
      if (open) {
        open.draws++;
      } else if (!inPass) {
        if (!computeRun) {
          const key = cmd.secondary || cb;
          const index = computeIndexOf.get(key) ?? 0;
          computeIndexOf.set(key, index + 1);
          computeRun = blank(cmd, index, true, key, null);
          passes.push(computeRun);
        }
        computeRun.draws++;
        computeRun.endIndex = cmd.index;
      }
    }
  }
  let gpuMs = 0;
  let vertexMs = 0;
  let fragmentMs = 0;
  let withCounters = 0;
  let timed = 0;
  const totals = { vertexInvocations: 0, fragmentInvocations: 0, primitives: 0, fragmentsPassed: 0 };
  let anyCounters = false;
  for (const p of passes) {
    const t = data.passTiming(p.frame, p.commandBuffer, p.passIndex, p.compute);
    p.timing = t;
    if (!t) continue;
    timed++;
    p.durationMs = t.durationMs;
    p.vertexMs = t.vertexMs ?? null;
    p.fragmentMs = t.fragmentMs ?? null;
    gpuMs += t.durationMs;
    vertexMs += t.vertexMs ?? 0;
    fragmentMs += t.fragmentMs ?? 0;
    const fragments = counter(t, "fragmentInvocations");
    const primitives = counter(t, "clipperPrimitivesOut");
    const vertexInvocations = counter(t, "vertexInvocations");
    const passed = counter(t, "fragmentsPassed");
    if (fragments !== null || vertexInvocations !== null) {
      withCounters++;
      anyCounters = true;
      totals.fragmentInvocations += fragments ?? 0;
      totals.vertexInvocations += vertexInvocations ?? 0;
      totals.primitives += primitives ?? 0;
      totals.fragmentsPassed += passed ?? 0;
    }
    p.overdraw = p.pixels > 0 ? ratio(fragments, p.pixels) : null;
    if (p.overdraw !== null) p.overdrawSource = "counters";
    p.fragmentsPerPrimitive = ratio(fragments, primitives);
    p.depthRejectRate = fragments !== null && passed !== null && fragments > 0 ? 1 - passed / fragments : null;
    if (p.depthRejectRate !== null) p.depthRejectSource = "counters";
    p.nsPerVertex = p.vertexMs !== null ? ratio(p.vertexMs * 1e6, vertexInvocations) : null;
    p.nsPerFragment = p.fragmentMs !== null ? ratio(p.fragmentMs * 1e6, fragments) : null;
    const u = t.utilization;
    const totalCycles = u?.totalCycles ?? 0;
    if (u && totalCycles > 0) {
      p.cycleShare = {
        vertex: (u.vertexCycles ?? 0) / totalCycles,
        fragment: (u.fragmentCycles ?? 0) / totalCycles,
        target: (u.renderTargetCycles ?? 0) / totalCycles
      };
    }
    decideBound(p);
  }
  if (data.drawStats?.length) {
    const sums = drawSumsByPass(data.drawStats);
    for (const p of passes) {
      if (p.compute || p.depthRejectRate !== null) continue;
      const s = sums.get(passSumKey(p.frame, p.commandBuffer, p.passIndex));
      if (!s?.sampled) continue;
      const fragments = counter(p.timing, "fragmentInvocations") ?? (s.counted ? s.fragmentInvocations : null);
      if (fragments === null || fragments <= 0 || s.samplesPassed > fragments) continue;
      p.depthRejectRate = 1 - s.samplesPassed / fragments;
      p.depthRejectSource = "replay";
    }
  }
  if (data.overdraw?.length) {
    for (const p of passes) {
      if (p.compute) continue;
      const measured = data.overdrawForPass(p.frame, p.commandBuffer, p.passIndex).filter((o) => o.info.measured !== false);
      if (!measured.length) continue;
      const depthTested = measured.find((o) => o.info.depthTested)?.info ?? null;
      const rasterized = measured.find((o) => !o.info.depthTested)?.info ?? null;
      p.measuredOverdraw = { depthTested, rasterized };
      const pixels = depthTested ? depthTested.width * depthTested.height : 0;
      if (p.overdraw === null && depthTested && pixels > 0) {
        p.overdraw = depthTested.fragments / pixels;
        p.overdrawSource = "measured";
      }
    }
  }
  return {
    passes,
    gpuMs,
    vertexMs,
    fragmentMs,
    withCounters,
    timed,
    usable: timed > 0,
    totals: anyCounters ? totals : null
  };
}
function targetOf(cmd, db) {
  const a = cmd.args;
  if (!a) return null;
  for (const key of ["pRenderPassBegin", "pRenderingInfo"]) {
    const info = a[key];
    if (!isObject(info) || !isObject(info.renderArea)) continue;
    const extent = info.renderArea.extent;
    if (!isObject(extent)) continue;
    const pixels = num(extent.width) * num(extent.height);
    if (pixels > 0) return { pixels, samples: 1 };
  }
  const attachment = (att) => {
    if (!isObject(att)) return null;
    const id = refId(att.texture);
    if (id === null) return null;
    const d = db.getObject(id)?.descriptor;
    if (!d) return null;
    const pixels = num(d.width) * num(d.height);
    return pixels > 0 ? { pixels, samples: Math.max(1, num(d.sampleCount)) } : null;
  };
  if (Array.isArray(a.colorAttachments)) {
    for (const c2 of a.colorAttachments) {
      const hit = attachment(c2);
      if (hit) return hit;
    }
  }
  return attachment(a.depthAttachment);
}
function blank(cmd, passIndex, compute, cb, target) {
  return {
    commandIndex: cmd.index,
    endIndex: cmd.index,
    label: passLabel(cmd, passIndex),
    frame: cmd.frame ?? 0,
    commandBuffer: cb,
    passIndex,
    compute,
    draws: 0,
    vertices: 0,
    pixels: target?.pixels ?? 0,
    samples: target?.samples ?? 1,
    timing: null,
    durationMs: null,
    vertexMs: null,
    fragmentMs: null,
    overdraw: null,
    overdrawSource: null,
    measuredOverdraw: null,
    fragmentsPerPrimitive: null,
    depthRejectRate: null,
    depthRejectSource: null,
    nsPerVertex: null,
    nsPerFragment: null,
    cycleShare: null,
    bound: null,
    boundReason: ""
  };
}
function passLabel(cmd, passIndex) {
  const a = cmd.args;
  const descriptor = a && isObject(a.descriptor) ? a.descriptor : null;
  const label = str(a?.label) || str(descriptor?.label);
  const m = cmd.method;
  const kind = m.startsWith("computeCommandEncoder") ? "Compute" : m.startsWith("blitCommandEncoder") ? "Blit" : m.startsWith("renderCommandEncoder") || m.startsWith("parallelRenderCommandEncoder") ? "Render Pass" : m.startsWith("vkCmdBeginRender") ? "Render Pass" : m.startsWith("vkCmdDispatch") ? "Compute" : "Pass";
  return label ? `${kind} ${passIndex}: ${label}` : `${kind} ${passIndex}`;
}
function decideBound(p) {
  const share = p.cycleShare;
  if (share && share.target > 0.4 && share.target > share.fragment) {
    p.bound = "target";
    p.boundReason = `${(100 * share.target).toFixed(0)}% of the pass's GPU cycles went to writing the render target`;
    return;
  }
  const v = p.vertexMs;
  const f = p.fragmentMs;
  if (v !== null && f !== null && (v > 0 || f > 0)) {
    if (f > v * 1.3) {
      p.bound = "fragment";
      p.boundReason = `the fragment stage ran ${f.toFixed(3)} ms against the vertex stage's ${v.toFixed(3)} ms`;
    } else if (v > f * 1.3) {
      p.bound = "vertex";
      p.boundReason = `the vertex stage ran ${v.toFixed(3)} ms against the fragment stage's ${f.toFixed(3)} ms`;
    } else {
      p.bound = "balanced";
      p.boundReason = `the vertex and fragment stages ran for about as long (${v.toFixed(3)} and ${f.toFixed(3)} ms)`;
    }
    return;
  }
  if (share) {
    if (share.fragment > share.vertex * 1.3) {
      p.bound = "fragment";
      p.boundReason = `${(100 * share.fragment).toFixed(0)}% of the pass's GPU cycles were fragment work`;
    } else if (share.vertex > share.fragment * 1.3) {
      p.bound = "vertex";
      p.boundReason = `${(100 * share.vertex).toFixed(0)}% of the pass's GPU cycles were vertex work`;
    }
  }
}
var BOUND_LABEL = {
  vertex: "Vertex bound",
  fragment: "Fragment bound",
  target: "Target write bound",
  balanced: "Balanced"
};
var BOUND_ADVICE = {
  vertex: "Cut vertices or vertex-stage work: mesh level of detail at distance, fewer or cheaper vertex attributes, and per-fragment rather than per-vertex evaluation of anything the fragment stage could do itself.",
  fragment: "Cut fragments or fragment-stage work: fewer overlapping surfaces, a smaller render target, cheaper texture sampling, and simpler shader maths.",
  target: "The pass spends its time writing the attachment rather than shading it. A smaller target, fewer targets, or a store action of DontCare on anything nothing reads afterwards.",
  balanced: "Neither stage dominates. The cheapest win is usually to remove work from the pass entirely: merge it with a neighbour, or skip it when nothing reads its output."
};
function passAdvice(p) {
  const out = [];
  if (p.overdraw !== null && p.overdraw > OVERDRAW_LIMIT) {
    out.push({
      severity: "high",
      title: `Each pixel is shaded ${formatRatio(p.overdraw)} times`,
      body: `A frame doing well sits near ${HEALTHY_OVERDRAW}. Overdraw this high is usually transparent surfaces stacking up, a full-screen effect drawn more than once, or opaque geometry drawn back to front so the depth test cannot reject anything.`
    });
  }
  if (p.fragmentsPerPrimitive !== null && p.fragmentsPerPrimitive < MICROTRIANGLE_LIMIT) {
    out.push({
      severity: "high",
      title: `Triangles cover ${formatRatio(p.fragmentsPerPrimitive)} fragments each`,
      body: `The rasterizer shades in 2x2 quads, so a triangle covering fewer than ${MICROTRIANGLE_LIMIT} fragments wastes lanes it has already paid for. This is dense geometry drawn small: add mesh level of detail, or cull the meshes that are far enough away to be smaller than their own triangles.`
    });
  }
  if (p.depthRejectRate !== null && p.overdraw !== null && p.overdraw > 1.5 && p.depthRejectRate < LOW_REJECTION_RATE) {
    out.push({
      severity: "medium",
      title: `The depth test rejects only ${formatPercent(p.depthRejectRate)} of shaded fragments`,
      body: "Fragments are being shaded and then thrown away by something later, or not thrown away at all. Drawing opaque geometry front to back lets the depth test reject work before the fragment shader runs; a depth prepass does the same for a scene that cannot be sorted."
    });
  }
  if (p.bound === "target" && p.cycleShare) {
    out.push({
      severity: "medium",
      title: "Most of the pass is spent writing the render target",
      body: "Fewer or smaller attachments, or a store action of DontCare on the ones nothing reads afterwards. On a tile-based GPU a target that is only read by the pass that follows never has to reach memory at all."
    });
  }
  return out;
}
function frameStageVerdict(m) {
  const staged = m.vertexMs + m.fragmentMs;
  if (staged <= 0) return "The stage split is not available for this capture, so the frame's balance cannot be stated.";
  const fragmentShare = m.fragmentMs / staged;
  if (fragmentShare > 0.65) return `This frame is fragment bound: ${formatPercent(fragmentShare)} of stage time is fragment work.`;
  if (fragmentShare < 0.35) return `This frame is vertex bound: ${formatPercent(1 - fragmentShare)} of stage time is vertex work.`;
  return `Vertex and fragment work are close to balanced (${formatPercent(fragmentShare)} fragment).`;
}
function formatRatio(v, digits = 2) {
  return v === null ? "\u2014" : v.toFixed(digits);
}
function formatPercent(v) {
  return v === null ? "\u2014" : `${(100 * v).toFixed(0)}%`;
}

// src/renderer/vulkan/spirv_debug.ts
var SOURCE_LANGUAGES = {
  0: "Unknown",
  1: "ESSL",
  2: "GLSL",
  3: "OpenCL C",
  4: "OpenCL C++",
  5: "HLSL",
  6: "C++ for OpenCL",
  7: "SYCL",
  8: "HERO C",
  9: "NZSL",
  10: "WGSL",
  11: "Slang",
  12: "Zig"
};
var GENERATORS = {
  0: "Khronos",
  1: "LunarG",
  2: "Valve",
  3: "Codeplay",
  4: "NVIDIA",
  5: "ARM",
  6: "Khronos LLVM/SPIR-V Translator",
  7: "Khronos SPIR-V Tools Assembler",
  8: "Khronos Glslang",
  9: "Qualcomm",
  10: "AMD",
  11: "Intel",
  12: "Imagination",
  13: "Google Shaderc over Glslang",
  14: "Google spiregg (DXC)",
  15: "Google rspirv",
  16: "X-LEGEND Mesa-IR/SPIR-V Translator",
  17: "Khronos SPIR-V Tools Linker",
  18: "Wine VKD3D",
  19: "Clay Shader Compiler",
  20: "W3C WHLSL Translator",
  21: "Google Clspv",
  22: "MLIR SPIR-V Serializer",
  23: "Google Tint",
  24: "Google ANGLE",
  25: "Netease Messiah",
  26: "Xenia",
  27: "Embark Rust GPU",
  28: "gfx-rs Naga",
  29: "Mikkosoft MSP",
  30: "SpvGenTwo",
  31: "Skia SkSL",
  32: "TornadoVM",
  33: "DragonJoker ShaderWriter",
  34: "Khronos SPIR-V Tools Optimizer",
  35: "Rayan Hatoum",
  36: "Khronos SPIR-V Tools Diff",
  37: "Nintendo",
  38: "Khronos Slang",
  39: "Zig",
  40: "Rendong Liang",
  41: "Mesa Rusticl",
  42: "Adobe",
  43: "Netease",
  44: "NVIDIA nvidia-spirv",
  45: "Roblox Studio"
};
function readString(words2, start, end) {
  const bytes = [];
  for (let i = start; i < end; i++) {
    const w = words2[i];
    for (let b = 0; b < 4; b++) {
      const c2 = w >>> b * 8 & 255;
      if (c2 === 0) return new TextDecoder().decode(new Uint8Array(bytes));
      bytes.push(c2);
    }
  }
  return new TextDecoder().decode(new Uint8Array(bytes));
}
function parseSpirvDebugInfo(data) {
  if (data.byteLength < 20 || data.byteLength % 4) return null;
  const bytes = data.byteOffset % 4 ? data.slice() : data;
  const words2 = new Uint32Array(bytes.buffer, bytes.byteOffset, bytes.byteLength / 4);
  if (words2[0] !== 119734787) return null;
  const genWord = words2[2];
  const genId = genWord >>> 16;
  const generator = `${GENERATORS[genId] ?? `generator ${genId}`} ${genWord & 65535}`;
  const strings = /* @__PURE__ */ new Map();
  const constants = /* @__PURE__ */ new Map();
  const files = [];
  const fileByName = /* @__PURE__ */ new Map();
  const fileByStringId = /* @__PURE__ */ new Map();
  const fileByDebugSourceId = /* @__PURE__ */ new Map();
  const locations = [];
  const processed = [];
  let language = "Unknown";
  let languageVersion = 0;
  let mainFile = -1;
  let form = "none";
  let debugSet = 0;
  let lastOpSourceFile = -1;
  let lastDebugSourceFile = -1;
  let current = null;
  const fileIndex = (name) => {
    let idx = fileByName.get(name);
    if (idx === void 0) {
      idx = files.length;
      files.push({ name, text: null });
      fileByName.set(name, idx);
    }
    return idx;
  };
  const append = (idx, text) => {
    if (idx < 0) return;
    files[idx].text = (files[idx].text ?? "") + text;
  };
  let i = 5;
  while (i < words2.length) {
    const w = words2[i];
    const op = w & 65535;
    const len = w >>> 16;
    if (len === 0) break;
    const a = i + 1;
    const end = Math.min(words2.length, i + len);
    switch (op) {
      case 7 /* String */:
        strings.set(words2[a], readString(words2, a + 1, end));
        break;
      case 43 /* Constant */:
        constants.set(words2[a + 1], words2[a + 2]);
        break;
      case 330 /* ModuleProcessed */:
        processed.push(readString(words2, a, end));
        break;
      case 3 /* Source */: {
        language = SOURCE_LANGUAGES[words2[a]] ?? `language ${words2[a]}`;
        languageVersion = words2[a + 1];
        lastOpSourceFile = -1;
        if (end > a + 2) {
          const name = strings.get(words2[a + 2]) ?? `source ${files.length + 1}`;
          lastOpSourceFile = fileIndex(name);
          fileByStringId.set(words2[a + 2], lastOpSourceFile);
          if (end > a + 3) {
            append(lastOpSourceFile, readString(words2, a + 3, end));
            if (mainFile < 0) mainFile = lastOpSourceFile;
          }
        }
        break;
      }
      case 2 /* SourceContinued */:
        append(lastOpSourceFile, readString(words2, a, end));
        break;
      case 8 /* Line */: {
        if (form === "none") form = "OpLine";
        let file = fileByStringId.get(words2[a]);
        if (file === void 0) {
          file = fileIndex(strings.get(words2[a]) ?? `file %${words2[a]}`);
          fileByStringId.set(words2[a], file);
        }
        current = { file, line: words2[a + 1], column: words2[a + 2] };
        break;
      }
      case 317 /* NoLine */:
        current = null;
        break;
      case 11 /* ExtInstImport */:
        if (readString(words2, a + 1, end) === "NonSemantic.Shader.DebugInfo.100") debugSet = words2[a];
        break;
      case 12 /* ExtInst */: {
        if (!debugSet || words2[a + 2] !== debugSet) break;
        const inst = words2[a + 3];
        const o = a + 4;
        if (inst === 35 /* Source */) {
          const name = strings.get(words2[o]) ?? `source ${files.length + 1}`;
          lastDebugSourceFile = fileIndex(name);
          fileByDebugSourceId.set(words2[a + 1], lastDebugSourceFile);
          if (end > o + 1) {
            const text = strings.get(words2[o + 1]);
            if (text !== void 0) append(lastDebugSourceFile, text);
          }
        } else if (inst === 102 /* SourceContinued */) {
          const text = strings.get(words2[o]);
          if (text !== void 0) append(lastDebugSourceFile, text);
        } else if (inst === 1 /* CompilationUnit */) {
          const src = fileByDebugSourceId.get(words2[o + 2]);
          if (src !== void 0 && mainFile < 0) mainFile = src;
          const lang = constants.get(words2[o + 3]);
          if (lang !== void 0 && language === "Unknown") language = SOURCE_LANGUAGES[lang] ?? language;
        } else if (inst === 103 /* Line */) {
          form = "NonSemantic.Shader.DebugInfo.100";
          const file = fileByDebugSourceId.get(words2[o]);
          const line = constants.get(words2[o + 1]);
          if (file !== void 0 && line !== void 0) current = { file, line, column: constants.get(words2[o + 3]) ?? 0 };
        } else if (inst === 104 /* NoLine */) {
          current = null;
        }
        break;
      }
      case 54 /* Function */:
        current = null;
        break;
      default:
        break;
    }
    locations.push(current);
    i += len;
  }
  if (mainFile < 0 && files.length) mainFile = files.findIndex((f) => f.text !== null);
  return { language, languageVersion, generator, files, mainFile, form, locations, processed };
}
function sourceLineMap(text) {
  const lines = text.split("\n");
  if (lines.length > 1 && lines[lines.length - 1] === "") lines.pop();
  const lineOf = new Array(lines.length).fill(0);
  const physicalOf = /* @__PURE__ */ new Map();
  let logical = 1;
  for (let i = 0; i < lines.length; i++) {
    const m = /^\s*#\s*line\s+(\d+)/.exec(lines[i]);
    if (m) {
      logical = Number(m[1]);
      continue;
    }
    lineOf[i] = logical;
    physicalOf.set(logical, i);
    logical++;
  }
  for (let i = 0; i < lines.length; i++) if (lineOf[i] && physicalOf.get(lineOf[i]) !== i) lineOf[i] = 0;
  return { lines, lineOf, physicalOf };
}
function hasEmbeddedSource(info) {
  return !!info && info.files.some((f) => f.text !== null);
}
function describeDebugInfo(info) {
  if (!info) return "";
  const parts2 = [];
  const withText = info.files.filter((f) => f.text !== null);
  if (withText.length && withText.every((f) => f.fromHost)) {
    parts2.push(`Source from this machine (source roots): ${withText.map((f) => f.name).join(", ")}`);
  } else if (withText.length) {
    parts2.push(`Embedded source: ${withText.map((f) => f.name).join(", ")}`);
  } else if (info.files.length) {
    parts2.push(`Source file name only: ${info.files.map((f) => f.name).join(", ")}`);
  } else {
    parts2.push("No embedded source");
  }
  const lang = info.language !== "Unknown" ? `${info.language}${info.languageVersion ? ` ${info.languageVersion}` : ""}` : "";
  const lines = info.form === "none" ? "no line mapping" : `line mapping via ${info.form}`;
  parts2.push([lang, info.generator, lines].filter(Boolean).join(", "));
  if (info.processed.length) parts2.push(`processed by ${info.processed.join("; ")}`);
  return parts2.join(" | ");
}

// src/renderer/vulkan/spirv_analysis.ts
var COST_DIMENSIONS = ["alu", "sfu", "texture", "memory"];
var COST_WEIGHTS = { alu: 1, sfu: 4, texture: 20, memory: 8 };
var LOOP_TRIPS = 8;
function emptyCost() {
  return { alu: 0, sfu: 0, texture: 0, memory: 0 };
}
function addCost(dst, src, scale = 1) {
  dst.alu += src.alu * scale;
  dst.sfu += src.sfu * scale;
  dst.texture += src.texture * scale;
  dst.memory += src.memory * scale;
}
function weighCost(c2, w = COST_WEIGHTS) {
  return c2.alu * w.alu + c2.sfu * w.sfu + c2.texture * w.texture + c2.memory * w.memory;
}
function dominantDimension(c2, w = COST_WEIGHTS) {
  let best = "alu";
  let bestValue = -1;
  for (const d of COST_DIMENSIONS) {
    const v = c2[d] * w[d];
    if (v > bestValue) {
      bestValue = v;
      best = d;
    }
  }
  return best;
}
var SEVERITY_RANK = { high: 3, medium: 2, low: 1, info: 0 };
var STAGES = {
  0: "vertex",
  1: "tess_control",
  2: "tess_eval",
  3: "geometry",
  4: "fragment",
  5: "compute",
  5267: "task",
  5268: "mesh",
  5313: "raygen",
  5314: "intersection",
  5315: "any_hit",
  5316: "closest_hit",
  5317: "miss",
  5318: "callable",
  5364: "task",
  5365: "mesh"
};
var c = (alu, sfu) => ({ alu, sfu, texture: 0, memory: 0 });
var GLSL_EXT = {
  1: { name: "round", cost: c(1, 0), tier: 0 },
  2: { name: "roundEven", cost: c(1, 0), tier: 0 },
  3: { name: "trunc", cost: c(1, 0), tier: 0 },
  4: { name: "abs", cost: c(1, 0), tier: 0 },
  5: { name: "abs", cost: c(1, 0), tier: 0 },
  6: { name: "sign", cost: c(1, 0), tier: 0 },
  7: { name: "sign", cost: c(1, 0), tier: 0 },
  8: { name: "floor", cost: c(1, 0), tier: 0 },
  9: { name: "ceil", cost: c(1, 0), tier: 0 },
  10: { name: "fract", cost: c(1, 0), tier: 0 },
  11: { name: "radians", cost: c(1, 0), tier: 0 },
  12: { name: "degrees", cost: c(1, 0), tier: 0 },
  13: { name: "sin", cost: c(1, 1), tier: 2 },
  14: { name: "cos", cost: c(1, 1), tier: 2 },
  15: { name: "tan", cost: c(1, 2), tier: 2 },
  16: { name: "asin", cost: c(2, 2), tier: 3 },
  17: { name: "acos", cost: c(2, 2), tier: 3 },
  18: { name: "atan", cost: c(2, 2), tier: 3 },
  19: { name: "sinh", cost: c(2, 2), tier: 3 },
  20: { name: "cosh", cost: c(2, 2), tier: 3 },
  21: { name: "tanh", cost: c(3, 2), tier: 3 },
  22: { name: "asinh", cost: c(3, 2), tier: 3 },
  23: { name: "acosh", cost: c(3, 2), tier: 3 },
  24: { name: "atanh", cost: c(3, 2), tier: 3 },
  25: { name: "atan2", cost: c(3, 2), tier: 3 },
  26: { name: "pow", cost: c(1, 2), tier: 3 },
  27: { name: "exp", cost: c(0, 1), tier: 3 },
  28: { name: "log", cost: c(1, 1), tier: 3 },
  29: { name: "exp2", cost: c(0, 1), tier: 3 },
  30: { name: "log2", cost: c(0, 1), tier: 3 },
  31: { name: "sqrt", cost: c(0, 1), tier: 2 },
  32: { name: "inversesqrt", cost: c(0, 1), tier: 2 },
  33: { name: "determinant", cost: c(9, 0), tier: 2 },
  34: { name: "inverse", cost: c(30, 1), tier: 3 },
  35: { name: "modf", cost: c(2, 0), tier: 0 },
  36: { name: "modf", cost: c(2, 0), tier: 0 },
  37: { name: "min", cost: c(1, 0), tier: 0 },
  38: { name: "min", cost: c(1, 0), tier: 0 },
  39: { name: "min", cost: c(1, 0), tier: 0 },
  40: { name: "max", cost: c(1, 0), tier: 0 },
  41: { name: "max", cost: c(1, 0), tier: 0 },
  42: { name: "max", cost: c(1, 0), tier: 0 },
  43: { name: "clamp", cost: c(2, 0), tier: 0 },
  44: { name: "clamp", cost: c(2, 0), tier: 0 },
  45: { name: "clamp", cost: c(2, 0), tier: 0 },
  46: { name: "mix", cost: c(2, 0), tier: 0 },
  47: { name: "mix", cost: c(2, 0), tier: 0 },
  48: { name: "step", cost: c(1, 0), tier: 0 },
  49: { name: "smoothstep", cost: c(5, 0), tier: 1 },
  50: { name: "fma", cost: c(1, 0), tier: 0 },
  51: { name: "frexp", cost: c(2, 1), tier: 2 },
  52: { name: "frexp", cost: c(2, 1), tier: 2 },
  53: { name: "ldexp", cost: c(0, 1), tier: 2 },
  54: { name: "packSnorm4x8", cost: c(3, 0), tier: 1 },
  55: { name: "packUnorm4x8", cost: c(3, 0), tier: 1 },
  56: { name: "packSnorm2x16", cost: c(2, 0), tier: 1 },
  57: { name: "packUnorm2x16", cost: c(2, 0), tier: 1 },
  58: { name: "packHalf2x16", cost: c(2, 0), tier: 1 },
  59: { name: "packDouble2x32", cost: c(1, 0), tier: 0 },
  60: { name: "unpackSnorm2x16", cost: c(2, 0), tier: 1 },
  61: { name: "unpackUnorm2x16", cost: c(2, 0), tier: 1 },
  62: { name: "unpackHalf2x16", cost: c(2, 0), tier: 1 },
  63: { name: "unpackSnorm4x8", cost: c(3, 0), tier: 1 },
  64: { name: "unpackUnorm4x8", cost: c(3, 0), tier: 1 },
  65: { name: "unpackDouble2x32", cost: c(1, 0), tier: 0 },
  66: { name: "length", cost: c(3, 1), tier: 2 },
  67: { name: "distance", cost: c(4, 1), tier: 2 },
  68: { name: "cross", cost: c(6, 0), tier: 1 },
  69: { name: "normalize", cost: c(4, 1), tier: 2 },
  70: { name: "faceforward", cost: c(5, 0), tier: 1 },
  71: { name: "reflect", cost: c(6, 0), tier: 1 },
  72: { name: "refract", cost: c(10, 1), tier: 2 },
  73: { name: "findLSB", cost: c(1, 0), tier: 0 },
  74: { name: "findMSB", cost: c(1, 0), tier: 0 },
  75: { name: "findMSB", cost: c(1, 0), tier: 0 },
  76: { name: "interpolateAtCentroid", cost: c(2, 0), tier: 0 },
  77: { name: "interpolateAtSample", cost: c(2, 0), tier: 0 },
  78: { name: "interpolateAtOffset", cost: c(2, 0), tier: 0 },
  79: { name: "nmin", cost: c(1, 0), tier: 0 },
  80: { name: "nmax", cost: c(1, 0), tier: 0 },
  81: { name: "nclamp", cost: c(2, 0), tier: 0 }
};
var TEXTURE_SAMPLE_COST = { alu: 2, sfu: 0, texture: 1, memory: 0 };
var TEXTURE_QUERY_COST = { alu: 1, sfu: 0, texture: 0, memory: 0 };
var MEMORY_COST = { alu: 0, sfu: 0, texture: 0, memory: 1 };
var ATOMIC_COST = { alu: 1, sfu: 0, texture: 0, memory: 1 };
var BARRIER_COST = { alu: 4, sfu: 0, texture: 0, memory: 0 };
var DERIVATIVE_COST = { alu: 2, sfu: 0, texture: 0, memory: 0 };
var ALU = { alu: 1, sfu: 0, texture: 0, memory: 0 };
var FDIV = { alu: 0, sfu: 1, texture: 0, memory: 0 };
var FMOD = { alu: 1, sfu: 1, texture: 0, memory: 0 };
var IDIV = { alu: 2, sfu: 1, texture: 0, memory: 0 };
function isSample(op) {
  return op >= 87 /* ImageSampleImplicitLod */ && op <= 97 /* ImageDrefGather */ || op >= 305 /* ImageSparseSampleImplicitLod */ && op < 320 /* ImageSparseRead */;
}
function isTextureOp(op) {
  return isSample(op) || op === 98 /* ImageRead */ || op === 99 /* ImageWrite */ || op === 320 /* ImageSparseRead */;
}
function isDerivative(op) {
  return op >= 207 /* DPdx */ && op <= 215 /* FwidthCoarse */;
}
function isAtomic(op) {
  return op >= 227 /* AtomicLoad */ && op <= 242 /* AtomicXor */ || op === 6035 /* AtomicFAddEXT */ || op === 5614 /* AtomicFMinEXT */ || op === 5615 /* AtomicFMaxEXT */;
}
function isDiscard(op) {
  return op === 252 /* Kill */ || op === 4416 /* TerminateInvocation */ || op === 5380 /* DemoteToHelperInvocation */;
}
function isAlu(op) {
  return op >= 77 && op <= 84 || op >= 109 && op <= 133 || op >= 149 && op <= 205 || op >= 79 && op <= 81;
}
function readString2(words2, start, end) {
  const bytes = [];
  for (let i = start; i < end; i++) {
    const w = words2[i];
    for (let b = 0; b < 4; b++) {
      const ch2 = w >>> b * 8 & 255;
      if (ch2 === 0) return new TextDecoder().decode(new Uint8Array(bytes));
      bytes.push(ch2);
    }
  }
  return new TextDecoder().decode(new Uint8Array(bytes));
}
function analyzeSpirv(data) {
  if (data.byteLength < 20 || data.byteLength % 4) return null;
  const bytes = data.byteOffset % 4 ? data.slice() : data;
  const words2 = new Uint32Array(bytes.buffer, bytes.byteOffset, bytes.byteLength / 4);
  if (words2[0] !== 119734787) return null;
  const debug = parseSpirvDebugInfo(data);
  const locations = debug?.locations ?? [];
  const fileName = (loc) => debug?.files[loc.file]?.name.replace(/^.*[\\/]/, "") ?? "";
  const names = /* @__PURE__ */ new Map();
  const constants = /* @__PURE__ */ new Set();
  const pointerClass = /* @__PURE__ */ new Map();
  const pointee = /* @__PURE__ */ new Map();
  const bufferBlocks = /* @__PURE__ */ new Set();
  const idClass = /* @__PURE__ */ new Map();
  const functions = /* @__PURE__ */ new Map();
  const entries = [];
  const findings = /* @__PURE__ */ new Map();
  const lineCosts = /* @__PURE__ */ new Map();
  const totals = { instructions: 0, functions: 0, loops: 0, branches: 0, textureOps: 0, memoryOps: 0, sfuOps: 0, atomics: 0, barriers: 0, derivatives: 0, discards: 0, workgroupBytes: 0 };
  const typeSizes = /* @__PURE__ */ new Map();
  const typeArrays = /* @__PURE__ */ new Map();
  const typeStructs = /* @__PURE__ */ new Map();
  const constantValues = /* @__PURE__ */ new Map();
  const sizeOf3 = (type, depth = 0) => {
    if (depth > 16) return 0;
    const direct = typeSizes.get(type);
    if (direct !== void 0) return direct;
    const arr = typeArrays.get(type);
    if (arr) return sizeOf3(arr[0], depth + 1) * (constantValues.get(arr[1]) ?? 0);
    const members = typeStructs.get(type);
    if (members) return members.reduce((acc, m) => acc + sizeOf3(m, depth + 1), 0);
    return 0;
  };
  const defs = /* @__PURE__ */ new Map();
  const globalVars = /* @__PURE__ */ new Map();
  const storesInLoop = /* @__PURE__ */ new Map();
  const chainBase = /* @__PURE__ */ new Map();
  const candidates = [];
  let glslSet = 0;
  let fn = null;
  const loopStack = [];
  const selectionStack = [];
  let hasLines = false;
  const finding = (rule, severity, confidence, message, ordinal2) => {
    const loc = locations[ordinal2] ?? null;
    const key = `${rule}|${fn?.id ?? 0}|${loc ? `${loc.file}:${loc.line}` : "-"}`;
    const existing = findings.get(key);
    if (existing) {
      existing.count++;
      if (SEVERITY_RANK[severity] > SEVERITY_RANK[existing.severity]) existing.severity = severity;
      return;
    }
    findings.set(key, {
      rule,
      severity,
      confidence,
      message,
      function: fn?.name ?? "",
      loopDepth: loopStack.length,
      count: 1,
      ...loc ? { file: fileName(loc), line: loc.line } : {}
    });
    if (loc) hasLines = true;
  };
  const depthSeverity = (base, deeper) => loopStack.length >= 2 ? deeper : base;
  let ordinal = 0;
  const lineOf = () => {
    const loc = fn ? locations[ordinal] : null;
    if (!fn || !loc) return null;
    const byLine = lineCosts.get(fn.id);
    const key = `${loc.file}:${loc.line}`;
    let entry = byLine.get(key);
    if (!entry) {
      entry = { file: fileName(loc), line: loc.line, cost: emptyCost(), weighted: 0, dominant: "alu", instructions: 0 };
      byLine.set(key, entry);
    }
    return entry;
  };
  const charge = (c2, s) => {
    addCost(fn.cost, c2, s);
    const entry = lineOf();
    if (entry) addCost(entry.cost, c2, s);
  };
  let i = 5;
  while (i < words2.length) {
    const w = words2[i];
    const op = w & 65535;
    const len = w >>> 16;
    if (len === 0) break;
    const a = i + 1;
    const end = Math.min(words2.length, i + len);
    const inLoop = loopStack.length > 0;
    const scale = Math.pow(LOOP_TRIPS, loopStack.length);
    switch (op) {
      case 5 /* Name */:
        names.set(words2[a], readString2(words2, a + 1, end));
        break;
      case 11 /* ExtInstImport */:
        if (readString2(words2, a + 1, end) === "GLSL.std.450") glslSet = words2[a];
        break;
      case 15 /* EntryPoint */:
        entries.push({ stage: STAGES[words2[a]] ?? "unknown", functionId: words2[a + 1], name: readString2(words2, a + 2, end) });
        break;
      case 32 /* TypePointer */:
        pointerClass.set(words2[a], words2[a + 1]);
        pointee.set(words2[a], words2[a + 2]);
        break;
      case 20 /* TypeBool */:
        typeSizes.set(words2[a], 4);
        break;
      case 21 /* TypeInt */:
      case 22 /* TypeFloat */:
        typeSizes.set(words2[a], Math.max(1, words2[a + 1] >>> 3));
        break;
      case 23 /* TypeVector */:
        typeSizes.set(words2[a], sizeOf3(words2[a + 1]) * words2[a + 2]);
        break;
      case 24 /* TypeMatrix */:
        typeSizes.set(words2[a], sizeOf3(words2[a + 1]) * words2[a + 2]);
        break;
      case 28 /* TypeArray */:
        typeArrays.set(words2[a], [words2[a + 1], words2[a + 2]]);
        break;
      case 29 /* TypeRuntimeArray */:
        typeSizes.set(words2[a], 0);
        break;
      case 30 /* TypeStruct */:
        typeStructs.set(words2[a], Array.from(words2.subarray(a + 1, end)));
        break;
      case 71 /* Decorate */:
        if (words2[a + 1] === 3 /* BufferBlock */) bufferBlocks.add(words2[a]);
        break;
      case 43 /* Constant */:
      case 41 /* ConstantTrue */:
      case 42 /* ConstantFalse */:
      case 44 /* ConstantComposite */:
      case 50 /* SpecConstant */:
      case 48 /* SpecConstantTrue */:
      case 49 /* SpecConstantFalse */:
      case 51 /* SpecConstantComposite */:
        constants.add(words2[a + 1]);
        if (op === 43 /* Constant */ && len === 4) constantValues.set(words2[a + 1], words2[a + 2]);
        break;
      case 59 /* Variable */: {
        let cls = words2[a + 2];
        if (cls === 2 /* Uniform */ && bufferBlocks.has(pointee.get(words2[a]) ?? -1)) cls = 12 /* StorageBuffer */;
        idClass.set(words2[a + 1], cls);
        if (!fn) globalVars.set(words2[a + 1], cls);
        if (cls === 4 /* Workgroup */) totals.workgroupBytes += sizeOf3(pointee.get(words2[a]) ?? -1);
        chainBase.set(words2[a + 1], words2[a + 1]);
        break;
      }
      case 65 /* AccessChain */:
      case 66 /* InBoundsAccessChain */:
      case 67 /* PtrAccessChain */:
      case 70 /* InBoundsPtrAccessChain */: {
        const base = idClass.get(words2[a + 2]);
        const cls = base ?? pointerClass.get(words2[a]);
        if (cls !== void 0) idClass.set(words2[a + 1], cls);
        chainBase.set(words2[a + 1], chainBase.get(words2[a + 2]) ?? words2[a + 2]);
        if (fn) defs.set(words2[a + 1], { op, operands: Array.from(words2.subarray(a + 2, end)), loops: [...loopStack], cost: 0, ordinal, fnId: fn.id });
        break;
      }
      case 54 /* Function */: {
        const id = words2[a + 1];
        fn = { id, name: names.get(id) ?? `function_${id}`, cost: emptyCost(), inclusive: emptyCost(), instructions: 0, loops: 0, branches: 0, calls: [], lines: [] };
        lineCosts.set(id, /* @__PURE__ */ new Map());
        functions.set(id, fn);
        totals.functions++;
        loopStack.length = 0;
        selectionStack.length = 0;
        break;
      }
      case 56 /* FunctionEnd */:
        fn = null;
        break;
      case 248 /* Label */: {
        const id = words2[a];
        while (loopStack.length && loopStack[loopStack.length - 1] === id) loopStack.pop();
        while (selectionStack.length && selectionStack[selectionStack.length - 1] === id) selectionStack.pop();
        const li = loopStack.indexOf(id);
        if (li >= 0) loopStack.splice(li);
        const si = selectionStack.indexOf(id);
        if (si >= 0) selectionStack.splice(si);
        break;
      }
      default:
        break;
    }
    if (fn && op !== 54 /* Function */ && op !== 56 /* FunctionEnd */ && op !== 248 /* Label */) {
      fn.instructions++;
      totals.instructions++;
      const lineEntry = lineOf();
      if (lineEntry) {
        lineEntry.instructions++;
        hasLines = true;
      }
      switch (op) {
        case 246 /* LoopMerge */:
          fn.loops++;
          totals.loops++;
          loopStack.push(words2[a]);
          break;
        case 247 /* SelectionMerge */:
          selectionStack.push(words2[a]);
          break;
        case 250 /* BranchConditional */:
        case 251 /* Switch */:
          fn.branches++;
          totals.branches++;
          charge(ALU, scale);
          break;
        case 57 /* FunctionCall */:
          fn.calls.push(words2[a + 2]);
          break;
        case 61 /* Load */:
        case 62 /* Store */: {
          const cls = idClass.get(op === 61 /* Load */ ? words2[a + 2] : words2[a]);
          if (cls === 2 /* Uniform */ || cls === 12 /* StorageBuffer */ || cls === 5349 /* PhysicalStorageBuffer */ || cls === 4 /* Workgroup */ || cls === 11 /* Image */) {
            charge(MEMORY_COST, scale);
            totals.memoryOps++;
            if (inLoop && (cls === 12 /* StorageBuffer */ || cls === 5349 /* PhysicalStorageBuffer */)) {
              finding(
                "storage-access-in-loop",
                depthSeverity("low", "medium"),
                "medium",
                `${op === 61 /* Load */ ? "Storage buffer read" : "Storage buffer write"} inside a loop: memory traffic scales with the trip count; load once outside the loop when the address does not change.`,
                ordinal
              );
            }
          } else if (cls === 9 /* PushConstant */) {
            charge(ALU, scale);
          }
          break;
        }
        case 136 /* FDiv */:
        case 140 /* FRem */:
        case 141 /* FMod */:
        case 134 /* UDiv */:
        case 135 /* SDiv */:
        case 137 /* UMod */:
        case 138 /* SRem */:
        case 139 /* SMod */: {
          const integer = op === 134 /* UDiv */ || op === 135 /* SDiv */ || op === 137 /* UMod */ || op === 138 /* SRem */ || op === 139 /* SMod */;
          charge(integer ? IDIV : op === 136 /* FDiv */ ? FDIV : FMOD, scale);
          totals.sfuOps++;
          const divisorConstant = constants.has(words2[a + 3]);
          if (inLoop && !divisorConstant) {
            finding(
              "costly-arithmetic-in-loop",
              depthSeverity("low", "medium"),
              "high",
              `${integer ? "Integer" : "Floating-point"} ${op === 136 /* FDiv */ || op === 134 /* UDiv */ || op === 135 /* SDiv */ ? "division" : "modulo"} by a non-constant inside a loop; multiply by a reciprocal computed once outside the loop.`,
              ordinal
            );
          } else if (integer && !divisorConstant) {
            finding("integer-division", "info", "high", "Integer division or modulo by a non-constant: several instructions on most GPUs; shifts and masks when the divisor is a power of two.", ordinal);
          }
          break;
        }
        case 148 /* Dot */:
          charge({ alu: 3, sfu: 0, texture: 0, memory: 0 }, scale);
          break;
        case 142 /* VectorTimesScalar */:
          charge(ALU, scale);
          break;
        case 143 /* MatrixTimesScalar */:
        case 144 /* VectorTimesMatrix */:
        case 145 /* MatrixTimesVector */:
        case 147 /* OuterProduct */:
        case 84 /* Transpose */:
          charge({ alu: 4, sfu: 0, texture: 0, memory: 0 }, scale);
          break;
        case 146 /* MatrixTimesMatrix */:
          charge({ alu: 16, sfu: 0, texture: 0, memory: 0 }, scale);
          break;
        case 12 /* ExtInst */: {
          if (words2[a + 2] === glslSet) {
            const info = GLSL_EXT[words2[a + 3]];
            if (info) {
              charge(info.cost, scale);
              if (info.cost.sfu) totals.sfuOps++;
              if (inLoop && info.tier >= 2) {
                finding(
                  "expensive-builtin-in-loop",
                  info.tier === 3 ? depthSeverity("medium", "high") : depthSeverity("low", "medium"),
                  "high",
                  `${info.name}() inside a loop${loopStack.length > 1 ? ` (depth ${loopStack.length})` : ""}: a special-function-unit operation repeated every iteration; hoist it out or replace it with cheaper arithmetic.`,
                  ordinal
                );
              }
            } else {
              charge(ALU, scale);
            }
          } else {
            charge(ALU, scale);
          }
          break;
        }
        case 224 /* ControlBarrier */:
        case 225 /* MemoryBarrier */:
          charge(BARRIER_COST, scale);
          totals.barriers++;
          if (inLoop) finding("barrier-in-loop", "medium", "high", "A barrier inside a loop serializes the workgroup every iteration.", ordinal);
          break;
        case 103 /* ImageQuerySizeLod */:
        case 104 /* ImageQuerySize */:
        case 105 /* ImageQueryLod */:
        case 106 /* ImageQueryLevels */:
        case 107 /* ImageQuerySamples */:
          charge(TEXTURE_QUERY_COST, scale);
          break;
        default:
          if (isTextureOp(op)) {
            charge(TEXTURE_SAMPLE_COST, scale);
            totals.textureOps++;
            if (inLoop) {
              finding(
                "texture-sample-in-loop",
                "high",
                "high",
                `${op === 99 /* ImageWrite */ ? "Image store" : isSample(op) ? "Texture sample" : "Image load"} inside a loop${loopStack.length > 1 ? ` (depth ${loopStack.length})` : ""}: ${LOOP_TRIPS}+ texture operations per invocation; sample once outside the loop or reduce the iteration count.`,
                ordinal
              );
            }
          } else if (isAtomic(op)) {
            charge(ATOMIC_COST, scale);
            totals.atomics++;
            if (inLoop) finding("atomic-in-loop", "medium", "high", "An atomic operation inside a loop: a contention point repeated every iteration; accumulate locally and issue one atomic.", ordinal);
          } else if (isDerivative(op)) {
            charge(DERIVATIVE_COST, scale);
            totals.derivatives++;
            if (selectionStack.length) finding("derivative-in-branch", "medium", "medium", "A derivative (dFdx / dFdy / fwidth, or an implicit-LOD sample) inside a branch: undefined where neighbouring invocations take a different path, and it forces quad-wide execution.", ordinal);
          } else if (isSample(op) && selectionStack.length) {
          } else if (isDiscard(op)) {
            totals.discards++;
            finding("discard", "low", "medium", "discard / demote in a fragment shader disables early depth and stencil testing on many GPUs for every draw using it; prefer alpha blending or a depth pre-pass when the discard is rare.", ordinal);
          } else if (isAlu(op)) {
            charge(ALU, scale);
          }
          break;
      }
    }
    if (fn) {
      if (op === 62 /* Store */) {
        const v = chainBase.get(words2[a]) ?? words2[a];
        for (const loop of loopStack) {
          let set = storesInLoop.get(loop);
          if (!set) {
            set = /* @__PURE__ */ new Set();
            storesInLoop.set(loop, set);
          }
          set.add(v);
        }
      } else if (op === 61 /* Load */) {
        defs.set(words2[a + 1], { op, operands: [], loops: [...loopStack], pointer: words2[a + 2], cost: 0, ordinal, fnId: fn.id });
      } else if (op === 245 /* Phi */ || op === 57 /* FunctionCall */) {
        defs.set(words2[a + 1], { op, operands: [], loops: [...loopStack], cost: 0, ordinal, fnId: fn.id });
      } else if (op === 12 /* ExtInst */) {
        const d = { op, operands: Array.from(words2.subarray(a + 4, end)), loops: [...loopStack], cost: 1, ordinal, fnId: fn.id };
        if (words2[a + 2] === glslSet) d.cost = weighCost(GLSL_EXT[words2[a + 3]]?.cost ?? ALU);
        defs.set(words2[a + 1], d);
        if (loopStack.length) candidates.push(d);
      } else if (isAlu(op) || op >= 134 /* UDiv */ && op <= 148 /* Dot */ || op === 84 /* Transpose */) {
        const cost = op === 148 /* Dot */ ? 3 : op === 146 /* MatrixTimesMatrix */ ? 16 : op >= 143 /* MatrixTimesScalar */ && op <= 147 /* OuterProduct */ || op === 84 /* Transpose */ ? 4 : op >= 134 /* UDiv */ && op <= 141 /* FMod */ ? COST_WEIGHTS.sfu : 1;
        const d = { op, operands: Array.from(words2.subarray(a + 2, end)), loops: [...loopStack], cost, ordinal, fnId: fn.id };
        defs.set(words2[a + 1], d);
        if (loopStack.length) candidates.push(d);
      }
    }
    ordinal++;
    i += len;
  }
  const invariantMemo = /* @__PURE__ */ new Map();
  const invariant = (id, loop, depth) => {
    if (constants.has(id) || globalVars.has(id)) return true;
    const d = defs.get(id);
    if (!d) return false;
    if (!d.loops.includes(loop)) return true;
    if (depth > 32) return false;
    const key = `${id}:${loop}`;
    const memo = invariantMemo.get(key);
    if (memo !== void 0) return memo;
    invariantMemo.set(key, false);
    let result = false;
    if (d.op === 61 /* Load */ && d.pointer !== void 0) {
      const v = chainBase.get(d.pointer) ?? d.pointer;
      const cls = idClass.get(v);
      const volatileClass = cls === 12 /* StorageBuffer */ || cls === 5349 /* PhysicalStorageBuffer */ || cls === 4 /* Workgroup */ || cls === 11 /* Image */;
      result = !volatileClass && !storesInLoop.get(loop)?.has(v) && invariant(d.pointer, loop, depth + 1);
    } else if (d.op === 245 /* Phi */ || d.op === 57 /* FunctionCall */) {
      result = false;
    } else {
      result = d.operands.every((o) => invariant(o, loop, depth + 1));
    }
    invariantMemo.set(key, result);
    return result;
  };
  const groups = /* @__PURE__ */ new Map();
  for (const d of candidates) {
    const loop = d.loops[d.loops.length - 1];
    if (!d.operands.every((o) => invariant(o, loop, 0))) continue;
    const loc = locations[d.ordinal] ?? null;
    const key = `${d.fnId}|${loop}|${loc ? `${loc.file}:${loc.line}` : "-"}`;
    const g = groups.get(key);
    if (g) {
      g.count++;
      g.cost += d.cost;
    } else groups.set(key, { fnId: d.fnId, loop, ordinal: d.ordinal, count: 1, cost: d.cost });
  }
  for (const g of groups.values()) {
    fn = functions.get(g.fnId) ?? null;
    loopStack.length = 0;
    loopStack.push(g.loop);
    const severity = g.cost >= COST_WEIGHTS.sfu ? "medium" : "low";
    finding(
      "loop-invariant",
      severity,
      "medium",
      `${g.count} instruction${g.count === 1 ? "" : "s"} inside the loop use${g.count === 1 ? "s" : ""} only values that do not change in it (${Math.round(g.cost)} op units repeated every iteration); compute ${g.count === 1 ? "it" : "them"} once before the loop.`,
      g.ordinal
    );
    if (g.count > 1) findings.get(`loop-invariant|${g.fnId}|${locations[g.ordinal] ?? null ? `${locations[g.ordinal].file}:${locations[g.ordinal].line}` : "-"}`).count = g.count;
  }
  fn = null;
  loopStack.length = 0;
  if (totals.workgroupBytes > 0) {
    const kb = totals.workgroupBytes / 1024;
    finding(
      "workgroup-memory",
      totals.workgroupBytes > 32 * 1024 ? "medium" : totals.workgroupBytes > 16 * 1024 ? "low" : "info",
      "high",
      `${kb >= 1 ? `${kb.toFixed(kb >= 10 ? 0 : 1)} KB` : `${totals.workgroupBytes} bytes`} of shared (workgroup) memory per workgroup${totals.workgroupBytes > 16 * 1024 ? ": it limits how many workgroups fit on a compute unit at once; halve it, or use a smaller workgroup, when occupancy matters" : ""}.`,
      -1
    );
  }
  const inclusive = (id, stack) => {
    const f = functions.get(id);
    if (!f) return emptyCost();
    if (stack.has(id)) return f.cost;
    stack.add(id);
    const total = { ...f.cost };
    for (const callee of f.calls) addCost(total, inclusive(callee, stack));
    stack.delete(id);
    return total;
  };
  for (const f of functions.values()) {
    f.inclusive = inclusive(f.id, /* @__PURE__ */ new Set());
    f.lines = [...lineCosts.get(f.id)?.values() ?? []].map((l) => ({ ...l, weighted: weighCost(l.cost), dominant: dominantDimension(l.cost) })).filter((l) => l.weighted > 0).sort((x, y) => y.weighted - x.weighted);
  }
  const entryPoints = entries.map((e) => {
    const reachable = /* @__PURE__ */ new Set();
    const stack = [e.functionId];
    while (stack.length) {
      const id = stack.pop();
      if (reachable.has(id)) continue;
      reachable.add(id);
      for (const callee of functions.get(id)?.calls ?? []) stack.push(callee);
    }
    const fns = [...reachable].map((id) => functions.get(id)).filter((f) => !!f).sort((x, y) => weighCost(y.inclusive) - weighCost(x.inclusive));
    const cost = functions.get(e.functionId)?.inclusive ?? emptyCost();
    return { name: e.name, stage: e.stage, functionId: e.functionId, cost, weighted: weighCost(cost), dominant: dominantDimension(cost), functions: fns };
  });
  const sorted = [...findings.values()].sort((x, y) => SEVERITY_RANK[y.severity] - SEVERITY_RANK[x.severity] || y.loopDepth - x.loopDepth || y.count - x.count);
  const hasFragment = entries.some((e) => e.stage === "fragment");
  const filtered = hasFragment ? sorted : sorted.filter((f) => f.rule !== "discard");
  return { entryPoints, functions: [...functions.values()], findings: filtered, totals, hasLines };
}
var cache2 = /* @__PURE__ */ new WeakMap();
function analyzeSpirvCached(data) {
  let a = cache2.get(data);
  if (a === void 0) {
    try {
      a = analyzeSpirv(data);
    } catch (e) {
      console.warn("shader analysis failed", e);
      a = null;
    }
    cache2.set(data, a);
  }
  return a;
}

// src/renderer/render_graph_analysis.ts
var SUPERSEDED_RULES = /* @__PURE__ */ new Set(["depth-store", "color-store", "mergeable-passes"]);
var RULE_ORDER = ["overwritten-before-read", "mergeable-passes", "unread-store", "transient-candidate", "oversynchronized-barrier"];
var Folded = class {
  first = null;
  count = 0;
  nodes = [];
  /** What the finding names: the first few resources involved. */
  subjects = [];
  add(node2, subject) {
    if (!this.first) this.first = node2;
    this.count++;
    if (this.nodes.length < 64) this.nodes.push(node2);
    if (this.subjects.length < 3 && !this.subjects.includes(subject)) this.subjects.push(subject);
  }
  /** "Bloom mip 1, Bloom mip 2 and 4 more". */
  get subjectText() {
    const rest = this.count - this.subjects.length;
    const names = this.subjects.join(", ");
    return rest > 0 ? `${names} and ${rest} more` : names;
  }
};
var GraphAnalysis = class {
  _graph;
  _findings = [];
  _byCommand = /* @__PURE__ */ new Map();
  /**
   * Something in the frame reads through a binding the capture could not resolve, so "nothing
   * reads this" is a statement about what the graph can see. Every rule that rests on it drops a
   * confidence level and says so.
   */
  _blind = false;
  /** Which API's spelling of a fix the advice should name. */
  _metal = false;
  constructor(graph) {
    this._graph = graph;
    this._blind = graph.nodes.some((n) => n.unresolvedReads > 0);
    this._metal = graph.api === "metal";
  }
  analyze() {
    this._unreadStores();
    const merged = this._mergeablePasses();
    this._overwrittenBeforeRead();
    this._transientCandidates(merged);
    this._oversynchronizedBarriers();
    this._findings.sort((a, b) => SEVERITY_RANK[b.severity] - SEVERITY_RANK[a.severity] || RULE_ORDER.indexOf(a.rule) - RULE_ORDER.indexOf(b.rule));
    return { findings: this._findings, byCommand: this._byCommand };
  }
  // -------------------------------------------------------------------------------- the rules
  /**
   * A pass stores an attachment to memory that no later pass reads, and that is not the image
   * being presented. On a tiled GPU the store is the expensive part of a pass, so a store nothing
   * consumes is pure bandwidth, and the fix is a store op away.
   *
   * Attachments only. A storage buffer or image nothing reads is also worth knowing, but the
   * advice would be different (drop the work, not the store), the graph counts every storage
   * binding as written whether the shader writes it or not, and the frame summary already reports
   * those passes as ones nothing reads.
   */
  _unreadStores() {
    const folded = new Folded();
    for (const node2 of this._graph.nodes) {
      for (const write of node2.writes) {
        if (usageClass(write.usage) !== "attachment") continue;
        if (write.dropped || write.resource.presented) continue;
        if (write.resolved) continue;
        if (write.version.readers.length) continue;
        if (node2.unresolvedReads && node2.writes.length === 1) continue;
        folded.add(node2, write.resource.label);
      }
    }
    if (!folded.count) return;
    this._add(
      "unread-store",
      "medium",
      this._blind ? "medium" : "high",
      `${count(folded.count, "write")} in the frame ${folded.count === 1 ? "reaches" : "reach"} memory that no later pass reads: ${folded.subjectText}. Discarding instead (${this._metal ? "MTLStoreActionDontCare" : "store op DONT_CARE"}) keeps the result in tile memory and skips the write. The graph only sees this capture, so a result the host reads back or the next frame consumes will look unread here${this._blindClause()}.`,
      folded
    );
  }
  /**
   * A version is replaced by a write that keeps nothing of it, and nothing read it in between:
   * the work that produced it was thrown away. A clear over a pass' output, a target written
   * twice, a compute pass whose result the next dispatch overwrites.
   */
  _overwrittenBeforeRead() {
    const folded = new Folded();
    for (const resource2 of this._graph.resources) {
      for (let i = 0; i < resource2.versions.length - 1; i++) {
        const version = resource2.versions[i];
        const next = resource2.versions[i + 1];
        if (!version.producer || version.readers.length || version.dropped) continue;
        const replaces = next.producer?.writes.find((w) => w.version === next)?.discards;
        if (!replaces) continue;
        folded.add(version.producer, `${resource2.label} (${next.producer?.label ?? "a later pass"} replaces it)`);
      }
    }
    if (!folded.count) return;
    this._add(
      "overwritten-before-read",
      "high",
      this._blind ? "medium" : "high",
      `${count(folded.count, "pass", "passes")} ${folded.count === 1 ? "writes a result that is" : "write results that are"} replaced before anything reads ${folded.count === 1 ? "it" : "them"}: ${folded.subjectText}. The work is done and thrown away \u2014 the pass can be dropped, or the write that replaces it can be dropped and the two merged${this._blindClause()}.`,
      folded
    );
  }
  /**
   * Two adjacent render passes where the second loads what the first stored: one pass keeps the
   * attachment in tile memory instead of storing and re-loading it. Returns the resources it
   * reported, so the transient rule does not say the same thing about them in other words.
   */
  _mergeablePasses() {
    const folded = new Folded();
    const reported = /* @__PURE__ */ new Set();
    const nodes = this._graph.nodes;
    for (let i = 1; i < nodes.length; i++) {
      const before = nodes[i - 1];
      const after = nodes[i];
      if (before.kind !== "render" || after.kind !== "render") continue;
      const carried = after.writes.filter((w) => usageClass(w.usage) === "attachment" && !w.discards && w.version.index > 1 && this._producedBy(w, before));
      if (!carried.length) continue;
      if (!sameTargets(before, after)) continue;
      for (const w of carried) reported.add(w.resource.key);
      folded.add(after, carried.map((w) => w.resource.label).join(", "));
    }
    if (!folded.count) return reported;
    this._add(
      "mergeable-passes",
      "medium",
      "medium",
      `${count(folded.count, "pass", "passes")} load exactly what the pass immediately before stored, to the same targets: ${folded.subjectText}. Recorded as one pass (a second subpass, or simply more draws) the attachment stays in tile memory and the store and load both go away.`,
      folded
    );
    return reported;
  }
  /**
   * A resource that is written and then consumed only by the very next pass, never presented and
   * never copied out: it never needs to exist in memory at all. On Vulkan that is
   * TRANSIENT_ATTACHMENT with LAZILY_ALLOCATED memory or an input attachment; on Metal,
   * MTLStorageModeMemoryless.
   */
  _transientCandidates(merged) {
    const folded = new Folded();
    for (const resource2 of this._graph.resources) {
      if (resource2.type !== "image" || resource2.presented || merged.has(resource2.key)) continue;
      if (resource2.externalInput) continue;
      let stored = false;
      let consumed = false;
      let local = true;
      for (const version of resource2.versions) {
        if (!version.producer) continue;
        const write = version.producer.writes.find((w) => w.version === version);
        if (write && !write.dropped) stored = true;
        for (const reader of version.readers) {
          consumed = true;
          if (reader.ordinal !== version.producer.ordinal + 1) local = false;
        }
      }
      if (!stored || !consumed || !local) continue;
      if (resource2.uses.some((u) => usageClass(u.usage) === "transfer")) continue;
      const producer = resource2.versions.find((v) => v.producer)?.producer;
      if (producer) folded.add(producer, resource2.label);
    }
    if (!folded.count) return;
    this._add(
      "transient-candidate",
      "medium",
      this._blind ? "low" : "medium",
      `${count(folded.count, "image")} ${folded.count === 1 ? "is" : "are"} written and then read only by the pass that follows, and never presented or copied: ${folded.subjectText}. A target used that way never has to reach memory: ${this._metal ? "MTLStorageModeMemoryless, or an imageblock read in the second pass" : "TRANSIENT_ATTACHMENT usage with LAZILY_ALLOCATED memory, or an input attachment in a second subpass"}${this._blindClause()}.`,
      folded
    );
  }
  /**
   * A barrier that names resources the frame does not use on both sides of it. Only barriers that
   * do nothing else are considered: a layout transition or a queue-family transfer is required
   * whatever the data does, and a global memory barrier names nothing to check.
   */
  _oversynchronizedBarriers() {
    const graph = this._graph;
    if (!graph.syncPoints.length) return;
    const touched = /* @__PURE__ */ new Map();
    for (const resource2 of graph.resources) {
      touched.set(resource2.key, resource2.uses.map((u) => u.node.ordinal));
    }
    const findings = [];
    for (const sync of graph.syncPoints) {
      if (sync.structural || !sync.resources.length) continue;
      const idle = sync.resources.filter((key) => {
        const ordinals = touched.get(key);
        if (!ordinals || !ordinals.length) return true;
        return !ordinals.some((o) => o <= sync.after) || !ordinals.some((o) => o > sync.after);
      });
      if (idle.length === sync.resources.length) findings.push({ commandIndex: sync.commandIndex, resources: idle });
    }
    if (!findings.length) return;
    const first = findings[0];
    const f = {
      rule: "oversynchronized-barrier",
      severity: "low",
      confidence: "medium",
      message: `${count(findings.length, "barrier")} synchronize only resources the frame does not both write before and use after them, and change no image layout or queue family. A barrier that guards nothing still costs a pipeline stall. The graph sees this capture only, so a barrier making a host write visible, or ordering against another frame or queue, will look idle here${this._blindClause()}.`,
      commandIndex: first.commandIndex,
      count: findings.length
    };
    this._findings.push(f);
    for (const entry of findings) this._attach(entry.commandIndex, f);
  }
  // ------------------------------------------------------------------------------- mechanics
  /** True when `use`'s version was produced by `node` (its immediately preceding version). */
  _producedBy(use, node2) {
    const previous = use.resource.versions[use.version.index - 1];
    return !!previous && previous.producer === node2;
  }
  _blindClause() {
    return this._blind ? ", and some of the frame's bindings could not be resolved to a resource at all" : "";
  }
  _add(rule, severity, confidence, message, folded) {
    const f = { rule, severity, confidence, message, commandIndex: folded.first?.commandIndex, count: folded.count };
    this._findings.push(f);
    for (const node2 of folded.nodes) this._attach(node2.commandIndex, f);
  }
  _attach(commandIndex, f) {
    const list = this._byCommand.get(commandIndex);
    if (list) list.push(f);
    else this._byCommand.set(commandIndex, [f]);
  }
};
function targetKey(node2) {
  return node2.writes.filter((w) => usageClass(w.usage) === "attachment").map((w) => w.resource.key).sort().join("|");
}
function sameTargets(a, b) {
  const key = targetKey(a);
  return key.length > 0 && key === targetKey(b);
}
function count(n, one, many = "") {
  return `${n} ${n === 1 ? one : many || `${one}s`}`;
}
function analyzeRenderGraph(graph) {
  return new GraphAnalysis(graph).analyze();
}

// src/renderer/metal/frame_analysis.ts
var TINY_DRAW_VERTICES = 12;
var TINY_DRAW_COUNT = 32;
var RULE_ORDER2 = [
  "undefined-load",
  "mergeable-passes",
  "msaa-store",
  "memoryless-candidate",
  "color-store",
  "depth-store",
  "color-load",
  "tiny-draws",
  "redundant-pipeline-bind",
  "redundant-buffer-bind",
  "single-threadgroup-dispatch"
];
var Folded2 = class {
  first = null;
  count = 0;
  commands = [];
  add(cmd) {
    if (!this.first) this.first = cmd;
    this.count++;
    if (this.commands.length < 64) this.commands.push(cmd);
  }
};
function argKey(v) {
  if (isHandleRef(v)) return `#${v.__id}`;
  if (Array.isArray(v)) return `[${v.map(argKey).join(",")}]`;
  if (isObject(v)) return `{${Object.entries(v).map(([k, e]) => `${k}:${argKey(e)}`).join(",")}}`;
  return str(v);
}
var BLIT_READS = {
  "copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:": ["sourceTexture"],
  "copyFromTexture:toTexture:": ["sourceTexture"],
  "copyFromTexture:sourceSlice:sourceLevel:toTexture:destinationSlice:destinationLevel:sliceCount:levelCount:": ["sourceTexture"],
  "copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:": ["sourceTexture"],
  "copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:options:": ["sourceTexture"],
  "generateMipmapsForTexture:": ["texture"],
  "synchronizeTexture:slice:level:": ["texture"],
  "synchronizeResource:": ["resource"],
  "presentDrawable:": ["texture"],
  "presentDrawable:atTime:": ["texture"],
  "presentDrawable:afterMinimumDuration:": ["texture"]
};
var MetalFrameAnalysis = class {
  findings = [];
  _db;
  _passes = [];
  _byCommand = /* @__PURE__ */ new Map();
  /** Texture id -> command indices that read it (a sample, a blit source, a load, a present). */
  _reads = /* @__PURE__ */ new Map();
  /** Texture id -> every pass that used it as an attachment, in order. */
  _attachmentUses = /* @__PURE__ */ new Map();
  byCommand() {
    return this._byCommand;
  }
  constructor(db) {
    this._db = db;
  }
  analyze(data) {
    this.findings = [];
    this._passes = [];
    this._byCommand = /* @__PURE__ */ new Map();
    this._reads = /* @__PURE__ */ new Map();
    this._attachmentUses = /* @__PURE__ */ new Map();
    this._walk(data.commands);
    this._attachmentRules();
    this._memorylessRule();
    this.findings.sort((a, b) => SEVERITY_RANK[b.severity] - SEVERITY_RANK[a.severity] || RULE_ORDER2.indexOf(a.rule) - RULE_ORDER2.indexOf(b.rule));
    return this.findings;
  }
  // ---------------------------------------------------------------------------- the walk
  _walk(commands) {
    const sets = METAL_SETS;
    const boundPipeline = /* @__PURE__ */ new Map();
    const boundBuffers = /* @__PURE__ */ new Map();
    const lastPass = /* @__PURE__ */ new Map();
    const openPass = /* @__PURE__ */ new Map();
    const redundantPipeline = new Folded2();
    const redundantBuffer = new Folded2();
    const tinyDraws = new Folded2();
    const singleGroup = new Folded2();
    const mergeable = new Folded2();
    let ordinal = 0;
    for (const cmd of commands) {
      const a = cmd.args;
      const cb = cmd.object?.__id ?? 0;
      const encoder = cmd.encoder?.__id ?? 0;
      const m = cmd.method;
      if (sets.PASS_BEGIN.has(m)) {
        const previous = lastPass.get(cb);
        lastPass.delete(cb);
        if (m === "renderCommandEncoderWithDescriptor:" || m === "parallelRenderCommandEncoderWithDescriptor:") {
          const pass = this._decodePass(cmd, ordinal++, cb);
          this._passes.push(pass);
          openPass.set(cb, pass);
          for (const att of pass.attachments) {
            const uses = this._attachmentUses.get(att.textureId) ?? [];
            uses.push({ pass, att });
            this._attachmentUses.set(att.textureId, uses);
            if (att.load === "Load") this._read(att.textureId, cmd.index);
          }
          if (previous) {
            const before = previous.attachments.find((x) => !x.depth && x.index === 0);
            const now = pass.attachments.find((x) => !x.depth && x.index === 0);
            if (before && now && before.textureId === now.textureId && now.load === "Load" && before.store.startsWith("Store")) {
              mergeable.add(cmd);
            }
          }
        }
        continue;
      }
      if (sets.PASS_END.has(m)) {
        const pass = openPass.get(cb);
        if (pass && cmd.encoder && pass.command.encoder && cmd.encoder.__id === pass.command.encoder.__id) {
          openPass.delete(cb);
          lastPass.set(cb, pass);
        } else if (!pass) {
          lastPass.delete(cb);
        }
        continue;
      }
      if (sets.SUBMIT.has(m)) {
        lastPass.delete(cb);
      }
      if (!a) continue;
      if (a.texture !== void 0 && a.index !== void 0) {
        const id = refId(a.texture);
        if (id !== null) this._read(id, cmd.index);
      }
      if (Array.isArray(a.textures)) {
        for (const t of a.textures) {
          const id = refId(t);
          if (id !== null) this._read(id, cmd.index);
        }
      }
      const readKeys = BLIT_READS[m];
      if (readKeys) {
        for (const key of readKeys) {
          const id = refId(a[key]);
          if (id !== null) this._read(id, cmd.index);
        }
      }
      if (sets.BIND_PIPELINE.has(m)) {
        const id = refId(a.pipeline);
        if (id !== null) {
          if (boundPipeline.get(encoder) === id) redundantPipeline.add(cmd);
          boundPipeline.set(encoder, id);
        }
        continue;
      }
      if (sets.BIND_STAGE_BUFFER?.has(m) && sets.stageBuffersOf) {
        let binds = boundBuffers.get(encoder);
        if (!binds) boundBuffers.set(encoder, binds = /* @__PURE__ */ new Map());
        for (const sb of sets.stageBuffersOf(cmd)) {
          if (sb.inline) continue;
          const slot = `${sb.stage}:${sb.index}`;
          const key = `${argKey(sb.buffer)}+${sb.offset}`;
          if (binds.get(slot) === key) redundantBuffer.add(cmd);
          binds.set(slot, key);
        }
        continue;
      }
      if (sets.DRAW.has(m)) {
        const pass = openPass.get(cb);
        if (pass) pass.draws++;
        const vertices = num(a.indexCount) || num(a.vertexCount);
        if (vertices > 0 && vertices <= TINY_DRAW_VERTICES) tinyDraws.add(cmd);
        continue;
      }
      if (m === "dispatchThreadgroups:threadsPerThreadgroup:" && isObject(a.threadgroupsPerGrid)) {
        const g = a.threadgroupsPerGrid;
        if (num(g.width) === 1 && num(g.height) === 1 && num(g.depth) === 1) singleGroup.add(cmd);
      }
    }
    if (mergeable.count) {
      this._addFolded(
        "mergeable-passes",
        "high",
        "medium",
        `${mergeable.count} render pass${mergeable.count === 1 ? "" : "es"} load${mergeable.count === 1 ? "s" : ""} the target the pass right before stored, with nothing between them: on a tile-based GPU the store and load round-trip the whole target through memory. Encoding both in one render encoder keeps it in tile memory.`,
        mergeable
      );
    }
    if (redundantPipeline.count) {
      this._addFolded("redundant-pipeline-bind", "low", "high", `${redundantPipeline.count} pipeline bind${redundantPipeline.count === 1 ? "" : "s"} of the pipeline the encoder already had.`, redundantPipeline);
    }
    if (redundantBuffer.count) {
      this._addFolded("redundant-buffer-bind", "low", "high", `${redundantBuffer.count} buffer bind${redundantBuffer.count === 1 ? "" : "s"} of the buffer, offset and slot the encoder already had.`, redundantBuffer);
    }
    if (tinyDraws.count >= TINY_DRAW_COUNT) {
      this._addFolded("tiny-draws", "low", "medium", `${tinyDraws.count} draws of ${TINY_DRAW_VERTICES} vertices or fewer: candidates for instancing or merging into one buffer.`, tinyDraws);
    }
    if (singleGroup.count) {
      this._addFolded("single-threadgroup-dispatch", "low", "medium", `${singleGroup.count} dispatch${singleGroup.count === 1 ? "" : "es"} of a single threadgroup: the rest of the GPU idles while it runs.`, singleGroup);
    }
  }
  _decodePass(cmd, ordinal, cb) {
    const a = cmd.args ?? {};
    const attachments = [];
    const decode = (att, depth, index) => {
      if (!isObject(att)) return;
      const textureId = refId(att.texture);
      if (textureId === null) return;
      attachments.push({
        textureId,
        depth,
        index,
        load: str(att.loadAction).replace("MTLLoadAction", ""),
        store: str(att.storeAction).replace("MTLStoreAction", ""),
        resolveId: refId(att.resolveTexture)
      });
    };
    if (Array.isArray(a.colorAttachments)) {
      for (const c2 of a.colorAttachments) if (isObject(c2)) decode(c2, false, num(c2.index));
    }
    decode(a.depthAttachment, true, 0);
    return { command: cmd, ordinal, commandBuffer: cb, attachments, draws: 0 };
  }
  _read(textureId, commandIndex) {
    const list = this._reads.get(textureId);
    if (list) list.push(commandIndex);
    else this._reads.set(textureId, [commandIndex]);
  }
  // ---------------------------------------------------------------------------- the rules
  _texture(id) {
    return this._db.getObject(id)?.args ?? null;
  }
  _isDrawable(id) {
    return this._texture(id)?.drawable === true;
  }
  _readAfter(textureId, commandIndex) {
    const reads = this._reads.get(textureId);
    return !!reads && reads.some((i) => i > commandIndex);
  }
  _attachmentRules() {
    const undefinedLoad = new Folded2();
    const colorLoad = new Folded2();
    const colorStore = new Folded2();
    const depthStore = new Folded2();
    const msaaStore = new Folded2();
    for (const pass of this._passes) {
      for (const att of pass.attachments) {
        const uses = this._attachmentUses.get(att.textureId) ?? [];
        const before = uses.filter((u) => u.pass.ordinal < pass.ordinal);
        const previous = before.length ? before[before.length - 1] : null;
        const texture = this._texture(att.textureId);
        const samples = num(texture?.sampleCount) || 1;
        const stored = att.store.startsWith("Store");
        if (att.load === "Load") {
          if (previous && previous.att.store === "DontCare") undefinedLoad.add(pass.command);
          else if (!previous && !att.depth && !this._isDrawable(att.textureId)) colorLoad.add(pass.command);
        }
        if (samples > 1 && stored) msaaStore.add(pass.command);
        if (stored && !this._isDrawable(att.textureId) && !this._readAfter(att.textureId, pass.command.index)) {
          (att.depth ? depthStore : colorStore).add(pass.command);
        }
      }
    }
    if (undefinedLoad.count) {
      this._addFolded("undefined-load", "high", "high", `${undefinedLoad.count} pass${undefinedLoad.count === 1 ? "" : "es"} load${undefinedLoad.count === 1 ? "s" : ""} an attachment the previous pass on it did not store (storeAction DontCare): the contents are undefined. Either store it, or clear instead of loading.`, undefinedLoad);
    }
    if (colorLoad.count) {
      this._addFolded("color-load", "medium", "medium", `${colorLoad.count} pass${colorLoad.count === 1 ? "" : "es"} load${colorLoad.count === 1 ? "s" : ""} a color attachment on its first use in the frame. If the pass covers the whole target, MTLLoadActionClear or DontCare skips reading it from memory.`, colorLoad);
    }
    if (msaaStore.count) {
      this._addFolded("msaa-store", "medium", "high", `${msaaStore.count} pass${msaaStore.count === 1 ? "" : "es"} store${msaaStore.count === 1 ? "s" : ""} a multisampled attachment. Resolving alone (MTLStoreActionMultisampleResolve) writes one sample per pixel; storing writes them all, and a memoryless multisample target needs no memory at all.`, msaaStore);
    }
    if (colorStore.count) {
      this._addFolded("color-store", "medium", "medium", `${colorStore.count} pass${colorStore.count === 1 ? "" : "es"} store${colorStore.count === 1 ? "s" : ""} a color attachment nothing reads afterwards in the frame. MTLStoreActionDontCare skips the write, unless a later frame reads it.`, colorStore);
    }
    if (depthStore.count) {
      this._addFolded("depth-store", "medium", "medium", `${depthStore.count} pass${depthStore.count === 1 ? "" : "es"} store${depthStore.count === 1 ? "s" : ""} a depth attachment nothing reads afterwards. MTLStoreActionDontCare keeps depth in tile memory, and a depth texture used that way can be MTLStorageModeMemoryless.`, depthStore);
    }
  }
  /**
   * A texture the frame only ever clears or discards on load and discards or resolves on store,
   * with no other use, never leaves the tile: MTLStorageModeMemoryless would give it no memory
   * at all. The usual cases are a multisample target with a resolve, and a depth buffer.
   */
  _memorylessRule() {
    const folded = new Folded2();
    const names = [];
    for (const [textureId, uses] of this._attachmentUses) {
      const texture = this._texture(textureId);
      if (!texture || texture.drawable === true) continue;
      if (str(texture.storageMode) !== "MTLStorageModePrivate") continue;
      if (this._reads.has(textureId)) continue;
      const transient = uses.every((u) => u.att.load !== "Load" && (u.att.store === "DontCare" || u.att.store === "MultisampleResolve"));
      if (!transient) continue;
      folded.add(uses[0].pass.command);
      if (names.length < 4) names.push(this._db.getObject(textureId)?.name ?? `texture ${textureId}`);
    }
    if (folded.count) {
      this._addFolded("memoryless-candidate", "medium", "medium", `${folded.count} texture${folded.count === 1 ? "" : "s"} (${names.join(", ")}${folded.count > names.length ? ", ..." : ""}) ${folded.count === 1 ? "is" : "are"} only ever cleared or discarded and never stored or read: MTLStorageModeMemoryless would keep ${folded.count === 1 ? "it" : "them"} in tile memory with no allocation behind.`, folded);
    }
  }
  _add(rule, severity, confidence, message, cmd, count2 = 1) {
    const f = { rule, severity, confidence, message, commandIndex: cmd?.index, count: count2 };
    this.findings.push(f);
    if (cmd) this._attach(cmd.index, f);
    return f;
  }
  _addFolded(rule, severity, confidence, message, folded) {
    const f = this._add(rule, severity, confidence, message, folded.first, folded.count);
    for (const cmd of folded.commands) if (cmd !== folded.first) this._attach(cmd.index, f);
  }
  _attach(index, f) {
    const list = this._byCommand.get(index);
    if (list) list.push(f);
    else this._byCommand.set(index, [f]);
  }
};
function analyzeMetalFrame(data, db) {
  const analysis = new MetalFrameAnalysis(db);
  const findings = analysis.analyze(data);
  return { findings, byCommand: analysis.byCommand() };
}

// src/renderer/counter_rules.ts
var Folded3 = class {
  first = null;
  count = 0;
  commands = [];
  add(commandIndex) {
    if (this.first === null) this.first = commandIndex;
    this.count++;
    if (this.commands.length < 64) this.commands.push(commandIndex);
  }
};
function analyzeCounters(data, db) {
  const findings = [];
  const byCommand = /* @__PURE__ */ new Map();
  const metrics = collectPassMetrics(data, db);
  const overdrawn = new Folded3();
  const micro = new Folded3();
  const shadedThenDropped = new Folded3();
  let worstOverdraw = 0;
  let worstFragments = Infinity;
  for (const p of metrics.passes) {
    if (p.overdraw !== null && p.overdraw > OVERDRAW_LIMIT) {
      overdrawn.add(p.commandIndex);
      worstOverdraw = Math.max(worstOverdraw, p.overdraw);
    }
    if (p.fragmentsPerPrimitive !== null && p.fragmentsPerPrimitive < MICROTRIANGLE_LIMIT) {
      micro.add(p.commandIndex);
      worstFragments = Math.min(worstFragments, p.fragmentsPerPrimitive);
    }
    if (p.depthRejectRate !== null && p.overdraw !== null && p.overdraw > 1.5 && p.depthRejectRate < LOW_REJECTION_RATE) {
      shadedThenDropped.add(p.commandIndex);
    }
  }
  const add = (rule, severity, confidence, message, folded) => {
    if (!folded.count) return;
    const f = { rule, severity, confidence, message, commandIndex: folded.first ?? void 0, count: folded.count };
    findings.push(f);
    for (const index of folded.commands) {
      const list = byCommand.get(index);
      if (list) list.push(f);
      else byCommand.set(index, [f]);
    }
  };
  const passWord = (n) => `${n} pass${n === 1 ? "" : "es"}`;
  add(
    "high-overdraw",
    "high",
    "high",
    `${passWord(overdrawn.count)} shade each pixel more than ${OVERDRAW_LIMIT} times over (worst ${formatRatio(worstOverdraw)}): stacked transparency, a full-screen effect drawn more than once, or opaque geometry drawn back to front.`,
    overdrawn
  );
  add(
    "microtriangles",
    "high",
    "high",
    `${passWord(micro.count)} rasterize triangles covering fewer than ${MICROTRIANGLE_LIMIT} fragments each (worst ${formatRatio(worstFragments, 1)}): the 2x2 rasterization quad shades lanes that are then thrown away. Mesh level of detail at distance is the usual answer.`,
    micro
  );
  add(
    "late-depth-rejection",
    "medium",
    "medium",
    `${passWord(shadedThenDropped.count)} overdraw while the depth test rejects little: fragments are shaded and then replaced. Drawing opaque geometry front to back, or a depth prepass, rejects that work before the fragment shader runs.`,
    shadedThenDropped
  );
  return { findings, byCommand };
}

// src/renderer/sampling_rules.ts
var LARGE_TEXTURE_PIXELS = 1024 * 1024;
var METAL_TEXTURE_BINDS = /* @__PURE__ */ new Set([
  "setFragmentTexture:atIndex:",
  "setFragmentTextures:withRange:",
  "setVertexTexture:atIndex:",
  "setVertexTextures:withRange:",
  "setTexture:atIndex:",
  "setTextures:withRange:"
]);
function samplesFrom(type) {
  return type.includes("SAMPLED_IMAGE") || type.includes("COMBINED_IMAGE_SAMPLER");
}
function collectSampled(data) {
  const out = /* @__PURE__ */ new Map();
  const note = (id, cmd) => {
    if (id !== null && !out.has(id)) out.set(id, cmd);
  };
  for (const cmd of data.commands) {
    if (cmd.descriptors) {
      for (const set of cmd.descriptors.sets) {
        for (const b of set.bindings) {
          if (!samplesFrom(b.type)) continue;
          for (const d of b.descriptors) note(refId(d?.imageView), cmd);
        }
      }
      continue;
    }
    const a = cmd.args;
    if (!a || !METAL_TEXTURE_BINDS.has(cmd.method)) continue;
    note(refId(a.texture), cmd);
    if (Array.isArray(a.textures)) for (const t of a.textures) note(refId(t), cmd);
  }
  return out;
}
function imageOf(id, db) {
  const object = db.getObject(id);
  const d = object?.descriptor;
  if (!d) return null;
  if (d.image !== void 0) {
    const image = db.getObject(refId(d.image));
    return image?.descriptor ?? null;
  }
  return d;
}
function sizeOf(d) {
  const extent = isObject(d.extent) ? d.extent : null;
  const width = num(extent?.width) || num(d.width);
  const height = num(extent?.height) || num(d.height);
  return { pixels: width * height, levels: num(d.mipLevels) || num(d.mipmapLevelCount) || 1 };
}
function isRenderTarget(d) {
  const usage = str(d.usage);
  return usage.includes("RenderTarget") || usage.includes("ATTACHMENT_BIT");
}
function analyzeSampling(data, db) {
  const findings = [];
  const byCommand = /* @__PURE__ */ new Map();
  const commands = [];
  let first = null;
  let count2 = 0;
  for (const [id, cmd] of collectSampled(data)) {
    const d = imageOf(id, db);
    if (!d) continue;
    const { pixels, levels } = sizeOf(d);
    if (levels > 1 || pixels < LARGE_TEXTURE_PIXELS || isRenderTarget(d)) continue;
    if (first === null) first = cmd.index;
    count2++;
    if (commands.length < 64) commands.push(cmd.index);
  }
  if (count2) {
    const megapixels = LARGE_TEXTURE_PIXELS / (1024 * 1024);
    const f = {
      rule: "unmipped-texture",
      severity: "medium",
      confidence: "medium",
      message: `${count2} texture${count2 === 1 ? " is" : "s are"} sampled with a single mip level at ${megapixels} megapixel or more. Drawn smaller than itself, such a texture reads scattered texels and misses the cache on most of them; a mip chain costs a third more memory and reads one texel per sample.`,
      commandIndex: first ?? void 0,
      count: count2
    };
    findings.push(f);
    for (const index of commands) {
      const list = byCommand.get(index);
      if (list) list.push(f);
      else byCommand.set(index, [f]);
    }
  }
  return { findings, byCommand };
}

// src/renderer/vulkan/frame_analysis.ts
var TINY_DRAW_VERTICES2 = 12;
var TINY_DRAW_COUNT2 = 32;
var RULE_ORDER3 = [
  "stereo-without-multiview",
  "clear-outside-pass",
  "depth-store",
  "msaa-store",
  "msaa-sampled",
  "barrier-in-render-pass",
  "color-load",
  "tiny-draws",
  "full-pipeline-barrier",
  "redundant-pipeline-bind",
  "redundant-descriptor-bind",
  "redundant-buffer-bind",
  "push-constants-unchanged",
  "barrier-adjacent",
  "single-workgroup-dispatch",
  "depth-transient"
];
function barrierStageMasks(a) {
  if (isObject(a.pDependencyInfo)) {
    const out = [];
    for (const key of ["pMemoryBarriers", "pBufferMemoryBarriers", "pImageMemoryBarriers"]) {
      const list = a.pDependencyInfo[key];
      if (Array.isArray(list)) {
        for (const b of list) if (isObject(b)) out.push({ src: str(b.srcStageMask), dst: str(b.dstStageMask) });
      }
    }
    return out;
  }
  return [{ src: str(a.srcStageMask), dst: str(a.dstStageMask) }];
}
var BARRIER_METHODS3 = /* @__PURE__ */ new Set(["vkCmdPipelineBarrier", "vkCmdPipelineBarrier2", "vkCmdPipelineBarrier2KHR"]);
function argKey2(v) {
  if (isHandleRef(v)) return `#${v.__id}`;
  if (Array.isArray(v)) return `[${v.map(argKey2).join(",")}]`;
  if (isObject(v)) return `{${Object.entries(v).map(([k, e]) => `${k}:${argKey2(e)}`).join(",")}}`;
  return str(v);
}
var Folded4 = class {
  first = null;
  count = 0;
  commands = [];
  add(cmd) {
    if (!this.first) this.first = cmd;
    this.count++;
    if (this.commands.length < 64) this.commands.push(cmd);
  }
};
function collectImageViews(v, out) {
  if (Array.isArray(v)) {
    for (const e of v) collectImageViews(e, out);
  } else if (isHandleRef(v)) {
    if (v.__class === "VkImageView") out.add(v.__id);
  } else if (isObject(v)) {
    for (const e of Object.values(v)) collectImageViews(e, out);
  }
}
var FrameAnalysis = class {
  findings = [];
  _db;
  _passes = [];
  _lazyMemory = false;
  /** Every command a finding applies to (folded findings list all of theirs), by command index. */
  _byCommand = /* @__PURE__ */ new Map();
  /** The findings that apply to a command (a folded finding counts for each of its commands). */
  byCommand() {
    return this._byCommand;
  }
  constructor(db) {
    this._db = db;
    for (const o of db.allObjects.values()) {
      if (o.type !== "VkPhysicalDevice") continue;
      const mem = o.updates.memoryProperties;
      const types = isObject(mem) && Array.isArray(mem.memoryTypes) ? mem.memoryTypes : [];
      if (types.some((t) => isObject(t) && str(t.propertyFlags).includes("LAZILY_ALLOCATED"))) this._lazyMemory = true;
    }
  }
  analyze(data) {
    this.findings = [];
    this._passes = [];
    this._byCommand = /* @__PURE__ */ new Map();
    this._walk(data.commands);
    this._passRules();
    this._stereoRule();
    this.findings.sort((a, b) => SEVERITY_RANK[b.severity] - SEVERITY_RANK[a.severity] || RULE_ORDER3.indexOf(a.rule) - RULE_ORDER3.indexOf(b.rule));
    return this.findings;
  }
  // ---------------------------------------------------------------------------- the walk
  _walk(commands) {
    const db = this._db;
    const open = /* @__PURE__ */ new Map();
    const boundPipeline = /* @__PURE__ */ new Map();
    const clears = /* @__PURE__ */ new Map();
    const readImages = /* @__PURE__ */ new Set();
    const loadedImages = /* @__PURE__ */ new Set();
    const lastWrite = /* @__PURE__ */ new Map();
    let lastPass = null;
    const redundantBinds = new Folded4();
    const redundantSets = new Folded4();
    const redundantBuffers = new Folded4();
    const unchangedPush = new Folded4();
    const adjacentBarriers = new Folded4();
    const passBarriers = new Folded4();
    const fullBarriers = new Folded4();
    const singleDispatches = new Folded4();
    const tinyDraws = new Folded4();
    let draws = 0;
    const boundSets = /* @__PURE__ */ new Map();
    const boundBuffers = /* @__PURE__ */ new Map();
    const pushed = /* @__PURE__ */ new Map();
    const previous = /* @__PURE__ */ new Map();
    for (const cmd of commands) {
      const method = cmd.method;
      const a = cmd.args;
      const cb = cmd.object?.__id ?? 0;
      if (cmd.descriptors) {
        const views = /* @__PURE__ */ new Set();
        collectImageViews(cmd.descriptors, views);
        for (const v of views) {
          const img = this._imageOfView(v);
          if (img !== null) readImages.add(img);
        }
      }
      if (PASS_BEGIN2.has(method)) {
        const pass = this._passInfo(cmd, this._passes.length);
        if (!pass) continue;
        for (const att of pass.attachments) {
          if (att.imageId === null) continue;
          const loads = att.loadOp === "LOAD" || att.kind === "depth" && att.stencilLoadOp === "LOAD";
          if (loads) {
            loadedImages.add(att.imageId);
            const clear = clears.get(att.imageId);
            const writtenAt = lastWrite.get(att.imageId) ?? -1;
            if (clear && clear.index > writtenAt) pass.clearedBefore.set(att.imageId, clear);
            else if (!clear && writtenAt < 0 && att.kind === "color") {
              this._add("color-load", "low", "medium", `${this._passName(pass)} loads ${this._imageName(att.imageId)} (loadOp LOAD) before anything in the frame rendered to it, so it reads the previous frame's contents into tile memory. CLEAR or DONT_CARE skips that read when the pass covers the whole target.`, cmd);
            }
          }
          lastWrite.set(att.imageId, cmd.index);
        }
        open.set(cb, pass);
        lastPass = pass;
        this._passes.push(pass);
      } else if (PASS_END2.has(method)) {
        open.delete(cb);
      } else if (method === "vkCmdClearColorImage" || method === "vkCmdClearDepthStencilImage") {
        const id = refId(a?.image);
        if (id !== null) clears.set(id, cmd);
      } else if (method === "vkCmdBindPipeline" && a) {
        const key = `${cb}:${str(a.pipelineBindPoint)}`;
        const id = refId(a.pipeline) ?? 0;
        if (boundPipeline.get(key) === id) redundantBinds.add(cmd);
        boundPipeline.set(key, id);
      } else if ((method === "vkCmdBindDescriptorSets" || method === "vkCmdBindDescriptorSets2" || method === "vkCmdBindDescriptorSets2KHR") && a) {
        const info = isObject(a.pBindDescriptorSetsInfo) ? a.pBindDescriptorSetsInfo : a;
        const sets = Array.isArray(info.pDescriptorSets) ? info.pDescriptorSets : [];
        const offsets = Array.isArray(info.pDynamicOffsets) ? info.pDynamicOffsets.map(str) : [];
        const first = num(info.firstSet);
        const bindPoint = str(info.pipelineBindPoint ?? info.stageFlags);
        let same = sets.length > 0;
        let offsetAt = 0;
        sets.forEach((set, i) => {
          const own = i === sets.length - 1 ? offsets.slice(offsetAt).join(",") : "";
          const value = `${argKey2(set)}|${own}`;
          const key = `${cb}:${bindPoint}:${first + i}`;
          if (boundSets.get(key) !== value) same = false;
          boundSets.set(key, value);
        });
        offsetAt = offsets.length;
        if (same) redundantSets.add(cmd);
      } else if ((method === "vkCmdBindVertexBuffers" || method === "vkCmdBindVertexBuffers2" || method === "vkCmdBindVertexBuffers2EXT") && a) {
        const buffers = Array.isArray(a.pBuffers) ? a.pBuffers : [];
        const field2 = (name, i) => Array.isArray(a[name]) ? str(a[name][i]) : "";
        let same = buffers.length > 0;
        buffers.forEach((b, i) => {
          const key = `${cb}:v${num(a.firstBinding) + i}`;
          const value = `${argKey2(b)}|${field2("pOffsets", i)}|${field2("pSizes", i)}|${field2("pStrides", i)}`;
          if (boundBuffers.get(key) !== value) same = false;
          boundBuffers.set(key, value);
        });
        if (same) redundantBuffers.add(cmd);
      } else if ((method === "vkCmdBindIndexBuffer" || method === "vkCmdBindIndexBuffer2" || method === "vkCmdBindIndexBuffer2KHR") && a) {
        const key = `${cb}:index`;
        const value = `${argKey2(a.buffer)}|${str(a.offset)}|${str(a.size)}|${str(a.indexType)}`;
        if (boundBuffers.get(key) === value) redundantBuffers.add(cmd);
        boundBuffers.set(key, value);
      } else if ((method === "vkCmdPushConstants" || method === "vkCmdPushConstants2" || method === "vkCmdPushConstants2KHR") && a) {
        const info = isObject(a.pPushConstantsInfo) ? a.pPushConstantsInfo : a;
        const bytes = isObject(info.pValues) ? str(info.pValues.base64) : "";
        if (bytes) {
          const key = `${cb}:${argKey2(info.layout)}:${str(info.stageFlags)}:${num(info.offset)}:${num(info.size)}`;
          if (pushed.get(key) === bytes) unchangedPush.add(cmd);
          pushed.set(key, bytes);
        }
      } else if (BARRIER_METHODS3.has(method)) {
        if (BARRIER_METHODS3.has(previous.get(cb) ?? "")) adjacentBarriers.add(cmd);
        if (open.has(cb)) passBarriers.add(cmd);
        if (a && barrierStageMasks(a).some((m) => m.src.includes("ALL_COMMANDS") && m.dst.includes("ALL_COMMANDS"))) fullBarriers.add(cmd);
      } else if ((method === "vkCmdDispatch" || method === "vkCmdDispatchBase" || method === "vkCmdDispatchBaseKHR") && a) {
        if (num(a.groupCountX) * num(a.groupCountY) * num(a.groupCountZ) === 1) singleDispatches.add(cmd);
      } else if (DRAW_METHODS.has(method)) {
        draws++;
        const pass = open.get(cb) ?? lastPass;
        const pipeline = boundPipeline.get(`${cb}:VK_PIPELINE_BIND_POINT_GRAPHICS`) ?? 0;
        const vertices = method.includes("Indirect") || method.includes("MeshTasks") ? -1 : num(a?.indexCount || a?.vertexCount) * Math.max(1, num(a?.instanceCount));
        if (pass) {
          pass.draws++;
          pass.drawSignature.push(`${pipeline}:${vertices}`);
        }
        if (vertices >= 0 && vertices <= TINY_DRAW_VERTICES2) tinyDraws.add(cmd);
      } else if (a) {
        for (const key of ["srcImage", "pCopyImageInfo", "pBlitImageInfo", "pResolveImageInfo", "pCopyImageToBufferInfo"]) {
          const v = a[key];
          const id = refId(isObject(v) ? v.srcImage : v);
          if (id !== null) readImages.add(id);
        }
      }
      if (!method.includes("DebugUtilsLabel") && !method.includes("DebugMarker")) previous.set(cb, method);
    }
    for (const pass of this._passes) {
      for (const att of pass.attachments) {
        if (att.imageId === null) continue;
        att.usage = this._imageUsage(att.imageId);
        att.read = readImages.has(att.imageId) || loadedImages.has(att.imageId);
      }
    }
    const times = (f) => f.count === 1 ? "once" : `${f.count} times`;
    if (redundantBinds.count) this._addFolded("redundant-pipeline-bind", "low", "high", `vkCmdBindPipeline binds the pipeline that is already bound ${times(redundantBinds)}. Drivers do not always skip the redundant bind; binding once per pipeline change is free.`, redundantBinds);
    if (redundantSets.count) this._addFolded("redundant-descriptor-bind", "low", "high", `vkCmdBindDescriptorSets binds the descriptor sets already bound at those set numbers, with the same dynamic offsets, ${times(redundantSets)}. Binding once per change saves the command and the driver's descriptor work.`, redundantSets);
    if (redundantBuffers.count) this._addFolded("redundant-buffer-bind", "low", "high", `The vertex or index buffers already bound (same buffers, offsets and sizes) are bound again ${times(redundantBuffers)}.`, redundantBuffers);
    if (unchangedPush.count) this._addFolded("push-constants-unchanged", "low", "high", `vkCmdPushConstants pushes the bytes that range already holds ${times(unchangedPush)}. Pushing only what changed saves the command and the constant update.`, unchangedPush);
    if (adjacentBarriers.count) this._addFolded("barrier-adjacent", "low", "medium", `A pipeline barrier directly follows another ${times(adjacentBarriers)}: nothing is recorded between them, so one barrier carrying both sets of transitions and stage masks would do, and each barrier can drain the pipeline.`, adjacentBarriers);
    if (fullBarriers.count) this._addFolded("full-pipeline-barrier", "low", "medium", `A barrier waits for every stage and blocks every stage (ALL_COMMANDS to ALL_COMMANDS) ${times(fullBarriers)}: the GPU drains completely before it continues. Naming the stages that produce and consume the data lets the rest overlap.`, fullBarriers);
    if (passBarriers.count) this._addFolded("barrier-in-render-pass", "medium", "medium", `A pipeline barrier is recorded inside a render pass ${times(passBarriers)}. On a tiled GPU a barrier inside a pass forces the tiles to be flushed and reloaded; move the dependency to a subpass dependency or before the pass.`, passBarriers);
    if (singleDispatches.count) this._addFolded("single-workgroup-dispatch", "low", "medium", `vkCmdDispatch launches a single workgroup ${times(singleDispatches)}: most of the GPU idles during it. Larger dispatches, or a dispatch that folds the work of several small ones, use the machine.`, singleDispatches);
    if (tinyDraws.count >= TINY_DRAW_COUNT2) this._addFolded("tiny-draws", "medium", "medium", `${tinyDraws.count} of ${draws} draws render at most ${TINY_DRAW_VERTICES2} vertices each. Per-draw overhead (command processing, state changes) outweighs such draws; instancing or merged geometry renders them in one draw.`, tinyDraws);
  }
  // ---------------------------------------------------------------------------- per-pass rules
  _passRules() {
    for (const pass of this._passes) {
      const cmd = pass.command;
      for (const [imageId, clear] of pass.clearedBefore) {
        this._add("clear-outside-pass", "high", "high", `${this._imageName(imageId)} is cleared with ${clear.method} and then loaded by ${this._passName(pass)} (loadOp LOAD). On a tiled GPU the clear is a separate pass over the whole image and the load reads it back into tile memory; loadOp CLEAR clears it for free when the pass begins.`, clear);
      }
      for (const att of pass.attachments) {
        const read = att.read ?? false;
        const canBeRead = /SAMPLED|INPUT_ATTACHMENT|STORAGE/.test(att.usage);
        if (att.kind === "depth") {
          const stored = att.storeOp === "STORE" || att.format.includes("S8") && att.stencilStoreOp === "STORE";
          if (stored && !read && att.usage !== "") {
            if (canBeRead) {
              this._add("depth-store", "low", "low", `${this._passName(pass)} stores its depth attachment ${this._imageName(att.imageId)} (storeOp STORE). Nothing in the captured frame reads it, but the image can be sampled or copied; if no later frame reads it either, storeOp DONT_CARE saves writing it back from tile memory.`, cmd);
            } else {
              this._add("depth-store", "medium", "high", `${this._passName(pass)} stores its depth attachment ${this._imageName(att.imageId)} (storeOp STORE) although the image is only ever a depth attachment: nothing can read it back. storeOp DONT_CARE keeps a tiled GPU from writing the depth tile to memory at the end of the pass.`, cmd);
            }
          } else if (!stored && att.loadOp !== "LOAD" && att.usage !== "" && !att.usage.includes("TRANSIENT_ATTACHMENT") && !canBeRead) {
            this._add("depth-transient", this._lazyMemory ? "low" : "info", this._lazyMemory ? "high" : "medium", `The depth attachment ${this._imageName(att.imageId)} of ${this._passName(pass)} is neither loaded nor stored, so it never needs memory outside the tile: VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT with LAZILY_ALLOCATED memory lets a tiled GPU skip allocating it${this._lazyMemory ? " (this device has a lazily allocated memory type)" : ""}.`, cmd);
          }
        } else if (att.kind === "color" && att.samples > 1 && att.resolved && att.storeOp === "STORE") {
          this._add("msaa-store", "medium", "high", `${this._passName(pass)} stores the ${att.samples}x multisampled attachment ${this._imageName(att.imageId)} (storeOp STORE) although the pass resolves it: the resolve target holds the result, so DONT_CARE saves writing ${att.samples} samples per pixel.`, cmd);
        } else if (att.kind === "color" && att.samples > 1 && !att.resolved && att.storeOp === "STORE" && att.usage.includes("SAMPLED")) {
          this._add("msaa-sampled", "low", "medium", `${this._passName(pass)} stores the ${att.samples}x multisampled attachment ${this._imageName(att.imageId)} without resolving it, and the image can be sampled: sampling a multisampled image costs ${att.samples} fetches per texel, while a resolve attachment on the pass produces the single-sampled result in tile memory.`, cmd);
        }
      }
    }
  }
  /** Passes without multiview that render the same draws to same-sized targets: one per eye. */
  _stereoRule() {
    const groups = /* @__PURE__ */ new Map();
    for (const pass of this._passes) {
      if (pass.draws === 0 || pass.viewMask > 1) continue;
      const formats = pass.attachments.map((a) => `${a.kind}:${a.format}:${a.samples}`).join(",");
      const key = `${pass.width}x${pass.height}|${formats}|${pass.drawSignature.join(",")}`;
      const g = groups.get(key);
      if (g) g.push(pass);
      else groups.set(key, [pass]);
    }
    for (const g of groups.values()) {
      if (g.length < 2) continue;
      const targets = new Set(g.map((p) => p.attachments.filter((a) => a.kind === "color").map((a) => `${a.imageId}/${a.baseLayer}`).join(",")));
      if (targets.size < 2) continue;
      const first = g[0];
      const pairs = g.length === 2 ? `${this._passName(g[0])} and ${this._passName(g[1])}` : `${g.length} passes starting with ${this._passName(first)}`;
      const f = this._add("stereo-without-multiview", "medium", "medium", `${pairs} record the same ${first.draws} draw${first.draws === 1 ? "" : "s"} with the same pipelines into different ${first.width}x${first.height} targets, which looks like one pass per eye. With multiview (VK_KHR_multiview: a view mask on the render pass or on vkCmdBeginRendering, gl_ViewIndex in the shaders) both eyes render in one pass: half the commands, and the GPU can share the vertex work between the views.`, first.command, g.length);
      for (const p of g.slice(1)) this._attach(p.command.index, f);
    }
  }
  // ---------------------------------------------------------------------------- pass decoding
  _passInfo(cmd, ordinal) {
    const decoded = decodePass(cmd, this._db);
    if (!decoded) return null;
    return { command: cmd, ordinal, ...decoded, draws: 0, drawSignature: [], clearedBefore: /* @__PURE__ */ new Map() };
  }
  _imageOfView(viewId) {
    return imageOfView(this._db, viewId);
  }
  _imageUsage(imageId) {
    const image = this._db.getObject(imageId);
    return str(image?.descriptor?.usage);
  }
  _imageName(imageId) {
    const o = this._db.getObject(imageId);
    return o ? o.name : "the image";
  }
  _passName(pass) {
    const begin = isObject(pass.command.args?.pRenderPassBegin) ? pass.command.args.pRenderPassBegin : null;
    const rp = this._db.getObject(refId(begin?.renderPass));
    return `pass ${pass.ordinal}${rp?.label ? ` (${rp.label})` : ""}`;
  }
  _add(rule, severity, confidence, message, cmd, count2 = 1) {
    const f = { rule, severity, confidence, message, commandIndex: cmd?.index, count: count2 };
    this.findings.push(f);
    if (cmd) this._attach(cmd.index, f);
    return f;
  }
  _addFolded(rule, severity, confidence, message, folded) {
    const f = this._add(rule, severity, confidence, message, folded.first, folded.count);
    for (const cmd of folded.commands) if (cmd !== folded.first) this._attach(cmd.index, f);
  }
  _attach(index, f) {
    const list = this._byCommand.get(index);
    if (list) list.push(f);
    else this._byCommand.set(index, [f]);
  }
};
function analyzeFrame(data, db, graph) {
  const base = data.api === "metal" ? analyzeMetalFrame(data, db) : perCommandAnalysis(data, db);
  const sources = [base, analyzeCounters(data, db), analyzeSampling(data, db)];
  if (graph) sources.push(analyzeRenderGraph(graph));
  const findings = [];
  const byCommand = /* @__PURE__ */ new Map();
  for (const source of sources) {
    const drop = graph && source === base ? SUPERSEDED_RULES : null;
    for (const f of source.findings) {
      if (!drop || !drop.has(f.rule)) findings.push(f);
    }
    for (const [index, list] of source.byCommand) {
      const kept = drop ? list.filter((f) => !drop.has(f.rule)) : list;
      if (!kept.length) continue;
      const existing = byCommand.get(index);
      if (existing) existing.push(...kept);
      else byCommand.set(index, [...kept]);
    }
  }
  findings.sort((a, b) => SEVERITY_RANK[b.severity] - SEVERITY_RANK[a.severity]);
  return { findings, byCommand };
}
function perCommandAnalysis(data, db) {
  const analysis = new FrameAnalysis(db);
  const findings = analysis.analyze(data);
  return { findings, byCommand: analysis.byCommand() };
}

// src/renderer/vulkan/object_database.ts
var HELD_REFERENCES = {
  VkImageView: /* @__PURE__ */ new Set(["VkImage"]),
  VkBufferView: /* @__PURE__ */ new Set(["VkBuffer"]),
  VkFramebuffer: /* @__PURE__ */ new Set(["VkImageView"]),
  VkDescriptorSet: /* @__PURE__ */ new Set(["VkImageView", "VkSampler", "VkBuffer", "VkBufferView"]),
  VkSwapchainKHR: /* @__PURE__ */ new Set(["VkSurfaceKHR"])
};
var ObjectDatabase = class {
  allObjects = /* @__PURE__ */ new Map();
  // live objects
  destroyedObjects = /* @__PURE__ */ new Map();
  // destroyed but still referenced by live objects
  objectsByType = /* @__PURE__ */ new Map();
  objectsByHandle = /* @__PURE__ */ new Map();
  // "VkImage:0x..." -> most recent object
  frameIndex = 0;
  frameTimeMs = 0;
  /** CPU time per frame inside vkQueueSubmit, from the last FrameStats. */
  submitMs = 0;
  /** Display refresh interval while vsync is on (0 without), the present mode, and dropped frames. */
  refreshMs = 0;
  refreshSource = "";
  displayRefreshMs = 0;
  presentMode = "";
  frameBoundary = "";
  droppedFrames = 0;
  // in the last reporting interval
  droppedFramesTotal = 0;
  // since the connection
  inspectedObject = null;
  /** Ids of the objects referenced by the most recent capture (for the object list filter). */
  capturedObjects = /* @__PURE__ */ new Set();
  /**
   * Memory totals of the live objects (see objectMemoryBytes): allocations (Metal: heaps),
   * buffers, images; and for Metal what the device reports allocated and recommends as the
   * working set, from the last FrameStats.
   */
  memory = { device: 0, allocations: 0, buffers: 0, images: 0, reported: 0, workingSet: 0 };
  /** Binary payloads received (ObjectBlob) or loaded from a capture file, keyed "id:index". */
  blobData = /* @__PURE__ */ new Map();
  /** Validation messages in arrival order, and by the objects they name. */
  validation = [];
  validationByKey = /* @__PURE__ */ new Map();
  validationByObject = /* @__PURE__ */ new Map();
  /** Messages by the command they fired on: "commandBuffer:slot" (see validationForCommand). */
  validationByCommand = /* @__PURE__ */ new Map();
  /** Unique messages the layer dropped after its cap. */
  validationDropped = 0;
  /** Leak reports (objects alive when their device or instance was destroyed), in arrival order. */
  leaks = [];
  _snapshotRemaining = 0;
  onReset = new Signal();
  onSnapshotBegin = new Signal();
  onAddObject = new Signal();
  onDeleteObject = new Signal();
  onObjectLabelChanged = new Signal();
  onObjectInvalidated = new Signal();
  onObjectUpdated = new Signal();
  onFrameStats = new Signal();
  onObjectBlob = new Signal();
  /** Messages not handled here (capture data) are forwarded to whoever listens. */
  onOtherMessage = new Signal();
  onCapturedObjectsChanged = new Signal();
  /** A validation message arrived (isNew) or its repeat count changed. */
  onValidationMessage = new Signal();
  onLeakReport = new Signal();
  /** Stack traces: creation stacks by object id, symbols by address, and whether the layer collects stacks. */
  stacks = /* @__PURE__ */ new Map();
  stacksAvailable = null;
  symbols = /* @__PURE__ */ new Map();
  onStacktraces = new Signal();
  onSymbols = new Signal();
  /** Leaked objects over every report. */
  get leakCount() {
    return this.leaks.reduce((n, r) => n + r.count, 0);
  }
  /** Validation errors and warnings by severity: [errors, warnings]. */
  get validationCounts() {
    let errors = 0;
    let warnings = 0;
    for (const v of this.validation) {
      if (v.severity === "error") errors++;
      else if (v.severity === "warning") warnings++;
    }
    return [errors, warnings];
  }
  /** Validation messages naming an object. */
  validationFor(id) {
    return this.validationByObject.get(id) ?? [];
  }
  /** Validation messages that fired while a command was recorded: the command buffer's id and the command's slot. */
  validationForCommand(commandBufferId, slot) {
    if (commandBufferId === void 0 || slot === void 0) return [];
    return this.validationByCommand.get(`${commandBufferId}:${slot}`) ?? [];
  }
  _indexByCommand(msg, add) {
    if (!msg.command) return;
    const key = `${msg.command.commandBuffer}:${msg.command.slot}`;
    const list = this.validationByCommand.get(key) ?? [];
    if (add) {
      if (!list.includes(msg)) list.push(msg);
      this.validationByCommand.set(key, list);
    } else {
      const i = list.indexOf(msg);
      if (i >= 0) list.splice(i, 1);
    }
  }
  _addValidation(msg) {
    const existing = this.validationByKey.get(msg.key);
    if (existing) {
      existing.count = msg.count;
      if (msg.command && (existing.command?.commandBuffer !== msg.command.commandBuffer || existing.command?.slot !== msg.command.slot)) {
        this._indexByCommand(existing, false);
        existing.command = msg.command;
        this._indexByCommand(existing, true);
      }
      this.onValidationMessage.emit(existing, false);
      return;
    }
    this.validation.push(msg);
    this.validationByKey.set(msg.key, msg);
    this._indexByCommand(msg, true);
    for (const o of msg.objects ?? []) {
      if (!o.object || !isHandleRef(o.object)) continue;
      const list = this.validationByObject.get(o.object.__id) ?? [];
      if (!list.includes(msg)) list.push(msg);
      this.validationByObject.set(o.object.__id, list);
    }
    this.onValidationMessage.emit(msg, true);
  }
  /** Validation messages of a capture file. */
  loadValidation(entries) {
    for (const e of entries) this._addValidation(e);
  }
  /** Records the objects a capture referenced (every {__id} in its commands). */
  setCapturedObjects(ids) {
    this.capturedObjects = ids;
    this.onCapturedObjectsChanged.emit();
  }
  /** Every {__id} reference inside a value, recursively. */
  collectReferences(value, into) {
    if (value === null || value === void 0 || typeof value !== "object") return;
    if (Array.isArray(value)) {
      for (const v of value) this.collectReferences(v, into);
      return;
    }
    const rec = value;
    if (typeof rec.__id === "number") {
      into.add(rec.__id);
      return;
    }
    for (const key in rec) this.collectReferences(rec[key], into);
  }
  reset() {
    this.allObjects = /* @__PURE__ */ new Map();
    this.destroyedObjects = /* @__PURE__ */ new Map();
    this.objectsByType = /* @__PURE__ */ new Map();
    this.objectsByHandle = /* @__PURE__ */ new Map();
    this.frameIndex = 0;
    this.frameTimeMs = 0;
    this.submitMs = 0;
    this.refreshMs = 0;
    this.refreshSource = "";
    this.displayRefreshMs = 0;
    this.presentMode = "";
    this.frameBoundary = "";
    this.droppedFrames = 0;
    this.droppedFramesTotal = 0;
    this.inspectedObject = null;
    this.capturedObjects = /* @__PURE__ */ new Set();
    this.memory = { device: 0, allocations: 0, buffers: 0, images: 0, reported: 0, workingSet: 0 };
    this.blobData = /* @__PURE__ */ new Map();
    this.validation = [];
    this.validationByKey = /* @__PURE__ */ new Map();
    this.validationByObject = /* @__PURE__ */ new Map();
    this.validationByCommand = /* @__PURE__ */ new Map();
    this.validationDropped = 0;
    this.stacks = /* @__PURE__ */ new Map();
    this.stacksAvailable = null;
    this.symbols = /* @__PURE__ */ new Map();
    this.leaks = [];
    this._snapshotRemaining = 0;
  }
  /**
   * Populates the database from a capture file's object graph (see capture_file.ts): the same
   * path as a live snapshot, then the objects destroyed before the save become ghosts without
   * the destroy cascade, so every link of the loaded capture still resolves.
   */
  loadObjects(objects, blobs, stats) {
    this.reset();
    this._snapshotRemaining = objects.length;
    this.onReset.emit();
    this.onSnapshotBegin.emit(objects.length);
    for (const rec of objects) {
      this._addObject({
        action: "AddObject",
        id: rec.id,
        parent: rec.parent,
        type: rec.type,
        cmd: rec.cmd,
        index: rec.index,
        handle: rec.handle,
        label: rec.label,
        args: rec.args,
        blobs: rec.blobs.map((b) => ({ name: b.name, size: b.size }))
      });
      const o = this.allObjects.get(rec.id);
      if (!o) continue;
      o.updates = rec.updates ?? {};
      this._collectReferences(o.updates, (id) => {
        const dep = this.getObject(id);
        if (dep && dep !== o) {
          o.dependencies.add(dep);
          dep.dependents.add(o);
        }
      });
    }
    for (const rec of objects) {
      if (!rec.deleted) continue;
      const o = this.allObjects.get(rec.id);
      if (!o) continue;
      o.isDeleted = true;
      this._accountMemory(o, -1);
      this.allObjects.delete(o.id);
      this.objectsByType.get(o.type)?.delete(o.id);
      if (this.objectsByHandle.get(`${o.type}:${o.handle}`) === o) this.objectsByHandle.delete(`${o.type}:${o.handle}`);
      this.destroyedObjects.set(o.id, o);
      this.onDeleteObject.emit(o.id, o);
    }
    for (const [key, data] of blobs) this.blobData.set(key, data);
    this.frameIndex = stats.frame;
    this.frameTimeMs = stats.frameTimeMs;
    this.submitMs = stats.submitMs;
    this.refreshMs = stats.refreshMs ?? 0;
    this.refreshSource = stats.refreshSource ?? "";
    this.displayRefreshMs = stats.displayRefreshMs ?? 0;
    this.frameBoundary = stats.frameBoundary ?? "";
    this.onFrameStats.emit({
      action: "FrameStats",
      frame: stats.frame,
      frameTimeMs: stats.frameTimeMs,
      submitMs: stats.submitMs,
      refreshMs: this.refreshMs,
      refreshSource: this.refreshSource,
      displayRefreshMs: this.displayRefreshMs,
      frameBoundary: this.frameBoundary
    });
  }
  _accountMemory(o, sign) {
    const bytes = objectMemoryBytes(o, this);
    if (o.type === "VkDeviceMemory" || o.type === "MTLHeap") {
      this.memory.device += sign * bytes;
      this.memory.allocations += sign;
    } else if (o.type === "VkBuffer" || o.type === "MTLBuffer") {
      this.memory.buffers += sign * bytes;
    } else if (o.type === "VkImage" || o.type === "MTLTexture") {
      this.memory.images += sign * bytes;
    }
  }
  getObject(id) {
    if (id === void 0 || id === null) return null;
    return this.allObjects.get(id) ?? this.destroyedObjects.get(id) ?? null;
  }
  getObjectByHandle(type, handle) {
    return this.objectsByHandle.get(`${type}:${handle}`) ?? null;
  }
  getObjectsOfType(type) {
    return this.objectsByType.get(type) ?? null;
  }
  handleMessage(msg) {
    switch (msg.action) {
      case "Snapshot":
        this.reset();
        this._snapshotRemaining = msg.count;
        this.onReset.emit();
        this.onSnapshotBegin.emit(msg.count);
        break;
      case "AddObject":
        this._addObject(msg);
        break;
      case "DeleteObjects":
        for (const id of msg.ids) this._deleteObject(id);
        break;
      case "ObjectSetLabel": {
        const o = this.getObject(msg.id);
        if (o) {
          o.label = msg.label ?? "";
          this.onObjectLabelChanged.emit(o.id, o, o.label);
        }
        break;
      }
      case "FrameStats":
        this.frameIndex = msg.frame;
        this.frameTimeMs = msg.frameTimeMs;
        this.submitMs = msg.submitMs ?? 0;
        this.refreshMs = msg.refreshMs ?? 0;
        this.refreshSource = msg.refreshSource ?? "";
        this.displayRefreshMs = msg.displayRefreshMs ?? 0;
        this.presentMode = msg.presentMode ?? "";
        this.frameBoundary = msg.frameBoundary ?? "";
        this.droppedFrames = msg.dropped ?? 0;
        this.droppedFramesTotal = msg.droppedTotal ?? this.droppedFramesTotal + (msg.dropped ?? 0);
        if (msg.allocatedBytes !== void 0) this.memory.reported = msg.allocatedBytes;
        if (msg.workingSetBytes !== void 0) this.memory.workingSet = msg.workingSetBytes;
        this.onFrameStats.emit(msg);
        break;
      case "ObjectBlob":
        if (msg.__binary) this.blobData.set(`${msg.id}:${msg.index ?? 0}`, msg.__binary);
        this.onObjectBlob.emit(msg.id, msg.index ?? 0, msg.__binary ?? null);
        break;
      case "ValidationMessage":
        this._addValidation(msg);
        break;
      case "Stacktraces":
        this.stacksAvailable = msg.available;
        for (const s of msg.stacks ?? []) this.stacks.set(s.id, s.frames ?? []);
        this.onStacktraces.emit();
        break;
      case "Symbols":
        for (const f of msg.frames ?? []) if (f && f.address) this.symbols.set(f.address, f);
        this.onSymbols.emit();
        break;
      case "LeakReport":
        this.leaks.push(msg);
        this.onLeakReport.emit(msg);
        break;
      case "ValidationCount":
        for (const [key, count2] of msg.counts ?? []) {
          const e = this.validationByKey.get(key);
          if (e && e.count !== count2) {
            e.count = count2;
            this.onValidationMessage.emit(e, false);
          }
        }
        if (msg.dropped !== void 0) this.validationDropped = msg.dropped;
        break;
      case "ObjectBlobs": {
        const o = this.getObject(msg.id);
        if (o) {
          o.blobs = msg.blobs ?? [];
          this.onObjectUpdated.emit(o.id, o);
        }
        break;
      }
      case "ObjectUpdate": {
        const o = this.getObject(msg.id);
        if (o) {
          for (const key in msg) {
            if (key !== "action" && key !== "id") o.updates[key] = msg[key];
          }
          this._collectReferences(msg, (id) => {
            const dep = this.getObject(id);
            if (dep && dep !== o) {
              o.dependencies.add(dep);
              dep.dependents.add(o);
            }
          });
          this.onObjectUpdated.emit(o.id, o);
        }
        break;
      }
      default:
        this.onOtherMessage.emit(msg);
        break;
    }
  }
  _addObject(msg) {
    const o = new VulkanObject(msg);
    this.allObjects.set(o.id, o);
    let map = this.objectsByType.get(o.type);
    if (!map) {
      map = /* @__PURE__ */ new Map();
      this.objectsByType.set(o.type, map);
    }
    map.set(o.id, o);
    this.objectsByHandle.set(`${o.type}:${o.handle}`, o);
    this._accountMemory(o, 1);
    this._collectReferences(o.args, (id) => {
      if (id === o.id) return;
      const dep = this.getObject(id);
      if (dep) {
        o.dependencies.add(dep);
        dep.dependents.add(o);
      }
    });
    if (this._snapshotRemaining > 0) this._snapshotRemaining--;
    this.onAddObject.emit(o, this._snapshotRemaining > 0);
  }
  _collectReferences(value, cb) {
    if (value === null || value === void 0 || typeof value !== "object") return;
    if (Array.isArray(value)) {
      for (const v of value) this._collectReferences(v, cb);
      return;
    }
    if (isHandleRef(value)) {
      cb(value.__id);
      return;
    }
    for (const key in value) this._collectReferences(value[key], cb);
  }
  _deleteObject(id) {
    const o = this.allObjects.get(id);
    if (!o) return;
    o.isDeleted = true;
    this._accountMemory(o, -1);
    this.allObjects.delete(id);
    this.objectsByType.get(o.type)?.delete(id);
    if (this.objectsByHandle.get(`${o.type}:${o.handle}`) === o) this.objectsByHandle.delete(`${o.type}:${o.handle}`);
    for (const dep of o.dependencies) {
      dep.dependents.delete(o);
      if (dep.isDeleted && dep.dependents.size === 0) this.destroyedObjects.delete(dep.id);
    }
    o.dependencies.clear();
    let referenced = false;
    for (const dependent of o.dependents) {
      if (dependent.isDeleted) continue;
      referenced = true;
      const held = HELD_REFERENCES[dependent.type];
      if (held && held.has(o.type) && dependent.parentId !== id) {
        dependent.invalidReason = `references destroyed ${o.type} ${o.id}`;
        this.onObjectInvalidated.emit(dependent.id, dependent, dependent.invalidReason);
      }
    }
    if (referenced) this.destroyedObjects.set(id, o);
    else o.dependents.clear();
    if (this.inspectedObject === o) this.inspectedObject = null;
    this.onDeleteObject.emit(o.id, o);
  }
};

// src/renderer/vulkan/spirv_reflect.ts
var ShaderReflection = class {
  entryPoints = [];
  resources = [];
  pushConstants = [];
  /** SPIR-V version as "1.5". */
  version = "";
  findResource(set, binding) {
    return this.resources.find((r) => r.set === set && r.binding === binding) ?? null;
  }
  entryPoint(name) {
    if (name) {
      const e = this.entryPoints.find((ep) => ep.name === name);
      if (e) return e;
    }
    return this.entryPoints[0] ?? null;
  }
};
function scalarName(s) {
  switch (s.base) {
    case "bool":
      return "bool";
    case "float":
      return s.width === 16 ? "float16_t" : s.width === 64 ? "double" : "float";
    case "int":
      return s.width === 32 ? "int" : `int${s.width}_t`;
    case "uint":
      return s.width === 32 ? "uint" : `uint${s.width}_t`;
  }
}
function vectorPrefix(s) {
  switch (s.base) {
    case "bool":
      return "bvec";
    case "float":
      return s.width === 16 ? "f16vec" : s.width === 64 ? "dvec" : "vec";
    case "int":
      return s.width === 32 ? "ivec" : `i${s.width}vec`;
    case "uint":
      return s.width === 32 ? "uvec" : `u${s.width}vec`;
  }
}
function typeName(t) {
  if (!t) return "";
  switch (t.kind) {
    case "scalar":
      return scalarName(t);
    case "vector":
      return `${vectorPrefix(t.element)}${t.count}`;
    case "matrix": {
      const p = t.element.width === 64 ? "dmat" : t.element.width === 16 ? "f16mat" : "mat";
      return t.columns === t.rows ? `${p}${t.columns}` : `${p}${t.columns}x${t.rows}`;
    }
    case "array":
      return `${typeName(t.element)}[${t.count || ""}]`;
    case "struct":
      return t.name || "struct";
    case "opaque":
      return t.name;
    case "format":
      return t.format.replace(/^VK_FORMAT_/, "");
  }
}
var STAGES2 = {
  0: "vertex",
  1: "tess_control",
  2: "tess_eval",
  3: "geometry",
  4: "fragment",
  5: "compute",
  5267: "task",
  5268: "mesh",
  5313: "raygen",
  5314: "intersection",
  5315: "any_hit",
  5316: "closest_hit",
  5317: "miss",
  5318: "callable",
  5364: "task",
  5365: "mesh"
};
function readString3(words2, start, end) {
  const bytes = [];
  for (let i = start; i < end; i++) {
    const w = words2[i];
    for (let b = 0; b < 4; b++) {
      const c2 = w >>> b * 8 & 255;
      if (c2 === 0) return { text: new TextDecoder().decode(new Uint8Array(bytes)), next: i + 1 };
      bytes.push(c2);
    }
  }
  return { text: new TextDecoder().decode(new Uint8Array(bytes)), next: end };
}
var Parser = class {
  names = /* @__PURE__ */ new Map();
  memberNames = /* @__PURE__ */ new Map();
  /** OpString text by id: what the debug instructions name things with. */
  strings = /* @__PURE__ */ new Map();
  debugSet = 0;
  /** DebugTypeComposite: its name, and the DebugTypeMember ids of its fields in declaration order. */
  debugComposites = /* @__PURE__ */ new Map();
  /** DebugTypeMember id -> the OpString id of its name. */
  debugMembers = /* @__PURE__ */ new Map();
  debugGlobals = [];
  decorations = /* @__PURE__ */ new Map();
  memberDecorations = /* @__PURE__ */ new Map();
  types = /* @__PURE__ */ new Map();
  constants = /* @__PURE__ */ new Map();
  variables = [];
  entries = [];
  localSize = /* @__PURE__ */ new Map();
  localSizeIds = /* @__PURE__ */ new Map();
  _cache = /* @__PURE__ */ new Map();
  parse(words2) {
    let i = 5;
    while (i < words2.length) {
      const w = words2[i];
      const op = w & 65535;
      const len = w >>> 16;
      if (len === 0) break;
      const end = Math.min(words2.length, i + len);
      this._instruction(op, words2, i + 1, end);
      if (op === 54 /* Function */) break;
      i += len;
    }
  }
  _instruction(op, words2, a, end) {
    const operands = () => Array.from(words2.subarray(a, end));
    switch (op) {
      case 5 /* Name */:
        this.names.set(words2[a], readString3(words2, a + 1, end).text);
        break;
      case 6 /* MemberName */: {
        let m = this.memberNames.get(words2[a]);
        if (!m) {
          m = /* @__PURE__ */ new Map();
          this.memberNames.set(words2[a], m);
        }
        m.set(words2[a + 1], readString3(words2, a + 2, end).text);
        break;
      }
      case 7 /* String */:
        this.strings.set(words2[a], readString3(words2, a + 1, end).text);
        break;
      case 11 /* ExtInstImport */:
        if (readString3(words2, a + 1, end).text === "NonSemantic.Shader.DebugInfo.100") this.debugSet = words2[a];
        break;
      case 12 /* ExtInst */: {
        if (!this.debugSet || words2[a + 2] !== this.debugSet) break;
        const o = a + 4;
        switch (words2[a + 3]) {
          // Name, Tag, Source, Line, Column, Parent, LinkageName, Size, Flags, then the members.
          case 10 /* TypeComposite */:
            if (o + 9 <= end) this.debugComposites.set(words2[a + 1], { name: words2[o], members: Array.from(words2.subarray(o + 9, end)) });
            break;
          case 11 /* TypeMember */:
            this.debugMembers.set(words2[a + 1], words2[o]);
            break;
          // Name, Type, Source, Line, Column, Parent, LinkageName, Variable, Flags.
          case 18 /* GlobalVariable */:
            if (o + 8 <= end) this.debugGlobals.push({ name: words2[o], type: words2[o + 1], variable: words2[o + 7] });
            break;
          default:
            break;
        }
        break;
      }
      case 15 /* EntryPoint */: {
        const s = readString3(words2, a + 2, end);
        this.entries.push({ model: words2[a], id: words2[a + 1], name: s.text, interfaces: Array.from(words2.subarray(s.next, end)) });
        break;
      }
      case 16 /* ExecutionMode */:
        if (words2[a + 1] === 17) this.localSize.set(words2[a], [words2[a + 2], words2[a + 3], words2[a + 4]]);
        break;
      case 331 /* ExecutionModeId */:
        if (words2[a + 1] === 38) this.localSizeIds.set(words2[a], [words2[a + 2], words2[a + 3], words2[a + 4]]);
        break;
      case 19 /* TypeVoid */:
      case 20 /* TypeBool */:
      case 21 /* TypeInt */:
      case 22 /* TypeFloat */:
      case 23 /* TypeVector */:
      case 24 /* TypeMatrix */:
      case 25 /* TypeImage */:
      case 26 /* TypeSampler */:
      case 27 /* TypeSampledImage */:
      case 28 /* TypeArray */:
      case 29 /* TypeRuntimeArray */:
      case 30 /* TypeStruct */:
      case 32 /* TypePointer */:
      case 5341 /* TypeAccelerationStructureKHR */:
        this.types.set(words2[a], { op, operands: operands() });
        break;
      case 43 /* Constant */:
      case 50 /* SpecConstant */:
        this.constants.set(words2[a + 1], words2[a + 2]);
        break;
      case 41 /* ConstantTrue */:
      case 48 /* SpecConstantTrue */:
        this.constants.set(words2[a + 1], 1);
        break;
      case 42 /* ConstantFalse */:
      case 49 /* SpecConstantFalse */:
        this.constants.set(words2[a + 1], 0);
        break;
      case 59 /* Variable */:
        this.variables.push({ typeId: words2[a], id: words2[a + 1], storageClass: words2[a + 2] });
        break;
      case 71 /* Decorate */: {
        let m = this.decorations.get(words2[a]);
        if (!m) {
          m = /* @__PURE__ */ new Map();
          this.decorations.set(words2[a], m);
        }
        m.set(words2[a + 1], Array.from(words2.subarray(a + 2, end)));
        break;
      }
      case 72 /* MemberDecorate */: {
        let s = this.memberDecorations.get(words2[a]);
        if (!s) {
          s = /* @__PURE__ */ new Map();
          this.memberDecorations.set(words2[a], s);
        }
        let m = s.get(words2[a + 1]);
        if (!m) {
          m = /* @__PURE__ */ new Map();
          s.set(words2[a + 1], m);
        }
        m.set(words2[a + 2], Array.from(words2.subarray(a + 3, end)));
        break;
      }
      default:
        break;
    }
  }
  decoration(id, dec) {
    return this.decorations.get(id)?.get(dec);
  }
  memberDecoration(structId, member, dec) {
    return this.memberDecorations.get(structId)?.get(member)?.get(dec);
  }
  hasMemberDecoration(structId, dec) {
    const s = this.memberDecorations.get(structId);
    if (!s) return false;
    for (const m of s.values()) if (m.has(dec)) return true;
    return false;
  }
  allMembersDecorated(structId, memberCount, dec) {
    if (memberCount === 0) return false;
    for (let i = 0; i < memberCount; i++) if (!this.memberDecoration(structId, i, dec)) return false;
    return true;
  }
  /** Follows pointers. */
  /**
   * Names from the Vulkan debug information, for what OpName and OpMemberName do not name: a
   * module stripped of them (spirv-opt --strip-debug, which keeps this set) otherwise shows a
   * buffer's fields as member0, member1. Only missing names are filled in.
   */
  applyDebugNames() {
    for (const g of this.debugGlobals) {
      const variableName = this.strings.get(g.name);
      if (variableName && !this.names.has(g.variable)) this.names.set(g.variable, variableName);
      const composite = this.debugComposites.get(g.type);
      const variable = this.variables.find((v) => v.id === g.variable);
      if (!composite || !variable) continue;
      const structId = this.unwrapArrays(this.pointee(variable.typeId)).id;
      if (this.types.get(structId)?.op !== 30 /* TypeStruct */) continue;
      const structName = this.strings.get(composite.name);
      if (structName && !this.names.has(structId)) this.names.set(structId, structName);
      const fields = composite.members.map((id) => this.debugMembers.get(id)).filter((id) => id !== void 0);
      if (!fields.length) continue;
      let members = this.memberNames.get(structId);
      if (!members) {
        members = /* @__PURE__ */ new Map();
        this.memberNames.set(structId, members);
      }
      fields.forEach((nameId, index) => {
        const name = this.strings.get(nameId);
        if (name && !members.has(index)) members.set(index, name);
      });
    }
  }
  pointee(typeId) {
    const t = this.types.get(typeId);
    return t && t.op === 32 /* TypePointer */ ? this.pointee(t.operands[2]) : typeId;
  }
  /** Strips array wrappers, returning the element type id and the total element count (0 = runtime). */
  unwrapArrays(typeId) {
    let count2 = 1;
    let id = typeId;
    for (; ; ) {
      const t = this.types.get(id);
      if (!t) break;
      if (t.op === 28 /* TypeArray */) {
        count2 *= this.constants.get(t.operands[2]) ?? 0;
        id = t.operands[1];
      } else if (t.op === 29 /* TypeRuntimeArray */) {
        count2 = 0;
        id = t.operands[1];
      } else {
        break;
      }
    }
    return { id, count: count2 };
  }
  resolve(typeId, matrixStride = 0, rowMajor = false) {
    const key = `${typeId}:${matrixStride}:${rowMajor ? 1 : 0}`;
    const cached = this._cache.get(key);
    if (cached) return cached;
    const t = this._resolve(typeId, matrixStride, rowMajor);
    this._cache.set(key, t);
    return t;
  }
  _resolve(typeId, matrixStride, rowMajor) {
    const t = this.types.get(typeId);
    if (!t) return { kind: "opaque", name: "?" };
    const o = t.operands;
    switch (t.op) {
      case 20 /* TypeBool */:
        return { kind: "scalar", base: "bool", width: 32, size: 4 };
      case 21 /* TypeInt */:
        return { kind: "scalar", base: o[2] ? "int" : "uint", width: o[1], size: o[1] / 8 };
      case 22 /* TypeFloat */:
        return { kind: "scalar", base: "float", width: o[1], size: o[1] / 8 };
      case 23 /* TypeVector */: {
        const e = this.resolve(o[1]);
        const element = e.kind === "scalar" ? e : { kind: "scalar", base: "float", width: 32, size: 4 };
        return { kind: "vector", element, count: o[2], size: o[2] * element.size };
      }
      case 24 /* TypeMatrix */: {
        const col = this.resolve(o[1]);
        const column = col.kind === "vector" ? col : { kind: "vector", element: { kind: "scalar", base: "float", width: 32, size: 4 }, count: 4, size: 16 };
        const columns = o[2];
        const rows = column.count;
        const vecLen = rowMajor ? columns : rows;
        const stride = matrixStride || (vecLen === 3 ? 4 : vecLen) * column.element.size;
        return { kind: "matrix", element: column.element, columns, rows, stride, rowMajor, size: (rowMajor ? rows : columns) * stride };
      }
      case 28 /* TypeArray */: {
        const element = this.resolve(o[1], matrixStride, rowMajor);
        const count2 = this.constants.get(o[2]) ?? 0;
        const stride = this.decoration(typeId, 6 /* ArrayStride */)?.[0] ?? sizeOf2(element);
        return { kind: "array", element, count: count2, stride, size: count2 * stride };
      }
      case 29 /* TypeRuntimeArray */: {
        const element = this.resolve(o[1], matrixStride, rowMajor);
        const stride = this.decoration(typeId, 6 /* ArrayStride */)?.[0] ?? sizeOf2(element);
        return { kind: "array", element, count: 0, stride, size: 0 };
      }
      case 30 /* TypeStruct */: {
        const members = [];
        let running = 0;
        let size2 = 0;
        for (let i = 1; i < o.length; i++) {
          const m = i - 1;
          const ms = this.memberDecoration(typeId, m, 7 /* MatrixStride */)?.[0] ?? 0;
          const rm = this.memberDecoration(typeId, m, 4 /* RowMajor */) !== void 0;
          const type = this.resolve(o[i], ms, rm);
          const offset = this.memberDecoration(typeId, m, 35 /* Offset */)?.[0] ?? running;
          members.push({ name: this.memberNames.get(typeId)?.get(m) ?? `member${m}`, offset, type });
          running = offset + sizeOf2(type);
          if (running > size2) size2 = running;
        }
        return { kind: "struct", name: this.names.get(typeId) ?? "", members, size: size2 };
      }
      case 25 /* TypeImage */:
        return { kind: "opaque", name: this.imageName(t, false) };
      case 26 /* TypeSampler */:
        return { kind: "opaque", name: "sampler" };
      case 27 /* TypeSampledImage */: {
        const img = this.types.get(o[1]);
        return { kind: "opaque", name: img && img.op === 25 /* TypeImage */ ? this.imageName(img, true) : "sampler" };
      }
      case 32 /* TypePointer */:
        return this.resolve(o[2], matrixStride, rowMajor);
      case 5341 /* TypeAccelerationStructureKHR */:
        return { kind: "opaque", name: "accelerationStructureEXT" };
      default:
        return { kind: "opaque", name: "?" };
    }
  }
  imageName(t, combined) {
    const o = t.operands;
    const sampled = this.resolve(o[1]);
    const dim = o[2];
    const depth = o[3] === 1;
    const arrayed = o[4] === 1;
    const ms = o[5] === 1;
    const storage = o[6] === 2;
    let prefix = "";
    if (sampled.kind === "scalar" && sampled.base === "int") prefix = "i";
    else if (sampled.kind === "scalar" && sampled.base === "uint") prefix = "u";
    if (dim === 6) return `${prefix}subpassInput${ms ? "MS" : ""}`;
    const base = storage ? "image" : combined ? "sampler" : "texture";
    const dims = { 0: "1D", 1: "2D", 2: "3D", 3: "Cube", 4: "2DRect", 5: "Buffer" };
    return `${prefix}${base}${dims[dim] ?? "2D"}${ms ? "MS" : ""}${arrayed ? "Array" : ""}${depth && combined ? "Shadow" : ""}`;
  }
};
function sizeOf2(t) {
  return t.kind === "opaque" ? 0 : t.size;
}
function reflectSpirv(data) {
  if (data.byteLength < 20) return null;
  const bytes = new Uint8Array(data.byteLength & ~3);
  bytes.set(data.subarray(0, bytes.byteLength));
  const words2 = new Uint32Array(bytes.buffer);
  if (words2[0] === 50471687) {
    for (let i = 0; i < words2.length; i++) {
      const w = words2[i];
      words2[i] = (w & 255) << 24 | (w & 65280) << 8 | w >>> 8 & 65280 | w >>> 24;
    }
  }
  if (words2[0] !== 119734787) return null;
  const p = new Parser();
  p.parse(words2);
  p.applyDebugNames();
  const r = new ShaderReflection();
  r.version = `${words2[1] >>> 16 & 255}.${words2[1] >>> 8 & 255}`;
  const location = (id) => p.decoration(id, 30 /* Location */)?.[0];
  const isBuiltIn = (v) => {
    if (p.decoration(v.id, 11 /* BuiltIn */)) return true;
    const pointee = p.pointee(v.typeId);
    const t = p.types.get(pointee);
    return !!t && t.op === 30 /* TypeStruct */ && p.hasMemberDecoration(pointee, 11 /* BuiltIn */);
  };
  const ioVariable = (v) => {
    if (isBuiltIn(v)) return null;
    const loc = location(v.id);
    if (loc === void 0) return null;
    const type = p.resolve(p.pointee(v.typeId));
    return { location: loc, name: p.names.get(v.id) ?? "", typeName: typeName(type), type };
  };
  for (const e of p.entries) {
    const inputs = [];
    const outputs = [];
    const inInterface = (id) => e.interfaces.length === 0 || e.interfaces.includes(id);
    for (const v of p.variables) {
      if (!inInterface(v.id)) continue;
      if (v.storageClass === 1 /* Input */) {
        const io = ioVariable(v);
        if (io) inputs.push(io);
      } else if (v.storageClass === 3 /* Output */) {
        const io = ioVariable(v);
        if (io) outputs.push(io);
      }
    }
    inputs.sort((a, b) => a.location - b.location);
    outputs.sort((a, b) => a.location - b.location);
    let workgroupSize = p.localSize.get(e.id) ?? null;
    const ids = p.localSizeIds.get(e.id);
    if (!workgroupSize && ids) workgroupSize = [p.constants.get(ids[0]) ?? 1, p.constants.get(ids[1]) ?? 1, p.constants.get(ids[2]) ?? 1];
    r.entryPoints.push({ name: e.name, stage: STAGES2[e.model] ?? "unknown", inputs, outputs, workgroupSize });
  }
  for (const v of p.variables) {
    const sc = v.storageClass;
    if (sc !== 2 /* Uniform */ && sc !== 12 /* StorageBuffer */ && sc !== 0 /* UniformConstant */ && sc !== 9 /* PushConstant */) continue;
    const pointee = p.pointee(v.typeId);
    const inner = p.unwrapArrays(pointee);
    const innerType = p.types.get(inner.id);
    if (!innerType) continue;
    const type = p.resolve(inner.id);
    let kind = "unknown";
    if (sc === 9 /* PushConstant */) kind = "pushConstant";
    else if (sc === 12 /* StorageBuffer */) kind = "storage";
    else if (sc === 2 /* Uniform */) kind = p.decoration(inner.id, 3 /* BufferBlock */) ? "storage" : "uniform";
    else {
      switch (innerType.op) {
        case 27 /* TypeSampledImage */:
          kind = "combinedImageSampler";
          break;
        case 26 /* TypeSampler */:
          kind = "sampler";
          break;
        case 5341 /* TypeAccelerationStructureKHR */:
          kind = "accelerationStructure";
          break;
        case 25 /* TypeImage */: {
          const dim = innerType.operands[2];
          const storage = innerType.operands[6] === 2;
          if (dim === 5) kind = storage ? "storageTexelBuffer" : "uniformTexelBuffer";
          else if (dim === 6) kind = "inputAttachment";
          else kind = storage ? "storageImage" : "sampledImage";
          break;
        }
        default:
          break;
      }
    }
    const memberCount = innerType.op === 30 /* TypeStruct */ ? innerType.operands.length - 1 : 0;
    const readOnly = !!p.decoration(v.id, 24 /* NonWritable */) || memberCount > 0 && p.allMembersDecorated(inner.id, memberCount, 24 /* NonWritable */);
    const writeOnly = !!p.decoration(v.id, 25 /* NonReadable */) || memberCount > 0 && p.allMembersDecorated(inner.id, memberCount, 25 /* NonReadable */);
    const structName = type.kind === "struct" ? type.name : "";
    const name = p.names.get(v.id) || structName || "";
    const res = {
      kind,
      set: p.decoration(v.id, 34 /* DescriptorSet */)?.[0] ?? 0,
      binding: p.decoration(v.id, 33 /* Binding */)?.[0] ?? 0,
      name,
      typeName: structName || typeName(type),
      type,
      count: inner.count,
      readOnly,
      writeOnly
    };
    if (kind === "pushConstant") r.pushConstants.push(res);
    else r.resources.push(res);
  }
  r.resources.sort((a, b) => a.set - b.set || a.binding - b.binding);
  return r;
}

// src/mcp/capture_store.ts
var Capture = class {
  constructor(id, path11, mtimeMs, bytes) {
    this.id = id;
    this.path = path11;
    this.mtimeMs = mtimeMs;
    const capture = parseCaptureFile(bytes);
    const m = capture.manifest;
    this.manifest = m;
    this.fileBytes = bytes.byteLength;
    this.db.loadObjects(capture.objects, capture.blobs, {
      frame: m.frame,
      frameTimeMs: m.frameTimeMs ?? 0,
      submitMs: m.submitMs ?? 0,
      refreshMs: m.refreshMs ?? 0,
      refreshSource: m.refreshSource ?? "",
      displayRefreshMs: m.displayRefreshMs ?? 0,
      frameBoundary: m.frameBoundary ?? ""
    });
    this.db.loadValidation(capture.validation);
    for (const [a, f] of Object.entries(m.symbols ?? {})) this.db.symbols.set(a, f);
    for (const [id2, frames] of Object.entries(m.stacks ?? {})) this.db.stacks.set(Number(id2), frames);
    this.db.stacksAvailable = m.stacks !== void 0;
    this.data.load(capture);
  }
  db = new ObjectDatabase();
  data = new CaptureData();
  manifest;
  fileBytes;
  _graph = null;
  _analysis = null;
  _metrics = null;
  _statistics = null;
  _passOf = null;
  _labels = null;
  _validationCommands = null;
  _reflections = /* @__PURE__ */ new Map();
  get graph() {
    return this._graph ??= frameRenderGraph(this.data, this.db);
  }
  /** Frame Issues: the per-command rules, the counter and sampling rules and the render graph rules. */
  get analysis() {
    return this._analysis ??= analyzeFrame(this.data, this.db, this.graph);
  }
  get metrics() {
    return this._metrics ??= collectPassMetrics(this.data, this.db);
  }
  /** Overdraw measured after the capture was saved (vkinsp_replay, for a Vulkan capture): what reads it is recomputed. */
  setOverdraw(measurements) {
    this.data.overdraw = measurements;
    this._metrics = null;
    this._analysis = null;
  }
  /** Per-draw timings and counters measured by replaying the capture (renderer/draw_stats.ts). */
  setDrawStats(draws) {
    this.data.drawStats = draws;
    this._metrics = null;
    this._analysis = null;
  }
  get statistics() {
    return this._statistics ??= new CaptureStatistics().compute(this.data, this.db);
  }
  /** Index into metrics.passes of the pass a command belongs to; -1 outside every pass. */
  passOf(commandIndex) {
    if (!this._passOf) {
      const commands = this.data.commands;
      const map = new Int32Array(commands.length).fill(-1);
      this.metrics.passes.forEach((p, i) => {
        for (let c2 = p.commandIndex; c2 <= p.endIndex && c2 < commands.length; c2++) {
          const cmd = commands[c2];
          const stream = p.compute ? cmd.secondary || cmd.object?.__id || 0 : cmd.object?.__id ?? 0;
          if (stream === p.commandBuffer && map[c2] < 0) map[c2] = i;
        }
      });
      this._passOf = map;
    }
    return this._passOf[commandIndex] ?? -1;
  }
  /** The debug groups open at a command, outermost first: "Frame / Shadows / Cascade 0". */
  labelsOf(commandIndex) {
    if (!this._labels) {
      const sets = this.data.sets;
      const stacks = /* @__PURE__ */ new Map();
      this._labels = this.data.commands.map((c2) => {
        const key = `${c2.object?.__id ?? 0}:${c2.secondary ?? 0}`;
        let stack = stacks.get(key);
        if (!stack || c2.method === "vkBeginCommandBuffer") stacks.set(key, stack = []);
        if (sets.LABEL_BEGIN.has(c2.method)) stack.push(labelNameOf(c2));
        const labels = stack.join(" / ");
        if (sets.LABEL_END.has(c2.method)) stack.pop();
        return labels;
      });
    }
    return this._labels[commandIndex] ?? "";
  }
  /** A pass's label with its render pass object's name and the debug groups around it. */
  passName(passIndex) {
    const p = this.metrics.passes[passIndex];
    if (!p) return "";
    let label = p.label;
    const begin = this.data.commands[p.commandIndex]?.args?.pRenderPassBegin;
    if (!label.includes(":") && isObject(begin)) {
      const rp = this.db.getObject(refId(begin.renderPass));
      if (rp?.label) label += `: ${rp.label}`;
    }
    const labels = this.labelsOf(p.commandIndex);
    return labels ? `${label} [${labels}]` : label;
  }
  /** The metrics pass a render target was read back at the end of; -1 for sampled images. */
  passOfTexture(info) {
    if (info.kind === "sampled") return -1;
    return this.metrics.passes.findIndex((p) => !p.compute && p.frame === info.frame && p.commandBuffer === info.commandBuffer && p.passIndex === info.passIndex);
  }
  /** The captured command a validation message fired on, when it names one. */
  commandOfValidation(v) {
    if (!v.command) return void 0;
    if (!this._validationCommands) {
      this._validationCommands = /* @__PURE__ */ new Map();
      for (const c2 of this.data.commands) {
        if (c2.slot !== void 0) this._validationCommands.set(`${c2.secondary ?? c2.object?.__id}:${c2.slot}`, c2.index);
      }
    }
    return this._validationCommands.get(`${v.command.commandBuffer}:${v.command.slot}`);
  }
  /** The SPIR-V of an object's payload, when the file carries it. */
  spirv(object, blobIndex) {
    return this.db.blobData.get(`${object.id}:${blobIndex}`) ?? null;
  }
  /** Reflection of an object's SPIR-V payload, parsed once. */
  reflection(object, blobIndex) {
    const key = `${object.id}:${blobIndex}`;
    if (!this._reflections.has(key)) {
      const data = this.spirv(object, blobIndex);
      let r = null;
      try {
        r = data ? reflectSpirv(data) : null;
      } catch {
        r = null;
      }
      this._reflections.set(key, r);
    }
    return this._reflections.get(key) ?? null;
  }
};
var CaptureStore = class {
  _captures = /* @__PURE__ */ new Map();
  _counter = 0;
  _latest = null;
  /** Opens a capture file, or returns the open capture of that file when it has not changed on disk. */
  open(file) {
    const abs = path.resolve(file);
    let stat;
    try {
      stat = fs.statSync(abs);
    } catch {
      throw new Error(`No file at ${abs}.`);
    }
    for (const c2 of this._captures.values()) {
      if (c2.path === abs && c2.mtimeMs === stat.mtimeMs) {
        this._latest = c2;
        return { capture: c2, reused: true };
      }
    }
    const contents = fs.readFileSync(abs);
    const bytes = new Uint8Array(contents.buffer, contents.byteOffset, contents.byteLength);
    const capture = new Capture(`cap-${++this._counter}`, abs, stat.mtimeMs, bytes);
    this._captures.set(capture.id, capture);
    this._latest = capture;
    return { capture, reused: false };
  }
  /** An open capture by id, a capture file by path (opened on the way), or the latest opened. */
  resolve(ref) {
    if (!ref) {
      if (!this._latest) throw new Error("No capture is open. Call open_capture with the path of a .gpucap file (list_captures shows the captures GPU Inspector opened recently).");
      return this._latest;
    }
    const open = this._captures.get(ref);
    if (open) return open;
    if (/\.gpucap$/i.test(ref) || fs.existsSync(ref)) return this.open(ref).capture;
    const ids = [...this._captures.keys()];
    throw new Error(`No open capture "${ref}". ${ids.length ? `Open: ${ids.join(", ")}.` : "None is open."}`);
  }
  close(ref) {
    const c2 = this._captures.get(ref) ?? [...this._captures.values()].find((x) => x.path === path.resolve(ref));
    if (!c2) return false;
    this._captures.delete(c2.id);
    if (this._latest === c2) this._latest = [...this._captures.values()].pop() ?? null;
    return true;
  }
  list() {
    return [...this._captures.values()];
  }
};
function settingsFile() {
  if (process.env.GPU_INSPECTOR_SETTINGS) return process.env.GPU_INSPECTOR_SETTINGS;
  const home = os.homedir();
  const base = process.platform === "win32" ? process.env.APPDATA ?? path.join(home, "AppData", "Roaming") : process.platform === "darwin" ? path.join(home, "Library", "Application Support") : process.env.XDG_CONFIG_HOME ?? path.join(home, ".config");
  return path.join(base, "gpu-inspector", "settings.json");
}
function appSetting(key) {
  try {
    return JSON.parse(fs.readFileSync(settingsFile(), "utf8"))[key];
  } catch {
    return void 0;
  }
}
function recentCaptureFiles() {
  const recent = appSetting("recentCaptures");
  return Array.isArray(recent) ? recent.filter((p) => typeof p === "string" && !!p) : [];
}

// src/renderer/utils/base64.ts
var _uint8Proto = Uint8Array.prototype;
var _uint8Ctor = Uint8Array;
var _hasNativeToBase64 = typeof _uint8Proto.toBase64 === "function";
var _hasNativeFromBase64 = typeof _uint8Ctor.fromBase64 === "function";
function decodeBase64(str3) {
  if (_hasNativeFromBase64) {
    return _uint8Ctor.fromBase64(str3);
  }
  const binary = atob(str3);
  const len = binary.length;
  const out = new Uint8Array(len);
  for (let i = 0; i < len; i++) {
    out[i] = binary.charCodeAt(i);
  }
  return out;
}

// src/renderer/draw_state.ts
function pushConstantBytes(a) {
  const v = a && isObject(a.pValues) ? a.pValues : null;
  if (!v || typeof v.base64 !== "string") return null;
  try {
    return decodeBase64(v.base64);
  } catch {
    return null;
  }
}
function sameStream(cmdSets, cmd, c2) {
  if (c2.object?.__id !== cmd.object?.__id) return false;
  if (cmdSets.SUBMIT.has(c2.method)) return false;
  return true;
}
function emptyDrawState(bindPoint) {
  return {
    bindPoint,
    pipelineCmd: null,
    pipeline: null,
    sets: /* @__PURE__ */ new Map(),
    vertexBuffers: /* @__PURE__ */ new Map(),
    stageBuffers: /* @__PURE__ */ new Map(),
    indexBuffer: null,
    vertexInput: null,
    viewports: null,
    scissors: null,
    pushConstants: []
  };
}
function pushConstantOf(c2) {
  let a = c2.args;
  if (!a) return null;
  if (isObject(a.pPushConstantsInfo)) a = a.pPushConstantsInfo;
  return { cmd: c2, stageFlags: str(a.stageFlags), offset: num(a.offset), size: num(a.size), data: pushConstantBytes(a) };
}
function drawState(data, db, cmd, bindPoint = data.sets.bindPointOf(cmd.method)) {
  const cmdSets = data.sets;
  const commands = data.commands;
  const state = emptyDrawState(bindPoint);
  state.indexBuffer = cmdSets.indexBufferOf(cmd);
  for (let i = cmd.index - 1; i >= 0; i--) {
    const c2 = commands[i];
    if (!c2 || !sameStream(cmdSets, cmd, c2)) break;
    if (cmd.secondary) {
      if (c2.secondary !== cmd.secondary) break;
    } else if (c2.secondary) {
      continue;
    }
    const a = c2.args;
    if (!a) continue;
    if (cmdSets.BIND_PIPELINE.has(c2.method)) {
      if (!state.pipelineCmd && cmdSets.pipelineBindPointOf(c2.method, a) === bindPoint) {
        state.pipelineCmd = c2;
        state.pipeline = db.getObject(refId(a.pipeline));
      }
      continue;
    }
    if (cmdSets.BIND_STAGE_BUFFER?.has(c2.method) && cmdSets.stageBuffersOf) {
      for (const sb of cmdSets.stageBuffersOf(c2)) {
        const key = `${sb.stage}:${sb.index}`;
        if (!state.stageBuffers.has(key)) state.stageBuffers.set(key, sb);
      }
    }
    if (cmdSets.BIND_VERTEX.has(c2.method)) {
      for (const vb of cmdSets.vertexBuffersOf(c2)) {
        if (!state.vertexBuffers.has(vb.binding)) state.vertexBuffers.set(vb.binding, vb);
      }
      continue;
    }
    if (cmdSets.BIND_INDEX.has(c2.method)) {
      if (!state.indexBuffer) state.indexBuffer = cmdSets.indexBufferOf(c2);
      continue;
    }
    switch (c2.method) {
      case "vkCmdSetVertexInputEXT":
        if (!state.vertexInput) state.vertexInput = a;
        break;
      case "vkCmdSetViewport":
      case "vkCmdSetViewportWithCount":
      case "vkCmdSetViewportWithCountEXT":
        if (!state.viewports) state.viewports = a.pViewports ?? null;
        break;
      case "vkCmdSetScissor":
      case "vkCmdSetScissorWithCount":
      case "vkCmdSetScissorWithCountEXT":
        if (!state.scissors) state.scissors = a.pScissors ?? null;
        break;
      case "vkCmdPushConstants":
      case "vkCmdPushConstants2":
      case "vkCmdPushConstants2KHR": {
        const pc = pushConstantOf(c2);
        if (pc) state.pushConstants.unshift(pc);
        break;
      }
      default:
        break;
    }
    if (c2.descriptors && c2.descriptors.bindPoint === bindPoint) {
      for (const set of c2.descriptors.sets) {
        if (!state.sets.has(set.set)) state.sets.set(set.set, { cmd: c2, set });
      }
    }
  }
  return state;
}
function bindingState(data, db, cmd, bindPoint) {
  const cmdSets = data.sets;
  const state = drawState(data, db, cmd, bindPoint);
  if (state.pipeline) return state;
  const commands = data.commands;
  for (let i = cmd.index + 1; i < commands.length; i++) {
    const c2 = commands[i];
    if (!c2 || !sameStream(cmdSets, cmd, c2)) break;
    if (cmd.secondary ? c2.secondary !== cmd.secondary : c2.secondary) {
      if (cmd.secondary) break;
      continue;
    }
    if (cmdSets.BIND_PIPELINE.has(c2.method) && cmdSets.pipelineBindPointOf(c2.method, c2.args) === bindPoint) {
      state.pipelineCmd = c2;
      state.pipeline = db.getObject(refId(c2.args?.pipeline));
      break;
    }
  }
  return state;
}
function findPass(data, cmd) {
  const cmdSets = data.sets;
  const commands = data.commands;
  let depth = 0;
  for (let i = cmd.index; i >= 0; i--) {
    const c2 = commands[i];
    if (!c2 || !sameStream(cmdSets, cmd, c2)) break;
    if (c2.secondary) continue;
    if (i !== cmd.index && cmdSets.PASS_END.has(c2.method)) depth++;
    if (cmdSets.PASS_BEGIN.has(c2.method)) {
      if (depth === 0) {
        let passIndex = 0;
        for (let j = i - 1; j >= 0; j--) {
          const p = commands[j];
          if (!p || !sameStream(cmdSets, cmd, p)) break;
          if (cmdSets.PASS_BEGIN.has(p.method)) passIndex++;
        }
        return { passBegin: c2, passIndex };
      }
      depth--;
    }
  }
  return null;
}
function vertexLayout(state, binding, vb) {
  const vd = state.pipeline?.descriptor?.vertexDescriptor;
  if (isObject(vd)) {
    const layouts = Array.isArray(vd.layouts) ? vd.layouts : [];
    const attrs2 = Array.isArray(vd.attributes) ? vd.attributes : [];
    const layout = layouts.find((l) => isObject(l) && num(l.index) === binding);
    if (!isObject(layout)) return null;
    const attributes2 = attrs2.filter((a) => isObject(a) && num(a.bufferIndex) === binding).map((a) => ({ location: num(a.index), format: str(a.vkFormat ?? a.format), offset: num(a.offset) })).sort((x, y) => x.offset - y.offset);
    return { stride: vb.stride ?? num(layout.stride), rate: str(layout.stepFunction).includes("PerInstance") ? "VK_VERTEX_INPUT_RATE_INSTANCE" : "VK_VERTEX_INPUT_RATE_VERTEX", attributes: attributes2 };
  }
  const vi = state.vertexInput ?? (isObject(state.pipeline?.descriptor?.pVertexInputState) ? state.pipeline.descriptor.pVertexInputState : null);
  if (!isObject(vi)) return null;
  const bindings = Array.isArray(vi.pVertexBindingDescriptions) ? vi.pVertexBindingDescriptions : [];
  const attrs = Array.isArray(vi.pVertexAttributeDescriptions) ? vi.pVertexAttributeDescriptions : [];
  const b = bindings.find((x) => isObject(x) && num(x.binding) === binding);
  if (!isObject(b)) return null;
  const attributes = attrs.filter((a) => isObject(a) && num(a.binding) === binding).map((a) => ({ location: num(a.location), format: str(a.format), offset: num(a.offset) })).sort((x, y) => x.offset - y.offset);
  return { stride: vb.stride ?? num(b.stride), rate: str(b.inputRate), attributes };
}

// src/renderer/metal/argument_buffer.ts
function handleOf(type) {
  if (type.kind !== "opaque") return null;
  const raw = type;
  return typeof raw.metal === "string" ? { metal: raw.metal, name: type.name } : null;
}
function isArgumentBufferType(type, depth = 0) {
  if (depth > 8) return false;
  if (handleOf(type)) return true;
  if (type.kind === "struct") return type.members.some((m) => isArgumentBufferType(m.type, depth + 1));
  if (type.kind === "array") return isArgumentBufferType(type.element, depth + 1);
  return false;
}
var HandleIndex = class {
  _buffers = [];
  _byResourceId = /* @__PURE__ */ new Map();
  constructor(db) {
    for (const o of db.allObjects.values()) {
      if (!o.type.startsWith("MTL") || !o.args) continue;
      const address = o.args.gpuAddress;
      if (typeof address === "string" && address.startsWith("0x")) {
        const start = BigInt(address);
        const length = BigInt(Math.max(0, num(o.args.length)));
        this._buffers.push({ start, end: start + length, object: o });
      }
      const id = o.args.gpuResourceID;
      if (typeof id === "string" && id.startsWith("0x")) this._byResourceId.set(id.toLowerCase(), o);
    }
  }
  buffer(address) {
    for (const b of this._buffers) {
      if (address >= b.start && address < b.end) return { object: b.object, offset: Number(address - b.start) };
    }
    return null;
  }
  resource(id) {
    return this._byResourceId.get(`0x${id.toString(16)}`) ?? null;
  }
};
var MAX_ENTRIES = 512;
function argumentBufferEntries(type, data, db) {
  const index = new HandleIndex(db);
  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const out = [];
  const walk = (t, offset, path11, depth) => {
    if (out.length >= MAX_ENTRIES || depth > 8) return;
    const handle = handleOf(t);
    if (handle) {
      let value = null;
      if (offset + 8 <= data.byteLength) value = view.getBigUint64(offset, true);
      let object = null;
      let objectOffset = null;
      if (value !== null && value !== 0n) {
        if (handle.metal === "pointer") {
          const hit = index.buffer(value);
          if (hit) {
            object = hit.object;
            objectOffset = hit.offset;
          }
        } else {
          object = index.resource(value);
        }
      }
      out.push({ path: path11, offset, kind: handle.metal, typeName: handle.name, value: value === null ? null : `0x${value.toString(16)}`, object, objectOffset });
      return;
    }
    if (t.kind === "struct") {
      for (const m of t.members) walk(m.type, offset + m.offset, path11 ? `${path11}.${m.name}` : m.name, depth + 1);
    } else if (t.kind === "array") {
      const stride = t.stride || (t.element.kind === "opaque" ? 8 : t.element.size);
      if (stride <= 0) return;
      const count2 = t.count > 0 ? t.count : Math.floor((data.byteLength - offset) / stride);
      for (let i = 0; i < count2 && out.length < MAX_ENTRIES; i++) walk(t.element, offset + i * stride, `${path11}[${i}]`, depth + 1);
    }
  };
  walk(type, 0, "", 0);
  return out;
}

// src/renderer/metal/reflection.ts
function asType(v) {
  if (!isObject(v)) return null;
  const kind = str(v.kind);
  if (!["scalar", "vector", "matrix", "array", "struct", "opaque"].includes(kind)) return null;
  return v;
}
function resource(entry, kind, typeOverride) {
  const type = typeOverride ?? asType(entry.type);
  if (!type) return null;
  const access = str(entry.access);
  return {
    kind,
    set: 0,
    binding: num(entry.index),
    name: str(entry.name),
    typeName: type.kind === "struct" ? type.name : typeName(type),
    type,
    count: 1,
    readOnly: access === "readOnly",
    writeOnly: access === "writeOnly"
  };
}
function metalStages(pipeline) {
  const refl = pipeline?.descriptor?.reflection;
  if (!isObject(refl)) return [];
  const stages = [];
  for (const [stage, value] of Object.entries(refl)) {
    if (!isObject(value)) continue;
    const s = { stage, buffers: /* @__PURE__ */ new Map(), textures: /* @__PURE__ */ new Map(), samplers: /* @__PURE__ */ new Map() };
    for (const b of Array.isArray(value.buffers) ? value.buffers : []) {
      if (!isObject(b) || b.used === false) continue;
      const r = resource(b, str(b.access) === "readOnly" ? "uniform" : "storage");
      if (r) s.buffers.set(r.binding, r);
    }
    for (const t of Array.isArray(value.textures) ? value.textures : []) {
      if (!isObject(t) || t.used === false) continue;
      const name = `${str(t.textureType).replace(/^MTLTextureType/, "texture")}<${str(t.dataType) || "float"}>`;
      const r = resource(t, str(t.access) === "readOnly" ? "sampledImage" : "storageImage", { kind: "opaque", name });
      if (r) s.textures.set(r.binding, r);
    }
    for (const sa of Array.isArray(value.samplers) ? value.samplers : []) {
      if (!isObject(sa) || sa.used === false) continue;
      const r = resource(sa, "sampler", { kind: "opaque", name: "sampler" });
      if (r) s.samplers.set(r.binding, r);
    }
    stages.push(s);
  }
  return stages;
}
function metalBufferResource(pipeline, stage, index) {
  for (const s of metalStages(pipeline)) {
    if (s.stage === stage) return s.buffers.get(index) ?? null;
  }
  return null;
}

// src/renderer/shader_cache.ts
var STAGE_FLAGS = {
  VK_SHADER_STAGE_VERTEX_BIT: "vertex",
  VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: "tess_control",
  VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: "tess_eval",
  VK_SHADER_STAGE_GEOMETRY_BIT: "geometry",
  VK_SHADER_STAGE_FRAGMENT_BIT: "fragment",
  VK_SHADER_STAGE_COMPUTE_BIT: "compute",
  VK_SHADER_STAGE_TASK_BIT_EXT: "task",
  VK_SHADER_STAGE_MESH_BIT_EXT: "mesh",
  VK_SHADER_STAGE_RAYGEN_BIT_KHR: "raygen",
  VK_SHADER_STAGE_ANY_HIT_BIT_KHR: "any_hit",
  VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR: "closest_hit",
  VK_SHADER_STAGE_MISS_BIT_KHR: "miss",
  VK_SHADER_STAGE_INTERSECTION_BIT_KHR: "intersection",
  VK_SHADER_STAGE_CALLABLE_BIT_KHR: "callable"
};
function stageFromFlag(flag) {
  return STAGE_FLAGS[flag] ?? "unknown";
}
function pipelineStages(pipeline, db) {
  const d = pipeline.descriptor;
  if (!d) return [];
  const stages = Array.isArray(d.pStages) ? d.pStages : isObject(d.stage) ? [d.stage] : [];
  const out = [];
  for (const s of stages) {
    if (!isObject(s)) continue;
    const stageFlag = str(s.stage);
    const stage = stageFromFlag(stageFlag);
    const entryPoint = str(s.pName) || "main";
    const module = db.getObject(refId(s.module));
    let blobIndex = pipeline.blobs.findIndex((b) => b.name === `${stage}:${entryPoint}`);
    if (blobIndex < 0) blobIndex = pipeline.blobs.findIndex((b) => b.name.startsWith(`${stage}:`));
    if (blobIndex >= 0) out.push({ stage, stageFlag, entryPoint, object: pipeline, blobIndex, module });
    else if (module && module.blobs.length) out.push({ stage, stageFlag, entryPoint, object: module, blobIndex: 0, module });
  }
  return out;
}
function pipelineUses(data) {
  const sets = data.sets;
  const bound = /* @__PURE__ */ new Map();
  const uses = /* @__PURE__ */ new Map();
  for (const c2 of data.commands) {
    if (!c2 || sets.SUBMIT.has(c2.method)) continue;
    const stream = `${c2.object?.__id ?? 0}:${c2.secondary ?? 0}`;
    if (sets.BIND_PIPELINE.has(c2.method) && c2.args) {
      const id = refId(c2.args.pipeline);
      if (id !== null) bound.set(`${stream}:${sets.pipelineBindPointOf(c2.method, c2.args)}`, id);
    } else if (isAction(sets, c2.method)) {
      const id = bound.get(`${stream}:${sets.bindPointOf(c2.method)}`);
      if (id !== void 0) uses.set(id, (uses.get(id) ?? 0) + 1);
    }
  }
  return uses;
}

// src/mcp/describe.ts
var MAX_TEXT = 8e4;
var MAX_ITEMS = 200;
var MAX_STRING = 4e3;
var MAX_ARRAY = 64;
function refText(db, v) {
  if (v === null || v === void 0 || v === 0) return void 0;
  const id = typeof v === "number" ? v : refId(v);
  if (id === null) return void 0;
  const o = db.getObject(id);
  const type = o?.type ?? (isHandleRef(v) ? v.__class ?? "object" : "object");
  const named = o && o.name !== `${o.shortType} ${o.id}` ? ` "${o.name}"` : "";
  return `${type}#${id}${named}`;
}
function compact(value, db, depth = 0) {
  if (value === null || value === void 0) return value;
  if (typeof value === "string") return clip(value, MAX_STRING);
  if (typeof value !== "object") return value;
  if (depth > 16) return "...";
  if (Array.isArray(value)) {
    const out2 = value.slice(0, MAX_ITEMS).map((v) => compact(v, db, depth + 1));
    if (value.length > MAX_ITEMS) out2.push(`... ${value.length - MAX_ITEMS} more`);
    return out2;
  }
  if (isHandleRef(value)) return refText(db, value);
  const out = {};
  for (const [k, v] of Object.entries(value)) {
    if (k === "base64" && typeof v === "string") out.bytes = base64Bytes(v);
    else out[k] = compact(v, db, depth + 1);
  }
  return out;
}
function base64Bytes(s) {
  const pad = s.endsWith("==") ? 2 : s.endsWith("=") ? 1 : 0;
  return Math.floor(s.length * 3 / 4) - pad;
}
function clip(s, max) {
  return s.length > max ? `${s.slice(0, max)}... (${s.length - max} more characters)` : s;
}
function round(v) {
  if (v === null || v === void 0) return void 0;
  if (!Number.isFinite(v) || Number.isInteger(v)) return v;
  return Math.abs(v) >= 100 ? Math.round(v * 10) / 10 : Number(v.toPrecision(4));
}
function jsonResult(value) {
  let text = JSON.stringify(value, null, 1);
  if (text.length > MAX_TEXT) {
    text = `${text.slice(0, MAX_TEXT)}
... the answer was cut at ${MAX_TEXT} of ${text.length} characters: ask for less (offset and limit, a filter, one item).`;
  }
  return { content: [{ type: "text", text }] };
}
function readTyped(type, view, offset, budget = { values: 2048 }) {
  if (budget.values <= 0) return "...";
  switch (type.kind) {
    case "scalar":
      budget.values--;
      return readScalar(view, offset, type);
    case "vector": {
      budget.values -= type.count;
      const out = [];
      for (let i = 0; i < type.count; i++) out.push(readScalar(view, offset + i * type.element.size, type.element));
      return out;
    }
    case "matrix": {
      budget.values -= type.columns * type.rows;
      const columns = [];
      for (let c2 = 0; c2 < type.columns; c2++) {
        const column = [];
        for (let r = 0; r < type.rows; r++) {
          const at = type.rowMajor ? offset + r * type.stride + c2 * type.element.size : offset + c2 * type.stride + r * type.element.size;
          column.push(readScalar(view, at, type.element));
        }
        columns.push(column);
      }
      return columns;
    }
    case "array": {
      const count2 = type.count || (type.stride > 0 ? Math.max(0, Math.floor((view.byteLength - offset) / type.stride)) : 0);
      const out = [];
      for (let i = 0; i < Math.min(count2, MAX_ARRAY) && budget.values > 0; i++) out.push(readTyped(type.element, view, offset + i * type.stride, budget));
      if (out.length < count2) out.push(`... ${count2 - out.length} more`);
      return out;
    }
    case "struct": {
      const out = {};
      for (const m of type.members) {
        if (budget.values <= 0) {
          out["..."] = "cut: read the buffer with read_buffer for the rest";
          break;
        }
        out[m.name || `offset${m.offset}`] = readTyped(m.type, view, offset + m.offset, budget);
      }
      return out;
    }
    case "format": {
      const f = vertexFormat(type.format);
      if (!f) return `<${type.format}>`;
      if (offset + f.size > view.byteLength) return null;
      budget.values -= f.channels.length;
      const v = f.read(view, offset).map(tidy);
      return v.length === 1 ? v[0] : v;
    }
    case "opaque":
      return `<${type.name}>`;
  }
}
function readScalar(view, offset, s) {
  if (offset < 0 || offset + s.size > view.byteLength) return null;
  switch (s.base) {
    case "float":
      return tidy(s.width === 16 ? float16ToFloat32(view.getUint16(offset, true)) : s.width === 64 ? view.getFloat64(offset, true) : view.getFloat32(offset, true));
    case "int":
      return s.width === 8 ? view.getInt8(offset) : s.width === 16 ? view.getInt16(offset, true) : s.width === 64 ? Number(view.getBigInt64(offset, true)) : view.getInt32(offset, true);
    case "uint":
      return s.width === 8 ? view.getUint8(offset) : s.width === 16 ? view.getUint16(offset, true) : s.width === 64 ? Number(view.getBigUint64(offset, true)) : view.getUint32(offset, true);
    case "bool":
      return (s.width === 8 ? view.getUint8(offset) : view.getUint32(offset, true)) !== 0;
  }
}
function tidy(v) {
  return Number.isFinite(v) && !Number.isInteger(v) ? Number(v.toPrecision(7)) : v;
}
function stringArg(args, name) {
  const v = args[name];
  if (v === void 0 || v === null || v === "") return void 0;
  if (typeof v !== "string") throw new Error(`${name} must be a string.`);
  return v;
}
function requireString(args, name) {
  const v = stringArg(args, name);
  if (v === void 0) throw new Error(`${name} is required.`);
  return v;
}
function intArg(args, name, fallback, min = -Infinity, max = Infinity) {
  const v = optionalInt(args, name);
  return v === void 0 ? fallback : Math.min(max, Math.max(min, v));
}
function optionalInt(args, name) {
  const v = args[name];
  if (v === void 0 || v === null || v === "") return void 0;
  const n = typeof v === "string" ? Number(v) : v;
  if (typeof n !== "number" || !Number.isInteger(n)) throw new Error(`${name} must be an integer.`);
  return n;
}
function requireInt(args, name) {
  const v = optionalInt(args, name);
  if (v === void 0) throw new Error(`${name} is required.`);
  return v;
}
function numberArg(args, name) {
  const v = args[name];
  if (v === void 0 || v === null || v === "") return void 0;
  const n = typeof v === "string" ? Number(v) : v;
  if (typeof n !== "number" || !Number.isFinite(n)) throw new Error(`${name} must be a number.`);
  return n;
}
function boolArg(args, name, fallback) {
  const v = args[name];
  if (v === void 0 || v === null) return fallback;
  if (typeof v === "boolean") return v;
  if (v === "true" || v === "false") return v === "true";
  throw new Error(`${name} must be true or false.`);
}
function regexArg(args, name) {
  const s = stringArg(args, name);
  if (s === void 0) return void 0;
  try {
    return new RegExp(s, "i");
  } catch (e) {
    throw new Error(`${name} is not a valid regular expression: ${e.message}`);
  }
}
function enumArg(args, name, values, fallback) {
  const s = stringArg(args, name);
  if (s === void 0) return fallback;
  if (!values.includes(s)) throw new Error(`${name} must be one of ${values.join(", ")}.`);
  return s;
}
function schema(properties, required = []) {
  return { type: "object", properties, ...required.length ? { required } : {} };
}
var CAPTURE_PARAM = {
  type: "string",
  description: `An open capture's id ("cap-1"), or the path of a .gpucap file, which is opened on the way. Defaults to the capture opened most recently.`
};
var PAGE_PARAMS = {
  offset: { type: "integer", minimum: 0, description: "Items to skip (default 0)." },
  limit: { type: "integer", minimum: 1, description: "Items to return." }
};
function findingBrief(c2, f) {
  const pass = f.commandIndex !== void 0 ? c2.passOf(f.commandIndex) : -1;
  return {
    rule: f.rule,
    severity: f.severity,
    confidence: f.confidence,
    count: f.count > 1 ? f.count : void 0,
    command: f.commandIndex,
    pass: pass >= 0 ? pass : void 0,
    passLabel: pass >= 0 ? c2.passName(pass) : void 0,
    message: f.message
  };
}
function validationBrief(c2, v, maxMessage = 1500) {
  return {
    severity: v.severity,
    id: v.idName ?? void 0,
    count: v.count > 1 ? v.count : void 0,
    frame: v.frame,
    command: c2.commandOfValidation(v),
    objects: (v.objects ?? []).map((o) => o.object && isHandleRef(o.object) ? refText(c2.db, o.object) : `${o.class} ${o.handle}${o.name ? ` "${o.name}"` : ""}`),
    message: clip(v.message, maxMessage)
  };
}
function stackLines(frames) {
  const lines = frames.filter((f) => !f.internal).map((f) => {
    const where = f.function ?? (f.module ? `${f.module}+0x${f.offset.toString(16)}` : f.address);
    return f.file ? `${where} (${f.file}:${f.line})` : where;
  });
  const hidden = frames.length - lines.length;
  if (hidden) lines.push(`(${hidden} frames inside the loader and layers left out)`);
  return lines;
}
function textureBrief(c2, t) {
  const info = t.info;
  const pass = c2.passOfTexture(info);
  const sampled = info.kind === "sampled";
  return {
    texture: c2.data.textures.indexOf(t),
    kind: sampled ? "sampled" : "attachment",
    image: refText(c2.db, info.id),
    view: refText(c2.db, info.view),
    format: info.format,
    size: `${info.width}x${info.height}${info.depth > 1 ? `x${info.depth}` : ""}`,
    layers: info.layers > 1 ? info.layers : void 0,
    mip: info.mip || void 0,
    mips: (info.mips ?? 1) > 1 ? info.mips : void 0,
    aspect: info.aspect,
    samples: (info.samples ?? 1) > 1 ? info.samples : void 0,
    attachment: sampled ? void 0 : info.attachment,
    resolve: info.resolve || void 0,
    pass: pass >= 0 ? pass : void 0,
    frame: c2.data.frames > 1 ? info.frame : void 0,
    bytes: t.data?.byteLength,
    error: info.error
  };
}
function page(items, args, defaultLimit, maxLimit) {
  const offset = intArg(args, "offset", 0, 0);
  const limit = intArg(args, "limit", defaultLimit, 1, maxLimit);
  const slice = items.slice(offset, offset + limit);
  return { items: slice, total: items.length, offset, ...offset + slice.length < items.length ? { nextOffset: offset + slice.length } : {} };
}

// src/mcp/search_paths.ts
import fs4 from "node:fs";

// src/main/shader_sources.ts
import fs2 from "node:fs";
import path2 from "node:path";
var SEARCH_DEPTH = 6;
var MAX_SOURCE_BYTES = 4 * 1024 * 1024;
var SKIP_DIRS = /* @__PURE__ */ new Set(["node_modules", ".git", ".svn", "__pycache__"]);
var indexCache = /* @__PURE__ */ new Map();
function indexRoot(root) {
  const cached = indexCache.get(root);
  if (cached) return cached;
  const byName = /* @__PURE__ */ new Map();
  const visit = (dir, depth) => {
    let entries;
    try {
      entries = fs2.readdirSync(dir, { withFileTypes: true });
    } catch {
      return;
    }
    for (const e of entries) {
      const full = path2.join(dir, e.name);
      if (e.isFile()) {
        const key = e.name.toLowerCase();
        const list = byName.get(key);
        if (list) list.push(full);
        else byName.set(key, [full]);
      } else if (e.isDirectory() && depth < SEARCH_DEPTH && !SKIP_DIRS.has(e.name) && !e.name.startsWith(".")) {
        visit(full, depth + 1);
      }
    }
  };
  visit(root, 0);
  indexCache.set(root, byName);
  return byName;
}
function forgetSourceIndex() {
  indexCache.clear();
}
function parts(p) {
  return p.replace(/\\/g, "/").split("/").filter((s) => s && s !== ".");
}
function suffixMatch(name, candidate) {
  const c2 = parts(candidate).map((s) => s.toLowerCase());
  let n = 0;
  while (n < name.length && n < c2.length && name[name.length - 1 - n].toLowerCase() === c2[c2.length - 1 - n]) n++;
  return n;
}
function readText(file) {
  try {
    if (fs2.statSync(file).size > MAX_SOURCE_BYTES) return null;
    return fs2.readFileSync(file, "utf8");
  } catch {
    return null;
  }
}
function findShaderSources(names, roots) {
  const out = {};
  const cleanRoots = roots.map((r) => r.trim()).filter((r) => r && fs2.existsSync(r));
  for (const name of names) {
    if (!name) continue;
    if (path2.isAbsolute(name) && fs2.existsSync(name)) {
      const text2 = readText(name);
      if (text2 !== null) {
        out[name] = text2;
        continue;
      }
    }
    const nameParts = parts(name);
    const base = nameParts[nameParts.length - 1]?.toLowerCase();
    if (!base) continue;
    let best = null;
    for (const root of cleanRoots) {
      for (const file of indexRoot(root).get(base) ?? []) {
        const score = suffixMatch(nameParts, file);
        if (!best || score > best.score) best = { file, score };
      }
    }
    if (!best) continue;
    const text = readText(best.file);
    if (text !== null) out[name] = text;
  }
  return out;
}

// src/main/symbolize.ts
import { execFile } from "node:child_process";
import fs3 from "node:fs";
import os2 from "node:os";
import path3 from "node:path";
var SEARCH_DEPTH2 = 5;
var SYMBOLIZER_TIMEOUT_MS = 3e4;
function findSymbolizer() {
  const exe = process.platform === "win32" ? ".exe" : "";
  const roots = [];
  for (const v of ["ANDROID_NDK_HOME", "ANDROID_NDK_ROOT", "ANDROID_NDK"]) if (process.env[v]) roots.push(process.env[v]);
  const sdks = [];
  for (const v of ["ANDROID_HOME", "ANDROID_SDK_ROOT"]) if (process.env[v]) sdks.push(process.env[v]);
  if (process.platform === "win32" && process.env.LOCALAPPDATA) sdks.push(path3.join(process.env.LOCALAPPDATA, "Android", "Sdk"));
  else if (process.platform === "darwin") sdks.push(path3.join(os2.homedir(), "Library", "Android", "sdk"));
  else sdks.push(path3.join(os2.homedir(), "Android", "Sdk"));
  for (const sdk of sdks) {
    const ndk = path3.join(sdk, "ndk");
    if (fs3.existsSync(ndk)) for (const v of fs3.readdirSync(ndk).sort().reverse()) roots.push(path3.join(ndk, v));
  }
  for (const root of roots) {
    const prebuilt = path3.join(root, "toolchains", "llvm", "prebuilt");
    if (!fs3.existsSync(prebuilt)) continue;
    for (const host of fs3.readdirSync(prebuilt)) {
      const candidate = path3.join(prebuilt, host, "bin", `llvm-symbolizer${exe}`);
      if (fs3.existsSync(candidate)) return { exe: candidate, llvm: true };
    }
  }
  for (const dir of (process.env.PATH ?? "").split(path3.delimiter)) {
    if (!dir) continue;
    for (const [name, llvm] of [["llvm-symbolizer", true], ["addr2line", false]]) {
      const candidate = path3.join(dir, name + exe);
      if (fs3.existsSync(candidate)) return { exe: candidate, llvm };
    }
  }
  return null;
}
function findModule(module, dirs) {
  let best = null;
  const visit = (dir, depth) => {
    let entries;
    try {
      entries = fs3.readdirSync(dir, { withFileTypes: true });
    } catch {
      return;
    }
    for (const e of entries) {
      const full = path3.join(dir, e.name);
      if (e.isFile() && e.name === module) {
        const size2 = fs3.statSync(full).size;
        if (!best || size2 > best.size) best = { file: full, size: size2 };
      } else if (e.isDirectory() && depth < SEARCH_DEPTH2 && !e.name.startsWith(".") && e.name !== "node_modules") {
        visit(full, depth + 1);
      }
    }
  };
  for (const d of dirs) if (d) visit(d, 0);
  return best ? best.file : null;
}
function run(exe, args) {
  return new Promise((resolve) => {
    execFile(exe, args, { timeout: SYMBOLIZER_TIMEOUT_MS, maxBuffer: 16 * 1024 * 1024, windowsHide: true }, (err, stdout) => resolve(err ? "" : stdout));
  });
}
var moduleCache = /* @__PURE__ */ new Map();
async function symbolizeFrames(frames, dirs) {
  const wanted = frames.filter((f) => f.module && f.offset > 0 && !f.file && !f.internal);
  if (!wanted.length || !dirs.length) return [];
  const tool = findSymbolizer();
  if (!tool) return [];
  const byModule = /* @__PURE__ */ new Map();
  for (const f of wanted) {
    const list = byModule.get(f.module);
    if (list) list.push(f);
    else byModule.set(f.module, [f]);
  }
  const out = [];
  for (const [module, list] of byModule) {
    const key = `${dirs.join(";")}|${module}`;
    let file = moduleCache.get(key);
    if (file === void 0) {
      file = findModule(module, dirs);
      moduleCache.set(key, file);
    }
    if (!file) continue;
    const offsets = list.map((f) => `0x${f.offset.toString(16)}`);
    if (tool.llvm) {
      const text2 = await run(tool.exe, [`--obj=${file}`, "--functions=linkage", "--demangle", "--inlining=true", "--output-style=JSON", ...offsets]);
      let entries = [];
      try {
        entries = JSON.parse(text2);
      } catch {
        continue;
      }
      for (let i = 0; i < list.length && i < entries.length; ++i) {
        const symbols = (entries[i].Symbol ?? []).map((s) => ({
          function: s.FunctionName && s.FunctionName !== "??" ? s.FunctionName : void 0,
          file: s.FileName && s.FileName !== "??" && (s.Line ?? 0) > 0 ? s.FileName : void 0,
          line: (s.Line ?? 0) > 0 ? s.Line : void 0
        }));
        const inner = symbols[0];
        if (!inner || !inner.function && !inner.file) continue;
        const resolved = { ...list[i] };
        if (inner.function) resolved.function = inner.function;
        if (inner.file) {
          resolved.file = inner.file;
          resolved.line = inner.line;
        }
        const callers = symbols.slice(1).filter((s) => s.function || s.file);
        if (callers.length) resolved.inlinedInto = callers;
        out.push(resolved);
      }
      continue;
    }
    const text = await run(tool.exe, ["-C", "-f", "-e", file, ...offsets]);
    const lines = text.split(/\r?\n/).map((l) => l.trim()).filter((l) => l.length);
    for (let i = 0; i < list.length && 2 * i + 1 < lines.length; ++i) {
      const fn = lines[2 * i];
      const loc = /^(.*?):(\d+)(?::\d+)?$/.exec(lines[2 * i + 1]);
      const f = list[i];
      const resolved = { ...f };
      let gained = false;
      if (fn && fn !== "??") {
        resolved.function = fn;
        gained = true;
      }
      if (loc && loc[1] !== "??" && Number(loc[2]) > 0) {
        resolved.file = loc[1];
        resolved.line = Number(loc[2]);
        gained = true;
      }
      if (gained) out.push(resolved);
    }
  }
  return out;
}

// src/mcp/search_paths.ts
var ENV = { sourceRoots: "GPU_INSPECTOR_SOURCE_ROOTS", symbolDirs: "GPU_INSPECTOR_SYMBOL_DIRS" };
var overrides = {};
function splitPaths(value) {
  const items = Array.isArray(value) ? value.map(String) : typeof value === "string" ? value.split(";") : [];
  return items.map((s) => s.trim()).filter(Boolean);
}
function searchPaths(kind) {
  const set = overrides[kind];
  if (set) return { dirs: set, from: "set_search_paths" };
  const env = splitPaths(process.env[ENV[kind]]);
  if (env.length) return { dirs: env, from: ENV[kind] };
  const saved = splitPaths(appSetting(kind));
  if (saved.length) return { dirs: saved, from: "GPU Inspector's settings" };
  return { dirs: [], from: "none" };
}
function setSearchPaths(kind, dirs) {
  if (dirs.length) overrides[kind] = dirs;
  else delete overrides[kind];
  if (kind === "sourceRoots") forgetSourceIndex();
}
function describeSearchPaths() {
  const describe = (kind) => {
    const { dirs, from } = searchPaths(kind);
    const missing = dirs.filter((d) => !fs4.existsSync(d));
    return { dirs, from, missing: missing.length ? missing : void 0 };
  };
  const symbolizer = findSymbolizer();
  return {
    sourceRoots: describe("sourceRoots"),
    symbolDirs: describe("symbolDirs"),
    symbolizer: symbolizer ? symbolizer.exe : "None found: llvm-symbolizer from the Android NDK (ANDROID_NDK_HOME), or llvm-symbolizer or addr2line on PATH, resolves frames under symbolDirs."
  };
}
function debugInfoWithSources(spirv) {
  const info = parseSpirvDebugInfo(spirv);
  if (!info) return { info, found: [] };
  const missing = info.files.filter((f) => f.text === null && f.name);
  if (missing.length) {
    const texts = findShaderSources(missing.map((f) => f.name), searchPaths("sourceRoots").dirs);
    for (const f of missing) {
      const text = texts[f.name];
      if (typeof text !== "string") continue;
      f.text = text;
      f.fromHost = true;
    }
    if (info.mainFile < 0) info.mainFile = info.files.findIndex((f) => f.text !== null);
  }
  return { info, found: info.files.filter((f) => f.fromHost).map((f) => f.name) };
}
function sourceLineTexts(info) {
  const out = /* @__PURE__ */ new Map();
  for (const f of info?.files ?? []) {
    if (f.text === null) continue;
    const map = sourceLineMap(f.text);
    const byLine = /* @__PURE__ */ new Map();
    map.lines.forEach((text, i) => {
      if (map.lineOf[i] > 0) byLine.set(map.lineOf[i], text.trim());
    });
    out.set(f.name, byLine);
    const base = f.name.split(/[\\/]/).pop();
    if (base && !out.has(base)) out.set(base, byLine);
  }
  return out;
}
function codeAt(texts, file, line) {
  if (!line) return void 0;
  const base = file?.split(/[\\/]/).pop();
  const byLine = file ? texts.get(file) ?? (base ? texts.get(base) : void 0) : texts.size === 1 ? [...texts.values()][0] : void 0;
  const code = byLine?.get(line);
  if (!code) return void 0;
  return code.length > 200 ? `${code.slice(0, 200)}...` : code;
}
async function symbolizeOnHost(frames) {
  const dirs = searchPaths("symbolDirs").dirs;
  if (!dirs.length || !frames.some((f) => f.module && f.offset > 0 && !f.file && !f.internal)) return frames;
  const resolved = await symbolizeFrames(frames, dirs);
  if (!resolved.length) return frames;
  const byKey = new Map(resolved.map((f) => [`${f.module}+${f.offset}`, f]));
  return frames.map((f) => byKey.get(`${f.module}+${f.offset}`) ?? f);
}
async function symbolizeSymbolMap(db, frames) {
  const list = [...frames.values()];
  const resolved = await symbolizeOnHost(list);
  resolved.forEach((f, i) => {
    if (f === list[i]) return;
    frames.set(f.address, f);
    db.symbols.set(f.address, f);
  });
}

// src/mcp/command_tools.ts
var KINDS = ["all", "draw", "dispatch", "action", "pass", "bind", "label", "submit", "issue", "validation"];
var SEVERITY_ORDER = ["error", "warning", "info", "verbose"];
var INDIRECT_FIELDS = {
  vkCmdDrawIndirect: ["vertexCount", "instanceCount", "firstVertex", "firstInstance"],
  vkCmdDrawIndexedIndirect: ["indexCount", "instanceCount", "firstIndex", "vertexOffset", "firstInstance"],
  vkCmdDispatchIndirect: ["x", "y", "z"]
};
function vertexStruct(layout, inputs) {
  return {
    kind: "struct",
    name: "Vertex",
    size: layout.stride,
    members: layout.attributes.map((a) => ({
      name: inputs.find((i) => i.location === a.location)?.name || `location${a.location}`,
      offset: a.offset,
      type: { kind: "format", format: a.format, size: vertexFormat(a.format)?.size ?? 0 }
    }))
  };
}
function vertexInputs(c2, pipeline) {
  if (!pipeline || pipeline.type.startsWith("MTL")) return [];
  const vs = pipelineStages(pipeline, c2.db).find((s) => s.stage === "vertex");
  return (vs && c2.reflection(vs.object, vs.blobIndex)?.entryPoint(vs.entryPoint)?.inputs) ?? [];
}
function indexSize(indexType) {
  if (/uint8/i.test(indexType)) return 1;
  if (/16/.test(indexType)) return 2;
  if (/32/.test(indexType)) return 4;
  return 0;
}
function readIndex(view, at, size2) {
  return size2 === 1 ? view.getUint8(at) : size2 === 2 ? view.getUint16(at, true) : view.getUint32(at, true);
}
function pick(o, keys) {
  if (!isObject(o)) return void 0;
  const out = {};
  for (const k of keys) if (o[k] !== void 0) out[k] = o[k];
  return out;
}
function fixedFunctionState(p) {
  const d = p.descriptor;
  if (p.type !== "VkPipeline" || !d || !Array.isArray(d.pStages)) return void 0;
  const blend = isObject(d.pColorBlendState) ? d.pColorBlendState : null;
  const dynamic = isObject(d.pDynamicState) ? d.pDynamicState.pDynamicStates : void 0;
  return {
    topology: isObject(d.pInputAssemblyState) ? d.pInputAssemblyState.topology : void 0,
    rasterization: pick(d.pRasterizationState, ["polygonMode", "cullMode", "frontFace", "depthClampEnable", "depthBiasEnable", "rasterizerDiscardEnable"]),
    depthStencil: pick(d.pDepthStencilState, ["depthTestEnable", "depthWriteEnable", "depthCompareOp", "depthBoundsTestEnable", "stencilTestEnable"]),
    blend: blend && Array.isArray(blend.pAttachments) ? blend.pAttachments.map((a) => pick(a, ["blendEnable", "srcColorBlendFactor", "dstColorBlendFactor", "colorBlendOp", "srcAlphaBlendFactor", "dstAlphaBlendFactor", "alphaBlendOp", "colorWriteMask"])) : void 0,
    samples: isObject(d.pMultisampleState) ? d.pMultisampleState.rasterizationSamples : void 0,
    dynamicStates: Array.isArray(dynamic) ? dynamic : void 0
  };
}
var StateReader = class {
  constructor(c2, state, values) {
    this.c = c2;
    this.state = state;
    this.values = values;
  }
  _stages = null;
  get stages() {
    if (!this._stages) {
      const p = this.state.pipeline;
      this._stages = p && !p.type.startsWith("MTL") ? pipelineStages(p, this.c.db).map((s) => ({ stage: s.stage, entryPoint: s.entryPoint, object: s.object, blobIndex: s.blobIndex, reflection: this.c.reflection(s.object, s.blobIndex) })) : [];
    }
    return this._stages;
  }
  action(cmd) {
    const sets = this.c.data.sets;
    const state = this.state;
    const graphics = state.bindPoint === sets.graphicsBindPoint;
    const stageBuffers = [...state.stageBuffers.values()].filter((sb) => graphics ? sb.stage !== "compute" : sb.stage === "compute");
    return {
      bindPoint: state.bindPoint,
      pipeline: this.pipeline(),
      descriptorSets: state.sets.size ? this.sets([...state.sets.values()].sort((a, b) => a.set.set - b.set.set)) : void 0,
      vertexBuffers: graphics && state.vertexBuffers.size ? this.vertexBuffers([...state.vertexBuffers.values()].sort((a, b) => a.binding - b.binding)) : void 0,
      indexBuffer: graphics && state.indexBuffer ? this.indexBuffer(state.indexBuffer, cmd) : void 0,
      stageBuffers: stageBuffers.length ? this.stageBuffers(stageBuffers) : void 0,
      pushConstants: state.pushConstants.length ? this.pushConstants() : void 0,
      viewports: state.viewports ? compact(state.viewports, this.c.db) : void 0,
      scissors: state.scissors ? compact(state.scissors, this.c.db) : void 0,
      indirect: sets.INDIRECT.has(cmd.method) ? this.indirect(cmd) : void 0,
      renderTargets: sets.DRAW.has(cmd.method) ? this.targetsOf(cmd) : void 0
    };
  }
  pipeline() {
    const p = this.state.pipeline;
    if (!p) return void 0;
    const db = this.c.db;
    const metal = p.type.startsWith("MTL");
    return {
      pipeline: refText(db, p.id),
      boundAt: this.state.pipelineCmd?.index,
      summary: p.summary(db) || void 0,
      stages: metal ? metalStages(p).map((s) => ({ stage: s.stage, buffers: s.buffers.size, textures: s.textures.size, samplers: s.samplers.size })) : this.stages.map((s) => ({ stage: s.stage, entryPoint: s.entryPoint, shader: refText(db, s.object.id), blob: s.blobIndex })),
      functions: metal ? [...p.dependencies].filter((o) => o.type === "MTLFunction").map((o) => refText(db, o.id)) : void 0,
      fixedFunction: fixedFunctionState(p)
    };
  }
  sets(bound) {
    const db = this.c.db;
    return bound.map(({ cmd, set }) => ({
      set: set.set,
      boundAt: cmd.index,
      descriptorSet: set.descriptorSet ? refText(db, set.descriptorSet) : "push descriptors",
      layout: refText(db, set.layout ?? void 0),
      bindings: set.bindings.map((b) => this.binding(set.set, b))
    }));
  }
  binding(set, b) {
    let res = null;
    for (const s of this.stages) {
      res = s.reflection?.findResource(set, b.binding) ?? null;
      if (res) break;
    }
    const shown = b.descriptors.slice(0, 16);
    return {
      binding: b.binding,
      type: b.type,
      stages: b.stages,
      name: res?.name || void 0,
      shaderType: res?.typeName || void 0,
      descriptors: shown.map((d) => d ? this.descriptor(d, res) : null),
      more: b.descriptors.length > shown.length ? b.descriptors.length - shown.length : void 0
    };
  }
  descriptor(d, res) {
    const c2 = this.c;
    const db = c2.db;
    if (d.buffer !== void 0) {
      return { buffer: refText(db, d.buffer), offset: d.offset, range: d.range, dynamicOffset: d.dynamicOffset, ...this.captured(d.data, res && res.type.kind !== "opaque" ? res.type : null) };
    }
    if (d.imageView !== void 0 || d.sampler !== void 0) {
      const texture = c2.data.capturedImage(d.data);
      return {
        imageView: refText(db, d.imageView),
        image: refText(db, imageOfView(db, refId(d.imageView ?? void 0))),
        layout: d.imageLayout,
        sampler: refText(db, d.sampler),
        texture: texture ? c2.data.textures.indexOf(texture) : void 0
      };
    }
    return compact(d, db);
  }
  /** What the capture read back of a bound range: its size, and its values when the type is known. */
  captured(dataId, type) {
    if (!dataId) return {};
    const b = this.c.data.buffer(dataId);
    if (!b) return { data: dataId };
    if (b.info.error) return { data: dataId, captureError: b.info.error };
    const out = { data: dataId, capturedBytes: b.data?.byteLength ?? 0, truncatedFrom: b.info.originalSize };
    if (this.values && type && b.data?.byteLength) {
      out.values = readTyped(type, new DataView(b.data.buffer, b.data.byteOffset, b.data.byteLength), 0, { values: 256 });
    }
    return out;
  }
  vertexBuffers(buffers) {
    const db = this.c.db;
    const vs = this.stages.find((s) => s.stage === "vertex");
    const inputs = vs?.reflection?.entryPoint(vs.entryPoint)?.inputs ?? [];
    const out = [];
    for (const vb of buffers) {
      const layout = vertexLayout(this.state, vb.binding, vb);
      if (!layout && this.c.data.sets.BIND_STAGE_BUFFER) continue;
      let firstVertices = null;
      if (layout?.stride) firstVertices = { kind: "array", element: vertexStruct(layout, inputs), count: 3, stride: layout.stride, size: 3 * layout.stride };
      out.push({
        binding: vb.binding,
        buffer: refText(db, vb.buffer),
        offset: vb.offset,
        boundAt: vb.cmd.index,
        stride: layout?.stride,
        perInstance: layout?.rate.includes("INSTANCE") || void 0,
        attributes: layout?.attributes.map((a) => ({ location: a.location, name: inputs.find((i) => i.location === a.location)?.name || void 0, format: a.format, offset: a.offset })),
        ...this.captured(vb.dataId, firstVertices)
      });
    }
    return out;
  }
  indexBuffer(ib, draw) {
    const out = { buffer: refText(this.c.db, ib.buffer), offset: ib.offset, indexType: ib.indexType, boundAt: ib.cmd.index, ...this.captured(ib.dataId, null) };
    const data = this.c.data.buffer(ib.dataId)?.data;
    const size2 = indexSize(ib.indexType);
    if (this.values && data && size2) {
      const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
      const first = draw ? num(draw.args?.firstIndex) : 0;
      const values = [];
      for (let i = first; i < first + 12 && (i + 1) * size2 <= data.byteLength; i++) values.push(readIndex(view, i * size2, size2));
      out.indices = { first, values };
    }
    return out;
  }
  stageBuffers(list) {
    const db = this.c.db;
    const pipeline = this.state.pipeline;
    const reflected = metalStages(pipeline).length > 0;
    const slots = [];
    let unread = 0;
    for (const sb of [...list].sort((a, b) => a.stage.localeCompare(b.stage) || a.index - b.index)) {
      if (sb.stage === "vertex" && !sb.inline && vertexLayout(this.state, sb.index, { cmd: sb.cmd, binding: sb.index, buffer: sb.buffer, offset: sb.offset, size: null, stride: null, dataId: sb.dataId })) continue;
      const res = metalBufferResource(pipeline, sb.stage, sb.index);
      if (reflected ? !res : !sb.buffer && !sb.inline) {
        unread++;
        continue;
      }
      const bytes = this.c.data.buffer(sb.dataId)?.data;
      slots.push({
        stage: sb.stage,
        index: sb.index,
        name: res?.name || void 0,
        shaderType: res?.typeName || void 0,
        buffer: sb.inline ? "inline bytes" : refText(db, sb.buffer),
        offset: sb.offset,
        boundAt: sb.cmd.index,
        ...this.captured(sb.dataId, res?.type ?? null),
        argumentBuffer: res && bytes && isArgumentBufferType(res.type) ? argumentBufferBrief(db, argumentBufferEntries(res.type, bytes, db)) : void 0
      });
    }
    return { slots, unreadSlots: unread || void 0 };
  }
  pushConstants() {
    const updates = this.state.pushConstants;
    const block = this.stages.map((s) => s.reflection?.pushConstants[0]).find((r) => !!r);
    const out = {
      name: block?.name || void 0,
      shaderType: block?.typeName || void 0,
      updates: updates.map((pc) => ({ stageFlags: pc.stageFlags, offset: pc.offset, size: pc.size, boundAt: pc.cmd.index }))
    };
    if (!this.values) return out;
    const blockType = block?.type;
    const size2 = Math.max(blockType?.kind === "struct" ? blockType.size : 0, ...updates.map((pc) => pc.offset + pc.size));
    const bytes = new Uint8Array(size2);
    for (const pc of updates) {
      if (pc.data) bytes.set(pc.data.subarray(0, Math.max(0, Math.min(pc.data.byteLength, size2 - pc.offset))), pc.offset);
    }
    if (blockType?.kind === "struct") out.values = readTyped(blockType, new DataView(bytes.buffer), 0, { values: 256 });
    else out.bytes = Buffer.from(bytes).toString("hex");
    return out;
  }
  indirect(cmd) {
    const fields = INDIRECT_FIELDS[cmd.method];
    const b = this.c.data.buffer(cmd.bufferData?.[0]);
    if (!fields || !b) return void 0;
    if (!b.data) return { data: b.info.id, captureError: b.info.error };
    const stride = Math.max(fields.length * 4, num(cmd.args?.stride));
    const count2 = Math.min(8, Math.max(1, num(cmd.args?.drawCount)), Math.floor(b.data.byteLength / stride));
    const view = new DataView(b.data.buffer, b.data.byteOffset, b.data.byteLength);
    const entries = [];
    for (let i = 0; i < count2; i++) {
      const e = {};
      fields.forEach((f, k) => {
        e[f] = f === "vertexOffset" ? view.getInt32(i * stride + k * 4, true) : view.getUint32(i * stride + k * 4, true);
      });
      entries.push(e);
    }
    return { data: b.info.id, drawCount: num(cmd.args?.drawCount) || void 0, entries };
  }
  /** The read-back targets of the pass a command is in (or begins). */
  targetsOf(cmd) {
    const pass = findPass(this.c.data, cmd);
    if (!pass) return void 0;
    return this.c.data.texturesForPass(cmd.frame, cmd.object?.__id ?? 0, pass.passIndex).map((t) => textureBrief(this.c, t));
  }
};
var MAX_ARGUMENT_ENTRIES = 64;
function argumentBufferBrief(db, entries) {
  if (!entries.length) return void 0;
  const out = entries.slice(0, MAX_ARGUMENT_ENTRIES).map((e) => ({
    member: e.path,
    kind: e.kind,
    type: e.typeName,
    resource: e.object ? refText(db, e.object.id) : void 0,
    offset: e.objectOffset || void 0,
    value: e.object ? void 0 : e.value === null ? "past the captured range" : e.value === "0x0" ? "null" : `${e.value}: no tracked ${e.kind === "pointer" ? "buffer holds this address" : "object has this id"}`
  }));
  if (entries.length > MAX_ARGUMENT_ENTRIES) out.push(`... ${entries.length - MAX_ARGUMENT_ENTRIES} more members`);
  return out;
}
function commandDetail(c2, cmd, values) {
  const d = c2.data;
  const db = c2.db;
  const sets = d.sets;
  const passIndex = c2.passOf(cmd.index);
  const pass = passIndex >= 0 ? c2.metrics.passes[passIndex] : null;
  const issues = c2.analysis.byCommand.get(cmd.index);
  const validation = db.validationForCommand(cmd.secondary ?? cmd.object?.__id, cmd.slot);
  const out = {
    capture: c2.id,
    index: cmd.index,
    frame: d.frames > 1 ? cmd.frame : void 0,
    method: cmd.method,
    object: refText(db, cmd.object),
    secondary: refText(db, cmd.secondary),
    labels: c2.labelsOf(cmd.index) || void 0,
    pass: pass ? { pass: passIndex, label: c2.passName(passIndex), begin: pass.commandIndex, end: pass.endIndex, ms: round(pass.durationMs) } : void 0,
    result: cmd.result || void 0,
    args: compact(cmd.args, db),
    issues: issues?.map((f) => ({ rule: f.rule, severity: f.severity, confidence: f.confidence, message: f.message })),
    validation: validation.length ? validation.map((v) => validationBrief(c2, v)) : void 0,
    stack: cmd.stack?.length ? stackLines(cmd.stack.map((a) => db.symbols.get(a) ?? { address: a, offset: 0 })) : void 0
  };
  const m = cmd.method;
  if (isAction(sets, m)) {
    out.state = new StateReader(c2, drawState(d, db, cmd), values).action(cmd);
  } else if (sets.BIND_PIPELINE.has(m)) {
    const state = emptyDrawState(sets.pipelineBindPointOf(m, cmd.args));
    state.pipelineCmd = cmd;
    state.pipeline = db.getObject(refId(cmd.args?.pipeline));
    out.pipeline = new StateReader(c2, state, values).pipeline();
  } else if (sets.BIND_DESCRIPTOR.has(m) && cmd.descriptors) {
    const reader = new StateReader(c2, bindingState(d, db, cmd, cmd.descriptors.bindPoint), values);
    out.descriptorSets = reader.sets(cmd.descriptors.sets.map((set) => ({ cmd, set })));
  } else if (sets.BIND_STAGE_BUFFER?.has(m) && sets.stageBuffersOf) {
    const bound = sets.stageBuffersOf(cmd);
    const reader = new StateReader(c2, bindingState(d, db, cmd, bound.some((sb) => sb.stage === "compute") ? "compute" : sets.graphicsBindPoint), values);
    if (sets.BIND_VERTEX.has(m)) out.vertexBuffers = reader.vertexBuffers(sets.vertexBuffersOf(cmd));
    out.stageBuffers = reader.stageBuffers(bound);
  } else if (sets.BIND_VERTEX.has(m)) {
    out.vertexBuffers = new StateReader(c2, bindingState(d, db, cmd, sets.graphicsBindPoint), values).vertexBuffers(sets.vertexBuffersOf(cmd));
  } else if (sets.BIND_INDEX.has(m)) {
    const ib = sets.indexBufferOf(cmd);
    if (ib) out.indexBuffer = new StateReader(c2, emptyDrawState(sets.graphicsBindPoint), values).indexBuffer(ib, null);
  } else if (sets.PUSH_CONSTANT.has(m)) {
    const pc = pushConstantOf(cmd);
    const state = bindingState(d, db, cmd, pc?.stageFlags.includes("COMPUTE") ? "VK_PIPELINE_BIND_POINT_COMPUTE" : "VK_PIPELINE_BIND_POINT_GRAPHICS");
    if (pc) state.pushConstants.push(pc);
    out.pushConstants = new StateReader(c2, state, values).pushConstants();
  } else if (sets.PASS_BEGIN.has(m)) {
    out.renderTargets = new StateReader(c2, emptyDrawState(""), values).targetsOf(cmd);
  }
  return out;
}
function objectDetail(db, o) {
  const dependencies = [...o.dependencies];
  const dependents = [...o.dependents];
  return {
    id: o.id,
    type: o.type,
    name: o.name !== `${o.shortType} ${o.id}` ? o.name : void 0,
    handle: o.handle,
    createdBy: o.cmd,
    parent: refText(db, o.parentId),
    destroyed: o.isDeleted || void 0,
    invalid: o.invalidReason ?? void 0,
    summary: o.summary(db) || void 0,
    memoryBytes: objectMemoryBytes(o, db) || void 0,
    args: compact(o.args, db),
    updates: Object.keys(o.updates).length ? compact(o.updates, db) : void 0,
    fixedFunction: fixedFunctionState(o),
    dependsOn: dependencies.length ? dependencies.slice(0, 100).map((x) => refText(db, x.id)) : void 0,
    dependents: dependents.length ? dependents.slice(0, 100).map((x) => refText(db, x.id)) : void 0,
    moreDependents: dependents.length > 100 ? dependents.length - 100 : void 0,
    payloads: o.blobs.length ? o.blobs.map((b, i) => ({ index: i, name: b.name, bytes: b.size })) : void 0
  };
}
function commandTools(store) {
  return [
    {
      name: "list_commands",
      description: "List a capture's commands in submission order (secondary command buffers inlined where they executed), with each command's key arguments, its pass, the debug groups around it, and the Frame Issues rules and validation messages that apply to it. Filter by kind, method, pass, debug group label or frame; page with offset and limit.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        kind: { type: "string", enum: KINDS, description: "draw, dispatch, action (draws, dispatches, ray tracing), pass (pass begins), bind, label (debug group begins), submit, issue (commands a Frame Issues finding names), validation (commands a validation message fired on), or all (default)." },
        method: { type: "string", description: 'Regular expression on the method name ("DrawIndexed", "^vkCmdBind").' },
        pass: { type: "integer", minimum: 0, description: "Only the commands of this pass (the pass numbers of get_bottlenecks and get_command)." },
        label: { type: "string", description: 'Regular expression on the debug group path ("Shadows", "Opaque / Terrain").' },
        frame: { type: "integer", minimum: 0, description: "Only this captured frame (0-based), for multi-frame captures." },
        ...PAGE_PARAMS
      }),
      readOnly: true,
      handler: (args) => {
        const c2 = store.resolve(stringArg(args, "capture"));
        const d = c2.data;
        const db = c2.db;
        const sets = d.sets;
        const kind = enumArg(args, "kind", KINDS, "all");
        const method = regexArg(args, "method");
        const label = regexArg(args, "label");
        const pass = optionalInt(args, "pass");
        const frame = optionalInt(args, "frame");
        const byCommand = c2.analysis.byCommand;
        const validationOf = (cmd) => db.validationForCommand(cmd.secondary ?? cmd.object?.__id, cmd.slot);
        const matches = d.commands.filter((cmd) => {
          if (frame !== void 0 && cmd.frame !== frame) return false;
          if (pass !== void 0 && c2.passOf(cmd.index) !== pass) return false;
          if (method && !method.test(cmd.method)) return false;
          if (label && !label.test(c2.labelsOf(cmd.index))) return false;
          const m = cmd.method;
          switch (kind) {
            case "draw":
              return sets.DRAW.has(m);
            case "dispatch":
              return sets.DISPATCH.has(m);
            case "action":
              return isAction(sets, m);
            case "pass":
              return sets.PASS_BEGIN.has(m);
            case "bind":
              return sets.BIND_PIPELINE.has(m) || sets.BIND_DESCRIPTOR.has(m) || sets.BIND_VERTEX.has(m) || sets.BIND_INDEX.has(m) || sets.PUSH_CONSTANT.has(m) || !!sets.BIND_STAGE_BUFFER?.has(m);
            case "label":
              return sets.LABEL_BEGIN.has(m);
            case "submit":
              return sets.SUBMIT.has(m);
            case "issue":
              return byCommand.has(cmd.index);
            case "validation":
              return validationOf(cmd).length > 0;
            default:
              return true;
          }
        });
        const p = page(matches, args, 100, 500);
        const nameOf = (v) => refText(db, v) ?? "";
        return jsonResult({
          capture: c2.id,
          total: p.total,
          offset: p.offset,
          nextOffset: p.nextOffset,
          commands: p.items.map((cmd) => {
            const passIndex = c2.passOf(cmd.index);
            const issues = byCommand.get(cmd.index);
            const validation = validationOf(cmd);
            return {
              i: cmd.index,
              method: cmd.method,
              args: sets.summarize?.(cmd, nameOf) || void 0,
              frame: d.frames > 1 ? cmd.frame : void 0,
              pass: passIndex >= 0 ? passIndex : void 0,
              labels: c2.labelsOf(cmd.index) || void 0,
              issues: issues ? [...new Set(issues.map((f) => f.rule))] : void 0,
              validation: validation.length ? SEVERITY_ORDER.find((s) => validation.some((v) => v.severity === s)) : void 0
            };
          })
        });
      }
    },
    {
      name: "get_command",
      description: "One command in full: its arguments (object references as Type#id), pass and debug groups, the Frame Issues and validation messages on it, its recording stack when captured, and for a draw, dispatch or binding command the state bound at it \u2014 the pipeline with its shader stages and fixed-function state (topology, cull mode, depth test, blending), every descriptor set with each binding's shader name and its uniform or storage buffer values decoded by the shader's reflection, sampled images (with their read-back texture numbers), vertex buffers with the layout and first vertices, the index buffer with the first indices, push constants with values, viewports and scissors, indirect arguments, the pass's render targets, and on Metal the stage buffers, with an argument buffer's members resolved to the buffers, textures and samplers they hold. Use it to see what a draw actually read.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        index: { type: "integer", minimum: 0, description: "The command's index (from list_commands, a finding or a message)." },
        values: { type: "boolean", description: "Decode buffer and push constant values (default true); false for the structure alone." }
      }, ["index"]),
      readOnly: true,
      handler: async (args) => {
        const c2 = store.resolve(stringArg(args, "capture"));
        const index = requireInt(args, "index");
        const cmd = c2.data.commands[index];
        if (!cmd) throw new Error(`No command ${index}: ${c2.id} has ${c2.data.commands.length} commands (0-${c2.data.commands.length - 1}).`);
        const detail = commandDetail(c2, cmd, boolArg(args, "values", true));
        if (cmd.stack?.length) detail.stack = stackLines(await symbolizeOnHost(cmd.stack.map((a) => c2.db.symbols.get(a) ?? { address: a, offset: 0 })));
        return jsonResult(detail);
      }
    },
    {
      name: "list_objects",
      description: "List the objects a capture references (images, buffers, pipelines, shader modules, render passes, Metal textures, libraries and pipeline states...), with a one-line summary each. Without a type filter it also counts them by type. Objects destroyed before the capture was saved are kept when something still references them.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        type: { type: "string", description: 'Only this type: "VkImage", "VkPipeline", "MTLTexture" (the Vk prefix may be left out).' },
        name: { type: "string", description: "Regular expression on the object's name or label." },
        ...PAGE_PARAMS
      }),
      readOnly: true,
      handler: (args) => {
        const c2 = store.resolve(stringArg(args, "capture"));
        const db = c2.db;
        const type = stringArg(args, "type")?.toLowerCase();
        const name = regexArg(args, "name");
        const all = [...db.allObjects.values(), ...db.destroyedObjects.values()].sort((a, b) => a.id - b.id);
        const list = all.filter((o) => (!type || o.type.toLowerCase() === type || o.shortType.toLowerCase() === type) && (!name || name.test(o.name) || name.test(o.label)));
        const types = {};
        if (!type) for (const o of all) types[o.type] = (types[o.type] ?? 0) + 1;
        const p = page(list, args, 100, 500);
        return jsonResult({
          capture: c2.id,
          types: type ? void 0 : types,
          total: p.total,
          offset: p.offset,
          nextOffset: p.nextOffset,
          objects: p.items.map((o) => ({
            id: o.id,
            type: o.type,
            name: o.name !== `${o.shortType} ${o.id}` ? o.name : void 0,
            summary: o.summary(db) || void 0,
            destroyed: o.isDeleted || void 0
          }))
        });
      }
    },
    {
      name: "get_object",
      description: "One object in full: the call that created it with its arguments (the create info), later updates (memory bindings, descriptor contents, device properties), its owner, what it depends on and what depends on it, its payloads, its read-back images or buffer ranges in the capture, the validation messages naming it, and its creation stack when captured. Shader-bearing objects point at get_shader.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        id: { type: "integer", minimum: 0, description: "The object's id: the number after # in a reference like VkImage#12." }
      }, ["id"]),
      readOnly: true,
      handler: async (args) => {
        const c2 = store.resolve(stringArg(args, "capture"));
        const db = c2.db;
        const d = c2.data;
        const id = requireInt(args, "id");
        const o = db.getObject(id);
        if (!o) throw new Error(`No object ${id} in ${c2.id}.`);
        const stack = db.stacks.get(o.id);
        const shaderTypes = ["VkPipeline", "VkShaderModule", "MTLLibrary", "MTLFunction", "MTLRenderPipelineState", "MTLComputePipelineState"];
        const images = d.textures.filter((t) => t.info.id === o.id).map((t) => d.textures.indexOf(t));
        const ranges = [...d.buffers.values()].filter((b) => b.info.buffer === o.id);
        const validation = db.validationFor(o.id);
        return jsonResult({
          capture: c2.id,
          ...objectDetail(db, o),
          payloads: o.blobs.length ? o.blobs.map((b, i) => ({ index: i, name: b.name, bytes: b.size, inFile: db.blobData.has(`${o.id}:${i}`) })) : void 0,
          shader: shaderTypes.includes(o.type) ? "get_shader shows this object's code, reflection and analysis." : void 0,
          textures: images.length ? images : void 0,
          bufferRanges: ranges.length ? ranges.slice(0, 50).map((b) => ({ data: b.info.id, offset: b.info.offset, bytes: b.info.size, error: b.info.error })) : void 0,
          validation: validation.length ? validation.slice(0, 20).map((v) => validationBrief(c2, v, 600)) : void 0,
          creationStack: stack?.length ? stackLines(await symbolizeOnHost(stack)) : void 0
        });
      }
    },
    {
      name: "get_validation",
      description: "The validation messages the capture carries (from the Khronos validation layer, the driver, or Metal's validation layer, when enabled at launch), with repeat counts, the objects they name, and the captured command each fired on when the layer could tell. Errors are real bugs; report them first.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        severity: { type: "string", enum: ["error", "warning", "info", "verbose", "all"], description: "Only this severity (default all)." },
        ...PAGE_PARAMS
      }),
      readOnly: true,
      handler: (args) => {
        const c2 = store.resolve(stringArg(args, "capture"));
        const db = c2.db;
        const severity = enumArg(args, "severity", ["error", "warning", "info", "verbose", "all"], "all");
        const list = db.validation.filter((v) => severity === "all" || v.severity === severity);
        const [errors, warnings] = db.validationCounts;
        const p = page(list, args, 50, 200);
        return jsonResult({
          capture: c2.id,
          errors,
          warnings,
          total: p.total,
          offset: p.offset,
          nextOffset: p.nextOffset,
          dropped: db.validationDropped || void 0,
          messages: p.items.map((v) => validationBrief(c2, v)),
          note: db.validation.length ? void 0 : `No messages. The capture only has them when the application was launched with GPU Inspector's "Validation layer" option.`
        });
      }
    }
  ];
}

// src/mcp/live_session.ts
import { spawn as spawn2 } from "node:child_process";
import fs8 from "node:fs";
import net2 from "node:net";
import os5 from "node:os";
import path7 from "node:path";
import { fileURLToPath as fileURLToPath2 } from "node:url";

// src/main/android.ts
import { execFile as execFile2, execFileSync, spawn } from "node:child_process";
import crypto from "node:crypto";
import fs5 from "node:fs";
import os3 from "node:os";
import path4 from "node:path";
var LAYER_NAME = "VK_LAYER_INSPECTOR_capture";
var LAYER_LIB = "libVkLayer_inspector_capture.so";
var LAYER_APK = "gpu_inspector_layer.apk";
var DEVICE_TMP = "/data/local/tmp";
var ADB_TIMEOUT_MS = 2e4;
var INSTALL_TIMEOUT_MS = 18e4;
var START_TIMEOUT_MS = 6e4;
var PID_RETRIES = 20;
var PID_RETRY_MS = 500;
var POLL_MS = 2e3;
var MIN_SDK = 28;
var LAYER_APP_SDK = 29;
function findAdb() {
  const exe = process.platform === "win32" ? "adb.exe" : "adb";
  const candidates = [];
  if (process.env.INSPECTOR_ADB) candidates.push(process.env.INSPECTOR_ADB);
  for (const v of ["ANDROID_HOME", "ANDROID_SDK_ROOT"]) {
    if (process.env[v]) candidates.push(path4.join(process.env[v], "platform-tools", exe));
  }
  if (process.platform === "win32") {
    if (process.env.LOCALAPPDATA) candidates.push(path4.join(process.env.LOCALAPPDATA, "Android", "Sdk", "platform-tools", exe));
  } else if (process.platform === "darwin") {
    candidates.push(path4.join(os3.homedir(), "Library", "Android", "sdk", "platform-tools", exe));
  } else {
    candidates.push(path4.join(os3.homedir(), "Android", "Sdk", "platform-tools", exe), "/opt/android-sdk/platform-tools/adb");
  }
  for (const c2 of candidates) if (fs5.existsSync(c2)) return c2;
  for (const dir of (process.env.PATH ?? "").split(path4.delimiter)) {
    if (dir && fs5.existsSync(path4.join(dir, exe))) return path4.join(dir, exe);
  }
  return null;
}
function adbArgs(serial, args) {
  return serial ? ["-s", serial, ...args] : args;
}
function adb(adbPath, serial, args, timeoutMs = ADB_TIMEOUT_MS) {
  return new Promise((resolve, reject) => {
    execFile2(adbPath, adbArgs(serial, args), { timeout: timeoutMs, maxBuffer: 16 * 1024 * 1024, windowsHide: true }, (err, stdout, stderr) => {
      if (err) {
        const detail = `${stderr ?? ""}${stdout ?? ""}`.trim() || err.message;
        reject(new Error(`adb ${args[0] === "shell" ? "shell" : args.slice(0, 2).join(" ")}: ${detail}`));
      } else {
        resolve(stdout);
      }
    });
  });
}
function shell(adbPath, serial, command, timeoutMs = ADB_TIMEOUT_MS) {
  return adb(adbPath, serial, ["shell", command], timeoutMs);
}
function parseDevices(text) {
  const out = [];
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || line.startsWith("List of devices") || line.startsWith("*")) continue;
    const parts2 = line.split(/\s+/);
    if (parts2.length < 2) continue;
    const model = parts2.find((p) => p.startsWith("model:"))?.substring(6).replace(/_/g, " ") ?? "";
    out.push({ serial: parts2[0], state: parts2[1], model });
  }
  return out;
}
async function listDevices(adbPath) {
  const listed = parseDevices(await adb(adbPath, null, ["devices", "-l"]));
  const devices = [];
  for (const d of listed) {
    const dev = { serial: d.serial, state: d.state, model: d.model, sdk: 0, abi: "" };
    if (d.state === "device") {
      try {
        const props = (await shell(adbPath, d.serial, "getprop ro.build.version.sdk; getprop ro.product.cpu.abi; getprop ro.product.manufacturer; getprop ro.product.model")).split(/\r?\n/).map((s) => s.trim());
        dev.sdk = Number(props[0]) || 0;
        dev.abi = props[1] ?? "";
        if (!dev.model) dev.model = [props[2], props[3]].filter(Boolean).join(" ");
      } catch {
      }
    }
    devices.push(dev);
  }
  return devices;
}
async function listPackages(adbPath, serial) {
  const text = await shell(adbPath, serial, "pm list packages -3");
  return text.split(/\r?\n/).map((l) => l.trim()).filter((l) => l.startsWith("package:")).map((l) => l.substring(8)).sort();
}
async function resolveActivity(adbPath, serial, pkg) {
  try {
    const text = await shell(adbPath, serial, `cmd package resolve-activity --brief ${pkg}`);
    const lines = text.split(/\r?\n/).map((l) => l.trim()).filter(Boolean);
    const component = lines.reverse().find((l) => l.startsWith(`${pkg}/`));
    return component ?? null;
  } catch {
    return null;
  }
}
function findAndroidLayer(candidates) {
  for (const dir of candidates) {
    const libRoot = path4.join(dir, "lib");
    if (!fs5.existsSync(libRoot)) continue;
    const libs = {};
    for (const abi of fs5.readdirSync(libRoot)) {
      const lib = path4.join(libRoot, abi, LAYER_LIB);
      if (fs5.existsSync(lib)) libs[abi] = lib;
    }
    if (!Object.keys(libs).length) continue;
    const apk = path4.join(dir, LAYER_APK);
    let apkInfo = null;
    if (fs5.existsSync(apk)) {
      try {
        apkInfo = JSON.parse(fs5.readFileSync(`${apk}.json`, "utf8"));
      } catch {
        apkInfo = null;
      }
    }
    return { dir, libs, apk: apkInfo ? apk : null, apkInfo };
  }
  return null;
}
var AndroidTarget = class {
  constructor(opts) {
    this.opts = opts;
  }
  pid = null;
  _logcat = null;
  _poll = null;
  _polling = false;
  _stopped = false;
  /** Installs and enables the layer, starts the application and the watches. Rejects with a readable message. */
  async start() {
    const { adb: adbPath, serial, package: pkg, port } = this.opts;
    const log = this.opts.onLog;
    const props = (await shell(adbPath, serial, "getprop ro.build.version.sdk; getprop ro.product.cpu.abi; getprop ro.product.cpu.abilist; getprop ro.product.model")).split(/\r?\n/).map((s) => s.trim());
    const sdk = Number(props[0]) || 0;
    const abi = props[1] ?? "";
    const abilist = (props[2] ?? abi).split(",").map((s) => s.trim()).filter(Boolean);
    log(`device ${serial}: ${props[3] ?? ""}, Android API ${sdk}, ${abi}`);
    if (sdk < MIN_SDK) throw new Error(`Android 9 (API ${MIN_SDK}) or newer is required for Vulkan layers; the device runs API ${sdk}`);
    const how = await this._installLayer(sdk, abilist);
    log(`layer: ${how}`);
    await shell(adbPath, serial, "settings put global enable_gpu_debug_layers 1");
    await shell(adbPath, serial, `settings put global gpu_debug_app ${pkg}`);
    await shell(adbPath, serial, `settings put global gpu_debug_layers ${LAYER_NAME}`);
    await shell(adbPath, serial, `setprop debug.vkinsp.port ${port}`);
    await shell(adbPath, serial, `setprop debug.vkinsp.log ${this.opts.log ? 1 : 0}`);
    await shell(adbPath, serial, `setprop debug.vkinsp.record_always ${this.opts.recordAlways ? 1 : 0}`);
    await shell(adbPath, serial, `setprop debug.vkinsp.stacktraces ${this.opts.stacktraces ? 1 : 0}`);
    await shell(adbPath, serial, `am force-stop ${pkg}`);
    for (let i = 0; i < 20 && !this._stopped; ++i) {
      const pid = await this._findPid();
      if (pid === null) break;
      if (i === 0) log(`waiting for the previous instance (pid ${pid}) to exit`);
      await new Promise((r) => setTimeout(r, 250));
    }
    if (!this._stopped) {
      const unix = await shell(adbPath, serial, "cat /proc/net/unix").catch(() => "");
      if (unix.includes(`@${this.socketName}`)) log(`warning: @${this.socketName} is still held on the device by another process; the layer waits for it`);
    }
    await adb(adbPath, serial, ["forward", `tcp:${port}`, `localabstract:${this.socketName}`]);
    log(`forwarding localhost:${port} to the device's @${this.socketName}`);
    if (this._stopped) return;
    this._startLogcat();
    let activity = this.opts.activity.trim();
    if (!activity) activity = await resolveActivity(adbPath, serial, pkg) ?? "";
    if (activity && !activity.includes("/")) activity = `${pkg}/${activity}`;
    if (activity) {
      log(`starting ${activity}`);
      const out = await shell(adbPath, serial, `am start -S -n ${activity}`, START_TIMEOUT_MS);
      const error = out.split(/\r?\n/).find((l) => /^Error/.test(l.trim()));
      if (error) throw new Error(`${error.trim()} (activity ${activity})`);
    } else {
      log(`starting ${pkg} (no launchable activity resolved; using the launcher intent)`);
      const out = await shell(adbPath, serial, `monkey -p ${pkg} -c android.intent.category.LAUNCHER 1`, START_TIMEOUT_MS);
      if (/No activities found|monkey aborted/i.test(out)) throw new Error(`no launchable activity in ${pkg}`);
    }
    for (let i = 0; i < PID_RETRIES && this.pid === null && !this._stopped; ++i) {
      this.pid = await this._findPid();
      if (this.pid === null) await new Promise((r) => setTimeout(r, PID_RETRY_MS));
      if (this.pid === null && i === 4) await this._launchDiagnostics(true);
    }
    if (this._stopped) return;
    if (this.pid === null) throw new Error(`${pkg} did not start (no process found)`);
    await this._launchDiagnostics(false);
    this._poll = setInterval(() => void this._pollProcess(), POLL_MS);
  }
  /** The layer's abstract socket on the device: the port and the package (see transport.cpp). */
  get socketName() {
    return `vkinsp:${this.opts.port}:${this.opts.package}`;
  }
  /**
   * Re-establishes the port forward when it is gone. adb drops a device's forwards whenever the
   * device disconnects, and a headset's USB link blips when it changes power state, so a
   * connection attempt refused on the host side is checked against `adb forward --list`.
   * Resolves true when the forward had to be re-created.
   */
  async ensureForward() {
    if (this._stopped) return false;
    const { adb: adbPath, serial, port } = this.opts;
    const target = `localabstract:${this.socketName}`;
    const list = await adb(adbPath, serial, ["forward", "--list"]);
    const present = list.split(/\r?\n/).some((l) => {
      const f = l.trim().split(/\s+/);
      return f[0] === serial && f[1] === `tcp:${port}` && f[2] === target;
    });
    if (present || this._stopped) return false;
    await adb(adbPath, serial, ["forward", `tcp:${port}`, target]);
    this.opts.onLog(`the port forward was gone (device reconnected?): forwarding localhost:${port} to @${this.socketName} again`);
    return true;
  }
  /** Terminates the application, the port forward and the watches. */
  async stop() {
    this._stopWatching();
    const { adb: adbPath, serial, package: pkg, port } = this.opts;
    try {
      await shell(adbPath, serial, `am force-stop ${pkg}`);
    } catch {
    }
    try {
      await adb(adbPath, serial, ["forward", "--remove", `tcp:${port}`]);
    } catch {
    }
  }
  /** stop() for application exit, where nothing can be awaited. */
  stopSync() {
    this._stopWatching();
    const { adb: adbPath, serial, package: pkg, port } = this.opts;
    for (const args of [["shell", `am force-stop ${pkg}`], ["forward", "--remove", `tcp:${port}`]]) {
      try {
        execFileSync(adbPath, adbArgs(serial, args), { timeout: 3e3, stdio: "ignore", windowsHide: true });
      } catch {
      }
    }
  }
  _stopWatching() {
    this._stopped = true;
    if (this._poll) {
      clearInterval(this._poll);
      this._poll = null;
    }
    if (this._logcat) {
      try {
        this._logcat.kill();
      } catch {
      }
      this._logcat = null;
    }
  }
  /**
   * Gets the layer where the device's loader will find it. Android 10+ with the layer APK:
   * install it (when the installed version differs) and point gpu_debug_layer_app at it. Otherwise
   * copy the .so into the target's data directory with run-as, skipped when the copy there
   * already matches.
   */
  async _installLayer(sdk, abilist) {
    const { adb: adbPath, serial, package: pkg, layer } = this.opts;
    const apkAbi = layer.apkInfo ? abilist.find((a) => layer.apkInfo.abis.includes(a)) : void 0;
    if (sdk >= LAYER_APP_SDK && layer.apk && layer.apkInfo && apkAbi) {
      const info = layer.apkInfo;
      let installed = "";
      try {
        const dump = await shell(adbPath, serial, `dumpsys package ${info.package}`);
        installed = /versionName=(\S+)/.exec(dump)?.[1] ?? "";
      } catch {
        installed = "";
      }
      if (installed !== info.versionName) {
        this.opts.onLog(`installing the layer package ${info.package} (${installed ? `replacing ${installed}` : "not installed"})`);
        await adb(adbPath, serial, ["install", "-r", "-d", "--force-queryable", layer.apk], INSTALL_TIMEOUT_MS);
      }
      await shell(adbPath, serial, `settings put global gpu_debug_layer_app ${info.package}`);
      return `${info.package} ${info.versionName} (${apkAbi})`;
    }
    const abi = abilist.find((a) => layer.libs[a]);
    if (!abi) {
      throw new Error(`no Android layer built for ${abilist.join(", ")}: run tools/build_android.py --abi ${abilist[0] ?? "arm64-v8a"}`);
    }
    const lib = layer.libs[abi];
    const local = crypto.createHash("md5").update(fs5.readFileSync(lib)).digest("hex");
    let remote = "";
    try {
      remote = (await shell(adbPath, serial, `run-as ${pkg} md5sum ${LAYER_LIB}`)).trim().split(/\s+/)[0] ?? "";
    } catch (e) {
      const message = e instanceof Error ? e.message : String(e);
      if (/not debuggable|is not debuggable|Could not set capabilities|run-as: Package/.test(message)) {
        throw new Error(`${pkg} is not debuggable: Android only loads layers into debuggable applications (a Unity Development Build) or on rooted devices`);
      }
      remote = "";
    }
    if (remote !== local) {
      this.opts.onLog(`copying ${LAYER_LIB} (${abi}) into ${pkg}'s data directory`);
      await adb(adbPath, serial, ["push", lib, `${DEVICE_TMP}/${LAYER_LIB}`], INSTALL_TIMEOUT_MS);
      try {
        await shell(adbPath, serial, `run-as ${pkg} cp ${DEVICE_TMP}/${LAYER_LIB} . && run-as ${pkg} chmod 700 ${LAYER_LIB}`);
      } catch (e) {
        const message = e instanceof Error ? e.message : String(e);
        throw new Error(`could not copy the layer into ${pkg}: ${message}. The application must be debuggable (a Unity Development Build).`);
      }
    }
    await shell(adbPath, serial, "settings delete global gpu_debug_layer_app");
    return `${LAYER_LIB} (${abi}) in ${pkg}'s data directory`;
  }
  _startLogcat() {
    const { adb: adbPath, serial } = this.opts;
    const proc = spawn(adbPath, adbArgs(serial, ["logcat", "-v", "tag", "-T", "1", "vkinsp:*", "DEBUG:E", "AndroidRuntime:E", "*:S"]), { stdio: ["ignore", "pipe", "ignore"], windowsHide: true });
    this._logcat = proc;
    let rest = "";
    proc.stdout?.on("data", (d) => {
      rest += d.toString("utf8");
      const lines = rest.split(/\r?\n/);
      rest = lines.pop() ?? "";
      for (const line of lines) {
        if (!line.length || line.startsWith("--------- beginning of")) continue;
        this.opts.onLog(line.startsWith("I/vkinsp") ? `[vkinsp] ${line.replace(/^I\/vkinsp\s*:\s?/, "")}` : line);
      }
    });
    proc.on("exit", () => {
      if (this._logcat === proc) this._logcat = null;
    });
    proc.on("error", () => {
      if (this._logcat === proc) this._logcat = null;
    });
  }
  /**
   * What can keep a launch from running, in the Log: a device that is asleep (an OpenXR session
   * stays idle until the headset is worn), and on a headset the shell's "controllers required"
   * dialog, which a launch attempted without controllers or tracked hands leaves behind and
   * which then blocks every later launch until the shell restarts.
   */
  async _launchDiagnostics(noProcess) {
    const { adb: adbPath, serial } = this.opts;
    const log = this.opts.onLog;
    try {
      const power = await shell(adbPath, serial, "dumpsys power | grep -m1 mWakefulness=");
      const state = /mWakefulness=(\w+)/.exec(power)?.[1];
      if (state && state !== "Awake") log(`the device is ${state.toLowerCase()}: an OpenXR session stays idle (no frames) until the headset is worn or woken (adb shell input keyevent KEYCODE_WAKEUP)`);
    } catch {
    }
    try {
      const windows = await shell(adbPath, serial, "dumpsys window windows | grep -c -i launchcheck");
      if (Number(windows.trim()) > 0) {
        log(`the headset shell is showing its launch check dialog ("controllers required"), which blocks ${noProcess ? "this launch" : "launches"}: put the headset on with controllers or tracked hands, or restart the shell (adb shell am force-stop com.oculus.vrshell)`);
      }
    } catch {
    }
    if (!noProcess) return;
    try {
      const blocked = await shell(adbPath, serial, "logcat -d -t 300 | grep 'Launch is blocked because' | tail -1");
      const reason = /Launch is blocked because:\s*([^.]*?)\.?\s*(?:Caching|$)/.exec(blocked)?.[1]?.trim();
      if (reason) log(`the headset shell blocked the launch: ${reason}. Put the headset on and dismiss the dialog, or restart the shell (adb shell am force-stop com.oculus.vrshell)`);
    } catch {
    }
  }
  async _findPid() {
    try {
      const out = (await shell(this.opts.adb, this.opts.serial, `pidof ${this.opts.package}`)).trim();
      const pid = Number(out.split(/\s+/)[0]);
      return pid > 0 ? pid : null;
    } catch {
      return null;
    }
  }
  async _pollProcess() {
    if (this._polling || this._stopped) return;
    this._polling = true;
    try {
      const pid = await this._findPid();
      if (this._stopped) return;
      if (pid === null || this.pid !== null && pid !== this.pid) {
        this._stopWatching();
        this.opts.onExit();
      }
    } finally {
      this._polling = false;
    }
  }
};
async function disableLayer(adbPath, serial) {
  for (const key of ["enable_gpu_debug_layers", "gpu_debug_app", "gpu_debug_layers", "gpu_debug_layer_app"]) {
    try {
      await shell(adbPath, serial, `settings delete global ${key}`);
    } catch {
    }
  }
}

// src/main/layer_protocol.ts
function encodeRequest(msg) {
  const payload = Buffer.from(JSON.stringify(msg), "utf8");
  const header = Buffer.alloc(5);
  header.writeUInt32LE(payload.length, 0);
  header.writeUInt8(0, 4);
  return Buffer.concat([header, payload]);
}
var FrameReader = class {
  _buffered = Buffer.alloc(0);
  /** The messages `chunk` completes, in order; a frame that does not parse goes to `onError` and is skipped. */
  push(chunk2, onError) {
    this._buffered = this._buffered.length ? Buffer.concat([this._buffered, chunk2]) : chunk2;
    const out = [];
    while (this._buffered.length >= 5) {
      const len = this._buffered.readUInt32LE(0);
      const kind = this._buffered.readUInt8(4);
      if (this._buffered.length < 5 + len) break;
      const payload = this._buffered.subarray(5, 5 + len);
      this._buffered = this._buffered.subarray(5 + len);
      if (kind === 0) {
        try {
          out.push(JSON.parse(payload.toString("utf8")));
        } catch (e) {
          onError?.({ kind: "json", error: String(e), payload });
        }
      } else if (kind === 1) {
        const hl = payload.readUInt32LE(0);
        let header;
        try {
          header = JSON.parse(payload.subarray(4, 4 + hl).toString("utf8"));
        } catch (e) {
          onError?.({ kind: "header", error: String(e), payload });
          continue;
        }
        out.push({ ...header, __binary: new Uint8Array(payload.subarray(4 + hl)) });
      }
    }
    return out;
  }
};

// src/main/launch_env.ts
import { execFile as execFile3 } from "node:child_process";
import fs6 from "node:fs";
import net from "node:net";
import os4 from "node:os";
import path5 from "node:path";
var LAYER_NAME2 = "VK_LAYER_INSPECTOR_capture";
var VALIDATION_LAYER_NAME = "VK_LAYER_KHRONOS_validation";
var DEFAULT_PORT = 47531;
function findLayerDir(roots, packaged = []) {
  if (process.env.INSPECTOR_LAYER_DIR) return process.env.INSPECTOR_LAYER_DIR;
  const candidates = [];
  for (const root of roots) {
    const bin = path5.join(root, "build", "bin");
    candidates.push(path5.join(bin, "Release"), path5.join(bin, "RelWithDebInfo"), path5.join(bin, "Debug"), bin);
  }
  candidates.push(...packaged);
  for (const dir of candidates) {
    if (fs6.existsSync(path5.join(dir, `${LAYER_NAME2}.json`))) return dir;
  }
  return null;
}
function findValidationLayerDir() {
  const manifest = "VkLayer_khronos_validation.json";
  const candidates = [];
  const sdk = process.env.VULKAN_SDK;
  if (sdk) candidates.push(path5.join(sdk, "Bin"), path5.join(sdk, "share", "vulkan", "explicit_layer.d"), path5.join(sdk, "etc", "vulkan", "explicit_layer.d"));
  if (process.platform === "win32") {
    for (const root of ["C:\\VulkanSDK", path5.join(os4.homedir(), "VulkanSDK")]) {
      try {
        const versions = fs6.readdirSync(root).filter((v) => /^\d/.test(v)).sort().reverse();
        for (const v of versions) candidates.push(path5.join(root, v, "Bin"));
      } catch {
      }
    }
  } else {
    candidates.push(
      "/usr/share/vulkan/explicit_layer.d",
      "/usr/local/share/vulkan/explicit_layer.d",
      "/etc/vulkan/explicit_layer.d",
      path5.join(os4.homedir(), ".local", "share", "vulkan", "explicit_layer.d")
    );
  }
  for (const c2 of candidates) if (fs6.existsSync(path5.join(c2, manifest))) return c2;
  return null;
}
function vulkanLayerEnvironment(o) {
  const layers = [LAYER_NAME2, ...o.validationDir ? [VALIDATION_LAYER_NAME] : []];
  const layerPaths = [o.layerDir, ...o.validationDir ? [o.validationDir] : []];
  return {
    VK_ADD_LAYER_PATH: layerPaths.join(path5.delimiter),
    VK_LOADER_LAYERS_ENABLE: layers.join(","),
    // Older loaders:
    VK_LAYER_PATH: [...layerPaths, ...process.env.VK_LAYER_PATH ? [process.env.VK_LAYER_PATH] : []].join(path5.delimiter),
    VK_INSTANCE_LAYERS: [...layers, ...process.env.VK_INSTANCE_LAYERS ? [process.env.VK_INSTANCE_LAYERS] : []].join(path5.delimiter),
    VKINSP_PORT: String(o.port),
    VKINSP_LOG: o.log ? "1" : "0",
    ...o.logFile ? { VKINSP_LOG_FILE: o.logFile } : {},
    VKINSP_RECORD_ALWAYS: o.recordAlways ? "1" : "0",
    VKINSP_STACKTRACES: o.stacktraces ? "1" : "0",
    // The validation layer stops reporting a message after a few repeats (its
    // duplicate_message_limit, 10 by default); the inspector's layer counts repeats itself and
    // attaches a message to the captured command it fired on, which needs every occurrence.
    ...o.validation && !process.env.VK_LAYER_DUPLICATE_MESSAGE_LIMIT ? { VK_LAYER_DUPLICATE_MESSAGE_LIMIT: "0" } : {},
    // Synchronization validation: the settings-file name for current layers, the enable list for older ones.
    ...o.validation && o.syncValidation ? { VK_LAYER_VALIDATE_SYNC: "true", VK_LAYER_ENABLES: "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT" } : {}
  };
}
function splitArgs(s) {
  const out = [];
  const re = /"([^"]*)"|'([^']*)'|(\S+)/g;
  let m;
  while (m = re.exec(s)) out.push(m[1] ?? m[2] ?? m[3]);
  return out;
}
function portFree(port) {
  return new Promise((resolve) => {
    const srv = net.createServer();
    srv.once("error", () => resolve(false));
    srv.listen({ port, host: "127.0.0.1", exclusive: true }, () => srv.close(() => resolve(true)));
  });
}
async function findFreePort(start, taken = () => false) {
  for (let port = start; port < start + 100 && port < 65536; port++) {
    if (taken(port)) continue;
    if (await portFree(port)) return port;
  }
  return start;
}
function terminate(proc) {
  if (process.platform === "win32" && proc.pid) {
    execFile3("taskkill", ["/PID", String(proc.pid), "/T", "/F"], () => {
      try {
        proc.kill();
      } catch {
      }
    });
    return;
  }
  proc.kill();
}

// src/main/metal.ts
import { execFileSync as execFileSync2, spawnSync } from "node:child_process";
import fs7 from "node:fs";
import path6 from "node:path";
import { fileURLToPath } from "node:url";
var moduleDir = path6.dirname(fileURLToPath(import.meta.url));
var CAPTURE_LIBRARY = "libmtlinsp_capture.dylib";
function findCaptureLibrary(roots = [path6.resolve(moduleDir, "..", "..", "..")], packaged = [path6.join(process.resourcesPath ?? "", "layer")]) {
  const candidates = [];
  if (process.env.INSPECTOR_METAL_LIB) candidates.push(process.env.INSPECTOR_METAL_LIB);
  for (const root of roots) {
    for (const dir of ["build/bin", "build/bin/Release", "build/bin/Debug"]) {
      candidates.push(path6.join(root, dir, CAPTURE_LIBRARY));
    }
  }
  for (const dir of packaged) candidates.push(path6.join(dir, CAPTURE_LIBRARY));
  return candidates.find((p) => fs7.existsSync(p)) ?? null;
}
function resolveExecutable(exe) {
  if (!exe.endsWith(".app")) return exe;
  const macOS = path6.join(exe, "Contents", "MacOS");
  const plist = path6.join(exe, "Contents", "Info.plist");
  if (fs7.existsSync(plist)) {
    try {
      const name = execFileSync2(
        "/usr/libexec/PlistBuddy",
        ["-c", "Print :CFBundleExecutable", plist],
        { encoding: "utf8" }
      ).trim();
      const candidate = path6.join(macOS, name);
      if (name && fs7.existsSync(candidate)) return candidate;
    } catch {
    }
  }
  const byBundleName = path6.join(macOS, path6.basename(exe, ".app"));
  if (fs7.existsSync(byBundleName)) return byBundleName;
  try {
    const entries = fs7.readdirSync(macOS);
    if (entries.length === 1) return path6.join(macOS, entries[0]);
  } catch {
  }
  return exe;
}
function injectionBlockedReason(exe) {
  const r = spawnSync(
    "codesign",
    ["-d", "-v", "--entitlements", "-", "--xml", exe],
    { encoding: "utf8" }
  );
  const output = `${r.stderr ?? ""}${r.stdout ?? ""}`;
  if (!/flags=[^\s]*runtime/.test(output)) return null;
  const hasDyld = output.includes("com.apple.security.cs.allow-dyld-environment-variables");
  const hasLibrary = output.includes("com.apple.security.cs.disable-library-validation");
  if (hasDyld && hasLibrary) return null;
  return `${path6.basename(exe)} is signed with the hardened runtime, so macOS drops DYLD_INSERT_LIBRARIES and the capture library can never load. Re-sign it for injection:

  /usr/bin/codesign --force --deep --sign - --options runtime \\
    --entitlements <(echo '<?xml version="1.0" encoding="UTF-8"?><!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd"><plist version="1.0"><dict><key>com.apple.security.cs.allow-dyld-environment-variables</key><true/><key>com.apple.security.cs.disable-library-validation</key><true/></dict></plist>') \\
    "<the .app>"

This invalidates the application's signature and notarization, so do it to a development build rather than to a shipping copy.`;
}
function captureEnvironment(library, port, log, validation = false, stacktraces = false) {
  const env = {
    // A stack at every object creation (metal/src/stacktrace.mm), the launch dialog's option.
    MTLINSP_STACKTRACES: stacktraces ? "1" : "0",
    // Appended rather than replacing: another inserted library is the caller's business.
    DYLD_INSERT_LIBRARIES: [library, ...process.env.DYLD_INSERT_LIBRARIES ? [process.env.DYLD_INSERT_LIBRARIES] : []].join(":"),
    MTLINSP_PORT: String(port),
    MTLINSP_LOG: log ? "1" : "0",
    // Lets the library write an Xcode GPU trace of a frame on request (metal/src/gpu_trace.mm);
    // without it MTLCaptureManager refuses the document destination.
    ...process.env.METAL_CAPTURE_ENABLED ? {} : { METAL_CAPTURE_ENABLED: "1" }
  };
  if (validation) {
    const defaults = {
      MTL_DEBUG_LAYER: "1",
      MTL_DEBUG_LAYER_ERROR_MODE: "nslog",
      MTL_DEBUG_LAYER_WARNING_MODE: "nslog",
      MTL_SHADER_VALIDATION: "1",
      MTL_SHADER_VALIDATION_REPORT_TO_STDERR: "1"
    };
    for (const [key, value] of Object.entries(defaults)) {
      if (!process.env[key]) env[key] = value;
    }
  }
  return env;
}

// src/renderer/stack_requests.ts
var REQUEST_TIMEOUT_MS = 15e3;
function requestStacks(session, ids) {
  const db = session.database;
  const out = /* @__PURE__ */ new Map();
  const missing = [];
  for (const id of ids) {
    const cached = db.stacks.get(id);
    if (cached) out.set(id, cached);
    else missing.push(id);
  }
  if (!missing.length || db.stacksAvailable === false) return Promise.resolve(out);
  return new Promise((resolve) => {
    let done = false;
    const finish2 = (ok) => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      db.onStacktraces.disconnect(listener);
      if (!ok) {
        resolve(out.size ? out : null);
        return;
      }
      for (const id of missing) {
        const s = db.stacks.get(id);
        if (s) out.set(id, s);
      }
      resolve(out);
    };
    const listener = () => finish2(true);
    const timer = setTimeout(() => finish2(false), REQUEST_TIMEOUT_MS);
    db.onStacktraces.addListener(listener);
    void session.send({ action: "RequestStacktraces", ids: missing }).then((ok) => {
      if (!ok) finish2(false);
    });
  });
}
async function resolveSymbols(session, addresses, symbolizeOnHost2) {
  const out = await resolveFromLayer(session, addresses);
  if (symbolizeOnHost2) await symbolizeOnHost2(out);
  return out;
}
function resolveFromLayer(session, addresses) {
  const db = session.database;
  const out = /* @__PURE__ */ new Map();
  const missing = [];
  for (const a of addresses) {
    const cached = db.symbols.get(a);
    if (cached) out.set(a, cached);
    else if (!missing.includes(a)) missing.push(a);
  }
  if (!missing.length) return Promise.resolve(out);
  return new Promise((resolve) => {
    let done = false;
    const finish2 = () => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      db.onSymbols.disconnect(listener);
      for (const a of missing) {
        const f = db.symbols.get(a);
        if (f) out.set(a, f);
      }
      resolve(out);
    };
    const listener = () => finish2();
    const timer = setTimeout(finish2, REQUEST_TIMEOUT_MS);
    db.onSymbols.addListener(listener);
    void session.send({ action: "RequestSymbols", addresses: missing }).then((ok) => {
      if (!ok) finish2();
    });
  });
}

// src/renderer/capture_file.ts
var BLOB_TIMEOUT_MS = 15e3;
function fetchBlob(session, object, index) {
  const db = session.database;
  const key = `${object.id}:${index}`;
  const cached = db.blobData.get(key);
  if (cached) return Promise.resolve(cached);
  if (!session.connected) return Promise.resolve(null);
  return new Promise((resolve) => {
    let done = false;
    const finish2 = (data) => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      db.onObjectBlob.disconnect(listener);
      resolve(data);
    };
    const listener = (id, idx, data) => {
      if (id === object.id && idx === index) finish2(data);
    };
    const timer = setTimeout(() => finish2(null), BLOB_TIMEOUT_MS);
    db.onObjectBlob.addListener(listener);
    void session.send({ action: "RequestBlob", id: object.id, index }).then((ok) => {
      if (!ok) finish2(null);
    });
  });
}
function referencedObjects(session, data) {
  const db = session.database;
  const ids = /* @__PURE__ */ new Set();
  for (const c2 of data.commands) {
    if (c2.object) ids.add(c2.object.__id);
    if (c2.secondary) ids.add(c2.secondary);
    db.collectReferences(c2.args, ids);
    db.collectReferences(c2.descriptors, ids);
  }
  for (const t of data.textures) {
    ids.add(t.info.id);
    ids.add(t.info.commandBuffer);
  }
  for (const b of data.buffers.values()) {
    ids.add(b.info.buffer);
    ids.add(b.info.commandBuffer);
  }
  for (const v of db.validation) db.collectReferences(v.objects, ids);
  const out = /* @__PURE__ */ new Map();
  const queue = [...ids];
  while (queue.length) {
    const id = queue.pop();
    if (out.has(id)) continue;
    const o = db.getObject(id);
    if (!o) continue;
    out.set(id, o);
    if (o.parentId && !out.has(o.parentId)) queue.push(o.parentId);
    for (const dep of o.dependencies) if (!out.has(dep.id)) queue.push(dep.id);
    const more = /* @__PURE__ */ new Set();
    db.collectReferences(o.updates, more);
    for (const m of more) if (!out.has(m)) queue.push(m);
  }
  return [...out.values()].sort((a, b) => a.id - b.id);
}
async function serializeCapture(session, data, options = {}) {
  const onProgress = options.onProgress;
  const payloads = [];
  let payloadBytes = 0;
  const addPayload = (bytes) => {
    if (!bytes) return void 0;
    const p = [payloadBytes, bytes.byteLength];
    payloads.push(bytes);
    payloadBytes += bytes.byteLength;
    return p;
  };
  const objects = referencedObjects(session, data);
  const records = [];
  let fetched = 0;
  const withBlobs = objects.filter((o) => o.blobs.length).length;
  for (const o of objects) {
    const blobs = [];
    for (let i = 0; i < o.blobs.length; i++) {
      const b = o.blobs[i];
      if (onProgress) onProgress(`saving: shader ${++fetched} of ${withBlobs}...`);
      const bytes = await fetchBlob(session, o, i);
      blobs.push({ name: b.name, size: b.size, ...bytes ? { payload: addPayload(bytes) } : {} });
    }
    records.push({
      id: o.id,
      parent: o.parentId,
      type: o.type,
      cmd: o.cmd,
      index: o.index,
      handle: o.handle,
      label: o.label || null,
      args: o.args,
      blobs,
      updates: o.updates,
      deleted: o.isDeleted
    });
  }
  const db = session.database;
  const addresses = /* @__PURE__ */ new Set();
  for (const c2 of data.commands) for (const a of c2.stack ?? []) addresses.add(a);
  let symbols;
  if (addresses.size && !options.forReplay) {
    if (onProgress) onProgress("saving: symbols...");
    const resolved = await (options.resolveSymbols ?? ((a) => resolveSymbols(session, a)))([...addresses]);
    symbols = {};
    for (const [a, f] of resolved) symbols[a] = f;
  }
  let stacks;
  if (!options.forReplay && db.stacksAvailable !== false && (session.connected || db.stacks.size)) {
    if (onProgress) onProgress("saving: stack traces...");
    const got = await requestStacks(session, objects.map((o) => o.id));
    if (got && db.stacksAvailable !== false) {
      stacks = {};
      for (const [id, frames] of got) if (frames.length) stacks[id] = frames;
    }
  }
  if (onProgress) onProgress("saving: writing...");
  const manifest = {
    format: CAPTURE_FORMAT,
    version: CAPTURE_VERSION,
    api: data.api,
    application: "GPU Inspector",
    savedAt: (/* @__PURE__ */ new Date()).toISOString(),
    source: { name: session.name },
    frame: data.frame,
    frames: data.frames,
    frameTimeMs: db.frameTimeMs,
    submitMs: db.submitMs,
    refreshMs: db.refreshMs,
    refreshSource: db.refreshSource,
    displayRefreshMs: db.displayRefreshMs,
    frameBoundary: db.frameBoundary,
    objects: records,
    // Secondary command buffers are already inlined into the list; their nested copies are dropped.
    commands: data.commands.map((c2) => {
      const { children: _children, ...rest } = c2;
      return rest;
    }),
    textures: data.textures.map((t) => ({ info: t.info, ...t.data ? { payload: addPayload(t.data) } : {} })),
    buffers: [...data.buffers.values()].map((b) => ({ info: b.info, ...b.data ? { payload: addPayload(b.data) } : {} })),
    passTimings: [...data.passTimings.values()],
    ...data.overdraw.length ? { overdraw: data.overdraw.map((o) => ({ info: o.info, ...o.data ? { payload: addPayload(o.data) } : {} })) } : {},
    ...data.pixelHistory ? { pixelHistory: data.pixelHistory } : {},
    ...data.drawStats?.length ? { drawStats: data.drawStats } : {},
    validation: db.validation,
    ...symbols ? { symbols } : {},
    ...stacks ? { stacks } : {}
  };
  return encodeCaptureFile(manifest, payloads);
}

// src/mcp/live_session.ts
var MAX_LOG_LINES = 2e3;
var MAX_FRAME_STATS = 600;
var DEFAULT_QUIET_MS = 2e3;
var SNAPSHOT_TIMEOUT_MS = 1e4;
var KILL_TIMEOUT_MS = 3e3;
var CAPTURE_ACTIONS = /* @__PURE__ */ new Set([
  "CaptureFrameResults",
  "CaptureFrameCommands",
  "CaptureTextureFrames",
  "CaptureTextureData",
  "CaptureBuffers",
  "CaptureBufferData",
  "CapturePassTimings",
  "CaptureOverdraw",
  "CaptureOverdrawData",
  "CapturePixelHistory"
]);
var sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
function capturesDir() {
  return process.env.GPU_INSPECTOR_CAPTURES_DIR ?? path7.join(os5.tmpdir(), "gpu-inspector-captures");
}
function checkoutRoots() {
  const roots = [];
  if (process.env.GPU_INSPECTOR_ROOT) roots.push(process.env.GPU_INSPECTOR_ROOT);
  roots.push(path7.resolve(path7.dirname(fileURLToPath2(import.meta.url)), "..", ".."));
  return roots;
}
function installedLayerDirs() {
  const home = os5.homedir();
  if (process.platform === "win32") {
    const local = process.env.LOCALAPPDATA ?? path7.join(home, "AppData", "Local");
    const apps = [path7.join(local, "Programs", "gpu-inspector"), path7.join(local, "Programs", "GPU Inspector")];
    for (const programFiles of [process.env.ProgramFiles, process.env["ProgramFiles(x86)"]]) {
      if (programFiles) apps.push(path7.join(programFiles, "GPU Inspector"));
    }
    return apps.map((dir) => path7.join(dir, "resources", "layer"));
  }
  if (process.platform === "darwin") {
    return ["/Applications", path7.join(home, "Applications")].map((dir) => path7.join(dir, "GPU Inspector.app", "Contents", "Resources", "layer"));
  }
  return ["/opt/GPU Inspector/resources/layer", "/opt/gpu-inspector/resources/layer"];
}
function androidLayer() {
  const candidates = [
    process.env.INSPECTOR_ANDROID_LAYER_DIR,
    ...checkoutRoots().map((root) => path7.join(root, "build", "android")),
    ...installedLayerDirs().map((dir) => path7.join(dir, "android"))
  ].filter((d) => !!d);
  return findAndroidLayer(candidates);
}
function applicationName(db) {
  for (const o of db.objectsByType.get("VkInstance")?.values() ?? []) {
    const info = o.descriptor?.pApplicationInfo;
    const name = isObject(info) ? str(info.pApplicationName) : "";
    if (name) return name;
  }
  return null;
}
var LiveSession = class {
  constructor(id, name, port, launched) {
    this.id = id;
    this.name = name;
    this.port = port;
    this.launched = launched;
    const db = this.database;
    db.onFrameStats.addListener((msg) => {
      this.frameStats.push({ at: Date.now(), msg });
      if (this.frameStats.length > MAX_FRAME_STATS) this.frameStats.splice(0, this.frameStats.length - MAX_FRAME_STATS);
    });
    db.onValidationMessage.addListener((entry, isNew) => {
      if (isNew) this.appendLog(`validation ${entry.severity}${entry.idName ? ` ${entry.idName}` : ""}: ${entry.message.split("\n")[0].slice(0, 300)}`);
    });
    db.onLeakReport.addListener((r) => this.appendLog(`leak report: ${r.ownerClass} ${r.owner} destroyed with ${r.count} live objects`));
    db.onOtherMessage.addListener((msg) => {
      if (msg.action === "ShaderReplaced") {
        this.appendLog(`shader edit: pipeline ${msg.pipeline} ${msg.stage}: ${msg.ok ? msg.replacement ? `applied as object ${msg.replacement}` : "restored" : `failed: ${msg.error ?? "unknown error"}`}`);
      }
    });
  }
  database = new ObjectDatabase();
  log = [];
  /** Frame reports with the time each arrived. */
  frameStats = [];
  startedAt = Date.now();
  state = "connecting";
  detail = "";
  pid = null;
  exitCode = null;
  _proc = null;
  _socket = null;
  _listeners = /* @__PURE__ */ new Set();
  _capturing = false;
  /** Set while stop() terminates the application, so its exit reads as that rather than as a crash. */
  _stopping = false;
  /** The launched application is gone: it exited, failed to start, or was stopped. */
  _ended = false;
  /**
   * A launched target that is not a child process of this server (an Android application): how
   * to stop it, and how to repair the way to it when connections are refused (a lost adb forward).
   */
  remote = null;
  get connected() {
    return this._socket !== null && !this._socket.destroyed;
  }
  /** The API the capture library reports objects of; null before any arrived. */
  get api() {
    for (const type of this.database.objectsByType.keys()) if (type.startsWith("MTL")) return "metal";
    return this.database.allObjects.size ? "vulkan" : null;
  }
  appendLog(line) {
    this.log.push(line);
    if (this.log.length > MAX_LOG_LINES) this.log.splice(0, this.log.length - MAX_LOG_LINES);
  }
  setState(state, detail = "") {
    this.state = state;
    this.detail = detail;
    this.appendLog(`[${state}]${detail ? ` ${detail}` : ""}`);
  }
  send(msg) {
    if (!this._socket || this._socket.destroyed) return Promise.resolve(false);
    this._socket.write(encodeRequest(msg));
    return Promise.resolve(true);
  }
  /** Hears every message from the capture library, after the object database; returns the unsubscribe. */
  onMessage(listener) {
    this._listeners.add(listener);
    return () => {
      this._listeners.delete(listener);
    };
  }
  /** The first message `match` accepts (its return value), or null after `timeoutMs`. */
  waitFor(match, timeoutMs) {
    return new Promise((resolve) => {
      const off = this.onMessage((msg) => {
        const hit = match(msg);
        if (hit === void 0) return;
        clearTimeout(timer);
        off();
        resolve(hit);
      });
      const timer = setTimeout(() => {
        off();
        resolve(null);
      }, timeoutMs);
    });
  }
  /** Starts the application; its output goes to the session's log. */
  startProcess(exe, args, cwd, env) {
    const proc = spawn2(exe, args, { cwd, env, stdio: ["ignore", "pipe", "pipe"] });
    this._proc = proc;
    this.pid = proc.pid ?? null;
    for (const stream of [proc.stdout, proc.stderr]) {
      let rest = "";
      stream?.on("data", (d) => {
        rest += d.toString("utf8");
        const lines = rest.split(/\r?\n/);
        rest = lines.pop() ?? "";
        for (const line of lines) if (line.length) this.appendLog(line);
      });
    }
    proc.on("exit", (code, signal) => {
      if (this._proc !== proc) return;
      this._proc = null;
      this.pid = null;
      this._ended = true;
      this.exitCode = String(signal ?? code);
      this._disconnect();
      this.setState("exited", this._stopping ? "terminated by stop_app" : `code ${this.exitCode}`);
    });
    proc.on("error", (e) => {
      if (this._proc !== proc) return;
      this._proc = null;
      this.pid = null;
      this._disconnect();
      this._ended = true;
      this.setState("error", e.message);
    });
  }
  /**
   * Connects to the capture library, retrying until it answers, the launched process exits, or
   * `timeoutMs` passes; resolves once the snapshot of live objects that follows a connection is in.
   */
  async connect(timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    this.setState("connecting", `port ${this.port}`);
    let attempts = 0;
    while (Date.now() < deadline) {
      if (this._ended) return false;
      const sock = await this._tryConnect();
      attempts++;
      if (sock) {
        const snapshot = this._waitForSnapshot(SNAPSHOT_TIMEOUT_MS, sock);
        this._attach(sock);
        if (await snapshot !== "closed" || this.connected) {
          const name = applicationName(this.database);
          if (name && !this.launched) this.name = name;
          return this.connected;
        }
      } else if (this.remote?.repair && attempts % 4 === 0) {
        await this.remote.repair().catch(() => void 0);
      }
      await sleep(this.launched && !this.remote ? 250 : 500);
    }
    this.setState("disconnected", `nothing answered on port ${this.port}`);
    return false;
  }
  _tryConnect() {
    return new Promise((resolve) => {
      const sock = net2.createConnection({ host: "127.0.0.1", port: this.port });
      sock.once("connect", () => {
        sock.removeAllListeners("error");
        resolve(sock);
      });
      sock.once("error", () => {
        sock.destroy();
        resolve(null);
      });
    });
  }
  _attach(sock) {
    sock.setNoDelay(true);
    this._socket = sock;
    const reader = new FrameReader();
    let heard = false;
    sock.on("data", (chunk2) => {
      if (!heard) {
        heard = true;
        this.setState("connected", `port ${this.port}`);
      }
      const messages = reader.push(chunk2, (e) => this.appendLog(`bad ${e.kind === "json" ? "JSON" : "binary header"} from the capture library: ${e.error}`));
      for (const msg of messages) {
        this.database.handleMessage(msg);
        for (const listener of [...this._listeners]) listener(msg);
      }
    });
    const gone = () => {
      if (this._socket !== sock) return;
      this._socket = null;
      if (this.state === "connected") this.setState("disconnected", "the connection closed (the application exited, or another client connected to it)");
    };
    sock.on("error", gone);
    sock.on("close", gone);
    void this.send({ action: "Ping" });
  }
  /**
   * Resolves when the snapshot the capture library sends on connection has arrived, when the
   * socket closes first ("closed" if no snapshot had begun), or after `timeoutMs`.
   */
  _waitForSnapshot(timeoutMs, sock) {
    const db = this.database;
    return new Promise((resolve) => {
      let started = false;
      const done = (outcome) => {
        clearTimeout(timer);
        db.onSnapshotBegin.disconnect(begin);
        db.onAddObject.disconnect(add);
        sock.off("close", closed);
        resolve(outcome);
      };
      const begin = (count2) => {
        started = true;
        if (count2 === 0) done("snapshot");
      };
      const add = (_object, inSnapshot) => {
        if (started && !inSnapshot) done("snapshot");
      };
      const closed = () => done(started ? "snapshot" : "closed");
      const timer = setTimeout(() => done("timeout"), timeoutMs);
      db.onSnapshotBegin.addListener(begin);
      db.onAddObject.addListener(add);
      sock.once("close", closed);
    });
  }
  _disconnect() {
    const sock = this._socket;
    this._socket = null;
    sock?.destroy();
  }
  /**
   * Requests a capture and waits for all of it: until the capture library marks its end, or, for
   * one built before that marker existed, until the stream has been silent for a while after the
   * commands and buffers are in.
   */
  async capture(o) {
    if (!this.connected) throw new Error(`${this.id} is not connected (${this.state}${this.detail ? `: ${this.detail}` : ""}).`);
    if (this._capturing) throw new Error(`${this.id} is already capturing.`);
    this._capturing = true;
    const data = new CaptureData();
    const started = Date.now();
    let commandsComplete = false;
    let marker = false;
    let lastTraffic = 0;
    const onCommands = () => {
      commandsComplete = true;
    };
    data.onCommandsComplete.addListener(onCommands);
    const off = this.onMessage((msg) => {
      if (msg.action === "CaptureComplete") {
        marker = true;
      } else if (CAPTURE_ACTIONS.has(msg.action)) {
        lastTraffic = Date.now();
        data.handleMessage(msg);
      }
    });
    const quietMs = Number(process.env.GPU_INSPECTOR_CAPTURE_QUIET_MS) || DEFAULT_QUIET_MS;
    try {
      const request = {
        action: "Capture",
        frameCount: o.frames,
        ...o.atFrame !== void 0 ? { atFrame: o.atFrame } : {},
        captureTextures: o.renderTargets,
        captureBuffers: o.buffers,
        captureImages: o.images,
        profilePasses: o.profilePasses,
        stacktraces: o.stacktraces,
        maxBufferSize: o.maxBufferBytes,
        ...o.overdraw ? { overdraw: true } : {},
        ...o.pixelHistory ? { pixelHistory: o.pixelHistory } : {}
      };
      await this.send(request);
      for (; ; ) {
        await sleep(50);
        const now = Date.now();
        if (marker) return { data, completion: "marker", elapsedMs: now - started };
        if (commandsComplete && lastTraffic && now - lastTraffic >= quietMs && !data.buffersLoading) return { data, completion: "quiet", elapsedMs: now - started };
        if (!this.connected) {
          throw new Error(data.commands.length ? "The connection was lost while the capture was streaming." : "The connection was lost before the capture arrived.");
        }
        if (now - started > o.timeoutMs) {
          throw new Error(lastTraffic ? `The capture did not finish streaming within ${o.timeoutMs / 1e3} s.` : `No capture arrived within ${o.timeoutMs / 1e3} s: a capture starts at ${o.atFrame !== void 0 ? `frame ${o.atFrame}` : "the next frame"}, so the application may not be rendering (minimized, paused, or waiting).`);
        }
      }
    } finally {
      off();
      data.onCommandsComplete.disconnect(onCommands);
      this._capturing = false;
    }
  }
  /** Saves a capture with the objects it references, fetching their shaders from the capture library; returns the file. */
  async saveCapture(data, file) {
    const bytes = await serializeCapture(this, data, {
      resolveSymbols: (addresses) => resolveSymbols(this, addresses, (frames) => symbolizeSymbolMap(this.database, frames))
    });
    let target;
    if (file) {
      target = path7.resolve(file);
      fs8.mkdirSync(path7.dirname(target), { recursive: true });
    } else {
      const dir = capturesDir();
      fs8.mkdirSync(dir, { recursive: true });
      const name = captureFileName(this.name, data.frame, data.frames);
      target = path7.join(dir, name);
      for (let n = 2; fs8.existsSync(target); n++) target = path7.join(dir, name.replace(/\.gpucap$/, `_${n}.gpucap`));
    }
    fs8.writeFileSync(target, bytes);
    return target;
  }
  /**
   * Rebuilds a pipeline with one stage's code replaced (Vulkan); the layer's answer, or null without
   * one. The request names the stage by its flag, the answer by the layer's stage name ("fragment").
   */
  async replaceShader(pipeline, stageFlag, stageName, spirv, timeoutMs = 15e3) {
    const answer = this.waitFor((msg) => msg.action === "ShaderReplaced" && msg.pipeline === pipeline && (msg.stage === stageName || msg.stage === stageFlag) ? msg : void 0, timeoutMs);
    await this.send({ action: "ReplaceShader", pipeline, stage: stageFlag, spirv: Buffer.from(spirv).toString("base64") });
    return answer;
  }
  /** Drops the replacement of one stage (or every stage) of a pipeline. */
  async restoreShader(pipeline, stageFlag, timeoutMs = 15e3) {
    const answer = this.waitFor((msg) => msg.action === "ShaderReplaced" && msg.pipeline === pipeline ? msg : void 0, timeoutMs);
    await this.send({ action: "RestoreShader", pipeline, ...stageFlag ? { stage: stageFlag } : {} });
    return answer;
  }
  /** One subresource of a live image, which the capture library reads back at the application's next frame; null without an answer. */
  async readImage(id, mip, layer, timeoutMs) {
    const answer = this.waitFor((msg) => msg.action === "ImageData" && msg.id === id ? msg : void 0, timeoutMs);
    await this.send({ action: "RequestImage", id, mip, layer });
    return answer;
  }
  /** Reads a descriptor set's current contents into its object's updates (`bindings`); false without an answer. */
  async readDescriptorSet(id, timeoutMs = 1e4) {
    const answer = this.waitFor((msg) => msg.action === "ObjectUpdate" && msg.id === id && "bindings" in msg ? true : void 0, timeoutMs);
    await this.send({ action: "RequestDescriptorSet", id });
    return await answer ?? false;
  }
  /** A remote target ended: it exited on its own, failed to start, or was stopped. */
  remoteEnded(state, detail) {
    if (this._ended) return;
    this._ended = true;
    this.pid = null;
    this._disconnect();
    this.setState(state, detail);
  }
  /** Terminates a launched application; an attached one is only disconnected. */
  async stop() {
    const proc = this._proc;
    const remote = this.remote;
    this._disconnect();
    if (remote) {
      this.remote = null;
      if (!this._ended) {
        this._stopping = true;
        await remote.stop().catch(() => void 0);
        this.remoteEnded("exited", "terminated by stop_app");
      }
      return;
    }
    if (!proc) {
      if (this.state === "connected" || this.state === "connecting") this.setState("disconnected", "detached");
      return;
    }
    this._stopping = true;
    await new Promise((resolve) => {
      const timer = setTimeout(resolve, KILL_TIMEOUT_MS);
      proc.once("exit", () => {
        clearTimeout(timer);
        resolve();
      });
      try {
        terminate(proc);
      } catch {
        clearTimeout(timer);
        resolve();
      }
    });
  }
};
var SessionManager = class {
  _sessions = /* @__PURE__ */ new Map();
  _counter = 0;
  _latest = null;
  /** Launches an application with the capture library in it and waits for it to connect. */
  async launch(o, waitMs) {
    const requested = path7.resolve(o.exe);
    if (!fs8.existsSync(requested)) throw new Error(`No executable at ${requested}.`);
    const args = Array.isArray(o.args) ? o.args : splitArgs(o.args ?? "");
    const taken = new Set([...this._sessions.values()].filter((s) => s.connected || s.pid !== null).map((s) => s.port));
    const port = await findFreePort(o.port ?? DEFAULT_PORT, (p) => taken.has(p));
    let exe = requested;
    let env;
    let note;
    if (process.platform === "darwin") {
      const library = findCaptureLibrary(checkoutRoots(), installedLayerDirs());
      if (!library) throw new Error("The Metal capture library (libmtlinsp_capture.dylib) was not found: build it in the GPU Inspector checkout, install GPU Inspector, or set INSPECTOR_METAL_LIB.");
      exe = resolveExecutable(requested);
      const blocked = injectionBlockedReason(exe);
      if (blocked) throw new Error(blocked);
      env = { ...process.env, ...o.env, ...captureEnvironment(library, port, true, !!o.validation, o.stacktraces ?? true) };
      note = `capture library: ${library}`;
    } else {
      const layerDir = o.layerDir ?? findLayerDir(checkoutRoots(), installedLayerDirs());
      if (!layerDir) {
        throw new Error("The GPU Inspector Vulkan layer was not found: build it (see GPU Inspector's README), install GPU Inspector, or pass layerDir (or set INSPECTOR_LAYER_DIR) to the directory holding VK_LAYER_INSPECTOR_capture.json.");
      }
      const validationDir = o.validation ? findValidationLayerDir() : null;
      env = {
        ...process.env,
        ...o.env,
        ...vulkanLayerEnvironment({
          layerDir,
          validationDir,
          port,
          log: true,
          recordAlways: !!o.recordAlways,
          stacktraces: o.stacktraces ?? true,
          validation: !!o.validation,
          syncValidation: !!o.syncValidation
        })
      };
      note = `layer: ${layerDir}${o.validation ? validationDir ? `; validation layer: ${validationDir}` : "; validation layer not found (install the Vulkan SDK or set VULKAN_SDK)" : ""}`;
    }
    const session = new LiveSession(`app-${++this._counter}`, `${path7.basename(requested)}${args.length ? ` ${args.join(" ")}` : ""}`, port, true);
    session.appendLog(`launching ${exe} ${args.join(" ")}`);
    session.appendLog(note);
    this._sessions.set(session.id, session);
    this._latest = session;
    session.startProcess(exe, args, o.cwd && fs8.existsSync(o.cwd) ? o.cwd : path7.dirname(exe), env);
    if (await session.connect(waitMs) && o.recordAlways) await session.send({ action: "Settings", recordAlways: true });
    return session;
  }
  /** The Android devices adb sees, and where the Android layer is; adb null when it was not found. */
  async androidDevices() {
    const adb2 = findAdb();
    return { adb: adb2, devices: adb2 ? await listDevices(adb2) : [], layer: androidLayer()?.dir ?? null };
  }
  /**
   * Starts an Android package on a device with the layer installed and enabled for it, and waits
   * for the layer to connect over the adb forward. A launch that fails on the device is reported
   * through the session's state and log rather than thrown.
   */
  async launchAndroid(o, waitMs) {
    const adb2 = findAdb();
    if (!adb2) throw new Error("adb was not found: install the Android SDK platform-tools, or set ANDROID_HOME or INSPECTOR_ADB.");
    const layer = androidLayer();
    if (!layer) {
      throw new Error("The Android layer was not found: build it with tools/build_android.py in the GPU Inspector checkout (it needs the Android NDK), install GPU Inspector, or set INSPECTOR_ANDROID_LAYER_DIR.");
    }
    const devices = await listDevices(adb2);
    const listed = devices.length ? devices.map((d) => `${d.serial} (${d.state}${d.model ? `, ${d.model}` : ""})`).join(", ") : "none";
    let device;
    if (o.device) {
      device = devices.find((d) => d.serial === o.device);
      if (!device) throw new Error(`No device ${o.device}: adb lists ${listed}.`);
      if (device.state !== "device") throw new Error(`${o.device} is ${device.state}${device.state === "unauthorized" ? ": accept the USB debugging prompt on the device" : ""}.`);
    } else {
      const usable = devices.filter((d) => d.state === "device");
      if (usable.length !== 1) {
        throw new Error(usable.length ? `${usable.length} devices are connected (${listed}): pass device.` : `No Android device is connected and authorized (adb lists ${listed}).`);
      }
      device = usable[0];
    }
    const taken = new Set([...this._sessions.values()].filter((s) => s.connected || s.pid !== null).map((s) => s.port));
    const port = await findFreePort(o.port ?? DEFAULT_PORT, (p) => taken.has(p));
    const serial = device.serial;
    const session = new LiveSession(`app-${++this._counter}`, `${o.package} (Android, ${device.model || serial})`, port, true);
    this._sessions.set(session.id, session);
    this._latest = session;
    const target = new AndroidTarget({
      adb: adb2,
      serial,
      package: o.package,
      activity: o.activity ?? "",
      port,
      log: true,
      recordAlways: !!o.recordAlways,
      stacktraces: o.stacktraces ?? true,
      layer,
      onLog: (line) => session.appendLog(line),
      onExit: () => session.remoteEnded("exited", "the application exited on the device")
    });
    const stop = async () => {
      await target.stop();
      await disableLayer(adb2, serial);
    };
    session.remote = { stop, repair: () => target.ensureForward() };
    session.appendLog(`launching ${o.package} on ${serial} (${device.model || "unknown model"}, Android API ${device.sdk}, ${device.abi})`);
    try {
      await target.start();
    } catch (e) {
      session.remote = null;
      await stop().catch(() => void 0);
      session.remoteEnded("error", e instanceof Error ? e.message : String(e));
      return session;
    }
    session.pid = target.pid;
    await session.connect(waitMs);
    return session;
  }
  /** Attaches to an application whose capture library already listens on `port`. */
  async attach(port, waitMs) {
    const session = new LiveSession(`app-${++this._counter}`, `port ${port}`, port, false);
    if (!await session.connect(waitMs)) {
      throw new Error(`Nothing answered on port ${port} within ${waitMs / 1e3} s. An application listens there when it was started with GPU Inspector's capture library (VKINSP_PORT, or MTLINSP_PORT on macOS).`);
    }
    this._sessions.set(session.id, session);
    this._latest = session;
    return session;
  }
  /** A session by id, or the one started most recently. */
  get(id) {
    if (!id) {
      if (!this._latest) throw new Error("No live session: launch_app starts an application with the capture library, attach_app connects to one already running.");
      return this._latest;
    }
    const s = this._sessions.get(id);
    if (!s) throw new Error(`No live session "${id}". ${this._sessions.size ? `Sessions: ${[...this._sessions.keys()].join(", ")}.` : "There are none."}`);
    return s;
  }
  list() {
    return [...this._sessions.values()];
  }
  async stopAll() {
    await Promise.all([...this._sessions.values()].map((s) => s.stop()));
  }
};

// src/main/shader_tools.ts
import { execFile as execFile4 } from "node:child_process";
import fs9 from "node:fs";
import os6 from "node:os";
import path8 from "node:path";
var tempCounter = 0;
function tempBase() {
  return path8.join(os6.tmpdir(), `vkinsp_${process.pid}_${Date.now()}_${++tempCounter}`);
}
function findTool(name) {
  const exe = process.platform === "win32" ? `${name}.exe` : name;
  const candidates = [];
  if (process.env.INSPECTOR_TOOLS_DIR) candidates.push(path8.join(process.env.INSPECTOR_TOOLS_DIR, exe));
  if (process.env.VULKAN_SDK) candidates.push(path8.join(process.env.VULKAN_SDK, "Bin", exe), path8.join(process.env.VULKAN_SDK, "bin", exe));
  for (const c2 of candidates) if (fs9.existsSync(c2)) return c2;
  return exe;
}
function shaderText(spirv, mode) {
  return new Promise((resolve) => {
    const tmp = `${tempBase()}.spv`;
    fs9.writeFileSync(tmp, Buffer.from(spirv));
    let tool;
    let args;
    if (mode === "dis") {
      tool = findTool("spirv-dis");
      args = ["--comment", "--no-color", tmp];
    } else {
      tool = findTool("spirv-cross");
      args = [tmp];
      if (mode === "hlsl") args.push("--hlsl", "--shader-model", "60");
      else if (mode === "msl") args.push("--msl");
      else args.push("--vulkan-semantics", "--version", "460");
    }
    execFile4(tool, args, { maxBuffer: 64 * 1024 * 1024 }, (err, stdout, stderr) => {
      try {
        fs9.unlinkSync(tmp);
      } catch {
      }
      if (err) resolve({ ok: false, text: `${path8.basename(tool)} failed: ${stderr || err.message}` });
      else resolve({ ok: true, text: stdout });
    });
  });
}
var GLSL_STAGES = {
  vertex: "vert",
  tess_control: "tesc",
  tess_eval: "tese",
  geometry: "geom",
  fragment: "frag",
  compute: "comp",
  task: "task",
  mesh: "mesh",
  raygen: "rgen",
  intersection: "rint",
  any_hit: "rahit",
  closest_hit: "rchit",
  miss: "rmiss",
  callable: "rcall"
};
var HLSL_PROFILES = {
  vertex: "vs_6_0",
  tess_control: "hs_6_0",
  tess_eval: "ds_6_0",
  geometry: "gs_6_0",
  fragment: "ps_6_0",
  compute: "cs_6_0",
  task: "as_6_5",
  mesh: "ms_6_5",
  raygen: "lib_6_3",
  intersection: "lib_6_3",
  any_hit: "lib_6_3",
  closest_hit: "lib_6_3",
  miss: "lib_6_3",
  callable: "lib_6_3"
};
function targetEnv(spirvVersion, tool) {
  const v = spirvVersion || "1.5";
  if (tool === "spirv-as") return `spv${v}`;
  const glslang = { "1.0": "vulkan1.0", "1.3": "vulkan1.1", "1.4": "vulkan1.1spirv1.4", "1.5": "vulkan1.2", "1.6": "vulkan1.3" };
  const env = glslang[v] ?? "vulkan1.2";
  return tool === "dxc" ? env : env.replace("spirv", "spv");
}
function needsIncludeExtension(source) {
  return /^[ \t]*#[ \t]*include/m.test(source) && !/GL_GOOGLE_include_directive|GL_ARB_shading_language_include/.test(source);
}
function compileShader(source, language, stage, entryPoint, spirvVersion, options = {}) {
  return new Promise((resolve) => {
    const base = tempBase();
    const includeDirs = (options.includeDirs ?? []).filter((d) => d && fs9.existsSync(d));
    const src = base + (language === "hlsl" ? ".hlsl" : language === "spirv-asm" ? ".spvasm" : ".glsl");
    const out = base + ".spv";
    fs9.writeFileSync(src, source);
    const entry = entryPoint || "main";
    let tool;
    let args;
    if (language === "spirv-asm") {
      tool = findTool("spirv-as");
      args = ["--target-env", targetEnv(spirvVersion, "spirv-as"), "-o", out, src];
    } else if (language === "hlsl") {
      tool = findTool("dxc");
      args = ["-spirv", "-T", HLSL_PROFILES[stage] ?? "ps_6_0", "-E", entry, `-fspv-target-env=${targetEnv(spirvVersion, "dxc")}`, "-Fo", out, src];
      for (const dir of includeDirs) args.push("-I", dir);
    } else {
      tool = findTool("glslangValidator");
      args = [
        "-V",
        "-S",
        GLSL_STAGES[stage] ?? "frag",
        "--target-env",
        targetEnv(spirvVersion, "glslang"),
        "--source-entrypoint",
        "main",
        "-e",
        entry,
        "-o",
        out,
        src
      ];
      for (const dir of includeDirs) args.push(`-I${dir}`);
      if (needsIncludeExtension(source)) args.push("-P#extension GL_GOOGLE_include_directive : require");
    }
    execFile4(tool, args, { maxBuffer: 64 * 1024 * 1024 }, (err, stdout, stderr) => {
      const log = `${stdout ?? ""}${stderr ?? ""}`.trim();
      let spirv;
      try {
        if (fs9.existsSync(out)) spirv = new Uint8Array(fs9.readFileSync(out));
      } catch {
        spirv = void 0;
      }
      for (const f of [src, out]) {
        try {
          fs9.unlinkSync(f);
        } catch {
        }
      }
      const name = path8.basename(tool);
      if (err || !spirv || spirv.byteLength < 20) {
        const reason = log || (err && "code" in err && err.code === "ENOENT" ? `${name} not found: install the Vulkan SDK or set VULKAN_SDK` : err?.message ?? `${name} produced no output`);
        resolve({ ok: false, log: reason, tool: name });
      } else {
        resolve({ ok: true, spirv, log, tool: name });
      }
    });
  });
}

// src/renderer/vulkan/astc_decode.ts
var q = (levels, bits, trits = false, quints = false) => ({ levels, bits, trits, quints });
var WEIGHT_QUANT = [
  null,
  null,
  q(2, 1),
  q(3, 0, true),
  q(4, 2),
  q(5, 0, false, true),
  q(6, 1, true),
  q(8, 3),
  null,
  null,
  q(10, 1, false, true),
  q(12, 2, true),
  q(16, 4),
  q(20, 2, false, true),
  q(24, 3, true),
  q(32, 5)
];
var COLOR_QUANT = [
  q(6, 1, true),
  q(8, 3),
  q(10, 1, false, true),
  q(12, 2, true),
  q(16, 4),
  q(20, 2, false, true),
  q(24, 3, true),
  q(32, 5),
  q(40, 3, false, true),
  q(48, 4, true),
  q(64, 6),
  q(80, 4, false, true),
  q(96, 5, true),
  q(128, 7),
  q(160, 5, false, true),
  q(192, 6, true),
  q(256, 8)
];
function iseBits(count2, quant) {
  let bits = count2 * quant.bits;
  if (quant.trits) bits += Math.ceil(count2 * 8 / 5);
  if (quant.quints) bits += Math.ceil(count2 * 7 / 3);
  return bits;
}
var Reader = class {
  constructor(bytes) {
    this.bytes = bytes;
  }
  pos = 0;
  bit() {
    const p = this.pos++;
    return p < 128 ? this.bytes[p >> 3] >> (p & 7) & 1 : 0;
  }
  read(n) {
    let v = 0;
    for (let i = 0; i < n; i++) v |= this.bit() << i;
    return v;
  }
  at(pos, n) {
    this.pos = pos;
    return this.read(n);
  }
};
function tritsOf(t) {
  let c2;
  let t3;
  let t4;
  if ((t >> 2 & 7) === 7) {
    c2 = (t >> 5 & 7) << 2 | t & 3;
    t4 = 2;
    t3 = 2;
  } else {
    c2 = t & 31;
    if ((t >> 5 & 3) === 3) {
      t4 = 2;
      t3 = t >> 7 & 1;
    } else {
      t4 = t >> 7 & 1;
      t3 = t >> 5 & 3;
    }
  }
  let t0;
  let t1;
  let t2;
  if ((c2 & 3) === 3) {
    t2 = 2;
    t1 = c2 >> 4 & 1;
    const c3 = c2 >> 3 & 1;
    t0 = c3 << 1 | c2 >> 2 & 1 & (c3 ^ 1);
  } else if ((c2 >> 2 & 3) === 3) {
    t2 = 2;
    t1 = 2;
    t0 = c2 & 3;
  } else {
    t2 = c2 >> 4 & 1;
    t1 = c2 >> 2 & 3;
    const c1 = c2 >> 1 & 1;
    t0 = c1 << 1 | c2 & 1 & (c1 ^ 1);
  }
  return [t0, t1, t2, t3, t4];
}
function quintsOf(qv) {
  let q0;
  let q1;
  let q2;
  if ((qv >> 1 & 3) === 3 && (qv >> 5 & 3) === 0) {
    const q0bit = qv & 1;
    q2 = q0bit << 2 | (qv >> 4 & 1 & (q0bit ^ 1)) << 1 | qv >> 3 & 1 & (q0bit ^ 1);
    q1 = 4;
    q0 = 4;
  } else {
    let c2;
    if ((qv >> 1 & 3) === 3) {
      q2 = 4;
      c2 = (qv >> 3 & 3) << 3 | (~qv >> 5 & 3) << 1 | qv & 1;
    } else {
      q2 = qv >> 5 & 3;
      c2 = qv & 31;
    }
    if ((c2 & 7) === 5) {
      q1 = 4;
      q0 = c2 >> 3 & 3;
    } else {
      q1 = c2 >> 3 & 3;
      q0 = c2 & 7;
    }
  }
  return [q0, q1, q2];
}
function decodeIse(r, count2, quant, out) {
  if (quant.trits) {
    const sizes = [2, 2, 1, 2, 1];
    for (let i = 0; i < count2; i += 5) {
      const n = Math.min(5, count2 - i);
      let t = 0;
      let shift = 0;
      const m = [];
      for (let j = 0; j < n; j++) {
        m.push(r.read(quant.bits));
        t |= r.read(sizes[j]) << shift;
        shift += sizes[j];
      }
      const trits = tritsOf(t);
      for (let j = 0; j < n; j++) out[i + j] = trits[j] << 8 | m[j];
    }
  } else if (quant.quints) {
    const sizes = [3, 2, 2];
    for (let i = 0; i < count2; i += 3) {
      const n = Math.min(3, count2 - i);
      let qv = 0;
      let shift = 0;
      const m = [];
      for (let j = 0; j < n; j++) {
        m.push(r.read(quant.bits));
        qv |= r.read(sizes[j]) << shift;
        shift += sizes[j];
      }
      const quints = quintsOf(qv);
      for (let j = 0; j < n; j++) out[i + j] = quints[j] << 8 | m[j];
    }
  } else {
    for (let i = 0; i < count2; i++) out[i] = r.read(quant.bits);
  }
}
function pattern(spec, m) {
  let v = 0;
  for (let i = 0; i < spec.length; i++) {
    const ch2 = spec[i];
    const bit = ch2 === "0" ? 0 : m >> ch2.charCodeAt(0) - 97 & 1;
    v = v << 1 | bit;
  }
  return v;
}
function replicate(m, bits, to) {
  let r = m;
  let n = bits;
  while (n < to) {
    r = r << n | r;
    n *= 2;
  }
  return r >> n - to;
}
var COLOR_TRIT = { 1: ["000000000", 204], 2: ["b000b0bb0", 93], 3: ["cb000cbcb", 44], 4: ["dcb000dcb", 22], 5: ["edcb000ed", 11], 6: ["fedcb000f", 5] };
var COLOR_QUINT = { 1: ["000000000", 113], 2: ["b0000bb00", 54], 3: ["cb0000cbc", 26], 4: ["dcb0000dc", 13], 5: ["edcb0000e", 6] };
var WEIGHT_TRIT = { 1: ["0000000", 50], 2: ["b000b0b", 23], 3: ["cb000cb", 11] };
var WEIGHT_QUINT = { 1: ["0000000", 28], 2: ["b0000b0", 13] };
function unquantizeColor(v, quant) {
  const m = v & 255;
  const d = v >> 8;
  if (!quant.trits && !quant.quints) return replicate(m, quant.bits, 8);
  const [b, c2] = quant.trits ? COLOR_TRIT[quant.bits] : COLOR_QUINT[quant.bits];
  const a = m & 1 ? 511 : 0;
  let t = d * c2 + pattern(b, m);
  t ^= a;
  return a & 128 | t >> 2;
}
function unquantizeWeight(v, quant) {
  const m = v & 255;
  const d = v >> 8;
  if (quant.bits === 0) return quant.trits ? d * 32 : d * 16;
  let w;
  if (!quant.trits && !quant.quints) {
    switch (quant.bits) {
      case 1:
        w = m ? 63 : 0;
        break;
      case 2:
        w = m * 21;
        break;
      case 3:
        w = m * 9;
        break;
      case 4:
        w = m << 2 | m >> 2;
        break;
      default:
        w = m << 1 | m >> 4;
        break;
    }
  } else {
    const [b, c2] = quant.trits ? WEIGHT_TRIT[quant.bits] : WEIGHT_QUINT[quant.bits];
    const a = m & 1 ? 127 : 0;
    let t = d * c2 + pattern(b, m);
    t ^= a;
    w = a & 32 | t >> 2;
  }
  return w > 32 ? w + 1 : w;
}
function hash52(p) {
  p = (p ^ p >>> 15) >>> 0;
  p = p - (p << 17) >>> 0;
  p = p + (p << 7) >>> 0;
  p = p + (p << 4) >>> 0;
  p = (p ^ p >>> 5) >>> 0;
  p = p + (p << 16) >>> 0;
  p = (p ^ p >>> 7) >>> 0;
  p = (p ^ p >>> 3) >>> 0;
  p = (p ^ p << 6) >>> 0;
  p = (p ^ p >>> 17) >>> 0;
  return p;
}
function selectPartition(seed, x, y, count2, small) {
  if (small) {
    x <<= 1;
    y <<= 1;
  }
  seed += (count2 - 1) * 1024;
  const rnum = hash52(seed);
  const s = [];
  for (let i = 0; i < 12; i++) {
    const v = rnum >>> i * 4 & 15;
    s.push(v * v);
  }
  let sh1;
  let sh2;
  if (seed & 1) {
    sh1 = seed & 2 ? 4 : 5;
    sh2 = count2 === 3 ? 6 : 5;
  } else {
    sh1 = count2 === 3 ? 6 : 5;
    sh2 = seed & 2 ? 4 : 5;
  }
  const sh3 = seed & 16 ? sh1 : sh2;
  s[0] >>= sh1;
  s[1] >>= sh2;
  s[2] >>= sh1;
  s[3] >>= sh2;
  s[4] >>= sh1;
  s[5] >>= sh2;
  s[6] >>= sh1;
  s[7] >>= sh2;
  s[8] >>= sh3;
  s[9] >>= sh3;
  s[10] >>= sh3;
  s[11] >>= sh3;
  let a = s[0] * x + s[1] * y + (rnum >>> 14) & 63;
  let b = s[2] * x + s[3] * y + (rnum >>> 10) & 63;
  let c2 = s[4] * x + s[5] * y + (rnum >>> 6) & 63;
  let d = s[6] * x + s[7] * y + (rnum >>> 2) & 63;
  if (count2 < 4) d = 0;
  if (count2 < 3) c2 = 0;
  if (a >= b && a >= c2 && a >= d) return 0;
  if (b >= c2 && b >= d) return 1;
  if (c2 >= d) return 2;
  return 3;
}
function clamp255(v) {
  return v < 0 ? 0 : v > 255 ? 255 : v;
}
function bitTransfer(v, i, j) {
  let a = v[i];
  let b = v[j];
  b >>= 1;
  b |= a & 128;
  a >>= 1;
  a &= 63;
  if (a & 32) a -= 64;
  v[i] = a;
  v[j] = b;
}
function blueContract(r, g, b, a) {
  return [r + b >> 1, g + b >> 1, b, a];
}
function decodeEndpoints(cem, v) {
  switch (cem) {
    case 0:
      return [[v[0], v[0], v[0], 255], [v[1], v[1], v[1], 255]];
    case 1: {
      const l0 = v[0] >> 2 | v[1] & 192;
      const l1 = Math.min(255, l0 + (v[1] & 63));
      return [[l0, l0, l0, 255], [l1, l1, l1, 255]];
    }
    case 4:
      return [[v[0], v[0], v[0], v[2]], [v[1], v[1], v[1], v[3]]];
    case 5: {
      bitTransfer(v, 1, 0);
      bitTransfer(v, 3, 2);
      const l1 = clamp255(v[0] + v[1]);
      return [[v[0], v[0], v[0], v[2]], [l1, l1, l1, clamp255(v[2] + v[3])]];
    }
    case 6:
      return [[v[0] * v[3] >> 8, v[1] * v[3] >> 8, v[2] * v[3] >> 8, 255], [v[0], v[1], v[2], 255]];
    case 8: {
      if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]) return [[v[0], v[2], v[4], 255], [v[1], v[3], v[5], 255]];
      return [blueContract(v[1], v[3], v[5], 255), blueContract(v[0], v[2], v[4], 255)];
    }
    case 9: {
      bitTransfer(v, 1, 0);
      bitTransfer(v, 3, 2);
      bitTransfer(v, 5, 4);
      const r1 = v[0] + v[1];
      const g1 = v[2] + v[3];
      const b1 = v[4] + v[5];
      let e0;
      let e1;
      if (v[1] + v[3] + v[5] >= 0) {
        e0 = [v[0], v[2], v[4], 255];
        e1 = [r1, g1, b1, 255];
      } else {
        e0 = blueContract(r1, g1, b1, 255);
        e1 = blueContract(v[0], v[2], v[4], 255);
      }
      return [e0.map(clamp255), e1.map(clamp255)];
    }
    case 10:
      return [[v[0] * v[3] >> 8, v[1] * v[3] >> 8, v[2] * v[3] >> 8, v[4]], [v[0], v[1], v[2], v[5]]];
    case 12: {
      if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]) return [[v[0], v[2], v[4], v[6]], [v[1], v[3], v[5], v[7]]];
      return [blueContract(v[1], v[3], v[5], v[7]), blueContract(v[0], v[2], v[4], v[6])];
    }
    case 13: {
      bitTransfer(v, 1, 0);
      bitTransfer(v, 3, 2);
      bitTransfer(v, 5, 4);
      bitTransfer(v, 7, 6);
      const r1 = v[0] + v[1];
      const g1 = v[2] + v[3];
      const b1 = v[4] + v[5];
      const a1 = v[6] + v[7];
      let e0;
      let e1;
      if (v[1] + v[3] + v[5] >= 0) {
        e0 = [v[0], v[2], v[4], v[6]];
        e1 = [r1, g1, b1, a1];
      } else {
        e0 = blueContract(r1, g1, b1, a1);
        e1 = blueContract(v[0], v[2], v[4], v[6]);
      }
      return [e0.map(clamp255), e1.map(clamp255)];
    }
    default:
      return null;
  }
}
function halfToFloat(h) {
  const s = h & 32768 ? -1 : 1;
  const e = h >> 10 & 31;
  const f = h & 1023;
  if (e === 0) return s * Math.pow(2, -14) * (f / 1024);
  if (e === 31) return f ? NaN : s * Infinity;
  return s * Math.pow(2, e - 15) * (1 + f / 1024);
}
function errorColor(px, texels) {
  for (let t = 0; t < texels; t++) {
    px[t * 4] = 1;
    px[t * 4 + 1] = 0;
    px[t * 4 + 2] = 1;
    px[t * 4 + 3] = 1;
  }
}
var scratchInts = new Int32Array(80);
var scratchWeights = new Int32Array(64);
function decodeAstcBlock(s, block, bw, bh, px, srgb) {
  const texels = bw * bh;
  const bytes = new Uint8Array(16);
  for (let i = 0; i < 16; i++) bytes[i] = s.getUint8(block + i);
  const r = new Reader(bytes);
  const mode = r.at(0, 11);
  if ((mode & 511) === 508) {
    const hdr = mode >> 9 & 1;
    const c2 = [];
    for (let i = 0; i < 4; i++) c2.push(r.at(64 + i * 16, 16));
    for (let t = 0; t < texels; t++) {
      for (let i = 0; i < 4; i++) px[t * 4 + i] = hdr ? halfToFloat(c2[i]) : (c2[i] >> 8) / 255;
    }
    return;
  }
  let dual = mode >> 10 & 1;
  let high = mode >> 9 & 1;
  let gw;
  let gh;
  let range;
  const a = mode >> 5 & 3;
  if (mode & 3) {
    range = (mode >> 1 & 1) << 2 | (mode & 1) << 1 | mode >> 4 & 1;
    const b = mode >> 7 & 3;
    switch (mode >> 2 & 3) {
      case 0:
        gw = b + 4;
        gh = a + 2;
        break;
      case 1:
        gw = b + 8;
        gh = a + 2;
        break;
      case 2:
        gw = a + 2;
        gh = b + 8;
        break;
      default:
        if (mode & 256) {
          gw = (b & 1) + 2;
          gh = a + 2;
        } else {
          gw = a + 2;
          gh = (b & 1) + 6;
        }
        break;
    }
  } else {
    range = (mode >> 3 & 1) << 2 | (mode >> 2 & 1) << 1 | mode >> 4 & 1;
    switch (mode >> 7 & 3) {
      case 0:
        gw = 12;
        gh = a + 2;
        break;
      case 1:
        gw = a + 2;
        gh = 12;
        break;
      case 2:
        gw = a + 6;
        gh = (mode >> 9 & 3) + 6;
        dual = 0;
        high = 0;
        break;
      default:
        if (a === 0) {
          gw = 6;
          gh = 10;
        } else if (a === 1) {
          gw = 10;
          gh = 6;
        } else {
          errorColor(px, texels);
          return;
        }
        break;
    }
  }
  const weightQuant = WEIGHT_QUANT[high << 3 | range];
  const weightCount = gw * gh * (dual ? 2 : 1);
  if (!weightQuant || gw > bw || gh > bh || weightCount > 64) {
    errorColor(px, texels);
    return;
  }
  const weightBits = iseBits(weightCount, weightQuant);
  if (weightBits < 24 || weightBits > 96) {
    errorColor(px, texels);
    return;
  }
  const partitions = r.at(11, 2) + 1;
  if (dual && partitions > 3) {
    errorColor(px, texels);
    return;
  }
  const cems = [];
  let partitionIndex = 0;
  let colorStart;
  let extraBits = 0;
  if (partitions === 1) {
    cems.push(r.at(13, 4));
    colorStart = 17;
  } else {
    partitionIndex = r.at(13, 10);
    colorStart = 29;
    let encoded = r.at(23, 6);
    const cls = encoded & 3;
    if (cls === 0) {
      for (let p = 0; p < partitions; p++) cems.push(encoded >> 2);
    } else {
      extraBits = 3 * partitions - 4;
      encoded |= r.at(128 - weightBits - extraBits, extraBits) << 6;
      for (let p = 0; p < partitions; p++) {
        const c2 = encoded >> 2 + p & 1;
        const m = encoded >> 2 + partitions + 2 * p & 3;
        cems.push(cls - 1 + c2 << 2 | m);
      }
    }
  }
  const ccs = dual ? r.at(128 - weightBits - extraBits - 2, 2) : 0;
  let intCount = 0;
  for (const cem of cems) intCount += ((cem >> 2) + 1) * 2;
  const colorBits = 128 - weightBits - extraBits - (dual ? 2 : 0) - colorStart;
  let colorQuant = null;
  for (const cq of COLOR_QUANT) {
    if (iseBits(intCount, cq) <= colorBits) colorQuant = cq;
    else break;
  }
  if (!colorQuant || intCount > 18) {
    errorColor(px, texels);
    return;
  }
  r.pos = colorStart;
  decodeIse(r, intCount, colorQuant, scratchInts);
  const values = [];
  for (let i = 0; i < intCount; i++) values.push(unquantizeColor(scratchInts[i], colorQuant));
  const endpoints = [];
  let at = 0;
  for (const cem of cems) {
    const n = ((cem >> 2) + 1) * 2;
    const e = decodeEndpoints(cem, values.slice(at, at + n));
    if (!e) {
      errorColor(px, texels);
      return;
    }
    endpoints.push(e);
    at += n;
  }
  const reversed = new Uint8Array(16);
  for (let i = 0; i < 128; i++) {
    if (bytes[15 - (i >> 3)] >> 7 - (i & 7) & 1) reversed[i >> 3] |= 1 << (i & 7);
  }
  const wr = new Reader(reversed);
  decodeIse(wr, weightCount, weightQuant, scratchWeights);
  const weights = new Int32Array(weightCount);
  for (let i = 0; i < weightCount; i++) weights[i] = unquantizeWeight(scratchWeights[i], weightQuant);
  const planes = dual ? 2 : 1;
  const weightAt = (plane, tx, ty) => {
    if (tx >= gw || ty >= gh) return 0;
    return weights[(ty * gw + tx) * planes + plane];
  };
  const ds = Math.floor((1024 + (bw >> 1)) / (bw - 1));
  const dt = Math.floor((1024 + (bh >> 1)) / (bh - 1));
  const small = texels < 31;
  for (let y = 0; y < bh; y++) {
    const gt = dt * y * (gh - 1) + 32 >> 6;
    const jt = gt >> 4;
    const ft = gt & 15;
    for (let x = 0; x < bw; x++) {
      const gs = ds * x * (gw - 1) + 32 >> 6;
      const js = gs >> 4;
      const fs12 = gs & 15;
      const w11 = fs12 * ft + 8 >> 4;
      const w10 = ft - w11;
      const w01 = fs12 - w11;
      const w00 = 16 - fs12 - ft + w11;
      const infill = (plane) => weightAt(plane, js, jt) * w00 + weightAt(plane, js + 1, jt) * w01 + weightAt(plane, js, jt + 1) * w10 + weightAt(plane, js + 1, jt + 1) * w11 + 8 >> 4;
      const w0 = infill(0);
      const w1 = dual ? infill(1) : w0;
      const part = partitions === 1 ? 0 : selectPartition(partitionIndex, x, y, partitions, small);
      const [e0, e1] = endpoints[part];
      const o = (y * bw + x) * 4;
      for (let c2 = 0; c2 < 4; c2++) {
        const lo = srgb && c2 < 3 ? e0[c2] << 8 | 128 : e0[c2] << 8 | e0[c2];
        const hi = srgb && c2 < 3 ? e1[c2] << 8 | 128 : e1[c2] << 8 | e1[c2];
        const w = dual && c2 === ccs ? w1 : w0;
        const v = lo * (64 - w) + hi * w + 32 >> 6;
        px[o + c2] = (v >> 8) / 255;
      }
    }
  }
}

// src/renderer/vulkan/bc67_decode.ts
var PARTITIONS_2 = new Uint8Array([
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  0,
  1,
  0,
  0,
  0,
  1,
  0,
  0,
  0,
  1,
  0,
  0,
  0,
  1,
  0,
  1,
  1,
  1,
  0,
  1,
  1,
  1,
  0,
  1,
  1,
  1,
  0,
  1,
  1,
  1,
  0,
  0,
  0,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  1,
  1,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  0,
  0,
  0,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  1,
  1,
  1,
  0,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  0,
  0,
  0,
  1,
  0,
  0,
  1,
  1,
  0,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  0,
  0,
  1,
  1,
  0,
  1,
  1,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  0,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  0,
  1,
  1,
  1,
  0,
  0,
  0,
  1,
  0,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  0,
  0,
  0,
  0,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  1,
  1,
  1,
  0,
  0,
  0,
  0,
  1,
  0,
  0,
  0,
  1,
  1,
  1,
  0,
  1,
  1,
  1,
  1,
  0,
  1,
  1,
  1,
  0,
  0,
  0,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  0,
  0,
  0,
  1,
  1,
  1,
  0,
  0,
  1,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  0,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  1,
  0,
  0,
  0,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  0,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  0,
  0,
  0,
  1,
  1,
  0,
  0,
  0,
  1,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  0,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  0,
  1,
  0,
  0,
  0,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  0,
  0,
  0,
  1,
  0,
  0,
  0,
  1,
  1,
  0,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  0,
  1,
  1,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  1,
  1,
  0,
  0,
  0,
  0,
  0,
  1,
  0,
  1,
  1,
  1,
  1,
  1,
  1,
  0,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  0,
  0,
  0,
  0,
  0,
  1,
  1,
  1,
  0,
  0,
  0,
  1,
  1,
  0,
  0,
  0,
  1,
  1,
  1,
  0,
  0,
  0,
  1,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  1,
  0,
  0,
  0,
  1,
  0,
  1,
  0,
  1,
  0,
  1,
  0,
  1,
  0,
  1,
  0,
  1,
  0,
  1,
  0,
  0,
  0,
  0,
  1,
  1,
  1,
  1,
  0,
  0,
  0,
  0,
  1,
  1,
  1,
  1,
  0,
  1,
  0,
  1,
  1,
  0,
  1,
  0,
  0,
  1,
  0,
  1,
  1,
  0,
  1,
  0,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  0,
  0,
  1,
  1,
  1,
  1,
  0,
  0,
  0,
  0,
  1,
  1,
  1,
  1,
  0,
  0,
  0,
  1,
  0,
  1,
  0,
  1,
  0,
  1,
  1,
  0,
  1,
  0,
  1,
  0,
  1,
  0,
  0,
  1,
  1,
  0,
  1,
  0,
  0,
  1,
  0,
  1,
  1,
  0,
  1,
  0,
  0,
  1,
  0,
  1,
  0,
  1,
  1,
  0,
  1,
  0,
  1,
  0,
  1,
  0,
  0,
  1,
  0,
  1,
  0,
  1,
  1,
  1,
  0,
  0,
  1,
  1,
  1,
  1,
  0,
  0,
  1,
  1,
  1,
  0,
  0,
  0,
  0,
  1,
  0,
  0,
  1,
  1,
  1,
  1,
  0,
  0,
  1,
  0,
  0,
  0,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  0,
  0,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  0,
  0,
  1,
  1,
  1,
  0,
  1,
  1,
  1,
  1,
  0,
  1,
  1,
  1,
  0,
  0,
  0,
  1,
  1,
  0,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  0,
  1,
  1,
  0,
  0,
  0,
  1,
  1,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  0,
  0,
  1,
  1,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  0,
  0,
  0,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  0,
  0,
  1,
  1,
  1,
  0,
  0,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  0,
  0,
  1,
  1,
  1,
  0,
  0,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  0,
  0,
  1,
  1,
  1,
  0,
  0,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  0,
  0,
  1,
  1,
  1,
  0,
  0,
  1,
  0,
  0,
  0,
  1,
  1,
  0,
  1,
  1,
  0,
  0,
  1,
  0,
  0,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  1,
  1,
  0,
  1,
  1,
  0,
  0,
  1,
  0,
  0,
  1,
  0,
  1,
  1,
  0,
  0,
  0,
  1,
  1,
  1,
  0,
  0,
  1,
  1,
  1,
  0,
  0,
  0,
  0,
  1,
  1,
  1,
  0,
  0,
  1,
  1,
  1,
  0,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  0,
  0,
  1,
  0,
  1,
  1,
  0,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  1,
  0,
  0,
  1,
  0,
  1,
  1,
  1,
  1,
  1,
  1,
  0,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  0,
  0,
  0,
  1,
  1,
  0,
  0,
  0,
  1,
  1,
  1,
  0,
  0,
  1,
  1,
  1,
  0,
  0,
  0,
  0,
  1,
  1,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  1,
  1,
  1,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  0,
  0,
  0,
  1,
  0,
  1,
  1,
  1,
  0,
  1,
  1,
  1,
  0,
  0,
  1,
  0,
  0,
  0,
  1,
  0,
  0,
  0,
  1,
  1,
  1,
  0,
  1,
  1,
  1
]);
var PARTITIONS_3 = new Uint8Array([
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  2,
  2,
  1,
  2,
  2,
  2,
  2,
  0,
  0,
  0,
  1,
  0,
  0,
  1,
  1,
  2,
  2,
  1,
  1,
  2,
  2,
  2,
  1,
  0,
  0,
  0,
  0,
  2,
  0,
  0,
  1,
  2,
  2,
  1,
  1,
  2,
  2,
  1,
  1,
  0,
  2,
  2,
  2,
  0,
  0,
  2,
  2,
  0,
  0,
  1,
  1,
  0,
  1,
  1,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  1,
  2,
  2,
  1,
  1,
  2,
  2,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  2,
  2,
  0,
  0,
  2,
  2,
  0,
  0,
  2,
  2,
  0,
  0,
  2,
  2,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  2,
  2,
  1,
  1,
  2,
  2,
  1,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  1,
  1,
  1,
  2,
  2,
  2,
  2,
  0,
  0,
  0,
  0,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  2,
  2,
  2,
  2,
  0,
  0,
  0,
  0,
  1,
  1,
  1,
  1,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  0,
  0,
  1,
  2,
  0,
  0,
  1,
  2,
  0,
  0,
  1,
  2,
  0,
  0,
  1,
  2,
  0,
  1,
  1,
  2,
  0,
  1,
  1,
  2,
  0,
  1,
  1,
  2,
  0,
  1,
  1,
  2,
  0,
  1,
  2,
  2,
  0,
  1,
  2,
  2,
  0,
  1,
  2,
  2,
  0,
  1,
  2,
  2,
  0,
  0,
  1,
  1,
  0,
  1,
  1,
  2,
  1,
  1,
  2,
  2,
  1,
  2,
  2,
  2,
  0,
  0,
  1,
  1,
  2,
  0,
  0,
  1,
  2,
  2,
  0,
  0,
  2,
  2,
  2,
  0,
  0,
  0,
  0,
  1,
  0,
  0,
  1,
  1,
  0,
  1,
  1,
  2,
  1,
  1,
  2,
  2,
  0,
  1,
  1,
  1,
  0,
  0,
  1,
  1,
  2,
  0,
  0,
  1,
  2,
  2,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  1,
  2,
  2,
  1,
  1,
  2,
  2,
  1,
  1,
  2,
  2,
  0,
  0,
  2,
  2,
  0,
  0,
  2,
  2,
  0,
  0,
  2,
  2,
  1,
  1,
  1,
  1,
  0,
  1,
  1,
  1,
  0,
  1,
  1,
  1,
  0,
  2,
  2,
  2,
  0,
  2,
  2,
  2,
  0,
  0,
  0,
  1,
  0,
  0,
  0,
  1,
  2,
  2,
  2,
  1,
  2,
  2,
  2,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  1,
  0,
  1,
  2,
  2,
  0,
  1,
  2,
  2,
  0,
  0,
  0,
  0,
  1,
  1,
  0,
  0,
  2,
  2,
  1,
  0,
  2,
  2,
  1,
  0,
  0,
  1,
  2,
  2,
  0,
  1,
  2,
  2,
  0,
  0,
  1,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  2,
  0,
  0,
  1,
  2,
  1,
  1,
  2,
  2,
  2,
  2,
  2,
  2,
  0,
  1,
  1,
  0,
  1,
  2,
  2,
  1,
  1,
  2,
  2,
  1,
  0,
  1,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  1,
  0,
  1,
  2,
  2,
  1,
  1,
  2,
  2,
  1,
  0,
  0,
  2,
  2,
  1,
  1,
  0,
  2,
  1,
  1,
  0,
  2,
  0,
  0,
  2,
  2,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  2,
  0,
  0,
  2,
  2,
  2,
  2,
  2,
  0,
  0,
  1,
  1,
  0,
  1,
  2,
  2,
  0,
  1,
  2,
  2,
  0,
  0,
  1,
  1,
  0,
  0,
  0,
  0,
  2,
  0,
  0,
  0,
  2,
  2,
  1,
  1,
  2,
  2,
  2,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  2,
  1,
  1,
  2,
  2,
  1,
  2,
  2,
  2,
  0,
  2,
  2,
  2,
  0,
  0,
  2,
  2,
  0,
  0,
  1,
  2,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  2,
  0,
  0,
  2,
  2,
  0,
  2,
  2,
  2,
  0,
  1,
  2,
  0,
  0,
  1,
  2,
  0,
  0,
  1,
  2,
  0,
  0,
  1,
  2,
  0,
  0,
  0,
  0,
  0,
  1,
  1,
  1,
  1,
  2,
  2,
  2,
  2,
  0,
  0,
  0,
  0,
  0,
  1,
  2,
  0,
  1,
  2,
  0,
  1,
  2,
  0,
  1,
  2,
  0,
  1,
  2,
  0,
  0,
  1,
  2,
  0,
  2,
  0,
  1,
  2,
  1,
  2,
  0,
  1,
  0,
  1,
  2,
  0,
  0,
  0,
  1,
  1,
  2,
  2,
  0,
  0,
  1,
  1,
  2,
  2,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  1,
  1,
  2,
  2,
  2,
  2,
  0,
  0,
  0,
  0,
  1,
  1,
  0,
  1,
  0,
  1,
  0,
  1,
  0,
  1,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  2,
  1,
  2,
  1,
  2,
  1,
  2,
  1,
  0,
  0,
  2,
  2,
  1,
  1,
  2,
  2,
  0,
  0,
  2,
  2,
  1,
  1,
  2,
  2,
  0,
  0,
  2,
  2,
  0,
  0,
  1,
  1,
  0,
  0,
  2,
  2,
  0,
  0,
  1,
  1,
  0,
  2,
  2,
  0,
  1,
  2,
  2,
  1,
  0,
  2,
  2,
  0,
  1,
  2,
  2,
  1,
  0,
  1,
  0,
  1,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  0,
  1,
  0,
  1,
  0,
  0,
  0,
  0,
  2,
  1,
  2,
  1,
  2,
  1,
  2,
  1,
  2,
  1,
  2,
  1,
  0,
  1,
  0,
  1,
  0,
  1,
  0,
  1,
  0,
  1,
  0,
  1,
  2,
  2,
  2,
  2,
  0,
  2,
  2,
  2,
  0,
  1,
  1,
  1,
  0,
  2,
  2,
  2,
  0,
  1,
  1,
  1,
  0,
  0,
  0,
  2,
  1,
  1,
  1,
  2,
  0,
  0,
  0,
  2,
  1,
  1,
  1,
  2,
  0,
  0,
  0,
  0,
  2,
  1,
  1,
  2,
  2,
  1,
  1,
  2,
  2,
  1,
  1,
  2,
  0,
  2,
  2,
  2,
  0,
  1,
  1,
  1,
  0,
  1,
  1,
  1,
  0,
  2,
  2,
  2,
  0,
  0,
  0,
  2,
  1,
  1,
  1,
  2,
  1,
  1,
  1,
  2,
  0,
  0,
  0,
  2,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  2,
  2,
  2,
  2,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  2,
  1,
  1,
  2,
  2,
  1,
  1,
  2,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  0,
  0,
  2,
  2,
  0,
  0,
  1,
  1,
  0,
  0,
  1,
  1,
  0,
  0,
  2,
  2,
  0,
  0,
  2,
  2,
  1,
  1,
  2,
  2,
  1,
  1,
  2,
  2,
  0,
  0,
  2,
  2,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  2,
  1,
  1,
  2,
  0,
  0,
  0,
  2,
  0,
  0,
  0,
  1,
  0,
  0,
  0,
  2,
  0,
  0,
  0,
  1,
  0,
  2,
  2,
  2,
  1,
  2,
  2,
  2,
  0,
  2,
  2,
  2,
  1,
  2,
  2,
  2,
  0,
  1,
  0,
  1,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  2,
  0,
  1,
  1,
  1,
  2,
  0,
  1,
  1,
  2,
  2,
  0,
  1,
  2,
  2,
  2,
  0
]);
var ANCHOR_2 = new Uint8Array([
  15,
  15,
  15,
  15,
  15,
  15,
  15,
  15,
  15,
  15,
  15,
  15,
  15,
  15,
  15,
  15,
  15,
  2,
  8,
  2,
  2,
  8,
  8,
  15,
  2,
  8,
  2,
  2,
  8,
  8,
  2,
  2,
  15,
  15,
  6,
  8,
  2,
  8,
  15,
  15,
  2,
  8,
  2,
  2,
  2,
  15,
  15,
  6,
  6,
  2,
  6,
  8,
  15,
  15,
  2,
  2,
  15,
  15,
  15,
  15,
  15,
  2,
  2,
  15
]);
var ANCHOR_3A = new Uint8Array([
  3,
  3,
  15,
  15,
  8,
  3,
  15,
  15,
  8,
  8,
  6,
  6,
  6,
  5,
  3,
  3,
  3,
  3,
  8,
  15,
  3,
  3,
  6,
  10,
  5,
  8,
  8,
  6,
  8,
  5,
  15,
  15,
  8,
  15,
  3,
  5,
  6,
  10,
  8,
  15,
  15,
  3,
  15,
  5,
  15,
  15,
  15,
  15,
  3,
  15,
  5,
  5,
  5,
  8,
  5,
  10,
  5,
  10,
  8,
  13,
  15,
  12,
  3,
  3
]);
var ANCHOR_3B = new Uint8Array([
  15,
  8,
  8,
  3,
  15,
  15,
  3,
  8,
  15,
  15,
  15,
  15,
  15,
  15,
  15,
  8,
  15,
  8,
  15,
  3,
  15,
  8,
  15,
  8,
  3,
  15,
  6,
  10,
  15,
  15,
  10,
  8,
  15,
  3,
  15,
  10,
  10,
  8,
  9,
  10,
  6,
  15,
  8,
  15,
  3,
  6,
  6,
  8,
  15,
  3,
  15,
  15,
  15,
  15,
  15,
  15,
  15,
  15,
  15,
  15,
  3,
  15,
  15,
  8
]);
var WEIGHTS_2 = [0, 21, 43, 64];
var WEIGHTS_3 = [0, 9, 18, 27, 37, 46, 55, 64];
var WEIGHTS_4 = [0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64];
var Bits = class {
  constructor(s, base) {
    this.s = s;
    this.base = base;
  }
  _pos = 0;
  get pos() {
    return this._pos;
  }
  bit() {
    const p = this._pos++;
    return this.s.getUint8(this.base + (p >> 3)) >> (p & 7) & 1;
  }
  read(n) {
    let v = 0;
    for (let i = 0; i < n; i++) v |= this.bit() << i;
    return v;
  }
};
function subsetOf(subsets, partition, texel2) {
  if (subsets === 1) return 0;
  return subsets === 2 ? PARTITIONS_2[partition * 16 + texel2] : PARTITIONS_3[partition * 16 + texel2];
}
function isAnchor(subsets, partition, texel2) {
  if (texel2 === 0) return true;
  if (subsets === 2) return ANCHOR_2[partition] === texel2;
  if (subsets === 3) return ANCHOR_3A[partition] === texel2 || ANCHOR_3B[partition] === texel2;
  return false;
}
var BC7_MODES = [
  { subsets: 3, partitionBits: 4, rotationBits: 0, indexSelectionBits: 0, colorBits: 4, alphaBits: 0, pBits: 1, indexBits: 3, index2Bits: 0 },
  { subsets: 2, partitionBits: 6, rotationBits: 0, indexSelectionBits: 0, colorBits: 6, alphaBits: 0, pBits: 2, indexBits: 3, index2Bits: 0 },
  { subsets: 3, partitionBits: 6, rotationBits: 0, indexSelectionBits: 0, colorBits: 5, alphaBits: 0, pBits: 0, indexBits: 2, index2Bits: 0 },
  { subsets: 2, partitionBits: 6, rotationBits: 0, indexSelectionBits: 0, colorBits: 7, alphaBits: 0, pBits: 1, indexBits: 2, index2Bits: 0 },
  { subsets: 1, partitionBits: 0, rotationBits: 2, indexSelectionBits: 1, colorBits: 5, alphaBits: 6, pBits: 0, indexBits: 2, index2Bits: 3 },
  { subsets: 1, partitionBits: 0, rotationBits: 2, indexSelectionBits: 0, colorBits: 7, alphaBits: 8, pBits: 0, indexBits: 2, index2Bits: 2 },
  { subsets: 1, partitionBits: 0, rotationBits: 0, indexSelectionBits: 0, colorBits: 7, alphaBits: 7, pBits: 1, indexBits: 4, index2Bits: 0 },
  { subsets: 2, partitionBits: 6, rotationBits: 0, indexSelectionBits: 0, colorBits: 5, alphaBits: 5, pBits: 1, indexBits: 2, index2Bits: 0 }
];
function expand8(v, bits) {
  return bits >= 8 ? v : v << 8 - bits | v >> 2 * bits - 8;
}
function weightsFor(bits) {
  return bits === 2 ? WEIGHTS_2 : bits === 3 ? WEIGHTS_3 : WEIGHTS_4;
}
function lerp8(a, b, w) {
  return (64 - w) * a + w * b + 32 >> 6;
}
function decodeBc7Block(s, block, px) {
  const bits = new Bits(s, block);
  let mode = 0;
  while (mode < 8 && bits.bit() === 0) mode++;
  if (mode === 8) {
    px.fill(0);
    return;
  }
  const m = BC7_MODES[mode];
  const partition = bits.read(m.partitionBits);
  const rotation = bits.read(m.rotationBits);
  const indexSelection = bits.read(m.indexSelectionBits);
  const endpoints = m.subsets * 2;
  const e = [];
  for (let i = 0; i < endpoints; i++) e.push([0, 0, 0, 255]);
  for (let c2 = 0; c2 < 3; c2++) for (let i = 0; i < endpoints; i++) e[i][c2] = bits.read(m.colorBits);
  if (m.alphaBits) for (let i = 0; i < endpoints; i++) e[i][3] = bits.read(m.alphaBits);
  let colorBits = m.colorBits;
  let alphaBits = m.alphaBits;
  if (m.pBits) {
    for (let i = 0; i < endpoints; i++) {
      if (m.pBits === 2 && i & 1) continue;
      const p = bits.bit();
      const last = m.pBits === 2 ? i + 1 : i;
      for (let j = i; j <= last; j++) {
        for (let c2 = 0; c2 < 3; c2++) e[j][c2] = e[j][c2] << 1 | p;
        if (m.alphaBits) e[j][3] = e[j][3] << 1 | p;
      }
    }
    colorBits++;
    if (m.alphaBits) alphaBits++;
  }
  for (let i = 0; i < endpoints; i++) {
    for (let c2 = 0; c2 < 3; c2++) e[i][c2] = expand8(e[i][c2], colorBits);
    if (m.alphaBits) e[i][3] = expand8(e[i][3], alphaBits);
  }
  const index1 = new Uint8Array(16);
  const index2 = new Uint8Array(16);
  for (let t = 0; t < 16; t++) index1[t] = bits.read(isAnchor(m.subsets, partition, t) ? m.indexBits - 1 : m.indexBits);
  if (m.index2Bits) for (let t = 0; t < 16; t++) index2[t] = bits.read(t === 0 ? m.index2Bits - 1 : m.index2Bits);
  const w1 = weightsFor(m.indexBits);
  const w2 = m.index2Bits ? weightsFor(m.index2Bits) : w1;
  for (let t = 0; t < 16; t++) {
    const subset = subsetOf(m.subsets, partition, t);
    const a = e[subset * 2];
    const b = e[subset * 2 + 1];
    let colorWeight;
    let alphaWeight;
    if (m.index2Bits) {
      if (indexSelection) {
        colorWeight = w2[index2[t]];
        alphaWeight = w1[index1[t]];
      } else {
        colorWeight = w1[index1[t]];
        alphaWeight = w2[index2[t]];
      }
    } else {
      colorWeight = alphaWeight = w1[index1[t]];
    }
    let r = lerp8(a[0], b[0], colorWeight);
    let g = lerp8(a[1], b[1], colorWeight);
    let bl = lerp8(a[2], b[2], colorWeight);
    let al = m.alphaBits ? lerp8(a[3], b[3], alphaWeight) : 255;
    if (rotation === 1) {
      const x = r;
      r = al;
      al = x;
    } else if (rotation === 2) {
      const x = g;
      g = al;
      al = x;
    } else if (rotation === 3) {
      const x = bl;
      bl = al;
      al = x;
    }
    px[t * 4] = r / 255;
    px[t * 4 + 1] = g / 255;
    px[t * 4 + 2] = bl / 255;
    px[t * 4 + 3] = al / 255;
  }
}
var R0 = 0;
var G0 = 1;
var B0 = 2;
var R1 = 3;
var G1 = 4;
var B1 = 5;
var R2 = 6;
var G2 = 7;
var B2 = 8;
var R3 = 9;
var G3 = 10;
var B3 = 11;
var PART = 12;
var BC6_MODES = [
  { code: 0, subsets: 2, endpointBits: 10, deltaBits: [5, 5, 5], transformed: true, layout: [
    [G2, 4, 1],
    [B2, 4, 1],
    [B3, 4, 1],
    [R0, 0, 10],
    [G0, 0, 10],
    [B0, 0, 10],
    [R1, 0, 5],
    [G3, 4, 1],
    [G2, 0, 4],
    [G1, 0, 5],
    [B3, 0, 1],
    [G3, 0, 4],
    [B1, 0, 5],
    [B3, 1, 1],
    [B2, 0, 4],
    [R2, 0, 5],
    [B3, 2, 1],
    [R3, 0, 5],
    [B3, 3, 1],
    [PART, 0, 5]
  ] },
  { code: 1, subsets: 2, endpointBits: 7, deltaBits: [6, 6, 6], transformed: true, layout: [
    [G2, 5, 1],
    [G3, 4, 1],
    [G3, 5, 1],
    [R0, 0, 7],
    [B3, 0, 1],
    [B3, 1, 1],
    [B2, 4, 1],
    [G0, 0, 7],
    [B2, 5, 1],
    [B3, 2, 1],
    [G2, 4, 1],
    [B0, 0, 7],
    [B3, 3, 1],
    [B3, 5, 1],
    [B3, 4, 1],
    [R1, 0, 6],
    [G2, 0, 4],
    [G1, 0, 6],
    [G3, 0, 4],
    [B1, 0, 6],
    [B2, 0, 4],
    [R2, 0, 6],
    [R3, 0, 6],
    [PART, 0, 5]
  ] },
  { code: 2, subsets: 2, endpointBits: 11, deltaBits: [5, 4, 4], transformed: true, layout: [
    [R0, 0, 10],
    [G0, 0, 10],
    [B0, 0, 10],
    [R1, 0, 5],
    [R0, 10, 1],
    [G2, 0, 4],
    [G1, 0, 4],
    [G0, 10, 1],
    [B3, 0, 1],
    [G3, 0, 4],
    [B1, 0, 4],
    [B0, 10, 1],
    [B3, 1, 1],
    [B2, 0, 4],
    [R2, 0, 5],
    [B3, 2, 1],
    [R3, 0, 5],
    [B3, 3, 1],
    [PART, 0, 5]
  ] },
  { code: 6, subsets: 2, endpointBits: 11, deltaBits: [4, 5, 4], transformed: true, layout: [
    [R0, 0, 10],
    [G0, 0, 10],
    [B0, 0, 10],
    [R1, 0, 4],
    [R0, 10, 1],
    [G3, 4, 1],
    [G2, 0, 4],
    [G1, 0, 5],
    [G0, 10, 1],
    [G3, 0, 4],
    [B1, 0, 4],
    [B0, 10, 1],
    [B3, 1, 1],
    [B2, 0, 4],
    [R2, 0, 4],
    [B3, 0, 1],
    [B3, 2, 1],
    [R3, 0, 4],
    [G2, 4, 1],
    [B3, 3, 1],
    [PART, 0, 5]
  ] },
  { code: 10, subsets: 2, endpointBits: 11, deltaBits: [4, 4, 5], transformed: true, layout: [
    [R0, 0, 10],
    [G0, 0, 10],
    [B0, 0, 10],
    [R1, 0, 4],
    [R0, 10, 1],
    [B2, 4, 1],
    [G2, 0, 4],
    [G1, 0, 4],
    [G0, 10, 1],
    [B3, 0, 1],
    [G3, 0, 4],
    [B1, 0, 5],
    [B0, 10, 1],
    [B2, 0, 4],
    [R2, 0, 4],
    [B3, 1, 1],
    [B3, 2, 1],
    [R3, 0, 4],
    [B3, 4, 1],
    [B3, 3, 1],
    [PART, 0, 5]
  ] },
  { code: 14, subsets: 2, endpointBits: 9, deltaBits: [5, 5, 5], transformed: true, layout: [
    [R0, 0, 9],
    [B2, 4, 1],
    [G0, 0, 9],
    [G2, 4, 1],
    [B0, 0, 9],
    [B3, 4, 1],
    [R1, 0, 5],
    [G3, 4, 1],
    [G2, 0, 4],
    [G1, 0, 5],
    [B3, 0, 1],
    [G3, 0, 4],
    [B1, 0, 5],
    [B3, 1, 1],
    [B2, 0, 4],
    [R2, 0, 5],
    [B3, 2, 1],
    [R3, 0, 5],
    [B3, 3, 1],
    [PART, 0, 5]
  ] },
  { code: 18, subsets: 2, endpointBits: 8, deltaBits: [6, 5, 5], transformed: true, layout: [
    [R0, 0, 8],
    [G3, 4, 1],
    [B2, 4, 1],
    [G0, 0, 8],
    [B3, 2, 1],
    [G2, 4, 1],
    [B0, 0, 8],
    [B3, 3, 1],
    [B3, 4, 1],
    [R1, 0, 6],
    [G2, 0, 4],
    [G1, 0, 5],
    [B3, 0, 1],
    [G3, 0, 4],
    [B1, 0, 5],
    [B3, 1, 1],
    [B2, 0, 4],
    [R2, 0, 6],
    [R3, 0, 6],
    [PART, 0, 5]
  ] },
  { code: 22, subsets: 2, endpointBits: 8, deltaBits: [5, 6, 5], transformed: true, layout: [
    [R0, 0, 8],
    [B3, 0, 1],
    [B2, 4, 1],
    [G0, 0, 8],
    [G2, 5, 1],
    [G2, 4, 1],
    [B0, 0, 8],
    [G3, 5, 1],
    [B3, 4, 1],
    [R1, 0, 5],
    [G3, 4, 1],
    [G2, 0, 4],
    [G1, 0, 6],
    [G3, 0, 4],
    [B1, 0, 5],
    [B3, 1, 1],
    [B2, 0, 4],
    [R2, 0, 5],
    [B3, 2, 1],
    [R3, 0, 5],
    [B3, 3, 1],
    [PART, 0, 5]
  ] },
  { code: 26, subsets: 2, endpointBits: 8, deltaBits: [5, 5, 6], transformed: true, layout: [
    [R0, 0, 8],
    [B3, 1, 1],
    [B2, 4, 1],
    [G0, 0, 8],
    [B2, 5, 1],
    [G2, 4, 1],
    [B0, 0, 8],
    [B3, 5, 1],
    [B3, 4, 1],
    [R1, 0, 5],
    [G3, 4, 1],
    [G2, 0, 4],
    [G1, 0, 5],
    [B3, 0, 1],
    [G3, 0, 4],
    [B1, 0, 6],
    [B2, 0, 4],
    [R2, 0, 5],
    [B3, 2, 1],
    [R3, 0, 5],
    [B3, 3, 1],
    [PART, 0, 5]
  ] },
  { code: 30, subsets: 2, endpointBits: 6, deltaBits: [6, 6, 6], transformed: false, layout: [
    [R0, 0, 6],
    [G3, 4, 1],
    [B3, 0, 1],
    [B3, 1, 1],
    [B2, 4, 1],
    [G0, 0, 6],
    [G2, 5, 1],
    [B2, 5, 1],
    [B3, 2, 1],
    [G2, 4, 1],
    [B0, 0, 6],
    [G3, 5, 1],
    [B3, 3, 1],
    [B3, 5, 1],
    [B3, 4, 1],
    [R1, 0, 6],
    [G2, 0, 4],
    [G1, 0, 6],
    [G3, 0, 4],
    [B1, 0, 6],
    [B2, 0, 4],
    [R2, 0, 6],
    [R3, 0, 6],
    [PART, 0, 5]
  ] },
  { code: 3, subsets: 1, endpointBits: 10, deltaBits: [10, 10, 10], transformed: false, layout: [
    [R0, 0, 10],
    [G0, 0, 10],
    [B0, 0, 10],
    [R1, 0, 10],
    [G1, 0, 10],
    [B1, 0, 10]
  ] },
  { code: 7, subsets: 1, endpointBits: 11, deltaBits: [9, 9, 9], transformed: true, layout: [
    [R0, 0, 10],
    [G0, 0, 10],
    [B0, 0, 10],
    [R1, 0, 9],
    [R0, 10, 1],
    [G1, 0, 9],
    [G0, 10, 1],
    [B1, 0, 9],
    [B0, 10, 1]
  ] },
  { code: 11, subsets: 1, endpointBits: 12, deltaBits: [8, 8, 8], transformed: true, layout: [
    [R0, 0, 10],
    [G0, 0, 10],
    [B0, 0, 10],
    [R1, 0, 8],
    [R0, 10, -2],
    [G1, 0, 8],
    [G0, 10, -2],
    [B1, 0, 8],
    [B0, 10, -2]
  ] },
  { code: 15, subsets: 1, endpointBits: 16, deltaBits: [4, 4, 4], transformed: true, layout: [
    [R0, 0, 10],
    [G0, 0, 10],
    [B0, 0, 10],
    [R1, 0, 4],
    [R0, 10, -6],
    [G1, 0, 4],
    [G0, 10, -6],
    [B1, 0, 4],
    [B0, 10, -6]
  ] }
];
var BC6_BY_CODE = new Map(BC6_MODES.map((m) => [m.code, m]));
function signExtend(v, bits) {
  const shift = 32 - bits;
  return v << shift >> shift;
}
function unquantize(v, bits, signed) {
  if (signed) {
    if (bits >= 16) return v;
    const negative = v < 0;
    const x = negative ? -v : v;
    let unq;
    if (x === 0) unq = 0;
    else if (x >= (1 << bits - 1) - 1) unq = 32767;
    else unq = (x << 15) + 16384 >> bits - 1;
    return negative ? -unq : unq;
  }
  if (bits >= 15) return v;
  if (v === 0) return 0;
  if (v === (1 << bits) - 1) return 65535;
  return (v << 15) + 16384 >> bits - 1;
}
function finishBc6(v, signed) {
  if (signed) return v < 0 ? -v * 31 >> 5 | 32768 : v * 31 >> 5;
  return v * 31 >> 6;
}
function halfBitsToFloat(h) {
  const sign = h & 32768 ? -1 : 1;
  const exp = h >> 10 & 31;
  const frac = h & 1023;
  if (exp === 0) return sign * Math.pow(2, -14) * (frac / 1024);
  if (exp === 31) return frac ? NaN : sign * Infinity;
  return sign * Math.pow(2, exp - 15) * (1 + frac / 1024);
}
function decodeBc6hBlock(s, block, px, signed) {
  const bits = new Bits(s, block);
  let code = bits.read(2);
  if (code >= 2) code |= bits.read(3) << 2;
  const m = BC6_BY_CODE.get(code);
  if (!m) {
    for (let t = 0; t < 16; t++) {
      px[t * 4] = 0;
      px[t * 4 + 1] = 0;
      px[t * 4 + 2] = 0;
      px[t * 4 + 3] = 1;
    }
    return;
  }
  const fields = new Int32Array(13);
  for (const [field2, low, count2] of m.layout) {
    if (count2 < 0) {
      for (let i = -count2 - 1; i >= 0; i--) fields[field2] |= bits.bit() << low + i;
    } else {
      for (let i = 0; i < count2; i++) fields[field2] |= bits.bit() << low + i;
    }
  }
  const partition = fields[PART];
  const epb = m.endpointBits;
  const endpoints = m.subsets * 2;
  const e = [];
  for (let i = 0; i < endpoints; i++) e.push([fields[i * 3], fields[i * 3 + 1], fields[i * 3 + 2]]);
  if (signed) for (let c2 = 0; c2 < 3; c2++) e[0][c2] = signExtend(e[0][c2], epb);
  if (m.transformed) {
    for (let i = 1; i < endpoints; i++) {
      for (let c2 = 0; c2 < 3; c2++) {
        const delta = signExtend(e[i][c2], m.deltaBits[c2]);
        const sum = e[0][c2] + delta & (1 << epb) - 1;
        e[i][c2] = signed ? signExtend(sum, epb) : sum;
      }
    }
  } else if (signed) {
    for (let i = 1; i < endpoints; i++) for (let c2 = 0; c2 < 3; c2++) e[i][c2] = signExtend(e[i][c2], epb);
  }
  for (let i = 0; i < endpoints; i++) for (let c2 = 0; c2 < 3; c2++) e[i][c2] = unquantize(e[i][c2], epb, signed);
  const indexBits = m.subsets === 1 ? 4 : 3;
  const weights = indexBits === 4 ? WEIGHTS_4 : WEIGHTS_3;
  for (let t = 0; t < 16; t++) {
    const anchor = t === 0 || m.subsets === 2 && ANCHOR_2[partition] === t;
    const index = bits.read(anchor ? indexBits - 1 : indexBits);
    const w = weights[index];
    const subset = m.subsets === 2 ? PARTITIONS_2[partition * 16 + t] : 0;
    const a = e[subset * 2];
    const b = e[subset * 2 + 1];
    for (let c2 = 0; c2 < 3; c2++) {
      const v = (64 - w) * a[c2] + w * b[c2] + 32 >> 6;
      px[t * 4 + c2] = halfBitsToFloat(finishBc6(v, signed));
    }
    px[t * 4 + 3] = 1;
  }
}

// src/renderer/vulkan/etc_decode.ts
var MODIFIERS = [
  [2, 8, -2, -8],
  [5, 17, -5, -17],
  [9, 29, -9, -29],
  [13, 42, -13, -42],
  [18, 60, -18, -60],
  [24, 80, -24, -80],
  [33, 106, -33, -106],
  [47, 183, -47, -183]
];
var DISTANCES = [3, 6, 11, 16, 23, 32, 41, 64];
var ALPHA_MODIFIERS = [
  [-3, -6, -9, -15, 2, 5, 8, 14],
  [-3, -7, -10, -13, 2, 6, 9, 12],
  [-2, -5, -8, -13, 1, 4, 7, 12],
  [-2, -4, -6, -13, 1, 3, 5, 12],
  [-3, -6, -8, -12, 2, 5, 7, 11],
  [-3, -7, -9, -11, 2, 6, 8, 10],
  [-4, -7, -8, -11, 3, 6, 7, 10],
  [-3, -5, -8, -11, 2, 4, 7, 10],
  [-2, -6, -8, -10, 1, 5, 7, 9],
  [-2, -5, -8, -10, 1, 4, 7, 9],
  [-2, -4, -8, -10, 1, 3, 7, 9],
  [-2, -5, -7, -10, 1, 4, 6, 9],
  [-3, -4, -7, -10, 2, 3, 6, 9],
  [-1, -2, -3, -10, 0, 1, 2, 9],
  [-4, -6, -8, -9, 3, 5, 7, 8],
  [-3, -5, -7, -9, 2, 4, 6, 8]
];
function clamp2552(v) {
  return v < 0 ? 0 : v > 255 ? 255 : v;
}
function field(word, hi, lo) {
  return word >>> lo & (1 << hi - lo + 1) - 1;
}
function expand4(v) {
  return v << 4 | v;
}
function expand5(v) {
  return v << 3 | v >> 2;
}
function expand6(v) {
  return v << 2 | v >> 4;
}
function expand7(v) {
  return v << 1 | v >> 6;
}
function put(px, x, y, r, g, b, a) {
  const o = (y * 4 + x) * 4;
  px[o] = r / 255;
  px[o + 1] = g / 255;
  px[o + 2] = b / 255;
  px[o + 3] = a;
}
function decodeEtc2Color(hi, lo, px, punchthrough) {
  const differential = punchthrough || field(hi, 1, 1) === 1;
  const opaque = !punchthrough || field(hi, 1, 1) === 1;
  let r1, g1, b1, r2, g2, b2;
  let mode = "block";
  if (differential) {
    r1 = field(hi, 31, 27);
    g1 = field(hi, 23, 19);
    b1 = field(hi, 15, 11);
    const dr = field(hi, 26, 24) << 29 >> 29;
    const dg = field(hi, 18, 16) << 29 >> 29;
    const db = field(hi, 10, 8) << 29 >> 29;
    r2 = r1 + dr;
    g2 = g1 + dg;
    b2 = b1 + db;
    if (r2 < 0 || r2 > 31) mode = "t";
    else if (g2 < 0 || g2 > 31) mode = "h";
    else if (b2 < 0 || b2 > 31) mode = "planar";
    if (mode === "block") {
      r1 = expand5(r1);
      g1 = expand5(g1);
      b1 = expand5(b1);
      r2 = expand5(r2);
      g2 = expand5(g2);
      b2 = expand5(b2);
    }
  } else {
    r1 = expand4(field(hi, 31, 28));
    g1 = expand4(field(hi, 23, 20));
    b1 = expand4(field(hi, 15, 12));
    r2 = expand4(field(hi, 27, 24));
    g2 = expand4(field(hi, 19, 16));
    b2 = expand4(field(hi, 11, 8));
  }
  if (mode === "planar") {
    const ro = expand6(field(hi, 30, 25));
    const go = expand7(field(hi, 24, 24) << 6 | field(hi, 22, 17));
    const bo = expand6(field(hi, 16, 16) << 5 | field(hi, 12, 11) << 3 | field(hi, 9, 7));
    const rh = expand6(field(hi, 6, 2) << 1 | field(hi, 0, 0));
    const gh = expand7(field(lo, 31, 25));
    const bh = expand6(field(lo, 24, 19));
    const rv = expand6(field(lo, 18, 13));
    const gv = expand7(field(lo, 12, 6));
    const bv = expand6(field(lo, 5, 0));
    for (let y = 0; y < 4; y++) {
      for (let x = 0; x < 4; x++) {
        const r = clamp2552(x * (rh - ro) + y * (rv - ro) + 4 * ro + 2 >> 2);
        const g = clamp2552(x * (gh - go) + y * (gv - go) + 4 * go + 2 >> 2);
        const b = clamp2552(x * (bh - bo) + y * (bv - bo) + 4 * bo + 2 >> 2);
        put(px, x, y, r, g, b, 1);
      }
    }
    return;
  }
  const indexOf = (x, y) => {
    const i = x * 4 + y;
    return (lo >>> 16 + i & 1) << 1 | lo >>> i & 1;
  };
  if (mode === "t" || mode === "h") {
    let paints;
    if (mode === "t") {
      const ra = expand4(field(hi, 28, 27) << 2 | field(hi, 25, 24));
      const ga = expand4(field(hi, 23, 20));
      const ba = expand4(field(hi, 19, 16));
      const rb = expand4(field(hi, 15, 12));
      const gb = expand4(field(hi, 11, 8));
      const bb = expand4(field(hi, 7, 4));
      const d = DISTANCES[field(hi, 3, 2) << 1 | field(hi, 0, 0)];
      paints = [[ra, ga, ba], [clamp2552(rb + d), clamp2552(gb + d), clamp2552(bb + d)], [rb, gb, bb], [clamp2552(rb - d), clamp2552(gb - d), clamp2552(bb - d)]];
    } else {
      const ra = expand4(field(hi, 30, 27));
      const ga = expand4(field(hi, 26, 24) << 1 | field(hi, 20, 20));
      const ba = expand4(field(hi, 19, 19) << 3 | field(hi, 17, 15));
      const rb = expand4(field(hi, 14, 11));
      const gb = expand4(field(hi, 10, 7));
      const bb = expand4(field(hi, 6, 3));
      const va = ra << 16 | ga << 8 | ba;
      const vb = rb << 16 | gb << 8 | bb;
      const d = DISTANCES[field(hi, 2, 2) << 2 | field(hi, 0, 0) << 1 | (va >= vb ? 1 : 0)];
      paints = [
        [clamp2552(ra + d), clamp2552(ga + d), clamp2552(ba + d)],
        [clamp2552(ra - d), clamp2552(ga - d), clamp2552(ba - d)],
        [clamp2552(rb + d), clamp2552(gb + d), clamp2552(bb + d)],
        [clamp2552(rb - d), clamp2552(gb - d), clamp2552(bb - d)]
      ];
    }
    for (let y = 0; y < 4; y++) {
      for (let x = 0; x < 4; x++) {
        const i = indexOf(x, y);
        if (!opaque && i === 2) put(px, x, y, 0, 0, 0, 0);
        else put(px, x, y, paints[i][0], paints[i][1], paints[i][2], 1);
      }
    }
    return;
  }
  const flip = field(hi, 0, 0) === 1;
  const table1 = MODIFIERS[field(hi, 7, 5)];
  const table2 = MODIFIERS[field(hi, 4, 2)];
  for (let y = 0; y < 4; y++) {
    for (let x = 0; x < 4; x++) {
      const second = flip ? y >= 2 : x >= 2;
      const table = second ? table2 : table1;
      const i = indexOf(x, y);
      if (!opaque && i === 2) {
        put(px, x, y, 0, 0, 0, 0);
        continue;
      }
      const m = !opaque && i === 0 ? 0 : table[i];
      const r = second ? r2 : r1;
      const g = second ? g2 : g1;
      const b = second ? b2 : b1;
      put(px, x, y, clamp2552(r + m), clamp2552(g + m), clamp2552(b + m), 1);
    }
  }
}
function decodeEac(hi, lo, out, channel, eleven, signed) {
  let base = field(hi, 31, 24);
  const multiplier = field(hi, 23, 20);
  const table = ALPHA_MODIFIERS[field(hi, 19, 16)];
  if (signed) {
    base = base << 24 >> 24;
    if (base === -128) base = -127;
  }
  const scale = !eleven ? multiplier : multiplier === 0 ? 1 : multiplier * 8;
  for (let i = 0; i < 16; i++) {
    const bit = 45 - i * 3;
    let sel;
    if (bit >= 32) sel = hi >>> bit - 32 & 7;
    else if (bit === 31) sel = (hi & 3) << 1 | lo >>> 31;
    else if (bit === 30) sel = (hi & 1) << 2 | lo >>> 30;
    else sel = lo >>> bit & 7;
    const x = i >> 2;
    const y = i & 3;
    let v;
    if (!eleven) {
      v = clamp2552(base + scale * table[sel]) / 255;
    } else if (signed) {
      const raw = base * 8 + scale * table[sel];
      v = Math.max(-1023, Math.min(1023, raw)) / 1023;
    } else {
      const raw = base * 8 + 4 + scale * table[sel];
      v = Math.max(0, Math.min(2047, raw)) / 2047;
    }
    out[(y * 4 + x) * 4 + channel] = v;
  }
}
function words(s, at) {
  return [s.getUint32(at, false), s.getUint32(at + 4, false)];
}
function decodeEtc2Rgb(s, block, px) {
  const [hi, lo] = words(s, block);
  decodeEtc2Color(hi, lo, px, false);
}
function decodeEtc2Rgba1(s, block, px) {
  const [hi, lo] = words(s, block);
  decodeEtc2Color(hi, lo, px, true);
}
function decodeEtc2Rgba8(s, block, px) {
  const [chi, clo] = words(s, block + 8);
  decodeEtc2Color(chi, clo, px, false);
  const [ahi, alo] = words(s, block);
  decodeEac(ahi, alo, px, 3, false, false);
}
function decodeEacR11(s, block, px, signed) {
  px.fill(0);
  const [hi, lo] = words(s, block);
  decodeEac(hi, lo, px, 0, true, signed);
}
function decodeEacRg11(s, block, px, signed) {
  px.fill(0);
  const [rhi, rlo] = words(s, block);
  decodeEac(rhi, rlo, px, 0, true, signed);
  const [ghi, glo] = words(s, block + 8);
  decodeEac(ghi, glo, px, 1, true, signed);
}

// src/renderer/vulkan/pvrtc_decode.ts
function unpackColors(word) {
  let b;
  if (word & 2147483648) {
    b = [word >>> 26 & 31, word >>> 21 & 31, word >>> 16 & 31, 15];
  } else {
    const r = word >>> 24 & 15;
    const g = word >>> 20 & 15;
    const bl = word >>> 16 & 15;
    b = [r << 1 | r >> 3, g << 1 | g >> 3, bl << 1 | bl >> 3, (word >>> 28 & 7) << 1];
  }
  let a;
  if (word & 32768) {
    const bl = word >>> 1 & 15;
    a = [word >>> 10 & 31, word >>> 5 & 31, bl << 1 | bl >> 3, 15];
  } else {
    const r = word >>> 8 & 15;
    const g = word >>> 4 & 15;
    const bl = word >>> 1 & 7;
    a = [r << 1 | r >> 3, g << 1 | g >> 3, bl << 2 | bl >> 1, (word >>> 12 & 7) << 1];
  }
  return [a, b];
}
function unpackBlock(s, at, twoBpp) {
  let bits = s.getUint32(at, true);
  const colors = s.getUint32(at + 4, true);
  const [a, b] = unpackColors(colors);
  const flag = colors & 1;
  const values = new Uint8Array(32);
  let mode = 0;
  if (!twoBpp) {
    mode = flag;
    for (let i = 0; i < 16; i++) {
      values[i] = bits & 3;
      bits >>>= 2;
    }
  } else if (flag) {
    mode = 1;
    if (bits & 1) {
      mode = bits & 1 << 20 ? 3 : 2;
      if (bits & 1 << 21) bits |= 1 << 20;
      else bits &= ~(1 << 20);
    }
    if (bits & 2) bits |= 1;
    else bits &= ~1;
    for (let y = 0; y < 4; y++) {
      for (let x = 0; x < 8; x++) {
        if (((x ^ y) & 1) === 0) {
          values[y * 8 + x] = bits & 3;
          bits >>>= 2;
        }
      }
    }
  } else {
    for (let i = 0; i < 32; i++) {
      values[i] = bits & 1 ? 3 : 0;
      bits >>>= 1;
    }
  }
  return { a, b, mode, values };
}
function twiddle(w, h, x, y) {
  const min = Math.min(w, h);
  let max = w > h ? x : y;
  let out = 0;
  let src = 1;
  let dst = 1;
  let shift = 0;
  while (src < min) {
    if (y & src) out |= dst;
    if (x & src) out |= dst << 1;
    src <<= 1;
    dst <<= 2;
    shift++;
  }
  max >>= shift;
  return out | max << 2 * shift;
}
var WEIGHTS_STANDARD = [0, 3, 5, 8];
var WEIGHTS_PUNCHTHROUGH = [0, 4, 4, 8];
function decodePvrtc(s, width, height, twoBpp, values) {
  const bw = twoBpp ? 8 : 4;
  const bh = 4;
  const blocksX = Math.max(1, width / bw);
  const blocksY = Math.max(1, height / bh);
  const blocks = new Array(blocksX * blocksY);
  for (let by = 0; by < blocksY; by++) {
    for (let bx = 0; bx < blocksX; bx++) {
      const at = twiddle(blocksX, blocksY, bx, by) * 8;
      blocks[by * blocksX + bx] = at + 8 <= s.byteLength ? unpackBlock(s, at, twoBpp) : { a: [0, 0, 0, 15], b: [0, 0, 0, 15], mode: 0, values: new Uint8Array(32) };
    }
  }
  const blockAt = (bx, by) => blocks[(by % blocksY + blocksY) % blocksY * blocksX + (bx % blocksX + blocksX) % blocksX];
  const rawValue = (x, y) => {
    const xx = (x % width + width) % width;
    const yy = (y % height + height) % height;
    return blockAt(Math.floor(xx / bw), Math.floor(yy / bh)).values[yy % bh * bw + xx % bw];
  };
  const modulation = (x, y, block) => {
    const v = block.values[y % bh * bw + x % bw];
    if (!twoBpp) {
      if (block.mode === 1) return { weight: WEIGHTS_PUNCHTHROUGH[v], punch: v === 2 };
      return { weight: WEIGHTS_STANDARD[v], punch: false };
    }
    if (block.mode === 0 || ((x ^ y) & 1) === 0) return { weight: WEIGHTS_STANDARD[v], punch: false };
    const left = WEIGHTS_STANDARD[rawValue(x - 1, y)];
    const right = WEIGHTS_STANDARD[rawValue(x + 1, y)];
    const up = WEIGHTS_STANDARD[rawValue(x, y - 1)];
    const down = WEIGHTS_STANDARD[rawValue(x, y + 1)];
    if (block.mode === 2) return { weight: left + right + 1 >> 1, punch: false };
    if (block.mode === 3) return { weight: up + down + 1 >> 1, punch: false };
    return { weight: left + right + up + down + 2 >> 2, punch: false };
  };
  const shift = twoBpp ? 5 : 4;
  const widen = (v, alpha) => alpha ? (v >> shift) + (v >> shift - 4) : (v >> shift + 2) + (v >> shift - 3);
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const px = x - (bw >> 1);
      const py = y - (bh >> 1);
      const bx = Math.floor(px / bw);
      const by = Math.floor(py / bh);
      const fx = px - bx * bw;
      const fy = py - by * bh;
      const p = blockAt(bx, by);
      const qb = blockAt(bx + 1, by);
      const r = blockAt(bx, by + 1);
      const sb = blockAt(bx + 1, by + 1);
      const { weight, punch } = modulation(x, y, blockAt(Math.floor(x / bw), Math.floor(y / bh)));
      const o = (y * width + x) * 4;
      for (let c2 = 0; c2 < 4; c2++) {
        const top = p.a[c2] * bw + fx * (qb.a[c2] - p.a[c2]);
        const bottom = r.a[c2] * bw + fx * (sb.a[c2] - r.a[c2]);
        const ca = widen(top * 4 + fy * (bottom - top), c2 === 3);
        const topB = p.b[c2] * bw + fx * (qb.b[c2] - p.b[c2]);
        const bottomB = r.b[c2] * bw + fx * (sb.b[c2] - r.b[c2]);
        const cb = widen(topB * 4 + fy * (bottomB - topB), c2 === 3);
        let v = ca * (8 - weight) + cb * weight >> 3;
        if (punch && c2 === 3) v = 0;
        values[o + c2] = v / 255;
      }
    }
  }
}

// src/renderer/vulkan/texture_decode.ts
var DEFAULT_DISPLAY = { channels: "rgb", exposure: 1, autoRange: false };
function halfToFloat2(h) {
  const s = h & 32768 ? -1 : 1;
  const e = h >> 10 & 31;
  const f = h & 1023;
  if (e === 0) return s * Math.pow(2, -14) * (f / 1024);
  if (e === 31) return f ? NaN : s * Infinity;
  return s * Math.pow(2, e - 15) * (1 + f / 1024);
}
function srgbEncode(v) {
  v = v < 0 ? 0 : v > 1 ? 1 : v;
  return v <= 31308e-7 ? v * 12.92 : 1.055 * Math.pow(v, 1 / 2.4) - 0.055;
}
var u8 = (s, t) => s.getUint8(t) / 255;
var s8 = (s, t) => Math.max(-1, s.getInt8(t) / 127);
var u16n = (s, t) => s.getUint16(t, true) / 65535;
var s16n = (s, t) => Math.max(-1, s.getInt16(t, true) / 32767);
var f16 = (s, t) => halfToFloat2(s.getUint16(t, true));
var f32 = (s, t) => s.getFloat32(t, true);
var u16 = (s, t) => s.getUint16(t, true);
var s16 = (s, t) => s.getInt16(t, true);
var u32 = (s, t) => s.getUint32(t, true);
var s32 = (s, t) => s.getInt32(t, true);
var s8i = (s, t) => s.getInt8(t);
var xr10 = (v) => (v - 384) / 510;
function smallFloat(bits, mantissaBits) {
  const e = bits >> mantissaBits & 31;
  const m = bits & (1 << mantissaBits) - 1;
  const scale = 1 << mantissaBits;
  return e === 0 ? Math.pow(2, -14) * (m / scale) : Math.pow(2, e - 15) * (1 + m / scale);
}
function ints(fn, size2, count2) {
  const order = [];
  for (let i = 0; i < count2; i++) order.push(i);
  return { bytes: size2 * count2, channels: count2, integer: true, read: ch(fn, size2, order) };
}
function ch(fn, size2, order) {
  return (s, t, out, o) => {
    for (let i = 0; i < order.length; i++) out[o + i] = fn(s, t + order[i] * size2);
  };
}
var FORMATS = {
  VK_FORMAT_A8_UNORM_KHR: { bytes: 1, channels: 1, names: ["A"], read: ch(u8, 1, [0]) },
  VK_FORMAT_R8G8_SINT: ints(s8i, 1, 2),
  VK_FORMAT_R8G8B8A8_SINT: ints(s8i, 1, 4),
  VK_FORMAT_R16_SINT: ints(s16, 2, 1),
  VK_FORMAT_R16G16_UINT: ints(u16, 2, 2),
  VK_FORMAT_R16G16_SINT: ints(s16, 2, 2),
  VK_FORMAT_R16G16B16A16_UINT: ints(u16, 2, 4),
  VK_FORMAT_R16G16B16A16_SINT: ints(s16, 2, 4),
  VK_FORMAT_R32G32_UINT: ints(u32, 4, 2),
  VK_FORMAT_R32G32_SINT: ints(s32, 4, 2),
  VK_FORMAT_R32G32B32A32_UINT: ints(u32, 4, 4),
  VK_FORMAT_R32G32B32A32_SINT: ints(s32, 4, 4),
  VK_FORMAT_A2B10G10R10_UINT_PACK32: { bytes: 4, channels: 4, integer: true, read: (s, t, out, o) => {
    const v = s.getUint32(t, true);
    out[o] = v & 1023;
    out[o + 1] = v >> 10 & 1023;
    out[o + 2] = v >> 20 & 1023;
    out[o + 3] = v >>> 30 & 3;
  } },
  VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: { bytes: 4, channels: 3, linear: true, read: (s, t, out, o) => {
    const v = s.getUint32(t, true);
    const scale = Math.pow(2, (v >>> 27 & 31) - 15 - 9);
    out[o] = (v & 511) * scale;
    out[o + 1] = (v >> 9 & 511) * scale;
    out[o + 2] = (v >> 18 & 511) * scale;
  } },
  VK_FORMAT_R5G5B5A1_UNORM_PACK16: { bytes: 2, channels: 4, read: (s, t, out, o) => {
    const v = s.getUint16(t, true);
    out[o] = (v >> 11 & 31) / 31;
    out[o + 1] = (v >> 6 & 31) / 31;
    out[o + 2] = (v >> 1 & 31) / 31;
    out[o + 3] = v & 1;
  } },
  VK_FORMAT_A1R5G5B5_UNORM_PACK16: { bytes: 2, channels: 4, read: (s, t, out, o) => {
    const v = s.getUint16(t, true);
    out[o] = (v >> 10 & 31) / 31;
    out[o + 1] = (v >> 5 & 31) / 31;
    out[o + 2] = (v & 31) / 31;
    out[o + 3] = v >> 15 & 1;
  } },
  VK_FORMAT_B5G5R5A1_UNORM_PACK16: { bytes: 2, channels: 4, read: (s, t, out, o) => {
    const v = s.getUint16(t, true);
    out[o + 2] = (v >> 11 & 31) / 31;
    out[o + 1] = (v >> 6 & 31) / 31;
    out[o] = (v >> 1 & 31) / 31;
    out[o + 3] = v & 1;
  } },
  VK_FORMAT_R4G4B4A4_UNORM_PACK16: { bytes: 2, channels: 4, read: (s, t, out, o) => {
    const v = s.getUint16(t, true);
    out[o] = (v >> 12 & 15) / 15;
    out[o + 1] = (v >> 8 & 15) / 15;
    out[o + 2] = (v >> 4 & 15) / 15;
    out[o + 3] = (v & 15) / 15;
  } },
  VK_FORMAT_B4G4R4A4_UNORM_PACK16: { bytes: 2, channels: 4, read: (s, t, out, o) => {
    const v = s.getUint16(t, true);
    out[o + 2] = (v >> 12 & 15) / 15;
    out[o + 1] = (v >> 8 & 15) / 15;
    out[o] = (v >> 4 & 15) / 15;
    out[o + 3] = (v & 15) / 15;
  } },
  VK_FORMAT_B5G6R5_UNORM_PACK16: { bytes: 2, channels: 3, read: (s, t, out, o) => {
    const v = s.getUint16(t, true);
    out[o + 2] = (v >> 11 & 31) / 31;
    out[o + 1] = (v >> 5 & 63) / 63;
    out[o] = (v & 31) / 31;
  } },
  // Metal's extended-range formats (no Vulkan spelling): 10 bits per channel, B lowest. The
  // 64-bit form keeps each channel's 10 bits in the low bits of a 16-bit word.
  MTLPixelFormatBGR10_XR: { bytes: 4, channels: 3, linear: true, read: (s, t, out, o) => {
    const v = s.getUint32(t, true);
    out[o + 2] = xr10(v & 1023);
    out[o + 1] = xr10(v >> 10 & 1023);
    out[o] = xr10(v >> 20 & 1023);
  } },
  MTLPixelFormatBGR10_XR_sRGB: { bytes: 4, channels: 3, read: (s, t, out, o) => {
    const v = s.getUint32(t, true);
    out[o + 2] = xr10(v & 1023);
    out[o + 1] = xr10(v >> 10 & 1023);
    out[o] = xr10(v >> 20 & 1023);
  } },
  MTLPixelFormatBGRA10_XR: { bytes: 8, channels: 4, linear: true, read: (s, t, out, o) => {
    out[o + 2] = xr10(s.getUint16(t, true) & 1023);
    out[o + 1] = xr10(s.getUint16(t + 2, true) & 1023);
    out[o] = xr10(s.getUint16(t + 4, true) & 1023);
    out[o + 3] = xr10(s.getUint16(t + 6, true) & 1023);
  } },
  MTLPixelFormatBGRA10_XR_sRGB: { bytes: 8, channels: 4, read: (s, t, out, o) => {
    out[o + 2] = xr10(s.getUint16(t, true) & 1023);
    out[o + 1] = xr10(s.getUint16(t + 2, true) & 1023);
    out[o] = xr10(s.getUint16(t + 4, true) & 1023);
    out[o + 3] = xr10(s.getUint16(t + 6, true) & 1023);
  } },
  VK_FORMAT_R8_UNORM: { bytes: 1, channels: 1, read: ch(u8, 1, [0]) },
  VK_FORMAT_R8_SRGB: { bytes: 1, channels: 1, read: ch(u8, 1, [0]) },
  VK_FORMAT_R8_SNORM: { bytes: 1, channels: 1, read: ch(s8, 1, [0]) },
  VK_FORMAT_R8_UINT: { bytes: 1, channels: 1, integer: true, read: (s, t, out, o) => {
    out[o] = s.getUint8(t);
  } },
  VK_FORMAT_R8_SINT: { bytes: 1, channels: 1, integer: true, read: (s, t, out, o) => {
    out[o] = s.getInt8(t);
  } },
  VK_FORMAT_R8G8_UNORM: { bytes: 2, channels: 2, read: ch(u8, 1, [0, 1]) },
  VK_FORMAT_R8G8_SRGB: { bytes: 2, channels: 2, read: ch(u8, 1, [0, 1]) },
  VK_FORMAT_R8G8_SNORM: { bytes: 2, channels: 2, read: ch(s8, 1, [0, 1]) },
  VK_FORMAT_R8G8_UINT: { bytes: 2, channels: 2, integer: true, read: (s, t, out, o) => {
    out[o] = s.getUint8(t);
    out[o + 1] = s.getUint8(t + 1);
  } },
  VK_FORMAT_R8G8B8_UNORM: { bytes: 3, channels: 3, read: ch(u8, 1, [0, 1, 2]) },
  VK_FORMAT_R8G8B8_SRGB: { bytes: 3, channels: 3, read: ch(u8, 1, [0, 1, 2]) },
  VK_FORMAT_B8G8R8_UNORM: { bytes: 3, channels: 3, read: ch(u8, 1, [2, 1, 0]) },
  VK_FORMAT_B8G8R8_SRGB: { bytes: 3, channels: 3, read: ch(u8, 1, [2, 1, 0]) },
  VK_FORMAT_R8G8B8A8_UNORM: { bytes: 4, channels: 4, read: ch(u8, 1, [0, 1, 2, 3]) },
  VK_FORMAT_R8G8B8A8_SRGB: { bytes: 4, channels: 4, read: ch(u8, 1, [0, 1, 2, 3]) },
  VK_FORMAT_R8G8B8A8_SNORM: { bytes: 4, channels: 4, read: ch(s8, 1, [0, 1, 2, 3]) },
  VK_FORMAT_R8G8B8A8_UINT: { bytes: 4, channels: 4, integer: true, read: (s, t, out, o) => {
    for (let i = 0; i < 4; i++) out[o + i] = s.getUint8(t + i);
  } },
  VK_FORMAT_B8G8R8A8_UNORM: { bytes: 4, channels: 4, read: ch(u8, 1, [2, 1, 0, 3]) },
  VK_FORMAT_B8G8R8A8_SRGB: { bytes: 4, channels: 4, read: ch(u8, 1, [2, 1, 0, 3]) },
  VK_FORMAT_A8B8G8R8_UNORM_PACK32: { bytes: 4, channels: 4, read: ch(u8, 1, [0, 1, 2, 3]) },
  VK_FORMAT_A8B8G8R8_SRGB_PACK32: { bytes: 4, channels: 4, read: ch(u8, 1, [0, 1, 2, 3]) },
  VK_FORMAT_R16_UNORM: { bytes: 2, channels: 1, read: ch(u16n, 2, [0]) },
  VK_FORMAT_R16_SNORM: { bytes: 2, channels: 1, read: ch(s16n, 2, [0]) },
  VK_FORMAT_R16_UINT: { bytes: 2, channels: 1, integer: true, read: (s, t, out, o) => {
    out[o] = s.getUint16(t, true);
  } },
  VK_FORMAT_R16_SFLOAT: { bytes: 2, channels: 1, linear: true, read: ch(f16, 2, [0]) },
  VK_FORMAT_R16G16_UNORM: { bytes: 4, channels: 2, read: ch(u16n, 2, [0, 1]) },
  VK_FORMAT_R16G16_SNORM: { bytes: 4, channels: 2, read: ch(s16n, 2, [0, 1]) },
  VK_FORMAT_R16G16_SFLOAT: { bytes: 4, channels: 2, linear: true, read: ch(f16, 2, [0, 1]) },
  VK_FORMAT_R16G16B16A16_UNORM: { bytes: 8, channels: 4, read: ch(u16n, 2, [0, 1, 2, 3]) },
  VK_FORMAT_R16G16B16A16_SNORM: { bytes: 8, channels: 4, read: ch(s16n, 2, [0, 1, 2, 3]) },
  VK_FORMAT_R16G16B16A16_SFLOAT: { bytes: 8, channels: 4, linear: true, read: ch(f16, 2, [0, 1, 2, 3]) },
  VK_FORMAT_R32_UINT: { bytes: 4, channels: 1, integer: true, read: (s, t, out, o) => {
    out[o] = s.getUint32(t, true);
  } },
  VK_FORMAT_R32_SINT: { bytes: 4, channels: 1, integer: true, read: (s, t, out, o) => {
    out[o] = s.getInt32(t, true);
  } },
  VK_FORMAT_R32_SFLOAT: { bytes: 4, channels: 1, linear: true, read: ch(f32, 4, [0]) },
  VK_FORMAT_R32G32_SFLOAT: { bytes: 8, channels: 2, linear: true, read: ch(f32, 4, [0, 1]) },
  VK_FORMAT_R32G32B32_SFLOAT: { bytes: 12, channels: 3, linear: true, read: ch(f32, 4, [0, 1, 2]) },
  VK_FORMAT_R32G32B32A32_SFLOAT: { bytes: 16, channels: 4, linear: true, read: ch(f32, 4, [0, 1, 2, 3]) },
  VK_FORMAT_B10G11R11_UFLOAT_PACK32: { bytes: 4, channels: 3, linear: true, read: (s, t, out, o) => {
    const v = s.getUint32(t, true);
    out[o] = smallFloat(v & 2047, 6);
    out[o + 1] = smallFloat(v >> 11 & 2047, 6);
    out[o + 2] = smallFloat(v >> 22 & 1023, 5);
  } },
  VK_FORMAT_A2B10G10R10_UNORM_PACK32: { bytes: 4, channels: 4, read: (s, t, out, o) => {
    const v = s.getUint32(t, true);
    out[o] = (v & 1023) / 1023;
    out[o + 1] = (v >> 10 & 1023) / 1023;
    out[o + 2] = (v >> 20 & 1023) / 1023;
    out[o + 3] = (v >>> 30 & 3) / 3;
  } },
  VK_FORMAT_A2R10G10B10_UNORM_PACK32: { bytes: 4, channels: 4, read: (s, t, out, o) => {
    const v = s.getUint32(t, true);
    out[o + 2] = (v & 1023) / 1023;
    out[o + 1] = (v >> 10 & 1023) / 1023;
    out[o] = (v >> 20 & 1023) / 1023;
    out[o + 3] = (v >>> 30 & 3) / 3;
  } },
  VK_FORMAT_R5G6B5_UNORM_PACK16: { bytes: 2, channels: 3, read: (s, t, out, o) => {
    const v = s.getUint16(t, true);
    out[o] = (v >> 11 & 31) / 31;
    out[o + 1] = (v >> 5 & 63) / 63;
    out[o + 2] = (v & 31) / 31;
  } }
};
var DEPTH_FORMATS = {
  VK_FORMAT_D16_UNORM: { bytes: 2, read: (s, t) => s.getUint16(t, true) / 65535 },
  VK_FORMAT_D16_UNORM_S8_UINT: { bytes: 2, read: (s, t) => s.getUint16(t, true) / 65535 },
  VK_FORMAT_X8_D24_UNORM_PACK32: { bytes: 4, read: (s, t) => (s.getUint32(t, true) & 16777215) / 16777215 },
  VK_FORMAT_D24_UNORM_S8_UINT: { bytes: 4, read: (s, t) => (s.getUint32(t, true) & 16777215) / 16777215 },
  VK_FORMAT_D32_SFLOAT: { bytes: 4, read: (s, t) => s.getFloat32(t, true) },
  VK_FORMAT_D32_SFLOAT_S8_UINT: { bytes: 4, read: (s, t) => s.getFloat32(t, true) }
};
function rgb565(v) {
  return [(v >> 11 & 31) / 31, (v >> 5 & 63) / 63, (v & 31) / 31];
}
function decodeBc1(s, t, px, alpha1Bit) {
  const c0 = s.getUint16(t, true);
  const c1 = s.getUint16(t + 2, true);
  const idx = s.getUint32(t + 4, true);
  const [r0, g0, b0] = rgb565(c0);
  const [r1, g1, b1] = rgb565(c1);
  const palette = [[r0, g0, b0, 1], [r1, g1, b1, 1]];
  if (c0 > c1 || !alpha1Bit) {
    palette.push([(2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3, 1]);
    palette.push([(r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3, 1]);
  } else {
    palette.push([(r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 1]);
    palette.push([0, 0, 0, 0]);
  }
  for (let i = 0; i < 16; i++) {
    const c2 = palette[idx >>> i * 2 & 3];
    px[i * 4] = c2[0];
    px[i * 4 + 1] = c2[1];
    px[i * 4 + 2] = c2[2];
    px[i * 4 + 3] = c2[3];
  }
}
function decodeBc4Channel(s, t, px, channel, signed) {
  const raw0 = s.getUint8(t);
  const raw1 = s.getUint8(t + 1);
  const toValue = signed ? (v) => Math.max(-127, v << 24 >> 24) / 127 : (v) => v / 255;
  const a0 = toValue(raw0);
  const a1 = toValue(raw1);
  const palette = [a0, a1];
  const gt = signed ? raw0 << 24 >> 24 > raw1 << 24 >> 24 : raw0 > raw1;
  if (gt) {
    for (let i = 1; i <= 6; i++) palette.push(((7 - i) * a0 + i * a1) / 7);
  } else {
    for (let i = 1; i <= 4; i++) palette.push(((5 - i) * a0 + i * a1) / 5);
    palette.push(signed ? -1 : 0, 1);
  }
  const lo = s.getUint32(t + 2, true);
  const hi = s.getUint16(t + 6, true);
  for (let i = 0; i < 16; i++) {
    const bit = i * 3;
    let v;
    if (bit + 3 <= 32) v = lo >>> bit & 7;
    else if (bit >= 32) v = hi >>> bit - 32 & 7;
    else v = (lo >>> bit | hi << 32 - bit) & 7;
    px[i * 4 + channel] = palette[v];
  }
}
var BC1 = { bytes: 8, width: 4, height: 4, channels: 4, decode: (s, b, px) => decodeBc1(s, b, px, true) };
var BC2 = { bytes: 16, width: 4, height: 4, channels: 4, decode: (s, b, px) => {
  decodeBc1(s, b + 8, px, false);
  for (let i = 0; i < 16; i++) px[i * 4 + 3] = (s.getUint16(b + (i >> 2) * 2, true) >> (i & 3) * 4 & 15) / 15;
} };
var BC3 = { bytes: 16, width: 4, height: 4, channels: 4, decode: (s, b, px) => {
  decodeBc1(s, b + 8, px, false);
  decodeBc4Channel(s, b, px, 3, false);
} };
var bc4 = (signed) => ({ bytes: 8, width: 4, height: 4, channels: 1, decode: (s, b, px) => {
  px.fill(0);
  decodeBc4Channel(s, b, px, 0, signed);
} });
var bc5 = (signed) => ({ bytes: 16, width: 4, height: 4, channels: 2, decode: (s, b, px) => {
  px.fill(0);
  decodeBc4Channel(s, b, px, 0, signed);
  decodeBc4Channel(s, b + 8, px, 1, signed);
} });
var BLOCK_FORMATS = {
  VK_FORMAT_BC1_RGB_UNORM_BLOCK: { ...BC1, channels: 3 },
  VK_FORMAT_BC1_RGB_SRGB_BLOCK: { ...BC1, channels: 3 },
  VK_FORMAT_BC1_RGBA_UNORM_BLOCK: BC1,
  VK_FORMAT_BC1_RGBA_SRGB_BLOCK: BC1,
  VK_FORMAT_BC2_UNORM_BLOCK: BC2,
  VK_FORMAT_BC2_SRGB_BLOCK: BC2,
  VK_FORMAT_BC3_UNORM_BLOCK: BC3,
  VK_FORMAT_BC3_SRGB_BLOCK: BC3,
  VK_FORMAT_BC4_UNORM_BLOCK: bc4(false),
  VK_FORMAT_BC4_SNORM_BLOCK: bc4(true),
  VK_FORMAT_BC5_UNORM_BLOCK: bc5(false),
  VK_FORMAT_BC5_SNORM_BLOCK: bc5(true),
  VK_FORMAT_BC6H_UFLOAT_BLOCK: { bytes: 16, width: 4, height: 4, channels: 3, linear: true, decode: (s, b, px) => decodeBc6hBlock(s, b, px, false) },
  VK_FORMAT_BC6H_SFLOAT_BLOCK: { bytes: 16, width: 4, height: 4, channels: 3, linear: true, decode: (s, b, px) => decodeBc6hBlock(s, b, px, true) },
  VK_FORMAT_BC7_UNORM_BLOCK: { bytes: 16, width: 4, height: 4, channels: 4, decode: decodeBc7Block },
  VK_FORMAT_BC7_SRGB_BLOCK: { bytes: 16, width: 4, height: 4, channels: 4, decode: decodeBc7Block },
  VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK: { bytes: 8, width: 4, height: 4, channels: 3, decode: decodeEtc2Rgb },
  VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK: { bytes: 8, width: 4, height: 4, channels: 3, decode: decodeEtc2Rgb },
  VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK: { bytes: 8, width: 4, height: 4, channels: 4, decode: decodeEtc2Rgba1 },
  VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK: { bytes: 8, width: 4, height: 4, channels: 4, decode: decodeEtc2Rgba1 },
  VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK: { bytes: 16, width: 4, height: 4, channels: 4, decode: decodeEtc2Rgba8 },
  VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK: { bytes: 16, width: 4, height: 4, channels: 4, decode: decodeEtc2Rgba8 },
  VK_FORMAT_EAC_R11_UNORM_BLOCK: { bytes: 8, width: 4, height: 4, channels: 1, decode: (s, b, px) => decodeEacR11(s, b, px, false) },
  VK_FORMAT_EAC_R11_SNORM_BLOCK: { bytes: 8, width: 4, height: 4, channels: 1, decode: (s, b, px) => decodeEacR11(s, b, px, true) },
  VK_FORMAT_EAC_R11G11_UNORM_BLOCK: { bytes: 16, width: 4, height: 4, channels: 2, decode: (s, b, px) => decodeEacRg11(s, b, px, false) },
  VK_FORMAT_EAC_R11G11_SNORM_BLOCK: { bytes: 16, width: 4, height: 4, channels: 2, decode: (s, b, px) => decodeEacRg11(s, b, px, true) },
  VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG: { bytes: 8, width: 8, height: 4, channels: 4, decodeImage: (s, w, h, v) => decodePvrtc(s, w, h, true, v) },
  VK_FORMAT_PVRTC1_2BPP_SRGB_BLOCK_IMG: { bytes: 8, width: 8, height: 4, channels: 4, decodeImage: (s, w, h, v) => decodePvrtc(s, w, h, true, v) },
  VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG: { bytes: 8, width: 4, height: 4, channels: 4, decodeImage: (s, w, h, v) => decodePvrtc(s, w, h, false, v) },
  VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG: { bytes: 8, width: 4, height: 4, channels: 4, decodeImage: (s, w, h, v) => decodePvrtc(s, w, h, false, v) },
  // Packed 4:2:2: two texels share their chroma. Shown as the stored Y, Cb and Cr values in
  // the G, B and R channels, the way Metal's sampler returns them, without a colour conversion.
  VK_FORMAT_G8B8G8R8_422_UNORM: { bytes: 4, width: 2, height: 1, channels: 3, names: ["R (Cr)", "G (Y)", "B (Cb)"], decode: (s, b, px) => {
    const g0 = s.getUint8(b) / 255, cb = s.getUint8(b + 1) / 255, g1 = s.getUint8(b + 2) / 255, cr = s.getUint8(b + 3) / 255;
    px[0] = cr;
    px[1] = g0;
    px[2] = cb;
    px[3] = 1;
    px[4] = cr;
    px[5] = g1;
    px[6] = cb;
    px[7] = 1;
  } },
  VK_FORMAT_B8G8R8G8_422_UNORM: { bytes: 4, width: 2, height: 1, channels: 3, names: ["R (Cr)", "G (Y)", "B (Cb)"], decode: (s, b, px) => {
    const cb = s.getUint8(b) / 255, g0 = s.getUint8(b + 1) / 255, cr = s.getUint8(b + 2) / 255, g1 = s.getUint8(b + 3) / 255;
    px[0] = cr;
    px[1] = g0;
    px[2] = cb;
    px[3] = 1;
    px[4] = cr;
    px[5] = g1;
    px[6] = cb;
    px[7] = 1;
  } }
};
for (const [w, h] of [[4, 4], [5, 4], [5, 5], [6, 5], [6, 6], [8, 5], [8, 6], [8, 8], [10, 5], [10, 6], [10, 8], [10, 10], [12, 10], [12, 12]]) {
  const ldr = (srgb) => ({ bytes: 16, width: w, height: h, channels: 4, decode: (s, b, px) => decodeAstcBlock(s, b, w, h, px, srgb) });
  BLOCK_FORMATS[`VK_FORMAT_ASTC_${w}x${h}_UNORM_BLOCK`] = ldr(false);
  BLOCK_FORMATS[`VK_FORMAT_ASTC_${w}x${h}_SRGB_BLOCK`] = ldr(true);
  BLOCK_FORMATS[`VK_FORMAT_ASTC_${w}x${h}_SFLOAT_BLOCK`] = { ...ldr(false), linear: true };
}
function sliceBytes(info) {
  const w = info.width;
  const h = info.height;
  if (info.aspect === "depth") return (DEPTH_FORMATS[info.format]?.bytes ?? 0) * w * h;
  if (info.aspect === "stencil") return w * h;
  const block = BLOCK_FORMATS[info.format];
  if (block) return Math.ceil(w / block.width) * Math.ceil(h / block.height) * block.bytes;
  return (FORMATS[info.format]?.bytes ?? 0) * w * h;
}
function isFormatSupported(info) {
  return sliceBytes(info) > 0;
}
function finish(tex) {
  const n = tex.width * tex.height;
  for (let c2 = 0; c2 < tex.channels; c2++) {
    let min = Infinity;
    let max = -Infinity;
    for (let i = 0; i < n; i++) {
      const v = tex.values[i * 4 + c2];
      if (v < min) min = v;
      if (v > max) max = v;
    }
    tex.min[c2] = min;
    tex.max[c2] = max;
  }
  return tex;
}
function decodeTexels(info, data, slice = 0) {
  const w = info.width;
  const h = info.height;
  const bytes = sliceBytes(info);
  if (!bytes) return null;
  const offset = slice * bytes;
  if (data.byteLength < offset + bytes) return null;
  const view = new DataView(data.buffer, data.byteOffset + offset, bytes);
  const n = w * h;
  const values = new Float32Array(n * 4);
  const tex = { width: w, height: h, channels: 4, linear: false, integer: false, names: ["R", "G", "B", "A"], values, min: [], max: [] };
  if (info.aspect === "depth") {
    const d = DEPTH_FORMATS[info.format];
    if (!d) return null;
    for (let i = 0; i < n; i++) values[i * 4] = d.read(view, i * d.bytes);
    tex.channels = 1;
    tex.linear = true;
    tex.names = ["D"];
    return finish(tex);
  }
  if (info.aspect === "stencil") {
    for (let i = 0; i < n; i++) values[i * 4] = view.getUint8(i);
    tex.channels = 1;
    tex.integer = true;
    tex.names = ["S"];
    return finish(tex);
  }
  const block = BLOCK_FORMATS[info.format];
  if (block) {
    tex.channels = block.channels;
    tex.linear = !!block.linear;
    if (block.names) tex.names = block.names;
    if (block.decodeImage) {
      block.decodeImage(view, w, h, values);
      return finish(tex);
    }
    const bw = Math.ceil(w / block.width);
    const bh = Math.ceil(h / block.height);
    const texels = block.width * block.height;
    const px = new Float32Array(texels * 4);
    for (let by = 0; by < bh; by++) {
      for (let bx = 0; bx < bw; bx++) {
        block.decode?.(view, (by * bw + bx) * block.bytes, px);
        for (let i = 0; i < texels; i++) {
          const x = bx * block.width + i % block.width;
          const y = by * block.height + Math.floor(i / block.width);
          if (x >= w || y >= h) continue;
          const o = (y * w + x) * 4;
          values[o] = px[i * 4];
          values[o + 1] = px[i * 4 + 1];
          values[o + 2] = px[i * 4 + 2];
          values[o + 3] = px[i * 4 + 3];
        }
      }
    }
    return finish(tex);
  }
  const f = FORMATS[info.format];
  if (!f) return null;
  for (let i = 0; i < n; i++) f.read(view, i * f.bytes, values, i * 4);
  tex.channels = f.channels;
  tex.linear = !!f.linear;
  tex.integer = !!f.integer;
  if (f.names) tex.names = f.names;
  return finish(tex);
}
function displayTexels(tex, display = DEFAULT_DISPLAY) {
  const n = tex.width * tex.height;
  const out = new Uint8ClampedArray(n * 4);
  const v = tex.values;
  const colorChannels = Math.min(tex.channels, 3);
  let lo = Infinity;
  let hi = -Infinity;
  for (let c2 = 0; c2 < colorChannels; c2++) {
    lo = Math.min(lo, tex.min[c2]);
    hi = Math.max(hi, tex.max[c2]);
  }
  const range = display.autoRange && hi - lo > 1e-5;
  const exposure = display.exposure;
  const encode = tex.linear ? srgbEncode : (x) => x;
  const mode = display.channels;
  for (let i = 0; i < n; i++) {
    const o = i * 4;
    let r = v[o];
    let g = v[o + 1];
    let b = v[o + 2];
    let a = v[o + 3];
    if (range) {
      r = (r - lo) / (hi - lo);
      g = (g - lo) / (hi - lo);
      b = (b - lo) / (hi - lo);
    }
    if (tex.channels === 1) {
      g = r;
      b = r;
      a = 1;
    } else if (tex.channels === 2) {
      b = 0;
      a = 1;
    } else if (tex.channels === 3) a = 1;
    let dr;
    let dg;
    let db;
    switch (mode) {
      case "r":
        dr = r * exposure;
        dg = 0;
        db = 0;
        break;
      case "g":
        dr = 0;
        dg = g * exposure;
        db = 0;
        break;
      case "b":
        dr = 0;
        dg = 0;
        db = b * exposure;
        break;
      case "a":
        dr = dg = db = a * exposure;
        break;
      case "luminance":
        dr = dg = db = (0.2126 * r + 0.7152 * g + 0.0722 * b) * exposure;
        break;
      default:
        dr = r * exposure;
        dg = g * exposure;
        db = b * exposure;
        break;
    }
    out[o] = encode(dr) * 255;
    out[o + 1] = encode(dg) * 255;
    out[o + 2] = encode(db) * 255;
    out[o + 3] = mode === "rgb" ? a * 255 : 255;
  }
  return out;
}

// src/main/replay.ts
import { spawn as spawn3 } from "node:child_process";
import fs10 from "node:fs";
import os7 from "node:os";
import path9 from "node:path";
var REPLAY_TOOL = process.platform === "win32" ? "vkinsp_replay.exe" : "vkinsp_replay";
function findReplayTool(roots, layerDirs) {
  const candidates = [
    process.env.INSPECTOR_REPLAY,
    ...roots.flatMap((root) => ["Release", "RelWithDebInfo", "Debug", ""].map((config) => path9.join(root, "build", "bin", config, REPLAY_TOOL))),
    ...layerDirs.map((dir) => path9.join(dir, REPLAY_TOOL))
  ].filter((f) => !!f);
  return candidates.find((f) => fs10.existsSync(f)) ?? null;
}
var NO_REPLAY_TOOL = `${REPLAY_TOOL} not found. Build it (cmake --build build --target vkinsp_replay), or set INSPECTOR_REPLAY to its path.`;
function tail(text, lines = 12) {
  return text.trim().split(/\r?\n/).slice(-lines).join("\n");
}
function analysisArgs(analysis, out) {
  if (analysis.kind === "overdraw") return ["--overdraw-data", out];
  if (analysis.kind === "draws") return ["--draw-data", out];
  if (analysis.kind === "overlay" || analysis.kind === "mesh") {
    const flag = `--${analysis.kind}`;
    return [...analysis.commands.flatMap((c2) => [flag, String(Math.max(0, Math.floor(c2)))]), `${flag}-data`, out];
  }
  const n = (v) => String(Math.max(0, Math.floor(v ?? 0)));
  return ["--pixel", n(analysis.image), n(analysis.x), n(analysis.y), "--mip", n(analysis.mip), "--layer", n(analysis.layer), "--pixel-data", out];
}
function runReplay(tool, capturePath, analysis, timeoutMs = 10 * 60 * 1e3) {
  return new Promise((resolve) => {
    const out = path9.join(os7.tmpdir(), `vkinsp_${analysis.kind}_${process.pid}_${Date.now()}_${Math.random().toString(36).slice(2)}.bin`);
    let output = "";
    let done = false;
    let timedOut = false;
    const child = spawn3(tool, [capturePath, ...analysisArgs(analysis, out)], { stdio: ["ignore", "pipe", "pipe"], windowsHide: true });
    const timer = setTimeout(() => {
      timedOut = true;
      child.kill();
    }, timeoutMs);
    const collect = (chunk2) => {
      output += chunk2.toString();
      if (output.length > 256 * 1024) output = output.slice(-128 * 1024);
    };
    child.stdout?.on("data", collect);
    child.stderr?.on("data", collect);
    const finish2 = (error) => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      let data = null;
      try {
        data = new Uint8Array(fs10.readFileSync(out));
        fs10.unlinkSync(out);
      } catch {
      }
      if (data) {
        resolve({ data, output: tail(output) });
        return;
      }
      resolve({
        data: null,
        output: tail(output),
        error: error ?? (timedOut ? `the replay did not finish within ${Math.round(timeoutMs / 1e3)} s` : `the replay wrote no data:
${tail(output)}`)
      });
    };
    child.on("error", (e) => finish2(`could not run ${tool}: ${e.message}`));
    child.on("close", () => finish2(null));
  });
}
function serveRequest(id, analysis, out) {
  if (analysis.kind === "pixel") {
    return { id, kind: "pixel", image: analysis.image, x: analysis.x, y: analysis.y, mip: analysis.mip ?? 0, layer: analysis.layer ?? 0, out };
  }
  return { id, ...analysis, out };
}
function tempOutput(kind) {
  return path9.join(os7.tmpdir(), `vkinsp_${kind}_${process.pid}_${Date.now()}_${Math.random().toString(36).slice(2)}.bin`);
}
var ReplayServer = class {
  tool;
  capturePath;
  lastUsed = Date.now();
  _child;
  _ready;
  _resolveReady = () => {
  };
  _pending = /* @__PURE__ */ new Map();
  _nextId = 1;
  _output = "";
  _exited = false;
  constructor(tool, capturePath) {
    this.tool = tool;
    this.capturePath = capturePath;
    this._ready = new Promise((resolve) => {
      this._resolveReady = resolve;
    });
    this._child = spawn3(tool, [capturePath, "--serve"], { stdio: ["pipe", "pipe", "pipe"], windowsHide: true });
    let buffered = "";
    this._child.stdout?.on("data", (chunk2) => {
      buffered += chunk2.toString();
      let newline;
      while ((newline = buffered.indexOf("\n")) >= 0) {
        const line = buffered.slice(0, newline).replace(/\r$/, "");
        buffered = buffered.slice(newline + 1);
        this._line(line);
      }
    });
    this._child.stderr?.on("data", (chunk2) => this._collect(chunk2.toString()));
    this._child.stdin?.on("error", () => {
    });
    this._child.on("error", (e) => this._exit(`could not run ${tool}: ${e.message}`));
    this._child.on("exit", (code) => this._exit(`the replay process exited (${code ?? "killed"})`));
  }
  get alive() {
    return !this._exited;
  }
  /**
   * Replays the frame for an analysis and reads the file it wrote. `fallback` is set when the answer
   * is not the analysis's: the process could not start (a tool from before --serve) or exited.
   */
  async run(analysis, timeoutMs = 10 * 60 * 1e3) {
    this.lastUsed = Date.now();
    const startError = await this._ready;
    if (startError) return { data: null, output: tail(this._output), error: startError, fallback: true };
    const id = this._nextId++;
    const out = tempOutput(analysis.kind);
    const answer = await new Promise((resolve) => {
      const timer = setTimeout(() => {
        this._pending.delete(id);
        this.dispose();
        resolve({ id, ok: false, error: `the replay did not finish within ${Math.round(timeoutMs / 1e3)} s` });
      }, timeoutMs);
      this._pending.set(id, (a) => {
        clearTimeout(timer);
        resolve(a);
      });
      this._child.stdin?.write(JSON.stringify(serveRequest(id, analysis, out)) + "\n");
    });
    this.lastUsed = Date.now();
    let data = null;
    try {
      data = new Uint8Array(fs10.readFileSync(out));
      fs10.unlinkSync(out);
    } catch {
    }
    if (answer.ok && data) return { data, output: tail(this._output) };
    return { data: null, output: tail(this._output), error: answer.error ?? "the replay wrote no data", fallback: this._exited };
  }
  /** Stops the process: asked to quit, and killed if it does not. */
  dispose() {
    if (this._exited) return;
    try {
      this._child.stdin?.write(JSON.stringify({ kind: "quit" }) + "\n");
      this._child.stdin?.end();
    } catch {
    }
    setTimeout(() => {
      if (!this._exited) this._child.kill();
    }, 2e3).unref();
  }
  _line(line) {
    if (!line.startsWith("@replay ")) {
      this._collect(line + "\n");
      return;
    }
    let answer;
    try {
      answer = JSON.parse(line.slice(8));
    } catch {
      return;
    }
    if (answer.ready !== void 0) {
      this._resolveReady(answer.ready ? null : answer.error ?? "the replay could not start");
      return;
    }
    if (answer.id === void 0) return;
    const resolve = this._pending.get(answer.id);
    this._pending.delete(answer.id);
    resolve?.(answer);
  }
  _collect(text) {
    this._output += text;
    if (this._output.length > 256 * 1024) this._output = this._output.slice(-128 * 1024);
  }
  _exit(message) {
    if (this._exited) return;
    this._exited = true;
    this._resolveReady(message);
    for (const [id, resolve] of this._pending) resolve({ id, ok: false, error: message });
    this._pending.clear();
  }
};
var ReplayServerPool = class {
  constructor(max = 3, idleMs = 5 * 60 * 1e3) {
    this.max = max;
    this.idleMs = idleMs;
  }
  _servers = /* @__PURE__ */ new Map();
  _sweep = null;
  /** Runs an analysis in the capture's replay, starting one if needed; a process that cannot serve falls back to a one-shot replay. */
  async run(tool, capturePath, analysis, timeoutMs) {
    let stamp = 0;
    try {
      stamp = fs10.statSync(capturePath).mtimeMs;
    } catch {
      return { data: null, output: "", error: `${capturePath} does not exist` };
    }
    const key = `${tool}
${path9.resolve(capturePath)}
${stamp}`;
    let server = this._servers.get(key);
    if (!server || !server.alive) {
      server?.dispose();
      server = new ReplayServer(tool, capturePath);
      this._servers.set(key, server);
      this._trim();
    }
    this._scheduleSweep();
    const result = await server.run(analysis, timeoutMs);
    if (!server.alive) this._servers.delete(key);
    if (result.fallback) return runReplay(tool, capturePath, analysis, timeoutMs);
    return result;
  }
  /** Stops the replays of a capture file (it is closed, or about to be deleted). */
  release(capturePath) {
    const resolved = path9.resolve(capturePath);
    for (const [key, server] of this._servers) {
      if (path9.resolve(server.capturePath) !== resolved) continue;
      server.dispose();
      this._servers.delete(key);
    }
  }
  disposeAll() {
    for (const server of this._servers.values()) server.dispose();
    this._servers.clear();
  }
  _trim() {
    const live = [...this._servers.entries()].sort((a, b) => a[1].lastUsed - b[1].lastUsed);
    while (live.length > this.max) {
      const [key, server] = live.shift();
      server.dispose();
      this._servers.delete(key);
    }
  }
  _scheduleSweep() {
    if (this._sweep) return;
    this._sweep = setInterval(() => {
      const now = Date.now();
      for (const [key, server] of this._servers) {
        if (now - server.lastUsed < this.idleMs) continue;
        server.dispose();
        this._servers.delete(key);
      }
      if (!this._servers.size && this._sweep) {
        clearInterval(this._sweep);
        this._sweep = null;
      }
    }, 30 * 1e3);
    this._sweep.unref();
  }
};
var replayServers = new ReplayServerPool();

// src/renderer/frame_cost_tree.ts
var MAX_LINE_FRAMES = 16;
function node(kind, name, totalCost = 0, children = []) {
  return { kind, name, totalCost, selfCost: 0, children };
}
function rollup(n) {
  let total = 0;
  for (const c2 of n.children) {
    total += c2.totalCost;
    n.estimated = n.estimated || c2.estimated;
  }
  n.totalCost = total;
  return n;
}
function scaleSubtree(n, factor) {
  n.totalCost *= factor;
  n.selfCost *= factor;
  for (const c2 of n.children) scaleSubtree(c2, factor);
}
function readU32(data, offset) {
  if (offset + 4 > data.byteLength) return null;
  return new DataView(data.buffer, data.byteOffset, data.byteLength).getUint32(offset, true);
}
function drawInvocations(cmd, data) {
  const a = cmd.args;
  if (!a) return null;
  switch (cmd.method) {
    case "vkCmdDraw":
      return num(a.vertexCount) * Math.max(1, num(a.instanceCount));
    case "vkCmdDrawIndexed":
      return num(a.indexCount) * Math.max(1, num(a.instanceCount));
    case "vkCmdDrawIndirect":
    case "vkCmdDrawIndexedIndirect": {
      const captured = data.buffer(cmd.bufferData?.[0]);
      if (!captured || !captured.data) return null;
      const count2 = Math.max(0, num(a.drawCount));
      const stride = Math.max(16, num(a.stride));
      let total = 0;
      for (let i = 0; i < count2; i++) {
        const n = readU32(captured.data, i * stride);
        const inst = readU32(captured.data, i * stride + 4);
        if (n === null || inst === null) return null;
        total += n * Math.max(1, inst);
      }
      return total;
    }
    default:
      return null;
  }
}
function dispatchGroups(cmd, data) {
  const a = cmd.args;
  if (!a) return null;
  switch (cmd.method) {
    case "vkCmdDispatch":
    case "vkCmdDispatchBase":
      return num(a.groupCountX) * num(a.groupCountY) * num(a.groupCountZ);
    case "vkCmdDispatchIndirect": {
      const captured = data.buffer(cmd.bufferData?.[0]);
      if (!captured || !captured.data) return null;
      const x = readU32(captured.data, 0), y = readU32(captured.data, 4), z = readU32(captured.data, 8);
      return x === null || y === null || z === null ? null : x * y * z;
    }
    default:
      return null;
  }
}
function rectArea(v) {
  if (!isObject(v)) return null;
  const extent = isObject(v.extent) ? v.extent : null;
  if (!extent) return null;
  const w = num(extent.width), h = num(extent.height);
  return w > 0 && h > 0 ? w * h : null;
}
function collectPasses2(o) {
  const { data, models } = o;
  const drawStats = drawStatsByCommand(data.drawStats ?? []);
  const sets = data.sets;
  const passes = [];
  const notes = [];
  let missingModels = 0;
  for (let frame = 0; frame < Math.max(1, data.frames); frame++) {
    const commands = data.commandsForFrame(frame);
    const passCounters = /* @__PURE__ */ new Map();
    const computeCounters = /* @__PURE__ */ new Map();
    const bound = /* @__PURE__ */ new Map();
    const scissor = /* @__PURE__ */ new Map();
    let currentCb = -1;
    let currentSecondary = 0;
    let renderPass = null;
    let compute = null;
    const closeCompute = () => {
      compute = null;
    };
    const closeSecondary = () => {
      closeCompute();
      currentSecondary = 0;
    };
    const closeCommandBuffer = () => {
      closeSecondary();
      currentCb = -1;
      renderPass = null;
    };
    for (const cmd of commands) {
      if (!cmd) continue;
      const objId = cmd.object?.__id ?? 0;
      if ((cmd.secondary ?? 0) !== currentSecondary) {
        closeSecondary();
        currentSecondary = cmd.secondary ?? 0;
      }
      if (sets.SUBMIT.has(cmd.method)) {
        closeCommandBuffer();
        continue;
      }
      if (objId !== currentCb) {
        closeCommandBuffer();
        currentCb = objId;
        if (cmd.method.startsWith("<")) continue;
      }
      const stream = `${objId}:${cmd.secondary ?? 0}`;
      const a = cmd.args;
      if (sets.PASS_BEGIN.has(cmd.method)) {
        closeCompute();
        const index = passCounters.get(objId) ?? 0;
        passCounters.set(objId, index + 1);
        const key = passKey(frame, objId, index);
        const timing = data.passTimings.get(key);
        let area = null;
        if (a && isObject(a.pRenderPassBegin)) area = rectArea(a.pRenderPassBegin.renderArea);
        else if (a && isObject(a.pRenderingInfo)) area = rectArea(a.pRenderingInfo.renderArea);
        const fragments = timing?.counters?.fragmentInvocations;
        renderPass = {
          key,
          kind: "render",
          label: `Render Pass ${index}`,
          command: cmd,
          items: [],
          durationMs: timing ? timing.durationMs : null,
          area,
          measuredFragments: typeof fragments === "number" && fragments > 0 ? fragments : null
        };
        passes.push(renderPass);
        continue;
      }
      if (sets.PASS_END.has(cmd.method)) {
        renderPass = null;
        continue;
      }
      if (sets.COMPUTE_PASS_END.has(cmd.method) || cmd.method === "vkEndCommandBuffer" || sets.LABEL_BEGIN.has(cmd.method) || sets.LABEL_END.has(cmd.method)) closeCompute();
      if (sets.BIND_PIPELINE.has(cmd.method) && a) {
        const id = refId(a.pipeline);
        if (id !== null) bound.set(`${stream}:${sets.pipelineBindPointOf(cmd.method, a)}`, id);
        continue;
      }
      if ((cmd.method === "vkCmdSetScissor" || cmd.method === "vkCmdSetScissorWithCount" || cmd.method === "vkCmdSetScissorWithCountEXT") && a) {
        const rects = a.pScissors;
        scissor.set(stream, Array.isArray(rects) && rects.length ? rectArea(rects[0]) : null);
        continue;
      }
      if (!isAction(sets, cmd.method)) continue;
      const isDispatch = sets.DISPATCH.has(cmd.method);
      const pipelineId = bound.get(`${stream}:${sets.bindPointOf(cmd.method)}`);
      if (pipelineId === void 0) continue;
      let pass;
      if (isDispatch && !renderPass) {
        if (!compute) {
          const index = computeCounters.get(objId) ?? 0;
          computeCounters.set(objId, index + 1);
          const key = passKey(frame, objId, index, true);
          const timing = data.passTimings.get(key);
          compute = {
            key,
            kind: "compute",
            label: `Compute ${index}`,
            command: cmd,
            items: [],
            durationMs: timing ? timing.durationMs : null,
            area: null,
            measuredFragments: null
          };
          passes.push(compute);
        }
        pass = compute;
      } else {
        pass = renderPass;
      }
      if (!pass) continue;
      const stageModels2 = models.get(pipelineId);
      if (!stageModels2) {
        missingModels++;
        continue;
      }
      const stages = [];
      let drawArea = null;
      const measured = drawStats.get(cmd.index);
      const counted = measured?.counted === true;
      if (isDispatch) {
        const groups = dispatchGroups(cmd, data);
        for (const m of stageModels2) {
          const wg = m.workgroupSize ? m.workgroupSize[0] * m.workgroupSize[1] * m.workgroupSize[2] : null;
          const fromArgs = groups !== null && wg !== null ? groups * wg : null;
          const inv = counted && measured.computeInvocations > 0 ? measured.computeInvocations : fromArgs;
          stages.push({ model: m, invocations: inv, confidence: inv === null ? "unknown" : "exact" });
        }
      } else {
        const vertices = drawInvocations(cmd, data);
        const scissorArea = scissor.get(stream);
        drawArea = scissorArea != null && pass.area != null ? Math.min(scissorArea, pass.area) : scissorArea ?? pass.area ?? null;
        const fragmentArea = o.estimateFragments ? drawArea : null;
        for (const m of stageModels2) {
          if (m.stage === "fragment") {
            if (counted) stages.push({ model: m, invocations: measured.fragmentInvocations, confidence: "exact" });
            else stages.push({ model: m, invocations: fragmentArea, confidence: fragmentArea === null ? "unknown" : "estimated" });
          } else if (m.stage === "vertex") {
            const inv = counted && measured.vertexInvocations > 0 ? measured.vertexInvocations : vertices;
            stages.push({ model: m, invocations: inv, confidence: inv === null ? "unknown" : "exact" });
          } else {
            stages.push({ model: m, invocations: vertices, confidence: vertices === null ? "unknown" : "estimated" });
          }
        }
      }
      pass.items.push({
        command: cmd,
        pipelineId,
        kind: isDispatch ? "dispatch" : "draw",
        stages,
        area: drawArea,
        ms: measured?.timed ? measured.ms : null
      });
    }
  }
  if (missingModels) notes.push(`${missingModels} draw(s) or dispatch(es) use a pipeline whose shaders could not be fetched and are left out.`);
  let measuredFragmentPasses = 0;
  for (const pass of passes) {
    const measured = pass.measuredFragments;
    if (measured === null) continue;
    const fragments = [];
    for (const item of pass.items) {
      for (const stage of item.stages) if (stage.model.stage === "fragment") fragments.push({ stage, area: item.area });
    }
    if (!fragments.length) continue;
    if (fragments.some((f) => f.stage.confidence === "exact")) continue;
    const totalArea = fragments.reduce((sum, f) => sum + (f.area ?? 0), 0);
    for (const f of fragments) {
      const share = totalArea > 0 ? (f.area ?? 0) / totalArea : 1 / fragments.length;
      f.stage.invocations = measured * share;
      f.stage.confidence = fragments.length === 1 ? "exact" : "estimated";
    }
    measuredFragmentPasses++;
  }
  return { passes, notes, measuredFragmentPasses };
}
function entryOf(model) {
  const a = model.analysis;
  if (!a) return null;
  return a.entryPoints.find((e) => e.name === model.entryPoint && e.stage === model.stage) ?? a.entryPoints.find((e) => e.name === model.entryPoint) ?? a.entryPoints.find((e) => e.stage === model.stage) ?? null;
}
function functionTree(fn, byId, factor, path11, depth) {
  const n = node("function", fn.name || `function ${fn.id}`);
  n.totalCost = weighCost(fn.inclusive) * factor;
  n.selfCost = weighCost(fn.cost) * factor;
  n.dimension = dominantDimension(fn.inclusive);
  if (depth < 24) {
    path11.add(fn.id);
    for (const calleeId of fn.calls) {
      const callee = byId.get(calleeId);
      if (!callee || path11.has(calleeId)) continue;
      n.children.push(functionTree(callee, byId, factor, path11, depth + 1));
    }
    path11.delete(fn.id);
  }
  if (fn.lines.length) {
    const shown = fn.lines.slice(0, MAX_LINE_FRAMES);
    for (const l of shown) {
      const ln = node("line", `${l.file ? `${l.file}:` : "line "}${l.line}`, l.weighted * factor);
      ln.selfCost = ln.totalCost;
      ln.dimension = l.dominant;
      ln.line = l.line;
      ln.file = l.file;
      n.children.push(ln);
    }
    const rest = fn.lines.slice(MAX_LINE_FRAMES).reduce((acc, l) => acc + l.weighted, 0);
    if (rest > 0) n.children.push(node("more", `+ ${fn.lines.length - MAX_LINE_FRAMES} more lines`, rest * factor));
  }
  const sum = n.children.reduce((s, c2) => s + c2.totalCost, 0);
  if (sum > n.totalCost && sum > 0) for (const c2 of n.children) scaleSubtree(c2, n.totalCost / sum);
  return n;
}
function buildFrameCostTree(o) {
  const { db } = o;
  const maxFramesPerPass = o.maxFramesPerPass ?? 32;
  const { passes, notes, measuredFragmentPasses } = collectPasses2(o);
  const stats = { passes: passes.length, items: 0, unknownStages: 0, estimatedStages: 0, collapsed: 0, measuredFragmentPasses, measuredDrawPasses: 0 };
  const measured = passes.filter((p) => p.durationMs !== null && p.durationMs > 0);
  const allMeasured = passes.length > 0 && measured.length === passes.length;
  const units = allMeasured ? "ms" : "ops";
  if (!allMeasured && measured.length > 0) {
    notes.push(`Only ${measured.length} of ${passes.length} passes have GPU timings, so the graph is in modeled op units rather than milliseconds.`);
  } else if (!allMeasured && passes.length > 0) {
    notes.push('No GPU pass timings in this capture, so the graph is in modeled op units. Capture with "Profile passes" to scale it to measured milliseconds.');
  }
  const passNodes = [];
  for (const pass of passes) {
    stats.items += pass.items.length;
    const buckets = /* @__PURE__ */ new Map();
    for (const item of pass.items) {
      const key = o.perDraw ? `d${item.command.index}` : `p${item.pipelineId}`;
      let b = buckets.get(key);
      if (!b) {
        b = { key, pipelineId: item.pipelineId, items: [] };
        buckets.set(key, b);
      }
      b.items.push(item);
    }
    const resolved = [];
    for (const bucket of buckets.values()) {
      const totals = /* @__PURE__ */ new Map();
      for (const item of bucket.items) {
        for (const s of item.stages) {
          const key = `${s.model.stage}:${s.model.objectId}:${s.model.entryPoint}`;
          let acc = totals.get(key);
          if (!acc) {
            acc = { model: s.model, invocations: 0, confidence: s.confidence, unknown: false };
            totals.set(key, acc);
          }
          if (s.invocations === null) acc.unknown = true;
          else {
            acc.invocations += s.invocations;
            if (s.confidence === "estimated") acc.confidence = "estimated";
          }
        }
      }
      const stages = [];
      let cost = 0;
      for (const acc of totals.values()) {
        const entry = entryOf(acc.model);
        const usable = !!entry && !acc.unknown && acc.invocations > 0;
        if (!usable) stats.unknownStages++;
        else if (acc.confidence === "estimated") stats.estimatedStages++;
        const c2 = usable ? weighCost(entry.cost) * acc.invocations : 0;
        cost += c2;
        stages.push({ ...acc, cost: c2 });
      }
      resolved.push({ bucket, stages, cost });
    }
    const bucketMs = (items) => items.reduce((sum, i) => sum + (i.ms ?? 0), 0);
    const timedItems = resolved.length > 0 && resolved.every((r) => r.bucket.items.every((i) => i.ms !== null));
    if (timedItems) stats.measuredDrawPasses++;
    let kept = resolved;
    let collapsed = null;
    if (resolved.length > maxFramesPerPass) {
      const sorted = resolved.slice().sort((x, y) => y.cost - x.cost);
      kept = sorted.slice(0, maxFramesPerPass);
      const tail2 = sorted.slice(maxFramesPerPass);
      collapsed = {
        count: tail2.length,
        draws: tail2.reduce((s, r) => s + r.bucket.items.length, 0),
        cost: timedItems ? tail2.reduce((s, r) => s + bucketMs(r.bucket.items), 0) : tail2.reduce((s, r) => s + r.cost, 0)
      };
      stats.collapsed += tail2.length;
    }
    const itemNodes = [];
    for (const { bucket, stages } of kept) {
      const stageNodes = [];
      for (const s of stages) {
        const label = `${s.model.stage}: ${s.model.entryPoint}`;
        const entry = entryOf(s.model);
        if (!entry || s.unknown || s.invocations <= 0) {
          const reason = !entry ? s.model.analysis ? "entry point not found" : "not analyzable" : "invocation count unknown";
          const n2 = node("stage", `${label} (${reason})`);
          n2.estimated = true;
          n2.reason = reason;
          n2.objectId = s.model.objectId;
          n2.stage = s.model.stage;
          n2.entryPoint = s.model.entryPoint;
          stageNodes.push(n2);
          continue;
        }
        const byId = new Map(s.model.analysis.functions.map((f) => [f.id, f]));
        const root2 = byId.get(entry.functionId);
        const suffix = s.confidence === "estimated" ? " estimated" : "";
        const n = node("stage", `${label}: ${s.invocations.toLocaleString()}${suffix} invocations`, s.cost);
        n.dimension = entry.dominant;
        n.invocations = s.invocations;
        n.confidence = s.confidence;
        n.estimated = s.confidence !== "exact";
        n.objectId = s.model.objectId;
        n.stage = s.model.stage;
        n.entryPoint = s.model.entryPoint;
        n.command = bucket.items[0].command;
        if (root2) {
          const tree = functionTree(root2, byId, s.invocations, /* @__PURE__ */ new Set(), 0);
          const tag = (c2) => {
            c2.objectId = s.model.objectId;
            c2.stage = s.model.stage;
            for (const cc of c2.children) tag(cc);
          };
          for (const c2 of tree.children) tag(c2);
          n.children = tree.children;
          n.selfCost = tree.selfCost;
          const sum = n.children.reduce((acc, c2) => acc + c2.totalCost, 0);
          if (sum > n.totalCost && sum > 0) for (const c2 of n.children) scaleSubtree(c2, n.totalCost / sum);
        }
        stageNodes.push(n);
      }
      const first = bucket.items[0];
      const pipeline = db.getObject(bucket.pipelineId);
      const count2 = bucket.items.length;
      const noun = first.kind === "draw" ? count2 === 1 ? "draw" : "draws" : count2 === 1 ? "dispatch" : "dispatches";
      const name = o.perDraw ? `${first.command.method.replace(/^vkCmd/, "")} #${first.command.index}${pipeline ? ` (${pipeline.name})` : ""}` : `${pipeline ? pipeline.name : `Pipeline ${bucket.pipelineId}`}: ${count2} ${noun}`;
      const itemNode = rollup(node("item", name, 0, stageNodes));
      itemNode.command = first.command;
      itemNode.objectId = bucket.pipelineId;
      if (timedItems) {
        const ms = bucketMs(bucket.items);
        const modeled = itemNode.totalCost;
        if (modeled > 0) scaleSubtree(itemNode, ms / modeled);
        else itemNode.totalCost = ms;
        itemNode.durationMs = ms;
      }
      itemNodes.push(itemNode);
    }
    if (collapsed) {
      const label = o.perDraw ? `+ ${collapsed.count} more draws` : `+ ${collapsed.count} more pipelines (${collapsed.draws} draws)`;
      itemNodes.push(node("more", label, collapsed.cost));
    }
    const passNode = rollup(node("pass", pass.label, 0, itemNodes));
    passNode.command = pass.command ?? void 0;
    passNode.durationMs = pass.durationMs;
    if (units === "ms") {
      const modeled = passNode.totalCost;
      if (modeled > 0) scaleSubtree(passNode, pass.durationMs / modeled);
      else {
        passNode.children = [];
        passNode.estimated = true;
      }
      passNode.totalCost = pass.durationMs;
    }
    passNodes.push(passNode);
  }
  const root = rollup(node("frame", "Frame", 0, passNodes));
  if (units === "ms") root.name = `Frame: ${root.totalCost.toFixed(2)} ms GPU`;
  if (stats.unknownStages > 0) notes.push(`${stats.unknownStages} shader stage(s) have no invocation count or no analysis and are shown unweighted (zero width).`);
  if (stats.measuredDrawPasses > 0) {
    notes.push(`The draws of ${stats.measuredDrawPasses} pass(es) were timed one at a time by replaying the frame, and those times set how each pass's measured duration is split between them. A draw's time overlaps its neighbours' on the GPU, so it is a share of the pass rather than what the draw costs alone.`);
  }
  if (stats.measuredFragmentPasses > 0) {
    notes.push(`Fragment stages in ${stats.measuredFragmentPasses} pass(es) are weighted by the fragment shader invocations the capture's GPU counters measured; a pass that draws more than once splits its measured total between its draws by scissor area.`);
  }
  if (stats.estimatedStages > 0) notes.push(`Fragment stages without measured counters are weighted by the scissor (or render) area: an upper bound without overdraw and before the depth test, so the split between vertex and fragment work is an estimate.`);
  else if (o.estimateFragments === false && stats.measuredFragmentPasses === 0) notes.push("Fragment stages are unweighted: only rasterization knows their invocation counts. Enable the scissor-area estimate to weight them.");
  if (stats.collapsed > 0) notes.push(`${stats.collapsed} lower-cost ${o.perDraw ? "draw" : "pipeline"} group(s) are collapsed into "+ more" frames (the ${maxFramesPerPass} costliest per pass are shown). Their cost still counts in the pass totals.`);
  return { root, units, notes, stats };
}

// src/renderer/vulkan/buffer_layout.ts
var SCALARS = {
  float: { kind: "scalar", base: "float", width: 32, size: 4 },
  double: { kind: "scalar", base: "float", width: 64, size: 8 },
  float16_t: { kind: "scalar", base: "float", width: 16, size: 2 },
  half: { kind: "scalar", base: "float", width: 16, size: 2 },
  int: { kind: "scalar", base: "int", width: 32, size: 4 },
  uint: { kind: "scalar", base: "uint", width: 32, size: 4 },
  bool: { kind: "scalar", base: "bool", width: 32, size: 4 },
  int8_t: { kind: "scalar", base: "int", width: 8, size: 1 },
  uint8_t: { kind: "scalar", base: "uint", width: 8, size: 1 },
  int16_t: { kind: "scalar", base: "int", width: 16, size: 2 },
  uint16_t: { kind: "scalar", base: "uint", width: 16, size: 2 },
  int64_t: { kind: "scalar", base: "int", width: 64, size: 8 },
  uint64_t: { kind: "scalar", base: "uint", width: 64, size: 8 }
};
var VEC_PREFIX = {
  vec: "float",
  dvec: "double",
  f16vec: "float16_t",
  ivec: "int",
  uvec: "uint",
  bvec: "bool",
  i8vec: "int8_t",
  u8vec: "uint8_t",
  i16vec: "int16_t",
  u16vec: "uint16_t",
  i64vec: "int64_t",
  u64vec: "uint64_t"
};
var MAT_PREFIX = { mat: "float", dmat: "double", f16mat: "float16_t" };
function alignUp(v, a) {
  return a > 0 ? Math.ceil(v / a) * a : v;
}
function alignment(t, rules) {
  switch (t.kind) {
    case "scalar":
      return t.size;
    case "vector":
      return (t.count === 3 ? 4 : t.count) * t.element.size;
    case "matrix":
      return rules === "std140" ? Math.max(16, t.stride) : t.stride;
    case "array":
      return rules === "std140" ? Math.max(16, alignment(t.element, rules)) : alignment(t.element, rules);
    case "struct": {
      let a = 1;
      for (const m of t.members) a = Math.max(a, alignment(m.type, rules));
      return rules === "std140" ? Math.max(16, a) : a;
    }
    default:
      return 4;
  }
}
function parseTypeName(name, structs, rules) {
  const s = SCALARS[name];
  if (s) return s;
  const st = structs.get(name);
  if (st) return st;
  const v = /^([a-z0-9]*vec)([234])$/.exec(name);
  if (v && VEC_PREFIX[v[1]]) {
    const element = SCALARS[VEC_PREFIX[v[1]]];
    const count2 = Number(v[2]);
    return { kind: "vector", element, count: count2, size: count2 * element.size };
  }
  const m = /^([a-z0-9]*mat)([234])(?:x([234]))?$/.exec(name);
  if (m && MAT_PREFIX[m[1]]) {
    const element = SCALARS[MAT_PREFIX[m[1]]];
    const columns = Number(m[2]);
    const rows = m[3] ? Number(m[3]) : columns;
    const column = { kind: "vector", element, count: rows, size: rows * element.size };
    let stride = alignment(column, rules);
    if (rules === "std140") stride = Math.max(16, stride);
    return { kind: "matrix", element, columns, rows, stride, rowMajor: false, size: columns * stride };
  }
  return null;
}
function parseLayout(text, rules, preferredName) {
  const structs = /* @__PURE__ */ new Map();
  const src = text.replace(/\/\/[^\n]*/g, " ").replace(/\/\*[\s\S]*?\*\//g, " ");
  const re = /struct\s+([A-Za-z_]\w*)\s*\{([^}]*)\}\s*;?/g;
  let last = null;
  let match;
  while (match = re.exec(src)) {
    const name = match[1];
    const members = [];
    let offset = 0;
    let maxAlign = 1;
    for (const decl of match[2].split(";")) {
      const d = decl.trim();
      if (!d) continue;
      const dm = /^(?:layout\s*\([^)]*\)\s*)?(?:(?:highp|mediump|lowp)\s+)?([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*((?:\[\s*\d*\s*\])*)$/.exec(d);
      if (!dm) throw new Error(`cannot parse "${d}"`);
      const base = parseTypeName(dm[1], structs, rules);
      if (!base) throw new Error(`unknown type "${dm[1]}" in "${d}"`);
      let type = base;
      const dims = [...dm[3].matchAll(/\[\s*(\d*)\s*\]/g)].map((x) => x[1] === "" ? 0 : Number(x[1])).reverse();
      for (const count2 of dims) {
        const stride = alignUp(sizeOfType(type), alignment({ kind: "array", element: type, count: count2, stride: 0, size: 0 }, rules));
        const arr = { kind: "array", element: type, count: count2, stride, size: count2 * stride };
        type = arr;
      }
      const a = alignment(type, rules);
      offset = alignUp(offset, a);
      members.push({ name: dm[2], offset, type });
      offset += sizeOfType(type);
      maxAlign = Math.max(maxAlign, a);
    }
    const structAlign = rules === "std140" ? Math.max(16, maxAlign) : maxAlign;
    const st = { kind: "struct", name, members, size: alignUp(offset, structAlign) };
    structs.set(name, st);
    last = st;
  }
  if (!last) throw new Error("no struct declaration found");
  if (preferredName && structs.has(preferredName)) return structs.get(preferredName);
  return last;
}
function sizeOfType(t) {
  return t.kind === "opaque" ? 0 : t.size;
}
function layoutText(type, rootName = "Buffer") {
  const out = [];
  const names = /* @__PURE__ */ new Map();
  const usedNames = /* @__PURE__ */ new Set();
  const memberText = (t) => {
    const inner = innermost(t);
    if (inner.kind === "struct") return names.get(inner) ?? inner.name ?? "Struct";
    if (inner.kind === "matrix") return typeName(matrixColumnMajor(inner));
    return typeName(inner);
  };
  const emit = (s) => {
    if (names.has(s)) return;
    for (const m of s.members) {
      const inner = innermost(m.type);
      if (inner.kind === "struct") emit(inner);
    }
    let name = s.name.replace(/[^A-Za-z0-9_]/g, "_") || "Struct";
    if (/^[0-9]/.test(name)) name = `_${name}`;
    while (usedNames.has(name)) name += "_";
    usedNames.add(name);
    names.set(s, name);
    const lines = s.members.map((m) => `    ${memberText(m.type)} ${m.name}${arraySuffix(m.type)};  // offset ${m.offset}`);
    out.push(`struct ${name} {
${lines.join("\n")}
};`);
  };
  if (type.kind === "struct") emit(type);
  else out.push(`struct ${rootName} {
    ${memberText(type)} value${arraySuffix(type)};
};`);
  return out.join("\n\n");
}
function innermost(t) {
  return t.kind === "array" ? innermost(t.element) : t;
}
function matrixColumnMajor(m) {
  return m.rowMajor ? { ...m, rowMajor: false } : m;
}
function arraySuffix(t) {
  let s = "";
  let cur = t;
  while (cur.kind === "array") {
    s += `[${cur.count || ""}]`;
    cur = cur.element;
  }
  return s;
}

// src/mcp/png.ts
import { deflateSync } from "node:zlib";
var CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c2 = n;
    for (let k = 0; k < 8; k++) c2 = c2 & 1 ? 3988292384 ^ c2 >>> 1 : c2 >>> 1;
    table[n] = c2 >>> 0;
  }
  return table;
})();
function crc32(bytes) {
  let c2 = 4294967295;
  for (let i = 0; i < bytes.length; i++) c2 = CRC_TABLE[(c2 ^ bytes[i]) & 255] ^ c2 >>> 8;
  return (c2 ^ 4294967295) >>> 0;
}
function chunk(type, data) {
  const out = new Uint8Array(12 + data.byteLength);
  const view = new DataView(out.buffer);
  view.setUint32(0, data.byteLength);
  for (let i = 0; i < 4; i++) out[4 + i] = type.charCodeAt(i);
  out.set(data, 8);
  view.setUint32(8 + data.byteLength, crc32(out.subarray(4, 8 + data.byteLength)));
  return out;
}
function encodePng(rgba, width, height) {
  const rowBytes = width * 4 + 1;
  const raw = new Uint8Array(rowBytes * height);
  for (let y = 0; y < height; y++) {
    raw[y * rowBytes] = 0;
    raw.set(rgba.subarray(y * width * 4, (y + 1) * width * 4), y * rowBytes + 1);
  }
  const header = new Uint8Array(13);
  const hv = new DataView(header.buffer);
  hv.setUint32(0, width);
  hv.setUint32(4, height);
  header[8] = 8;
  header[9] = 6;
  const parts2 = [
    new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10]),
    chunk("IHDR", header),
    chunk("IDAT", new Uint8Array(deflateSync(raw))),
    chunk("IEND", new Uint8Array(0))
  ];
  const out = new Uint8Array(parts2.reduce((n, p) => n + p.byteLength, 0));
  let pos = 0;
  for (const p of parts2) {
    out.set(p, pos);
    pos += p.byteLength;
  }
  return out;
}
function fitPixels(rgba, width, height, max) {
  const scale = Math.min(1, max / Math.max(width, height));
  if (scale >= 1) return { rgba, width, height };
  const w = Math.max(1, Math.round(width * scale));
  const h = Math.max(1, Math.round(height * scale));
  const out = new Uint8ClampedArray(w * h * 4);
  for (let y = 0; y < h; y++) {
    const sy = Math.min(height - 1, Math.floor((y + 0.5) * height / h));
    for (let x = 0; x < w; x++) {
      const sx = Math.min(width - 1, Math.floor((x + 0.5) * width / w));
      const s = (sy * width + sx) * 4;
      const d = (y * w + x) * 4;
      out[d] = rgba[s];
      out[d + 1] = rgba[s + 1];
      out[d + 2] = rgba[s + 2];
      out[d + 3] = rgba[s + 3];
    }
  }
  return { rgba: out, width: w, height: h };
}

// src/mcp/resource_tools.ts
var COST_MODEL = "Modeled cost of one invocation, not a measurement: instructions weighted ALU 1, special functions 4, texture 20, memory 8, with loops counted as 8 iterations per nesting level. It ranks shaders and functions against each other.";
var FLAME_MS = "Milliseconds. Each pass is its measured GPU time; the split inside a pass is modeled (each stage's modeled cost times its invocations), so compare frames inside a pass with each other rather than with the clock.";
var FLAME_MS_DRAWS = "Milliseconds. Each pass is its measured GPU time, split between its draws by what the replay timed each draw at; only the split between the stages of one draw is modeled.";
var FLAME_OPS = "Modeled op units (each stage's modeled cost times its invocations): they rank frames against each other and are not time. A capture with Profile passes scales each pass to its measured milliseconds.";
var SHADER_VIEWS = ["reflection", "source", "analysis", "glsl", "hlsl", "msl", "disassembly"];
var CHANNELS = ["rgb", "r", "g", "b", "a", "luminance"];
var SCALAR_BYTES = { float32: 4, uint32: 4, int32: 4, uint16: 2, int16: 2, uint8: 1 };
var IMAGE_PARAMS = {
  channels: { type: "string", enum: CHANNELS, description: "What the image shows (default rgb)." },
  exposure: { type: "number", description: "Multiplier applied before display (default 1)." },
  autoRange: { type: "boolean", description: "Stretch the value range to black..white (default on for depth and integer formats)." },
  image: { type: "boolean", description: "Return the PNG (default true); false for the numbers alone." },
  maxSize: { type: "integer", minimum: 16, maximum: 2048, description: "Longest side of the returned image in pixels (default 512)." },
  texels: { type: "array", items: { type: "array", items: { type: "integer" }, minItems: 2, maxItems: 2 }, description: "[x, y] texel coordinates to read exactly (up to 64)." }
};
function texelStats(tex) {
  const n = tex.width * tex.height;
  const out = [];
  for (let ch2 = 0; ch2 < tex.channels; ch2++) {
    let sum = 0;
    let finite = 0;
    let nan = 0;
    let infinite = 0;
    let zero = 0;
    for (let i = 0; i < n; i++) {
      const v = tex.values[i * 4 + ch2];
      if (Number.isNaN(v)) {
        nan++;
      } else if (!Number.isFinite(v)) {
        infinite++;
      } else {
        sum += v;
        finite++;
        if (v === 0) zero++;
      }
    }
    out.push({
      channel: tex.names[ch2],
      min: tidy(tex.min[ch2]),
      max: tidy(tex.max[ch2]),
      mean: finite ? tidy(sum / finite) : void 0,
      zeroFraction: round(zero / n) || void 0,
      nan: nan || void 0,
      infinite: infinite || void 0
    });
  }
  return out;
}
function texel(tex, x, y) {
  const o = (y * tex.width + x) * 4;
  const v = [];
  for (let ch2 = 0; ch2 < tex.channels; ch2++) v.push(tidy(tex.values[o + ch2]));
  return v;
}
function texelAnswer(tex, args, head, aspect) {
  const grid = [];
  for (let gy = 0; gy < 3; gy++) {
    for (let gx = 0; gx < 3; gx++) {
      const x = Math.min(tex.width - 1, Math.floor((gx + 0.5) * tex.width / 3));
      const y = Math.min(tex.height - 1, Math.floor((gy + 0.5) * tex.height / 3));
      grid.push({ x, y, value: texel(tex, x, y) });
    }
  }
  const requested = Array.isArray(args.texels) ? args.texels.slice(0, 64) : [];
  const texels = requested.map((pt) => {
    const x = Array.isArray(pt) ? Number(pt[0]) : NaN;
    const y = Array.isArray(pt) ? Number(pt[1]) : NaN;
    if (!(x >= 0 && x < tex.width && y >= 0 && y < tex.height)) throw new Error(`texel [${String(pt)}] is outside the ${tex.width}x${tex.height} image.`);
    return { x: Math.floor(x), y: Math.floor(y), value: texel(tex, Math.floor(x), Math.floor(y)) };
  });
  const stats = texelStats(tex);
  const uniform = tex.min.slice(0, tex.channels).every((v, ch2) => v === tex.max[ch2]);
  const result = jsonResult({
    ...head,
    width: tex.width,
    height: tex.height,
    channels: tex.names,
    linear: tex.linear || void 0,
    integer: tex.integer || void 0,
    uniform: uniform || void 0,
    stats,
    sampleGrid: grid,
    texels: texels.length ? texels : void 0
  });
  if (boolArg(args, "image", true)) {
    const rgba = displayTexels(tex, {
      channels: enumArg(args, "channels", CHANNELS, "rgb"),
      exposure: numberArg(args, "exposure") ?? 1,
      autoRange: boolArg(args, "autoRange", aspect !== "color" || tex.integer)
    });
    const fit = fitPixels(rgba, tex.width, tex.height, intArg(args, "maxSize", 512, 16, 2048));
    result.content.unshift({ type: "image", data: Buffer.from(encodePng(fit.rgba, fit.width, fit.height)).toString("base64"), mimeType: "image/png" });
  }
  return result;
}
function usersOf(c2, dataId) {
  const out = [];
  for (const cmd of c2.data.commands) {
    if (out.length >= 8) break;
    if (cmd.bufferData?.includes(dataId) || cmd.descriptors?.sets.some((s) => s.bindings.some((b) => b.descriptors.some((d) => d?.data === dataId)))) out.push(cmd.index);
  }
  return out;
}
function attributeBounds(element, view, stride, vertices) {
  const out = {};
  const n = Math.min(vertices, 1e6);
  for (const m of element.members) {
    if (m.type.kind !== "format") continue;
    const f = vertexFormat(m.type.format);
    if (!f) continue;
    const min = new Array(f.channels.length).fill(Infinity);
    const max = new Array(f.channels.length).fill(-Infinity);
    let nan = 0;
    for (let v = 0; v < n; v++) {
      const at = v * stride + m.offset;
      if (at + f.size > view.byteLength) break;
      const values = f.read(view, at);
      for (let k = 0; k < values.length; k++) {
        const x = values[k];
        if (Number.isNaN(x)) {
          nan++;
        } else {
          if (x < min[k]) min[k] = x;
          if (x > max[k]) max[k] = x;
        }
      }
    }
    out[m.name] = { min: min.map(tidy), max: max.map(tidy), nan: nan || void 0 };
  }
  return out;
}
function roundCost(c2) {
  return { alu: round(c2.alu), sfu: round(c2.sfu), texture: round(c2.texture), memory: round(c2.memory) };
}
function reflectionDetail(r) {
  if (!r) return { note: "The SPIR-V could not be reflected." };
  const variable = (v) => ({ location: v.location, name: v.name || void 0, type: v.typeName });
  const resource2 = (res) => ({
    set: res.set,
    binding: res.binding,
    kind: res.kind,
    name: res.name || void 0,
    type: res.typeName,
    count: res.count !== 1 ? res.count : void 0,
    readOnly: res.readOnly || void 0,
    writeOnly: res.writeOnly || void 0,
    layout: res.type.kind === "struct" ? layoutText(res.type) : void 0
  });
  return {
    spirvVersion: r.version || void 0,
    entryPoints: r.entryPoints.map((e) => ({ name: e.name, stage: e.stage, workgroupSize: e.workgroupSize ?? void 0, inputs: e.inputs.map(variable), outputs: e.outputs.map(variable) })),
    resources: r.resources.map(resource2),
    pushConstants: r.pushConstants.length ? r.pushConstants.map(resource2) : void 0
  };
}
function sourceDetail(spirv, maxChars) {
  const { info, found } = debugInfoWithSources(spirv);
  if (!info || !hasEmbeddedSource(info)) {
    const named = (info?.files ?? []).map((f) => f.name).filter(Boolean);
    const roots = searchPaths("sourceRoots").dirs;
    return {
      debugInfo: describeDebugInfo(info),
      note: named.length ? `The debug information names ${named.join(", ")} without the text, and ${roots.length ? `the source roots (${roots.join("; ")}) do not hold it` : "no source roots are set"}: set_search_paths with the directory holding the shader sources finds it. The "glsl" or "hlsl" view cross-compiles the SPIR-V instead.` : 'No source is embedded. Compiling with -g (glslc, glslangValidator), -gVS (glslangValidator) or -fspv-debug=vulkan-with-source (dxc) embeds it; the "glsl" or "hlsl" view cross-compiles the SPIR-V instead.'
    };
  }
  let left = maxChars;
  const files = info.files.map((f, i) => ({ f, i })).filter((x) => x.f.text !== null).map(({ f, i }) => {
    const text = clip(f.text, Math.max(200, left));
    left -= f.text.length;
    return { name: f.name || void 0, main: i === info.mainFile || void 0, fromThisMachine: f.fromHost || void 0, text };
  });
  return { language: info.language, debugInfo: describeDebugInfo(info), foundOnThisMachine: found.length ? found : void 0, files };
}
function analysisDetail(spirv, entryPoint) {
  const a = analyzeSpirvCached(spirv);
  if (!a) return { note: "The SPIR-V could not be analyzed." };
  const texts = sourceLineTexts(debugInfoWithSources(spirv).info);
  const named = a.entryPoints.filter((e) => e.name === entryPoint);
  const entries = named.length ? named : a.entryPoints;
  const lines = a.functions.flatMap((f) => f.lines.map((l) => ({ fn: f.name, l }))).sort((x, y) => y.l.weighted - x.l.weighted).slice(0, 12);
  const findings = [...a.findings].sort((x, y) => SEVERITY_RANK[y.severity] - SEVERITY_RANK[x.severity]);
  return {
    model: COST_MODEL,
    entryPoints: entries.map((e) => ({
      name: e.name,
      stage: e.stage,
      cost: round(e.weighted),
      dominant: e.dominant,
      breakdown: roundCost(e.cost),
      functions: e.functions.slice(0, 10).map((f) => ({
        name: f.name,
        inclusive: round(weighCost(f.inclusive)),
        own: round(weighCost(f.cost)),
        loops: f.loops || void 0,
        branches: f.branches || void 0
      }))
    })),
    findings: findings.map((f) => ({
      rule: f.rule,
      severity: f.severity,
      confidence: f.confidence,
      function: f.function,
      line: f.line ? f.file ? `${f.file}:${f.line}` : String(f.line) : void 0,
      loopDepth: f.loopDepth || void 0,
      count: f.count > 1 ? f.count : void 0,
      code: codeAt(texts, f.file, f.line),
      message: f.message
    })),
    costliestLines: a.hasLines ? lines.map(({ fn, l }) => ({ line: `${l.file}:${l.line}`, code: codeAt(texts, l.file, l.line), function: fn, cost: round(l.weighted), dominant: l.dominant })) : void 0,
    totals: a.totals
  };
}
function utf8Text(data) {
  for (const b of data.subarray(0, 4096)) if (b === 0) return null;
  return new TextDecoder().decode(data);
}
function metalShader(c2, o, view, maxChars) {
  const db = c2.db;
  if (o.type === "MTLRenderPipelineState" || o.type === "MTLComputePipelineState") {
    const resource2 = (r) => ({
      index: r.binding,
      name: r.name || void 0,
      type: r.typeName,
      access: r.readOnly ? "read" : r.writeOnly ? "write" : "read_write",
      layout: r.type.kind === "struct" ? layoutText(r.type) : void 0
    });
    return {
      capture: c2.id,
      object: refText(db, o.id),
      functions: [...o.dependencies].filter((x) => x.type === "MTLFunction").map((x) => refText(db, x.id)),
      stages: metalStages(o).map((s) => ({ stage: s.stage, buffers: [...s.buffers.values()].map(resource2), textures: [...s.textures.values()].map(resource2), samplers: [...s.samplers.values()].map(resource2) })),
      note: view === "reflection" ? void 0 : "A Metal pipeline's code is its functions': get_shader on one of its MTLFunction objects shows the source."
    };
  }
  const library = o.type === "MTLFunction" ? db.getObject(o.parentId) : o.type === "MTLLibrary" ? o : null;
  if (!library || library.type !== "MTLLibrary") {
    throw new Error(`${refText(db, o.id)} has no shader code: get_shader takes a VkPipeline, a VkShaderModule, an MTLLibrary, an MTLFunction or a Metal pipeline state.`);
  }
  const fn = o.type === "MTLFunction" ? str(o.args?.name) : "";
  const payloads = library.blobs.map((b, i) => {
    const data = db.blobData.get(`${library.id}:${i}`);
    if (!data) return { name: b.name, bytes: b.size, note: "Not in the capture file." };
    const text = utf8Text(data);
    if (text === null) return { name: b.name, bytes: data.byteLength, note: "A compiled metallib: the application did not build this library from source, so there is none." };
    if (fn) {
      const lines = text.split("\n");
      const pattern2 = new RegExp(`\\b${fn.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")}\\s*\\(`);
      const at = lines.findIndex((l) => pattern2.test(l));
      if (at >= 0) {
        return {
          name: b.name,
          function: fn,
          line: at + 1,
          text: clip(lines.slice(Math.max(0, at - 8), at + 150).join("\n"), maxChars),
          note: "From eight lines above the function's definition; get_shader on the MTLLibrary shows the whole source."
        };
      }
    }
    return { name: b.name, text: clip(text, maxChars) };
  });
  return {
    capture: c2.id,
    object: refText(db, o.id),
    library: refText(db, library.id),
    functionNames: Array.isArray(library.args?.functionNames) ? library.args.functionNames : void 0,
    payloads
  };
}
function stageModels(c2) {
  const db = c2.db;
  const models = /* @__PURE__ */ new Map();
  const spirv = /* @__PURE__ */ new Map();
  for (const pipelineId of pipelineUses(c2.data).keys()) {
    const pipeline = db.getObject(pipelineId);
    if (!pipeline) continue;
    models.set(pipelineId, pipelineStages(pipeline, db).map((s) => {
      const bytes = c2.spirv(s.object, s.blobIndex);
      if (bytes) spirv.set(`${s.object.id}|${s.stage}`, bytes);
      const reflection = s.stage === "compute" ? c2.reflection(s.object, s.blobIndex) : null;
      const entry = reflection?.entryPoints.find((e) => e.name === s.entryPoint) ?? reflection?.entryPoints[0] ?? null;
      return {
        stage: s.stage,
        entryPoint: s.entryPoint,
        objectId: s.object.id,
        analysis: bytes ? analyzeSpirvCached(bytes) : null,
        workgroupSize: entry?.workgroupSize ?? null
      };
    }));
  }
  return { models, spirv };
}
function shareOf(cost, total) {
  return total > 0 ? round(cost / total) : void 0;
}
function flameFrame(v, n, level) {
  const db = v.c.db;
  const out = { kind: n.kind, name: n.name, cost: round(n.totalCost), share: shareOf(n.totalCost, v.total) };
  if (n.kind === "pass") {
    const pass = n.command ? v.c.passOf(n.command.index) : -1;
    if (pass >= 0) {
      out.pass = pass;
      out.name = v.c.passName(pass);
    }
    out.command = n.command?.index;
    out.measuredMs = round(n.durationMs);
    if (!n.children.length && n.totalCost > 0) out.note = "Nothing in this pass could be weighed: it has no draws or dispatches, or their shaders have no analysis.";
  } else if (n.kind === "item") {
    out.command = n.command?.index;
    out.pipeline = refText(db, n.objectId);
  } else if (n.kind === "stage") {
    if (n.stage) out.name = `${n.stage}: ${n.entryPoint}`;
    out.shader = refText(db, n.objectId);
    out.invocations = n.invocations;
    out.invocationCount = n.confidence;
    out.unweighted = n.reason;
  } else if (n.kind === "function") {
    out.own = round(n.selfCost) || void 0;
  }
  out.dominant = n.dimension;
  if (!n.children.length) return out;
  if (level >= v.depth) {
    out.hiddenChildren = n.children.length;
    return out;
  }
  const sorted = [...n.children].sort((x, y) => y.totalCost - x.totalCost);
  const kept = sorted.filter((ch2) => v.total <= 0 || ch2.totalCost / v.total >= v.minShare);
  const folded = sorted.slice(kept.length);
  const children = kept.map((ch2) => flameFrame(v, ch2, level + 1));
  const foldedCost = folded.reduce((sum, ch2) => sum + ch2.totalCost, 0);
  if (folded.length && (v.total <= 0 || foldedCost / v.total >= 1e-3)) {
    const cost = foldedCost;
    children.push({ kind: "other", name: `${folded.length} smaller frame${folded.length === 1 ? "" : "s"}`, cost: round(cost), share: shareOf(cost, v.total) });
  }
  out.children = children;
  return out;
}
function flameHotspots(c2, root, total, top, codeOf) {
  const functions = /* @__PURE__ */ new Map();
  const lines = /* @__PURE__ */ new Map();
  const unweighted = /* @__PURE__ */ new Map();
  const add = (map, key, spot, cost) => {
    const s = map.get(key) ?? { ...spot, cost: 0 };
    s.cost += cost;
    map.set(key, s);
  };
  const walk = (n, fn) => {
    let name = fn;
    if (n.kind === "stage" || n.kind === "function") {
      name = n.kind === "stage" ? n.entryPoint ?? "" : n.name;
      if (n.selfCost > 0) add(functions, `${n.objectId}|${n.stage}|${name}`, { function: name, stage: n.stage, object: n.objectId }, n.selfCost);
      if (n.reason) {
        unweighted.set(`${n.objectId}|${n.stage}|${name}`, { stage: n.stage, entryPoint: n.entryPoint, shader: refText(c2.db, n.objectId), reason: n.reason });
      }
    } else if (n.kind === "line") {
      add(lines, `${n.objectId}|${n.stage}|${n.name}`, { function: fn, stage: n.stage, object: n.objectId, line: n.name, file: n.file, lineNo: n.line }, n.totalCost);
    }
    for (const ch2 of n.children) walk(ch2, name);
  };
  walk(root, "");
  const ranked = (map) => [...map.values()].sort((x, y) => y.cost - x.cost).slice(0, top).map((s) => ({
    function: s.function,
    line: s.line,
    code: s.lineNo ? codeOf(s.object, s.stage, s.file, s.lineNo) : void 0,
    stage: s.stage,
    shader: refText(c2.db, s.object),
    cost: round(s.cost),
    share: shareOf(s.cost, total)
  }));
  const f = ranked(functions);
  const l = ranked(lines);
  return {
    hottestFunctions: f.length ? f : void 0,
    hottestLines: l.length ? l : void 0,
    unweightedStages: unweighted.size ? [...unweighted.values()].slice(0, 20) : void 0
  };
}
function resourceTools(store) {
  return [
    {
      name: "list_textures",
      description: "List the images a capture read back: every render pass attachment at the end of its pass (kind attachment, with the pass number) and the images bound through descriptor sets (kind sampled), with format, size, mips and layers, and why a read-back failed. The texture numbers are what read_texture takes.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        kind: { type: "string", enum: ["all", "attachment", "sampled"], description: "Which read-backs (default all)." },
        pass: { type: "integer", minimum: 0, description: "Only the attachments of this pass." },
        image: { type: "integer", minimum: 0, description: "Only read-backs of this image object id." },
        ...PAGE_PARAMS
      }),
      readOnly: true,
      handler: (args) => {
        const c2 = store.resolve(stringArg(args, "capture"));
        const kind = enumArg(args, "kind", ["all", "attachment", "sampled"], "all");
        const pass = optionalInt(args, "pass");
        const image = optionalInt(args, "image");
        const list = c2.data.textures.filter((t) => (kind === "all" || kind === "sampled" === (t.info.kind === "sampled")) && (image === void 0 || t.info.id === image) && (pass === void 0 || c2.passOfTexture(t.info) === pass));
        const p = page(list, args, 100, 500);
        return jsonResult({ capture: c2.id, total: p.total, offset: p.offset, nextOffset: p.nextOffset, textures: p.items.map((t) => textureBrief(c2, t)) });
      }
    },
    {
      name: "read_texture",
      description: "Look at a read-back image: returns it as a PNG (scaled to fit maxSize, displayed like GPU Inspector's image viewer: linear data sRGB-encoded, depth auto-ranged) together with per-channel minimum, maximum and mean, the share of zero texels, NaN and infinity counts, a 3x3 grid of sampled texel values, and exact values at requested texels. Use it to see what a pass rendered and to find where a rendering problem appears.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        texture: { type: "integer", minimum: 0, description: "The texture number from list_textures or get_command's renderTargets." },
        mip: { type: "integer", minimum: 0, description: "Mip level, for sampled images read back with their mips (default the first read back)." },
        layer: { type: "integer", minimum: 0, description: "Array layer or 3D slice (default 0)." },
        ...IMAGE_PARAMS
      }, ["texture"]),
      readOnly: true,
      handler: (args) => {
        const c2 = store.resolve(stringArg(args, "capture"));
        const d = c2.data;
        const index = requireInt(args, "texture");
        const t = d.textures[index];
        if (!t) throw new Error(`No texture ${index}: ${c2.id} read back ${d.textures.length} images (list_textures).`);
        const info = t.info;
        const brief = textureBrief(c2, t);
        if (info.error) return jsonResult({ ...brief, note: "The read-back failed, so there are no pixels." });
        if (!t.data) return jsonResult({ ...brief, note: "The capture has no pixel data for this image." });
        const mips = Math.max(1, info.mips ?? 1);
        const mip = intArg(args, "mip", info.mip, info.mip, info.mip + mips - 1);
        const dims = (m) => ({
          width: Math.max(1, info.width >> m - info.mip),
          height: Math.max(1, info.height >> m - info.mip),
          depth: Math.max(1, (info.depth || 1) >> m - info.mip)
        });
        const layers = Math.max(1, info.layers || 1);
        const bytesOf = (m) => {
          const dd = dims(m);
          return sliceBytes({ format: info.format, aspect: info.aspect, width: dd.width, height: dd.height }) * Math.max(dd.depth, layers);
        };
        let offset = 0;
        for (let m = info.mip; m < mip; m++) offset += bytesOf(m);
        const size2 = dims(mip);
        const imageInfo = { format: info.format, aspect: info.aspect, width: size2.width, height: size2.height };
        if (!isFormatSupported(imageInfo)) return jsonResult({ ...brief, note: `Decoding ${info.format} is not supported.` });
        const slices = Math.max(size2.depth, layers);
        const layer = intArg(args, "layer", 0, 0, slices - 1);
        const tex = decodeTexels(imageInfo, t.data.subarray(offset, offset + bytesOf(mip)), layer);
        if (!tex) return jsonResult({ ...brief, note: "The pixel data is shorter than the image's size says." });
        return texelAnswer(tex, args, { ...brief, mip, layer, slices: slices > 1 ? slices : void 0 }, info.aspect);
      }
    },
    {
      name: "read_buffer",
      description: "Read a buffer range the capture read back (its data id from get_command: a descriptor binding, vertex, index or indirect buffer) as numbers of one scalar type, as hex, or through a GLSL struct layout (std140 or std430 offsets), optionally as an array of that struct. Also says which commands bound the range. Captured ranges stop at the capture's buffer size limit (64 KB by default); truncatedFrom gives the bound size.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        data: { type: "integer", minimum: 1, description: "The captured range's data id." },
        as: { type: "string", enum: [...Object.keys(SCALAR_BYTES), "hex"], description: "Scalar type to read the bytes as when no layout is given (default float32)." },
        layout: { type: "string", description: 'GLSL struct declarations; the last struct is the type: "struct Light { vec4 position; vec4 color; }; struct Lights { Light lights[8]; uint count; };".' },
        rules: { type: "string", enum: ["std140", "std430"], description: "Layout rules for `layout` (default std430; uniform blocks are std140)." },
        count: { type: "integer", minimum: 1, description: "With a layout: read this many structs one after another. Without: how many scalars (default 256) or bytes of hex." },
        offset: { type: "integer", minimum: 0, description: "Byte offset into the captured range (default 0)." }
      }, ["data"]),
      readOnly: true,
      handler: (args) => {
        const c2 = store.resolve(stringArg(args, "capture"));
        const id = requireInt(args, "data");
        const b = c2.data.buffer(id);
        if (!b) throw new Error(`No captured buffer range ${id} in ${c2.id}: get_command lists the data ids of a command's bindings.`);
        const base = {
          capture: c2.id,
          data: id,
          buffer: refText(c2.db, b.info.buffer),
          bufferOffset: b.info.offset,
          capturedBytes: b.data?.byteLength ?? 0,
          truncatedFrom: b.info.originalSize,
          boundBy: usersOf(c2, id)
        };
        if (b.info.error) return jsonResult({ ...base, captureError: b.info.error });
        const bytes = b.data ?? new Uint8Array(0);
        const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        const offset = intArg(args, "offset", 0, 0, bytes.byteLength);
        const layout = stringArg(args, "layout");
        if (layout) {
          const struct = parseLayout(layout, enumArg(args, "rules", ["std140", "std430"], "std430"));
          const count3 = optionalInt(args, "count");
          const type = count3 ? { kind: "array", element: struct, count: count3, stride: struct.size, size: count3 * struct.size } : struct;
          return jsonResult({ ...base, offset, layout: layoutText(struct), structBytes: struct.size, values: readTyped(type, view, offset, { values: 8192 }) });
        }
        const as = enumArg(args, "as", ["float32", "uint32", "int32", "uint16", "int16", "uint8", "hex"], "float32");
        if (as === "hex") {
          const end = Math.min(bytes.byteLength, offset + intArg(args, "count", 256, 1, 16384));
          const lines = [];
          for (let at = offset; at < end; at += 16) {
            lines.push(`${at.toString(16).padStart(6, "0")}: ${Buffer.from(bytes.subarray(at, Math.min(end, at + 16))).toString("hex").replace(/(..)(?!$)/g, "$1 ")}`);
          }
          return jsonResult({ ...base, offset, hex: lines });
        }
        const size2 = SCALAR_BYTES[as];
        const count2 = Math.min(intArg(args, "count", 256, 1, 16384), Math.floor((bytes.byteLength - offset) / size2));
        const values = [];
        for (let i = 0; i < count2; i++) {
          const at = offset + i * size2;
          switch (as) {
            case "float32":
              values.push(tidy(view.getFloat32(at, true)));
              break;
            case "uint32":
              values.push(view.getUint32(at, true));
              break;
            case "int32":
              values.push(view.getInt32(at, true));
              break;
            case "uint16":
              values.push(view.getUint16(at, true));
              break;
            case "int16":
              values.push(view.getInt16(at, true));
              break;
            default:
              values.push(view.getUint8(at));
              break;
          }
        }
        return jsonResult({ ...base, offset, as, values });
      }
    },
    {
      name: "read_vertices",
      description: "Decode the vertices a draw read, through its pipeline's vertex layout with attribute names from the vertex shader: for an indexed draw the indices from firstIndex and the vertices they name, otherwise the run from firstVertex. Also gives each attribute's minimum and maximum over every captured vertex and its NaN count, which finds collapsed, exploded or uninitialized geometry at a glance.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        command: { type: "integer", minimum: 0, description: "The draw command's index." },
        count: { type: "integer", minimum: 1, maximum: 256, description: "Vertices (or indices) to decode (default 8)." },
        first: { type: "integer", minimum: 0, description: "Start at this index or vertex instead of the draw's own first one." },
        binding: { type: "integer", minimum: 0, description: "Only this vertex buffer binding." }
      }, ["command"]),
      readOnly: true,
      handler: (args) => {
        const c2 = store.resolve(stringArg(args, "capture"));
        const d = c2.data;
        const index = requireInt(args, "command");
        const cmd = d.commands[index];
        if (!cmd || !d.sets.DRAW.has(cmd.method)) throw new Error(`Command ${index} is not a draw: read_vertices takes a draw command (list_commands with kind draw).`);
        const state = drawState(d, c2.db, cmd);
        const inputs = vertexInputs(c2, state.pipeline);
        const a = cmd.args ?? {};
        const count2 = intArg(args, "count", 8, 1, 256);
        const first = optionalInt(args, "first");
        const indexed = /Indexed/i.test(cmd.method) && state.indexBuffer ? state.indexBuffer : null;
        let vertexIds = [];
        let indices;
        const ibData = indexed ? d.buffer(indexed.dataId)?.data : null;
        const ibSize = indexed ? indexSize(indexed.indexType) : 0;
        if (indexed && ibData && ibSize) {
          const firstIndex = first ?? num(a.firstIndex);
          const baseVertex = num(a.vertexOffset ?? a.baseVertex);
          const view = new DataView(ibData.buffer, ibData.byteOffset, ibData.byteLength);
          const values = [];
          for (let i = firstIndex; i < firstIndex + count2 && (i + 1) * ibSize <= ibData.byteLength; i++) values.push(readIndex(view, i * ibSize, ibSize));
          vertexIds = values.map((v) => v + baseVertex);
          indices = { first: firstIndex, baseVertex: baseVertex || void 0, values, truncatedFrom: d.buffer(indexed.dataId)?.info.originalSize };
        } else {
          const start = first ?? num(a.firstVertex ?? a.vertexStart);
          for (let i = 0; i < count2; i++) vertexIds.push(start + i);
        }
        const wanted = optionalInt(args, "binding");
        const bindings = [...state.vertexBuffers.values()].filter((vb) => wanted === void 0 || vb.binding === wanted).sort((x, y) => x.binding - y.binding);
        const out = [];
        for (const vb of bindings) {
          const layout = vertexLayout(state, vb.binding, vb);
          const head = { binding: vb.binding, buffer: refText(c2.db, vb.buffer), offset: vb.offset, data: vb.dataId || void 0 };
          if (!layout?.stride) {
            if (!d.sets.BIND_STAGE_BUFFER) out.push({ ...head, note: "The bound pipeline has no vertex layout for this binding." });
            continue;
          }
          const captured = d.buffer(vb.dataId);
          if (!captured?.data) {
            out.push({ ...head, stride: layout.stride, note: captured?.info.error ?? "The contents were not captured." });
            continue;
          }
          const view = new DataView(captured.data.buffer, captured.data.byteOffset, captured.data.byteLength);
          const element = vertexStruct(layout, inputs);
          const available = Math.floor(captured.data.byteLength / layout.stride);
          const perInstance = layout.rate.includes("INSTANCE");
          const ids = perInstance ? Array.from({ length: Math.min(count2, Math.max(1, num(a.instanceCount))) }, (_, i) => num(a.firstInstance) + i) : vertexIds;
          out.push({
            ...head,
            stride: layout.stride,
            perInstance: perInstance || void 0,
            capturedVertices: available,
            truncatedFrom: captured.info.originalSize,
            attributes: element.members.map((m) => ({ name: m.name, format: m.type.kind === "format" ? m.type.format : void 0, offset: m.offset })),
            bounds: attributeBounds(element, view, layout.stride, available),
            vertices: ids.map((id) => id < available ? { vertex: id, ...readTyped(element, view, id * layout.stride) } : { vertex: id, beyondCapturedRange: true })
          });
        }
        return jsonResult({
          capture: c2.id,
          command: index,
          method: cmd.method,
          pipeline: refText(c2.db, state.pipeline?.id),
          indices,
          bindings: out.length ? out : void 0,
          note: bindings.length ? void 0 : "No vertex buffers are bound at this draw (the vertices may come from the shader, or from a storage buffer)."
        });
      }
    },
    {
      name: "get_shader",
      description: `A shader of a capture. For a VkPipeline (every stage, or one with \`stage\`) or a VkShaderModule: view "reflection" (entry points, inputs and outputs, resources by set and binding with struct layouts, push constants), "source" (the source the compiler embedded, when it did), "glsl" / "hlsl" / "msl" (cross-compiled with spirv-cross), "disassembly" (spirv-dis), or "analysis" (the modeled per-invocation cost by function and source line, and findings for expensive constructs). For Metal: an MTLLibrary's or MTLFunction's source, or a pipeline state's reflection.`,
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        object: { type: "integer", minimum: 0, description: "The pipeline, shader module, library, function or pipeline state object id." },
        view: { type: "string", enum: SHADER_VIEWS, description: "What to show (default reflection)." },
        stage: { type: "string", description: "Only this stage of a pipeline: vertex, fragment, compute, ..." },
        maxChars: { type: "integer", minimum: 1e3, maximum: 2e5, description: "Longest text to return (default 40000)." }
      }, ["object"]),
      readOnly: true,
      handler: async (args) => {
        const c2 = store.resolve(stringArg(args, "capture"));
        const db = c2.db;
        const id = requireInt(args, "object");
        const o = db.getObject(id);
        if (!o) throw new Error(`No object ${id} in ${c2.id}.`);
        const view = enumArg(args, "view", SHADER_VIEWS, "reflection");
        const maxChars = intArg(args, "maxChars", 4e4, 1e3, 2e5);
        if (o.type.startsWith("MTL")) return jsonResult(metalShader(c2, o, view, maxChars));
        let sources;
        if (o.type === "VkPipeline") sources = pipelineStages(o, db);
        else if (o.type === "VkShaderModule") sources = o.blobs.length ? [{ stage: str(o.updates.stage) || "unknown", entryPoint: "", object: o, blobIndex: 0 }] : [];
        else throw new Error(`${refText(db, id)} has no shader code: get_shader takes a VkPipeline, a VkShaderModule, an MTLLibrary, an MTLFunction or a Metal pipeline state.`);
        const stage = stringArg(args, "stage")?.toLowerCase();
        if (stage) sources = sources.filter((s) => s.stage.startsWith(stage));
        const stages = [];
        for (const s of sources) {
          const spirv = c2.spirv(s.object, s.blobIndex);
          const head = { stage: s.stage, entryPoint: s.entryPoint || void 0, shader: refText(db, s.object.id), spirvBytes: spirv?.byteLength };
          if (!spirv) {
            stages.push({ ...head, note: "The capture file carries no SPIR-V for this stage." });
            continue;
          }
          if (view === "reflection") {
            stages.push({ ...head, ...reflectionDetail(c2.reflection(s.object, s.blobIndex)) });
          } else if (view === "source") {
            stages.push({ ...head, ...sourceDetail(spirv, maxChars) });
          } else if (view === "analysis") {
            stages.push({ ...head, ...analysisDetail(spirv, s.entryPoint) });
          } else {
            const r = await shaderText(spirv, view === "disassembly" ? "dis" : view);
            stages.push(r.ok ? { ...head, text: clip(r.text.replace(/\r\n/g, "\n"), maxChars) } : { ...head, error: r.text, note: "Cross-compiling and disassembling use spirv-cross and spirv-dis from the Vulkan SDK: set VULKAN_SDK or INSPECTOR_TOOLS_DIR, or put them on PATH." });
          }
        }
        return jsonResult({
          capture: c2.id,
          object: refText(db, id),
          view,
          stages,
          note: sources.length ? void 0 : "No shader stages with code were found for this object."
        });
      }
    },
    {
      name: "analyze_shaders",
      description: `Rank the shaders a Vulkan capture's frame used: for each pipeline its draws and dispatches bound, every stage's modeled per-invocation cost, the dominant kind of work, how many draws or dispatches used it, and its analysis findings by severity, ordered by uses times cost. get_shader with view "analysis" explains one in detail.`,
      inputSchema: schema({ capture: CAPTURE_PARAM, ...PAGE_PARAMS }),
      readOnly: true,
      handler: (args) => {
        const c2 = store.resolve(stringArg(args, "capture"));
        const d = c2.data;
        const db = c2.db;
        if (d.api === "metal") {
          return jsonResult({ capture: c2.id, note: "The static shader analysis reads SPIR-V, so it covers Vulkan captures. For Metal shaders, GPU Inspector's Xcode Trace button writes a .gputrace whose shader profiler has per-line costs." });
        }
        const rows = [];
        for (const [pipelineId, uses] of pipelineUses(d)) {
          const p2 = db.getObject(pipelineId);
          if (!p2) continue;
          for (const s of pipelineStages(p2, db)) {
            const spirv = c2.spirv(s.object, s.blobIndex);
            const a = spirv ? analyzeSpirvCached(spirv) : null;
            const e = a?.entryPoints.find((x) => x.name === s.entryPoint) ?? a?.entryPoints[0];
            const findings = {};
            for (const f of a?.findings ?? []) findings[f.severity] = (findings[f.severity] ?? 0) + 1;
            const worst = [...a?.findings ?? []].sort((x, y) => SEVERITY_RANK[y.severity] - SEVERITY_RANK[x.severity]).slice(0, 3);
            rows.push({
              score: uses * (e?.weighted ?? 0),
              row: {
                pipeline: refText(db, p2.id),
                stage: s.stage,
                entryPoint: s.entryPoint,
                shader: refText(db, s.object.id),
                uses,
                cost: round(e?.weighted),
                dominant: e?.dominant,
                findings: Object.keys(findings).length ? findings : void 0,
                worst: worst.length ? worst.map((f) => `${f.severity} ${f.rule}${f.line ? ` (${f.file ? `${f.file}:` : "line "}${f.line})` : ""}: ${f.message}`) : void 0,
                note: spirv ? a ? void 0 : "The SPIR-V could not be analyzed." : "No SPIR-V in the capture file."
              }
            });
          }
        }
        rows.sort((x, y) => y.score - x.score);
        const p = page(rows, args, 30, 200);
        return jsonResult({
          capture: c2.id,
          model: COST_MODEL,
          pipelines: pipelineUses(d).size,
          total: p.total,
          offset: p.offset,
          nextOffset: p.nextOffset,
          stages: p.items.map((r) => r.row)
        });
      }
    },
    {
      name: "get_shader_flame_graph",
      description: "The Shader Flame Graph of a Vulkan capture: the frame's GPU work by pass, pipeline (or draw), shader stage, function and source line, with the frame's hottest functions and lines. Each stage weighs its modeled per-invocation cost times its invocations: vertex and compute counts are exact (from the draw and dispatch arguments, indirect ones from the captured buffers), fragment counts come from the pass's measured GPU counters where it has them (split between its draws by scissor area) and from the scissor area otherwise. When every pass was timed (Profile passes) the costs are milliseconds, each pass its measured GPU time with only the split inside it modeled; otherwise modeled op units. Where analyze_shaders ranks shaders, this shows where the frame's shading work goes.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        pass: { type: "integer", minimum: 0, description: "Only this pass (the pass number get_bottlenecks and list_commands give); shares are then of the pass." },
        perDraw: { type: "boolean", description: "One frame per draw or dispatch instead of one per pipeline (default false)." },
        estimateFragments: { type: "boolean", description: "Weight fragment stages by the scissor or render area where the pass has no measured fragment counters, an upper bound without overdraw (default true); false leaves those stages unweighted." },
        depth: { type: "integer", minimum: 1, maximum: 32, description: "Levels to show: 1 passes, 2 pipelines or draws, 3 stages, then functions, their callees and source lines (default 6)." },
        minShare: { type: "number", minimum: 0, maximum: 1, description: 'Fold frames below this share of the total into one "other" frame, left out when it is under 0.001 (default 0.01).' },
        top: { type: "integer", minimum: 0, maximum: 100, description: "How many of the hottest functions and lines to list (default 15)." },
        measureDraws: { type: "boolean", description: "Replay the capture to time and count every draw the first time this is asked (default true; seconds to minutes, and it needs vkinsp_replay built). False leaves the draws weighted by the model." }
      }),
      readOnly: true,
      handler: async (args) => {
        const c2 = store.resolve(stringArg(args, "capture"));
        if (c2.data.api === "metal") {
          return jsonResult({ capture: c2.id, note: "The flame graph weighs SPIR-V shaders, so it covers Vulkan captures. For Metal, get_bottlenecks has each pass's vertex/fragment split, and GPU Inspector's Xcode Trace button writes a .gputrace whose shader profiler has per-line costs." });
        }
        let drawNote;
        if (boolArg(args, "measureDraws", true) && !c2.data.drawStats) {
          const tool = findReplayTool(checkoutRoots(), installedLayerDirs());
          if (!tool) drawNote = `The draws are weighted by the model: measuring them replays the capture, and ${NO_REPLAY_TOOL}`;
          else {
            const run2 = await replayServers.run(tool, c2.path, { kind: "draws" });
            if (!run2.data) drawNote = `The draws are weighted by the model: the replay could not measure them (${run2.error ?? "no data"}).`;
            else {
              const file = parseDrawStats(run2.data);
              c2.setDrawStats(file.draws);
              drawNote = drawStatsSummary(file) + (file.note ? ` (${file.note})` : "");
            }
          }
        }
        const { models, spirv } = stageModels(c2);
        const result = buildFrameCostTree({
          data: c2.data,
          db: c2.db,
          models,
          perDraw: boolArg(args, "perDraw", false),
          estimateFragments: boolArg(args, "estimateFragments", true)
        });
        const texts = /* @__PURE__ */ new Map();
        const codeOf = (object, stage, file, line) => {
          const key = `${object}|${stage}`;
          let t = texts.get(key);
          if (!t) {
            const bytes = spirv.get(key);
            t = bytes ? sourceLineTexts(debugInfoWithSources(bytes).info) : /* @__PURE__ */ new Map();
            texts.set(key, t);
          }
          return codeAt(t, file, line);
        };
        let root = result.root;
        const pass = optionalInt(args, "pass");
        if (pass !== void 0) {
          const found = root.children.find((n) => n.command && c2.passOf(n.command.index) === pass);
          if (!found) throw new Error(`Pass ${pass} is not in the flame graph: it has ${root.children.length} passes with draws or dispatches (get_bottlenecks lists every pass).`);
          root = found;
        }
        const total = root.totalCost;
        const view = { c: c2, total, depth: intArg(args, "depth", 6, 1, 32), minShare: Math.min(1, Math.max(0, numberArg(args, "minShare") ?? 0.01)) };
        return jsonResult({
          capture: c2.id,
          units: result.units,
          meaning: result.units !== "ms" ? FLAME_OPS : result.stats.measuredDrawPasses > 0 ? FLAME_MS_DRAWS : FLAME_MS,
          model: COST_MODEL,
          total: round(total),
          passes: pass === void 0 ? result.stats.passes : void 0,
          drawsAndDispatches: pass === void 0 ? result.stats.items : void 0,
          graph: flameFrame(view, root, 0),
          ...flameHotspots(c2, root, total, intArg(args, "top", 15, 0, 100), codeOf),
          notes: [...drawNote ? [drawNote] : [], ...result.notes].length ? [...drawNote ? [drawNote] : [], ...result.notes] : void 0
        });
      }
    }
  ];
}

// src/mcp/tools.ts
import fs11 from "node:fs";
import path10 from "node:path";

// src/renderer/pixel_history.ts
function hexBytes(text) {
  const s = typeof text === "string" ? text : "";
  const out = new Uint8Array(Math.floor(s.length / 2));
  for (let i = 0; i < out.length; i++) out[i] = parseInt(s.substr(i * 2, 2), 16);
  return out;
}
function parsePixelHistory(input) {
  let json;
  if (input instanceof Uint8Array || typeof input === "string") {
    const text = typeof input === "string" ? input : new TextDecoder().decode(input);
    try {
      json = JSON.parse(text);
    } catch (e) {
      throw new Error(`The pixel history is not valid JSON: ${e.message}`);
    }
  } else {
    json = input;
  }
  if (json?.format !== "gpu-inspector-pixel-history") throw new Error("Not a pixel history.");
  const num2 = (v) => typeof v === "number" ? v : 0;
  const str3 = (v) => typeof v === "string" ? v : "";
  const events = (Array.isArray(json.events) ? json.events : []).map((raw) => {
    const e = raw;
    const kind = str3(e.kind);
    return {
      kind: kind === "load" || kind === "clear" ? kind : "draw",
      command: num2(e.command),
      method: str3(e.method),
      detail: str3(e.detail),
      commandBuffer: num2(e.commandBuffer),
      frame: num2(e.frame),
      passIndex: num2(e.passIndex),
      pipeline: num2(e.pipeline),
      scissored: e.scissored === true,
      testsMeasured: num2(e.testsMeasured),
      covered: num2(e.covered),
      facing: num2(e.facing),
      shaded: num2(e.shaded),
      depthPassed: num2(e.depthPassed),
      stencilPassed: num2(e.stencilPassed),
      passed: num2(e.passed),
      value: hexBytes(e.value),
      depth: hexBytes(e.depth)
    };
  });
  const strings = (v) => Array.isArray(v) ? v.filter((s) => typeof s === "string") : [];
  return {
    device: str3(json.device),
    image: num2(json.image),
    requestedImage: typeof json.requestedImage === "number" ? json.requestedImage : num2(json.image),
    x: num2(json.x),
    y: num2(json.y),
    mip: num2(json.mip),
    layer: num2(json.layer),
    pixelFormat: str3(json.pixelFormat),
    depthFormat: str3(json.depthFormat),
    events,
    notes: strings(json.notes),
    problems: strings(json.problems)
  };
}
var MEASURED_COVERED = 1;
var MEASURED_FACING = 2;
var MEASURED_SHADED = 4;
var MEASURED_DEPTH = 8;
var MEASURED_STENCIL = 16;
var MEASURED_ALL = 32;
function drawOutcome(e) {
  const measured = (bit) => (e.testsMeasured & bit) !== 0;
  if (e.scissored) return "scissored";
  if (!e.testsMeasured) return "unmeasured";
  if (measured(MEASURED_COVERED) && !e.covered) return "missed";
  if (measured(MEASURED_FACING) && !e.facing) return "culled";
  if (measured(MEASURED_SHADED) && !e.shaded) return "discarded";
  const depthFailed = measured(MEASURED_DEPTH) && !e.depthPassed;
  const stencilFailed = measured(MEASURED_STENCIL) && !e.stencilPassed;
  if (depthFailed && stencilFailed) return "depth-stencil";
  if (depthFailed) return "depth";
  if (stencilFailed) return "stencil";
  if (measured(MEASURED_ALL)) return e.passed ? "wrote" : "tests";
  return "covers";
}
var OUTCOME_TEXT = {
  scissored: "outside the scissor",
  unmeasured: "not measured (the draw's pipeline could not be copied)",
  missed: "does not reach the pixel",
  culled: "culled",
  discarded: "discarded by the fragment shader",
  depth: "failed the depth test",
  stencil: "failed the stencil test",
  "depth-stencil": "failed the depth and stencil tests",
  tests: "failed the depth and stencil tests together",
  wrote: "wrote the pixel",
  covers: "covers the pixel"
};
function touchesPixel(e) {
  if (e.kind !== "draw") return true;
  const outcome = drawOutcome(e);
  return outcome !== "scissored" && outcome !== "missed";
}
function eventSummary(e) {
  if (e.kind === "load") return `pass ${e.passIndex} begins (${e.detail.replace(/^(VK_ATTACHMENT_LOAD_OP_|MTLLoadAction)/, "") || "load"})`;
  if (e.kind === "clear") return `${e.method}: cleared`;
  const outcome = drawOutcome(e);
  const samples = outcome === "wrote" ? ` (${e.passed} sample${e.passed === 1 ? "" : "s"} passed)` : "";
  return `${e.method}: ${OUTCOME_TEXT[outcome]}${samples}`;
}
function texelInfo(format, depth) {
  return { format, aspect: depth ? "depth" : "color", width: 1, height: 1 };
}
function texelValues(format, bytes, depth = false) {
  if (!bytes.byteLength || !format) return null;
  const tex = decodeTexels(texelInfo(format, depth), bytes);
  return tex ? Array.from(tex.values.slice(0, tex.channels)) : null;
}

// src/renderer/overdraw.ts
var OVERDRAW_BUCKETS = ["1", "2", "3", "4", "5-8", "9-16", "17-32", "33+"];
function overdrawCount(o, x, y) {
  const { width, height } = o.info;
  if (!o.data || x < 0 || y < 0 || x >= width || y >= height) return 0;
  const i = (y * width + x) * 2;
  return i + 1 < o.data.byteLength ? o.data[i] | o.data[i + 1] << 8 : 0;
}
var RAMP = [
  [0, 0, 0, 0],
  [1, 20, 40, 150],
  [2, 0, 120, 230],
  [3, 0, 190, 170],
  [4, 110, 210, 40],
  [6, 240, 210, 0],
  [10, 250, 120, 0],
  [16, 220, 20, 20],
  [32, 240, 0, 200],
  [65535, 255, 255, 255]
];
var OVERDRAW_LEGEND = RAMP.map(([upTo, r, g, b], i) => {
  const from = i === 0 ? 0 : RAMP[i - 1][0] + 1;
  const label = i === RAMP.length - 1 ? `${from}+` : from === upTo ? String(upTo) : `${from}-${upTo}`;
  return { label, color: [r, g, b] };
});
function heatColor(n) {
  for (const [upTo, r, g, b] of RAMP) if (n <= upTo) return [r, g, b];
  return [255, 255, 255];
}
function overdrawRgba(o, transparentZero = false) {
  const { width, height } = o.info;
  const pixels = width * height;
  if (!o.data || o.data.byteLength < pixels * 2) return null;
  const out = new Uint8ClampedArray(pixels * 4);
  for (let p = 0; p < pixels; p++) {
    const count2 = o.data[p * 2] | o.data[p * 2 + 1] << 8;
    const [r, g, b] = heatColor(count2);
    out[p * 4] = r;
    out[p * 4 + 1] = g;
    out[p * 4 + 2] = b;
    out[p * 4 + 3] = transparentZero && count2 === 0 ? 0 : 255;
  }
  return out;
}
function overdrawAverages(info) {
  const pixels = info.width * info.height;
  return {
    perPixel: pixels > 0 ? info.fragments / pixels : 0,
    perCovered: info.coveredPixels > 0 ? info.fragments / info.coveredPixels : 0
  };
}
var OVERDRAW_MAGIC = "OVERDRAW 1\n";
function parseOverdrawFile(bytes) {
  const magic = new TextEncoder().encode(OVERDRAW_MAGIC);
  if (bytes.byteLength < magic.byteLength + 4 || magic.some((b, i) => bytes[i] !== b)) throw new Error("Not an overdraw file from vkinsp_replay.");
  const length = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(magic.byteLength, true);
  const start = magic.byteLength + 4;
  const base = start + length;
  if (base > bytes.byteLength) throw new Error("The overdraw file is truncated.");
  const manifest = JSON.parse(new TextDecoder().decode(bytes.subarray(start, base)));
  const measurements = (manifest.passes ?? []).map(({ payload, ...info }) => {
    let data = null;
    if (payload) {
      const [offset, size2] = payload;
      if (base + offset + size2 > bytes.byteLength) throw new Error("The overdraw file is truncated (counts out of range).");
      data = bytes.slice(base + offset, base + offset + size2);
    }
    return { info, data };
  });
  return { device: manifest.device ?? "", measurements, problems: manifest.problems ?? [] };
}

// src/renderer/mesh_output.ts
var MESH_MAGIC = "MESH 1\n";
function parseMeshFile(bytes) {
  const magic = new TextEncoder().encode(MESH_MAGIC);
  if (bytes.byteLength < magic.byteLength + 4 || magic.some((b, i) => bytes[i] !== b)) throw new Error("Not a mesh output file from vkinsp_replay.");
  const length = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(magic.byteLength, true);
  const start = magic.byteLength + 4;
  const base = start + length;
  if (base > bytes.byteLength) throw new Error("The mesh output file is truncated.");
  const manifest = JSON.parse(new TextDecoder().decode(bytes.subarray(start, base)));
  const draws = (manifest.draws ?? []).map(({ payload, ...info }) => {
    let data = null;
    if (payload) {
      const [offset, size2] = payload;
      if (base + offset + size2 > bytes.byteLength) throw new Error("The mesh output file is truncated (vertices out of range).");
      data = bytes.slice(base + offset, base + offset + size2);
    }
    return { ...info, outputs: info.outputs ?? [], data };
  });
  return { device: manifest.device ?? "", draws, problems: manifest.problems ?? [] };
}
function primitiveKind(topology) {
  if (/POINT/.test(topology)) return "points";
  if (/LINE/.test(topology)) return "lines";
  return "triangles";
}
function verticesPerPrimitive(kind) {
  return kind === "triangles" ? 3 : kind === "lines" ? 2 : 1;
}
function positionOutput(m) {
  return m.outputs.find((o) => o.builtin === "Position" && o.base === "float" && o.components === 4) ?? null;
}
function outputValues(m, output, vertex) {
  if (!m.data) return [];
  const view = new DataView(m.data.buffer, m.data.byteOffset, m.data.byteLength);
  const at = vertex * m.stride + output.offset;
  if (at + output.components * 4 > m.data.byteLength) return [];
  const out = [];
  for (let k = 0; k < output.components; k++) {
    const o = at + k * 4;
    out.push(output.base === "float" ? view.getFloat32(o, true) : output.base === "int" ? view.getInt32(o, true) : view.getUint32(o, true));
  }
  return out;
}
function clipPositions(m) {
  const p = positionOutput(m);
  if (!p || !m.data) return null;
  const out = new Float32Array(m.vertices * 4);
  const view = new DataView(m.data.buffer, m.data.byteOffset, m.data.byteLength);
  for (let v = 0; v < m.vertices; v++) {
    const at = v * m.stride + p.offset;
    if (at + 16 > m.data.byteLength) break;
    for (let k = 0; k < 4; k++) out[v * 4 + k] = view.getFloat32(at + k * 4, true);
  }
  return out;
}
function clipStats(m) {
  const clip2 = clipPositions(m);
  if (!clip2) return null;
  const kind = primitiveKind(m.topology);
  const per = verticesPerPrimitive(kind);
  const stats = { vertices: m.vertices, primitives: Math.floor(m.vertices / per), behind: 0, invalid: 0, outside: 0, degenerate: 0, ndc: null };
  const min = [Infinity, Infinity, Infinity];
  const max = [-Infinity, -Infinity, -Infinity];
  for (let v = 0; v < m.vertices; v++) {
    const [x, y, z, w] = clip2.subarray(v * 4, v * 4 + 4);
    if (![x, y, z, w].every(Number.isFinite)) {
      stats.invalid++;
      continue;
    }
    if (w <= 0) {
      stats.behind++;
      continue;
    }
    const ndc = [x / w, y / w, z / w];
    for (let k = 0; k < 3; k++) {
      min[k] = Math.min(min[k], ndc[k]);
      max[k] = Math.max(max[k], ndc[k]);
    }
  }
  if (min[0] <= max[0]) stats.ndc = { min, max };
  const planes = [
    (x, _y, _z, w) => x < -w,
    (x, _y, _z, w) => x > w,
    (_x, y, _z, w) => y < -w,
    (_x, y, _z, w) => y > w,
    (_x, _y, z) => z < 0,
    (_x, _y, z, w) => z > w
  ];
  for (let p = 0; p < stats.primitives; p++) {
    const at = p * per;
    const vertex = (i) => Array.from(clip2.subarray((at + i) * 4, (at + i) * 4 + 4));
    const vs = Array.from({ length: per }, (_, i) => vertex(i));
    if (planes.some((out) => vs.every(([x, y, z, w]) => out(x, y, z, w)))) {
      stats.outside++;
      continue;
    }
    if (kind === "triangles" && vs.every(([, , , w]) => w > 0)) {
      const s = vs.map(([x, y, , w]) => [x / w, y / w]);
      const area = (s[1][0] - s[0][0]) * (s[2][1] - s[0][1]) - (s[2][0] - s[0][0]) * (s[1][1] - s[0][1]);
      if (Math.abs(area) < 1e-12) stats.degenerate++;
    }
  }
  return stats;
}
function meshSummary(m) {
  if (!m.measured) return `Not captured: ${m.note ?? "the replay could not capture it"}`;
  const kind = primitiveKind(m.topology);
  const stats = clipStats(m);
  const parts2 = [`${m.vertices.toLocaleString()} vertices, ${Math.floor(m.vertices / verticesPerPrimitive(kind)).toLocaleString()} ${kind}`];
  if (m.truncated) parts2.push("truncated");
  if (!stats) {
    parts2.push("no gl_Position the replay could capture");
  } else {
    if (stats.outside) parts2.push(`${stats.outside.toLocaleString()} outside the view`);
    if (stats.behind) parts2.push(`${stats.behind.toLocaleString()} vertices behind the eye`);
    if (stats.degenerate) parts2.push(`${stats.degenerate.toLocaleString()} with no area`);
    if (stats.invalid) parts2.push(`${stats.invalid.toLocaleString()} NaN or infinite positions`);
  }
  return parts2.join(", ");
}

// src/mcp/tools.ts
var SEVERITIES = ["high", "medium", "low", "info"];
function unique(values) {
  return values.length ? [...new Set(values)] : void 0;
}
function frameTiming(c2) {
  const db = c2.db;
  const timings = [...c2.data.passTimings.values()];
  let start = Infinity;
  let end = -Infinity;
  for (const t of timings) {
    start = Math.min(start, t.startMs);
    end = Math.max(end, t.startMs + t.durationMs);
  }
  const gpuSpanMs = timings.length ? end - start : 0;
  const bound = timings.length ? frameBound({ frameMs: db.frameTimeMs, refreshMs: db.refreshMs, submitMs: db.submitMs, gpuSpanMs, frames: c2.data.frames }) : null;
  return {
    frameMs: round(db.frameTimeMs) || void 0,
    submitMs: round(db.submitMs) || void 0,
    refreshMs: round(db.refreshMs) || void 0,
    refreshSource: db.refreshSource ? REFRESH_SOURCE_NOTE[db.refreshSource] ?? db.refreshSource : void 0,
    frameBoundary: db.frameBoundary || void 0,
    profiled: timings.length > 0,
    gpuPassMs: timings.length ? round(c2.metrics.gpuMs) : void 0,
    gpuSpanMs: timings.length ? round(gpuSpanMs) : void 0,
    frameBound: bound ? { verdict: bound.verdict, budgetMs: round(bound.budgetMs), gpuMsPerFrame: round(bound.gpuMs) } : void 0
  };
}
function captureNotes(c2) {
  const d = c2.data;
  const notes = [];
  if (!d.passTimings.size) {
    notes.push('No pass timings: the capture was taken without "Profile passes", so it has no GPU times, no Frame Bound verdict and no GPU Bottlenecks report. Capture again with it on to profile.');
  } else if (!c2.metrics.withCounters) {
    notes.push(d.api === "metal" ? "The passes carry timestamps only (the GPU exposes no statistic counters through public Metal), so overdraw and fragments per primitive are not measured." : "The passes carry timestamps but no pipeline statistics (the device lacks pipelineStatisticsQuery, or the layer could not enable it), so overdraw and fragments per primitive are not measured.");
  }
  const failedImages = d.textures.filter((t) => t.info.error).length;
  if (failedImages) notes.push(`${failedImages} image read-backs failed (list_textures says why).`);
  if (!d.textures.length) notes.push("No render targets or images were read back.");
  const buffers = [...d.buffers.values()];
  const failedBuffers = buffers.filter((b) => b.info.error).length;
  if (failedBuffers) notes.push(`${failedBuffers} buffer read-backs failed.`);
  const truncated = buffers.filter((b) => b.info.originalSize).length;
  if (truncated) notes.push(`${truncated} buffer ranges were cut to the capture's buffer size limit.`);
  return notes;
}
function passBrief(c2, i) {
  const p = c2.metrics.passes[i];
  return { pass: i, label: c2.passName(i), command: p.commandIndex, ms: round(p.durationMs), draws: p.draws, bound: p.bound ?? void 0 };
}
function overdrawBrief(o) {
  if (!o) return void 0;
  const a = overdrawAverages(o);
  const histogram = {};
  (o.histogram ?? []).forEach((n, i) => {
    if (n) histogram[OVERDRAW_BUCKETS[i]] = n;
  });
  return {
    perPixel: round(a.perPixel),
    perCoveredPixel: round(a.perCovered),
    maxCount: o.maxCount,
    fragments: o.fragments,
    coveredPixels: o.coveredPixels,
    draws: o.draws,
    skippedDraws: o.skippedDraws || void 0,
    pixelsByCount: Object.keys(histogram).length ? histogram : void 0,
    note: o.note
  };
}
function passMeasurements(c2, p, i, gpuMs) {
  const problems = passAdvice(p);
  return {
    pass: i,
    label: c2.passName(i),
    command: p.commandIndex,
    kind: p.compute ? "compute" : "render",
    ms: round(p.durationMs),
    shareOfGpu: gpuMs > 0 && p.durationMs !== null ? round(p.durationMs / gpuMs) : void 0,
    vertexMs: round(p.vertexMs),
    fragmentMs: round(p.fragmentMs),
    draws: p.draws,
    vertices: p.vertices || void 0,
    pixels: p.pixels || void 0,
    overdraw: round(p.overdraw),
    overdrawSource: p.overdrawSource ?? void 0,
    measuredOverdraw: p.measuredOverdraw ? { depthTested: overdrawBrief(p.measuredOverdraw.depthTested), rasterized: overdrawBrief(p.measuredOverdraw.rasterized) } : void 0,
    fragmentsPerPrimitive: round(p.fragmentsPerPrimitive),
    depthRejectRate: round(p.depthRejectRate),
    depthRejectSource: p.depthRejectSource ?? void 0,
    nsPerVertex: round(p.nsPerVertex),
    nsPerFragment: round(p.nsPerFragment),
    cycleShare: p.cycleShare ? { vertex: round(p.cycleShare.vertex), fragment: round(p.cycleShare.fragment), target: round(p.cycleShare.target) } : void 0,
    bound: p.bound ?? void 0,
    boundReason: p.boundReason || void 0,
    problems: problems.length ? problems : void 0
  };
}
function pixelHistoryAnswer(c2, h, all, extra) {
  const events = h.events.filter((e) => all || touchesPixel(e));
  const value = (format, bytes, depth) => {
    if (!bytes.byteLength) return void 0;
    const v = texelValues(format, bytes, depth);
    return v ? v.map(tidy) : [...bytes].map((b) => b.toString(16).padStart(2, "0")).join("");
  };
  return {
    capture: c2.id,
    image: refText(c2.db, h.image),
    x: h.x,
    y: h.y,
    mip: h.mip,
    layer: h.layer,
    pixelFormat: h.pixelFormat || void 0,
    depthFormat: h.depthFormat || void 0,
    events: events.map((e) => {
      const pass = c2.passOf(e.command);
      return {
        command: e.command,
        kind: e.kind,
        what: eventSummary(e),
        outcome: e.kind === "draw" ? drawOutcome(e) : void 0,
        pass: pass >= 0 ? c2.passName(pass) : void 0,
        pipeline: e.pipeline ? refText(c2.db, e.pipeline) : void 0,
        samples: e.kind === "draw" && e.testsMeasured ? { covering: e.covered, facing: e.facing, shaded: e.shaded, passingDepth: e.depthPassed, passingStencil: e.stencilPassed, passingAll: e.passed } : void 0,
        valueAfter: value(h.pixelFormat, e.value, false),
        depthAfter: value(h.depthFormat, e.depth, true)
      };
    }),
    drawsNotReachingThePixel: all ? void 0 : h.events.length - events.length || void 0,
    notes: h.notes.length ? h.notes : void 0,
    ...extra
  };
}
function captureSummary(c2) {
  const d = c2.data;
  const db = c2.db;
  const sets = d.sets;
  let draws = 0;
  let dispatches = 0;
  for (const cmd of d.commands) {
    if (sets.DRAW.has(cmd.method)) draws++;
    else if (sets.DISPATCH.has(cmd.method)) dispatches++;
  }
  const passes = c2.metrics.passes;
  const findings = c2.analysis.findings;
  const bySeverity = {};
  for (const f of findings) bySeverity[f.severity] = (bySeverity[f.severity] ?? 0) + 1;
  const [errors, warnings] = db.validationCounts;
  const sampled = d.textures.filter((t) => t.info.kind === "sampled").length;
  const g = c2.graph;
  const slowest = passes.map((p, i) => ({ p, i })).filter((x) => x.p.durationMs !== null).sort((a, b) => (b.p.durationMs ?? 0) - (a.p.durationMs ?? 0)).slice(0, 5);
  return {
    capture: c2.id,
    file: c2.path,
    application: c2.manifest.source?.name || void 0,
    api: d.api,
    savedAt: c2.manifest.savedAt,
    frame: d.frame,
    frames: d.frames,
    counts: {
      commands: d.commands.length,
      draws,
      dispatches,
      renderPasses: passes.filter((p) => !p.compute).length,
      computePasses: passes.filter((p) => p.compute).length,
      objects: db.allObjects.size + db.destroyedObjects.size,
      pipelinesUsed: pipelineUses(d).size,
      renderTargets: d.textures.length - sampled,
      sampledImages: sampled,
      bufferRanges: d.buffers.size
    },
    timing: frameTiming(c2),
    slowestPasses: slowest.length ? slowest.map((x) => passBrief(c2, x.i)) : void 0,
    issues: { total: findings.length, bySeverity, top: findings.slice(0, 8).map((f) => findingBrief(c2, f)) },
    validation: {
      errors,
      warnings,
      total: db.validation.length,
      first: db.validation.length ? db.validation.slice(0, 5).map((v) => validationBrief(c2, v, 400)) : void 0
    },
    renderGraph: {
      passes: g.nodes.length,
      resources: g.resources.length,
      externalInputs: g.externalInputs.length,
      unreadPasses: g.unreadNodes.length,
      criticalPathMs: round(g.criticalPathMs) || void 0,
      warnings: g.warnings.length ? g.warnings : void 0
    },
    statistics: Object.fromEntries(c2.statistics.sections().map((s) => [s.title, Object.fromEntries(s.rows.filter((r) => r.value).map((r) => [r.label, r.value]))])),
    notes: captureNotes(c2)
  };
}
function nodeDetail(c2, n) {
  const db = c2.db;
  const resource2 = (r) => ({ resource: r.label, detail: r.detail || void 0, object: refText(db, r.objectId), key: r.key });
  return {
    capture: c2.id,
    node: n.ordinal,
    label: n.label,
    kind: n.kind,
    command: n.commandIndex,
    ms: round(n.durationMs),
    draws: n.draws || void 0,
    pathMs: round(n.pathMs) || void 0,
    unread: n.unread || void 0,
    unresolvedReads: n.unresolvedReads || void 0,
    reads: n.reads.map((u) => ({
      ...resource2(u.resource),
      usage: u.usage,
      version: u.version.index,
      from: u.version.producer ? u.version.producer.ordinal : "before the capture"
    })),
    writes: n.writes.map((u) => ({
      ...resource2(u.resource),
      usage: u.usage,
      version: u.version.index,
      readBy: u.version.readers.map((r) => r.ordinal),
      replacesContents: u.discards || void 0,
      discardedByStoreOp: u.dropped || void 0,
      presented: u.resource.presented || void 0
    }))
  };
}
function passKeys(c2) {
  const seen = /* @__PURE__ */ new Map();
  const out = /* @__PURE__ */ new Map();
  c2.metrics.passes.forEach((p, i) => {
    const base = `${p.compute ? "compute" : "render"}|${c2.labelsOf(p.commandIndex)}|${p.label.replace(/^[A-Za-z ]+ \d+:?\s*/, "")}`;
    const n = seen.get(base) ?? 0;
    seen.set(base, n + 1);
    out.set(`${base}#${n}`, i);
  });
  return out;
}
function change(before, after) {
  if ((before ?? null) === null && (after ?? null) === null) return void 0;
  const out = { before: round(before), after: round(after) };
  if (typeof before === "number" && typeof after === "number") {
    out.change = round(after - before);
    if (before) out.percent = round((after - before) / before * 100);
  }
  return out;
}
function captureTools(store) {
  return [
    {
      name: "open_capture",
      description: `Open a GPU Inspector capture file (.gpucap: a Vulkan or Metal frame saved from GPU Inspector's capture bar) and return its summary. The capture stays open under the returned id ("cap-1") for the other tools; opening an unchanged file again returns the capture already open.`,
      inputSchema: schema({ path: { type: "string", description: "Path of the .gpucap file." } }, ["path"]),
      readOnly: true,
      handler: (args) => {
        const { capture, reused } = store.open(requireString(args, "path"));
        return jsonResult({ ...captureSummary(capture), reused: reused || void 0 });
      }
    },
    {
      name: "list_captures",
      description: "List the captures open in this server, and the capture files GPU Inspector opened or saved most recently (from its settings), so a capture can be found without asking for its path.",
      inputSchema: schema({}),
      readOnly: true,
      handler: () => {
        const open = store.list();
        const recent = [...new Set(recentCaptureFiles().map((p) => path10.normalize(p)))];
        return jsonResult({
          open: open.map((c2) => ({
            capture: c2.id,
            file: c2.path,
            application: c2.manifest.source?.name || void 0,
            api: c2.data.api,
            frame: c2.data.frame,
            frames: c2.data.frames > 1 ? c2.data.frames : void 0,
            commands: c2.data.commands.length,
            megabytes: round(c2.fileBytes / 1048576)
          })),
          recent: recent.map((file) => ({
            file,
            missing: fs11.existsSync(file) ? void 0 : true,
            open: open.find((c2) => c2.path === path10.resolve(file))?.id
          })),
          note: recent.length ? void 0 : `No recent captures in ${settingsFile()}.`
        });
      }
    },
    {
      name: "set_search_paths",
      description: "Where to look on this machine for what captures only name. sourceRoots: the directories holding the shader sources, for shaders compiled with line information but no embedded text (dxc -Zi, glslc without -g, stripped builds), so get_shader shows their source and the analyses quote their costliest lines. symbolDirs: the directories holding the application's unstripped libraries (the build tree), so stack frames named only by module and offset (Android, Linux) resolve to functions, files and lines. A list replaces the previous one for this server; an empty list goes back to GPU_INSPECTOR_SOURCE_ROOTS / GPU_INSPECTOR_SYMBOL_DIRS, else the directories GPU Inspector's launch dialog used last. Without arguments it shows what is in effect.",
      inputSchema: schema({
        sourceRoots: { type: "array", items: { type: "string" }, description: "Directories searched (six levels deep) for the shader files debug information names." },
        symbolDirs: { type: "array", items: { type: "string" }, description: "Directories searched (five levels deep) for the libraries stack frames name." }
      }),
      handler: (args) => {
        if (args.sourceRoots !== void 0) setSearchPaths("sourceRoots", splitPaths(args.sourceRoots));
        if (args.symbolDirs !== void 0) setSearchPaths("symbolDirs", splitPaths(args.symbolDirs));
        return jsonResult(describeSearchPaths());
      }
    },
    {
      name: "close_capture",
      description: "Close an open capture and free its memory (captures with many read-back images can be hundreds of megabytes).",
      inputSchema: schema({ capture: { type: "string", description: "The capture's id or file path." } }, ["capture"]),
      handler: (args) => jsonResult({ closed: store.close(requireString(args, "capture")) })
    },
    {
      name: "get_capture_summary",
      description: "Summarize a capture: counts (commands, draws, passes, objects, read-backs), the frame timing with the Frame Bound verdict (GPU bound, CPU bound, vsync bound) when the passes were profiled, the slowest passes, the Frame Issues by severity with the top ones, validation messages, the render graph in numbers, frame statistics, and notes on what the capture lacks. Start here.",
      inputSchema: schema({ capture: CAPTURE_PARAM }),
      readOnly: true,
      handler: (args) => jsonResult(captureSummary(store.resolve(stringArg(args, "capture"))))
    },
    {
      name: "get_frame_issues",
      description: "The Frame Issues of a capture: the rules GPU Inspector runs over the frame (attachment load and store ops, clears outside passes, transient and memoryless candidates, MSAA stores, one pass per eye without multiview, redundant binds, barriers, tiny draws, overdraw and microtriangles from the GPU counters, unmipped textures, and the render graph's unread stores, overwritten results and mergeable passes). Each finding names the command it is about; a finding over many commands names the first and counts the rest. Worst first.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        severity: { type: "string", enum: SEVERITIES, description: "The lowest severity to list (default info: all)." },
        rule: { type: "string", description: 'Only this rule, by name ("tiny-draws").' },
        ...PAGE_PARAMS
      }),
      readOnly: true,
      handler: (args) => {
        const c2 = store.resolve(stringArg(args, "capture"));
        const min = SEVERITY_RANK[enumArg(args, "severity", SEVERITIES, "info")];
        const rule = stringArg(args, "rule");
        const all = c2.analysis.findings;
        const rules = {};
        for (const f of all) {
          const r = rules[f.rule] ??= { severity: f.severity, findings: 0, commands: 0 };
          if (SEVERITY_RANK[f.severity] > SEVERITY_RANK[r.severity]) r.severity = f.severity;
          r.findings++;
          r.commands += f.count;
        }
        const list = all.filter((f) => SEVERITY_RANK[f.severity] >= min && (!rule || f.rule === rule));
        const p = page(list, args, 50, 200);
        return jsonResult({ capture: c2.id, total: p.total, offset: p.offset, nextOffset: p.nextOffset, rules, findings: p.items.map((f) => findingBrief(c2, f)) });
      }
    },
    {
      name: "get_bottlenecks",
      description: 'The GPU Bottlenecks report: every timed pass, slowest first, measured the way a bottleneck is described \u2014 GPU time and share of the frame, draws and vertices, overdraw (fragment shader runs per target pixel), fragments per primitive (microtriangles below 4), depth rejection and the vertex/fragment split (Metal), which stage the pass is bound by, and each measured problem with what usually causes it. Needs a capture taken with "Profile passes"; the counters also need a GPU that exposes them. docs/PROFILING.md in GPU Inspector is the method behind it.',
      inputSchema: schema({ capture: CAPTURE_PARAM, ...PAGE_PARAMS }),
      readOnly: true,
      handler: (args) => {
        const c2 = store.resolve(stringArg(args, "capture"));
        const m = c2.metrics;
        if (!m.passes.length) return jsonResult({ capture: c2.id, note: "The capture has no passes." });
        if (!m.timed) {
          return jsonResult({
            capture: c2.id,
            passes: m.passes.length,
            note: 'No pass was timed: the capture was taken without "Profile passes". The timings and counters this report reads are sampled during the capture and cannot be recovered afterwards; capture again with Profile passes on.' + (c2.data.overdraw.length ? " The capture did measure overdraw: get_overdraw has it." : "")
          });
        }
        const ranked = m.passes.map((p2, i) => ({ p: p2, i })).filter((x) => x.p.durationMs !== null).sort((a, b) => (b.p.durationMs ?? 0) - (a.p.durationMs ?? 0));
        const slowest = ranked[0];
        const metal = c2.data.api === "metal";
        const notes = [];
        if (!m.withCounters) {
          notes.push(metal ? "The GPU exposes only the timestamp counter set through public Metal, so overdraw, fragments per primitive and depth rejection are not measured." : "No pass carried pipeline statistics (the device may lack pipelineStatisticsQuery), so overdraw and fragments per primitive are not measured.");
        } else {
          notes.push(`${m.withCounters} of ${m.timed} timed passes carried counters.`);
        }
        if (!metal) notes.push("The vertex/fragment split is Metal only: Vulkan has no portable stage-boundary timestamps. Depth rejection comes from an occlusion query the layer runs around each pass, which it skips where the application has a query of its own open.");
        const totals = m.totals;
        const p = page(ranked, args, 30, 200);
        return jsonResult({
          capture: c2.id,
          api: c2.data.api,
          verdict: frameStageVerdict(m),
          gpuMs: round(m.gpuMs),
          vertexMs: round(m.vertexMs) || void 0,
          fragmentMs: round(m.fragmentMs) || void 0,
          timedPasses: m.timed,
          untimedPasses: m.passes.length - m.timed || void 0,
          totals: totals ? {
            vertexInvocations: totals.vertexInvocations,
            fragmentInvocations: totals.fragmentInvocations,
            primitives: totals.primitives,
            fragmentsPerPrimitive: totals.primitives ? round(totals.fragmentInvocations / totals.primitives) : void 0
          } : void 0,
          thresholds: { healthyOverdraw: HEALTHY_OVERDRAW, overdrawFlaggedAbove: OVERDRAW_LIMIT, microtrianglesBelow: MICROTRIANGLE_LIMIT, lowDepthRejectionBelow: LOW_REJECTION_RATE },
          slowest: slowest ? {
            pass: slowest.i,
            label: c2.passName(slowest.i),
            ms: round(slowest.p.durationMs),
            bound: slowest.p.bound ? BOUND_LABEL[slowest.p.bound] : void 0,
            reason: slowest.p.boundReason || void 0,
            firstThingToTry: slowest.p.bound ? BOUND_ADVICE[slowest.p.bound] : void 0
          } : void 0,
          total: p.total,
          offset: p.offset,
          nextOffset: p.nextOffset,
          passes: p.items.map((x) => passMeasurements(c2, x.p, x.i, m.gpuMs)),
          notes
        });
      }
    },
    {
      name: "get_overdraw",
      description: "Overdraw measured per pixel: every render pass drawn a second time with a counting fragment shader. A Metal capture measures it while it is taken (capture_frames overdraw: true); a Vulkan capture is replayed on this machine's GPU with vkinsp_replay the first time this is called (seconds to minutes, and it needs the tool built). Without `pass`: every measured pass, worst first, with the fragments that passed its depth and stencil tests and every fragment it rasterized \u2014 per pixel, per covered pixel, the maximum and pixels by count. With `pass` (get_bottlenecks' pass numbers): that pass's heatmap as a PNG (black none, dark blue 1, blue 2, teal 3, green 4, yellow 5-6, orange 7-10, red 11-16, magenta 17-32, white 33 and more) and the counts at `texels`. Discarded fragments are counted, since the counting shader does not discard.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        pass: { type: "integer", minimum: 0, description: "A render pass: its heatmap and the counts at `texels`." },
        depthTested: { type: "boolean", description: "With pass: the fragments that passed depth and stencil (default true), or every rasterized fragment." },
        image: { type: "boolean", description: "With pass: return the PNG (default true)." },
        maxSize: { type: "integer", minimum: 16, maximum: 2048, description: "Longest side of the returned image in pixels (default 512)." },
        texels: { type: "array", items: { type: "array", items: { type: "integer" }, minItems: 2, maxItems: 2 }, description: "With pass: [x, y] pixels to read the count of (up to 64)." },
        ...PAGE_PARAMS
      }),
      readOnly: true,
      handler: async (args) => {
        const c2 = store.resolve(stringArg(args, "capture"));
        let replayNote;
        if (!c2.data.overdraw.length && c2.data.api !== "metal") {
          const tool = findReplayTool(checkoutRoots(), installedLayerDirs());
          if (!tool) return jsonResult({ capture: c2.id, note: `A Vulkan capture's overdraw is measured by replaying it on this machine's GPU, and ${NO_REPLAY_TOOL}` });
          const run2 = await replayServers.run(tool, c2.path, { kind: "overdraw" });
          if (!run2.data) return jsonResult({ capture: c2.id, note: `The replay could not measure overdraw: ${run2.error ?? "no data"}` });
          const file = parseOverdrawFile(run2.data);
          c2.setOverdraw(file.measurements);
          replayNote = `Measured by replaying the capture on ${file.device || "this machine's GPU"} (vkinsp_replay).` + (file.problems.length ? ` ${file.problems.length} parts of the capture could not be replayed; passes that depend on them are missing or may differ from the frame.` : "");
        }
        if (!c2.data.overdraw.length) {
          return jsonResult({
            capture: c2.id,
            note: c2.data.api === "metal" ? "The capture did not measure overdraw. Capture again with capture_frames overdraw: true." : `The replay measured no pass. ${replayNote ?? ""}`
          });
        }
        const passes = c2.metrics.passes;
        const passArg = optionalInt(args, "pass");
        if (passArg === void 0) {
          const perPixel = (x) => {
            const o2 = x.p.measuredOverdraw?.depthTested ?? x.p.measuredOverdraw?.rasterized;
            return o2 ? overdrawAverages(o2).perPixel : 0;
          };
          const ranked = passes.map((p2, i) => ({ p: p2, i })).filter((x) => x.p.measuredOverdraw).sort((a, b) => perPixel(b) - perPixel(a));
          const unmeasured = c2.data.overdraw.filter((o2) => o2.info.measured === false);
          const pg = page(ranked, args, 30, 200);
          return jsonResult({
            capture: c2.id,
            measuredBy: replayNote,
            healthyOverdraw: HEALTHY_OVERDRAW,
            overdrawFlaggedAbove: OVERDRAW_LIMIT,
            total: pg.total,
            offset: pg.offset,
            nextOffset: pg.nextOffset,
            passes: pg.items.map((x) => ({
              pass: x.i,
              label: c2.passName(x.i),
              command: x.p.commandIndex,
              depthTested: overdrawBrief(x.p.measuredOverdraw.depthTested),
              rasterized: overdrawBrief(x.p.measuredOverdraw.rasterized)
            })),
            notMeasured: unmeasured.length ? unmeasured.slice(0, 20).map((o2) => ({ commandBuffer: o2.info.commandBuffer, passIndex: o2.info.passIndex, depthTested: o2.info.depthTested, note: o2.info.note })) : void 0
          });
        }
        const p = passes[passArg];
        if (!p) throw new Error(`No pass ${passArg}: the capture has ${passes.length} (get_bottlenecks lists them).`);
        if (p.compute) throw new Error(`Pass ${passArg} (${c2.passName(passArg)}) is a compute pass, which has no overdraw. get_overdraw without pass lists the render passes.`);
        const depthTested = boolArg(args, "depthTested", true);
        const o = c2.data.overdrawForPass(p.frame, p.commandBuffer, p.passIndex).find((m) => m.info.depthTested === depthTested);
        if (!o) throw new Error(`Pass ${passArg} (${c2.passName(passArg)}) has no overdraw measurement.`);
        const requested = Array.isArray(args.texels) ? args.texels.slice(0, 64) : [];
        const texels = requested.map((pt) => {
          const x = Array.isArray(pt) ? Number(pt[0]) : NaN;
          const y = Array.isArray(pt) ? Number(pt[1]) : NaN;
          if (!(x >= 0 && x < o.info.width && y >= 0 && y < o.info.height)) throw new Error(`pixel [${String(pt)}] is outside the ${o.info.width}x${o.info.height} pass.`);
          return { x: Math.floor(x), y: Math.floor(y), count: overdrawCount(o, Math.floor(x), Math.floor(y)) };
        });
        const result = jsonResult({
          capture: c2.id,
          pass: passArg,
          label: c2.passName(passArg),
          command: p.commandIndex,
          depthTested,
          width: o.info.width,
          height: o.info.height,
          ...overdrawBrief(o.info),
          texels: texels.length ? texels : void 0,
          note: o.data ? o.info.note : [o.info.note, "The per-pixel counts were not kept, so there is no heatmap."].filter(Boolean).join(" ")
        });
        const rgba = overdrawRgba(o);
        if (rgba && boolArg(args, "image", true)) {
          const fit = fitPixels(rgba, o.info.width, o.info.height, intArg(args, "maxSize", 512, 16, 2048));
          result.content.unshift({ type: "image", data: Buffer.from(encodePng(fit.rgba, fit.width, fit.height)).toString("base64"), mimeType: "image/png" });
        }
        return result;
      }
    },
    {
      name: "get_pixel_history",
      description: `A pixel's history, the way RenderDoc gives it: every pass start, clear and draw of the frame that touched one pixel of a render target, what each draw's fragments at the pixel met (outside the scissor, culled, discarded by the fragment shader, failed the depth or stencil test, or wrote the pixel, with sample counts), and the pixel's value and depth after each. A Vulkan capture is replayed on this machine's GPU with vkinsp_replay (under a second; later questions about the same capture are quicker); a Metal application follows the pixel while it captures, so a Metal capture answers for the pixel capture_frames' pixelHistory named. Name the image by id (list_textures lists the render targets), or by pass and attachment. Use it for "why is this pixel this colour": the last draw that wrote it, and the draws that should have but were culled or failed a test.`,
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        image: { type: "integer", description: "The image's object id." },
        pass: { type: "integer", minimum: 0, description: "Instead of image: a render pass (get_bottlenecks' numbers) whose attachment to follow." },
        attachment: { type: "integer", minimum: 0, description: "With pass: the colour attachment index (default 0)." },
        x: { type: "integer", minimum: 0, description: "The pixel's column, at the mip level." },
        y: { type: "integer", minimum: 0, description: "The pixel's row, at the mip level." },
        mip: { type: "integer", minimum: 0, description: "The mip level the pass renders to (default: the read-back target's, else 0)." },
        layer: { type: "integer", minimum: 0, description: "The array layer (default 0)." },
        allDraws: { type: "boolean", description: "Also list the draws that do not reach the pixel (default false: they are only counted)." }
      }),
      readOnly: true,
      handler: async (args) => {
        const c2 = store.resolve(stringArg(args, "capture"));
        if (c2.data.api === "metal") {
          if (!c2.data.pixelHistory) {
            return jsonResult({
              capture: c2.id,
              note: "This Metal capture did not follow a pixel. A Metal application follows one while it captures: capture_frames with pixelHistory { texture, x, y } (a render target's texture id from list_textures; a drawable's follows the next frame's drawable), then get_pixel_history on that capture."
            });
          }
          const h2 = parsePixelHistory(c2.data.pixelHistory);
          const askedImage = optionalInt(args, "image");
          const askedX = optionalInt(args, "x");
          const askedY = optionalInt(args, "y");
          const other = askedImage !== void 0 && askedImage !== h2.image && askedImage !== h2.requestedImage || askedX !== void 0 && askedX !== h2.x || askedY !== void 0 && askedY !== h2.y;
          return jsonResult(pixelHistoryAnswer(c2, h2, boolArg(args, "allDraws", false), {
            followed: other ? `This capture followed pixel (${h2.x}, ${h2.y}) of ${refText(c2.db, h2.image)}, not the one asked for: a Metal capture answers for the pixel it was taken with (capture_frames pixelHistory follows another).` : void 0,
            requestedImage: h2.requestedImage !== h2.image ? `${refText(c2.db, h2.requestedImage)} (the frame rendered into its own drawable, which was followed instead)` : void 0,
            measuredOn: h2.device || void 0,
            method: "While capturing, the Metal library issued each draw again in the application's own command buffer after its pass, under visibility results in counting mode with a one-pixel scissor and pipeline and depth-stencil copies that add one step at a time (coverage, culling, the fragment shader, the depth and stencil tests), against copies of the pass's attachments, with depth and stencil writes off. Counts are samples."
          }));
        }
        let image = optionalInt(args, "image");
        let mip = optionalInt(args, "mip");
        if (image === void 0) {
          const passArg = optionalInt(args, "pass");
          if (passArg === void 0) throw new Error("Name the pixel's image: image (an object id), or pass and attachment.");
          const p = c2.metrics.passes[passArg];
          if (!p || p.compute) throw new Error(`Pass ${passArg} is not a render pass (get_bottlenecks lists the passes).`);
          const attachment = intArg(args, "attachment", 0, 0);
          const tex = c2.data.texturesForPass(p.frame, p.commandBuffer, p.passIndex).find((t) => t.info.attachment === attachment && t.info.aspect === "color" && !t.info.resolve);
          if (!tex) throw new Error(`Pass ${passArg} (${c2.passName(passArg)}) has no colour attachment ${attachment} read back (list_textures lists the render targets).`);
          image = tex.info.id;
          mip ??= tex.info.mip;
        }
        const x = requireInt(args, "x");
        const y = requireInt(args, "y");
        const tool = findReplayTool(checkoutRoots(), installedLayerDirs());
        if (!tool) return jsonResult({ capture: c2.id, note: `Pixel history replays the capture on this machine's GPU, and ${NO_REPLAY_TOOL}` });
        const run2 = await replayServers.run(tool, c2.path, { kind: "pixel", image, x, y, mip: mip ?? 0, layer: intArg(args, "layer", 0, 0) });
        if (!run2.data) return jsonResult({ capture: c2.id, note: `The replay could not follow the pixel: ${run2.error ?? "no data"}` });
        const h = parsePixelHistory(run2.data);
        return jsonResult(pixelHistoryAnswer(c2, h, boolArg(args, "allDraws", false), {
          replayedOn: h.device || void 0,
          replayProblems: h.problems.length ? { count: h.problems.length, first: h.problems.slice(0, 10) } : void 0,
          method: "Each draw is issued again under occlusion queries with a one-pixel scissor and pipeline copies that add one step at a time (coverage, culling, the fragment shader, the depth and stencil tests), against what the pass held before the draw, with depth and stencil writes off. Counts are samples: two overlapping triangles of one draw that both pass count twice."
        }));
      }
    },
    {
      name: "get_mesh_output",
      description: `What a draw's vertex shader wrote, the way RenderDoc's mesh viewer gives VS Out, for "why can I not see this mesh": a Vulkan capture is replayed on this machine's GPU with the draw's vertex shader writing transform feedback (under a second, quicker for later draws of the same capture). Gives every output captured (gl_Position and each located output, named from the shader), how many vertices are behind the eye (w <= 0), how many primitives lie entirely outside the view volume, how many triangles have no area on screen, NaN positions, the normalized device coordinates the rest span, and vertices' values. Vertices are the ones the draw assembled: an indexed draw's in index order, strips and fans as lists, every instance. read_vertices gives what the draw read (VS In).`,
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        command: { type: "integer", minimum: 0, description: "The draw command's index." },
        first: { type: "integer", minimum: 0, description: "The first vertex to list (default 0)." },
        count: { type: "integer", minimum: 0, maximum: 256, description: "Vertices to list (default 8)." }
      }, ["command"]),
      readOnly: true,
      handler: async (args) => {
        const c2 = store.resolve(stringArg(args, "capture"));
        const index = requireInt(args, "command");
        const cmd = c2.data.commands[index];
        if (!cmd || !c2.data.sets.DRAW.has(cmd.method)) throw new Error(`Command ${index} is not a draw: get_mesh_output takes a draw command (list_commands with kind draw).`);
        if (c2.data.api === "metal") {
          return jsonResult({ capture: c2.id, command: index, note: "A Metal draw's vertex function outputs need a replay, which Metal captures do not have yet; read_vertices gives what the draw read." });
        }
        const tool = findReplayTool(checkoutRoots(), installedLayerDirs());
        if (!tool) return jsonResult({ capture: c2.id, note: `The mesh output replays the capture on this machine's GPU, and ${NO_REPLAY_TOOL}` });
        const run2 = await replayServers.run(tool, c2.path, { kind: "mesh", commands: [index] });
        if (!run2.data) return jsonResult({ capture: c2.id, command: index, note: `The replay could not capture the draw's vertices: ${run2.error ?? "no data"}` });
        const file = parseMeshFile(run2.data);
        const m = file.draws.find((d) => d.command === index);
        if (!m || !m.measured) return jsonResult({ capture: c2.id, command: index, method: cmd.method, note: `Not captured: ${m?.note ?? "the replay did not reach the draw"}` });
        const stats = clipStats(m);
        const first = intArg(args, "first", 0, 0);
        const count2 = intArg(args, "count", 8, 0, 256);
        const values = [];
        for (let v = first; v < Math.min(m.vertices, first + count2); v++) {
          const row = { vertex: v };
          for (const o of m.outputs) row[o.name] = outputValues(m, o, v).map(tidy);
          values.push(row);
        }
        return jsonResult({
          capture: c2.id,
          command: index,
          method: cmd.method,
          topology: m.topology,
          summary: meshSummary(m),
          vertices: m.vertices,
          bytesPerVertex: m.stride,
          truncated: m.truncated || void 0,
          outputs: m.outputs.map((o) => ({ name: o.name, builtin: o.builtin, location: o.location, type: `${o.components} ${o.base}`, offset: o.offset })),
          clipSpace: stats ? {
            primitives: stats.primitives,
            behindEye: stats.behind,
            outsideViewVolume: stats.outside,
            noArea: stats.degenerate || void 0,
            nanOrInfinite: stats.invalid || void 0,
            ndcInFront: stats.ndc ? { min: stats.ndc.min.map((x) => round(x)), max: stats.ndc.max.map((x) => round(x)) } : void 0
          } : void 0,
          values,
          note: m.note,
          replayedOn: file.device || void 0,
          replayProblems: file.problems.length ? { count: file.problems.length, first: file.problems.slice(0, 10) } : void 0,
          measuredBy: "The pass's state is issued again after the replay has run it, then the draw alone with a copy of its pipeline whose vertex shader is edited to write its outputs to a transform feedback buffer, with rasterization discarded. Pipelines with tessellation or geometry stages are not captured."
        });
      }
    },
    {
      name: "get_render_graph",
      description: "The capture's render graph: its passes (nodes, numbered in execution order) with the passes they read from and write for, the critical path by GPU time, resources read from before the capture, passes whose output nothing reads, and the graph rules' suggestions. With `node`, one pass in full: every resource it reads (and which pass produced that version) and writes (and which passes read it). Node numbers are the graph's own, not get_bottlenecks' pass numbers.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        node: { type: "integer", minimum: 0, description: "One node to show in full." },
        ...PAGE_PARAMS
      }),
      readOnly: true,
      handler: (args) => {
        const c2 = store.resolve(stringArg(args, "capture"));
        const g = c2.graph;
        const nodeIndex = optionalInt(args, "node");
        if (nodeIndex !== void 0) {
          const n = g.nodes.find((x) => x.ordinal === nodeIndex);
          if (!n) throw new Error(`No node ${nodeIndex}: the graph has ${g.nodes.length} nodes.`);
          return jsonResult(nodeDetail(c2, n));
        }
        const critical = new Set(g.criticalPath.map((n) => n.ordinal));
        const suggestions = analyzeRenderGraph(g).findings;
        const p = page(g.nodes, args, 100, 500);
        return jsonResult({
          capture: c2.id,
          nodes: g.nodes.length,
          resources: g.resources.length,
          edges: g.edges.length,
          criticalPath: g.criticalPath.map((n) => n.ordinal),
          criticalPathMs: round(g.criticalPathMs) || void 0,
          warnings: g.warnings.length ? g.warnings : void 0,
          externalInputs: g.externalInputs.length ? g.externalInputs.slice(0, 40).map((r) => r.detail ? `${r.label} (${r.detail})` : r.label) : void 0,
          unreadNodes: unique(g.unreadNodes.map((n) => n.ordinal)),
          suggestions: suggestions.length ? suggestions.map((f) => findingBrief(c2, f)) : void 0,
          offset: p.offset,
          nextOffset: p.nextOffset,
          list: p.items.map((n) => ({
            node: n.ordinal,
            label: n.label,
            kind: n.kind,
            command: n.commandIndex,
            frame: c2.data.frames > 1 ? n.frame : void 0,
            ms: round(n.durationMs),
            draws: n.draws || void 0,
            reads: n.reads.length,
            writes: n.writes.length,
            inputsFrom: unique(n.inputs.map((e) => e.from.ordinal)),
            outputsTo: unique(n.outputs.map((e) => e.to.ordinal)),
            unread: n.unread || void 0,
            critical: critical.has(n.ordinal) || void 0,
            unresolvedReads: n.unresolvedReads || void 0
          }))
        });
      }
    },
    {
      name: "compare_captures",
      description: "Compare two captures of the same application, before and after a change: frame, submit and GPU time, the statistics that differ, Frame Issues by rule, validation counts, and per pass (matched by debug groups and label) the GPU time, draws, overdraw and fragments per primitive, largest change first. Confirms whether a fix moved anything.",
      inputSchema: schema({
        before: { type: "string", description: "The capture before the change: an open capture's id or a .gpucap path." },
        after: { type: "string", description: "The capture after the change: an open capture's id or a .gpucap path." }
      }, ["before", "after"]),
      readOnly: true,
      handler: (args) => {
        const a = store.resolve(requireString(args, "before"));
        const b = store.resolve(requireString(args, "after"));
        const sa = a.statistics;
        const sb = b.statistics;
        const counts = {};
        const keys = [
          "apiCalls",
          "submits",
          "draws",
          "dispatches",
          "renderPasses",
          "computePasses",
          "bindPipeline",
          "uniquePipelines",
          "descriptorSetsBound",
          "uniqueDescriptorSets",
          "totalVertices",
          "totalTriangles",
          "updateBufferBytes",
          "bufferCopyBytes"
        ];
        for (const key of keys) if (sa[key] !== sb[key]) counts[key] = change(sa[key], sb[key]);
        const ka = passKeys(a);
        const kb = passKeys(b);
        const rows = [];
        for (const [key, ia] of ka) {
          const ib = kb.get(key);
          if (ib === void 0) continue;
          const pa = a.metrics.passes[ia];
          const pb = b.metrics.passes[ib];
          rows.push({
            label: b.passName(ib),
            before: ia,
            after: ib,
            ms: change(pa.durationMs, pb.durationMs),
            draws: pa.draws !== pb.draws ? change(pa.draws, pb.draws) : void 0,
            overdraw: change(pa.overdraw, pb.overdraw),
            fragmentsPerPrimitive: change(pa.fragmentsPerPrimitive, pb.fragmentsPerPrimitive)
          });
        }
        const magnitude = (r) => Math.abs(typeof r.ms?.change === "number" ? r.ms.change : 0);
        rows.sort((x, y) => magnitude(y) - magnitude(x));
        const ruleCounts = (c2) => {
          const out = /* @__PURE__ */ new Map();
          for (const f of c2.analysis.findings) out.set(f.rule, (out.get(f.rule) ?? 0) + f.count);
          return out;
        };
        const ra = ruleCounts(a);
        const rb = ruleCounts(b);
        const issues = [.../* @__PURE__ */ new Set([...ra.keys(), ...rb.keys()])].map((rule) => ({ rule, before: ra.get(rule) ?? 0, after: rb.get(rule) ?? 0 })).filter((r) => r.before !== r.after);
        const profiled = a.data.passTimings.size > 0 && b.data.passTimings.size > 0;
        const [ea, wa] = a.db.validationCounts;
        const [eb, wb] = b.db.validationCounts;
        return jsonResult({
          before: { capture: a.id, file: a.path, frame: a.data.frame },
          after: { capture: b.id, file: b.path, frame: b.data.frame },
          timing: {
            frameMs: change(a.db.frameTimeMs, b.db.frameTimeMs),
            submitMs: change(a.db.submitMs, b.db.submitMs),
            gpuPassMs: profiled ? change(a.metrics.gpuMs, b.metrics.gpuMs) : void 0
          },
          counts: Object.keys(counts).length ? counts : void 0,
          issuesByRule: issues.length ? issues : void 0,
          validation: ea !== eb || wa !== wb ? { errors: change(ea, eb), warnings: change(wa, wb) } : void 0,
          passes: rows.slice(0, 60),
          onlyBefore: [...ka].filter(([key]) => !kb.has(key)).map(([, i]) => a.passName(i)),
          onlyAfter: [...kb].filter(([key]) => !ka.has(key)).map(([, i]) => b.passName(i)),
          notes: profiled ? void 0 : ["At least one capture was taken without Profile passes, so GPU times cannot be compared."]
        });
      }
    }
  ];
}

// src/mcp/live_tools.ts
var SESSION_PARAM = { type: "string", description: `A live session's id ("app-1"). Defaults to the session started most recently.` };
var LANGUAGES = ["glsl", "hlsl", "spirv-asm"];
var sleep2 = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
function deviceName(db) {
  for (const o of db.objectsByType.get("VkPhysicalDevice")?.values() ?? []) {
    const summary = o.summary(db).trim();
    if (summary) return summary;
  }
  const metal = db.objectsByType.get("MTLDevice")?.values().next().value;
  return metal ? metal.name : void 0;
}
function sessionStatus(s) {
  const db = s.database;
  const byType = [...db.objectsByType].filter(([, objects]) => objects.size).sort((a, b) => b[1].size - a[1].size).slice(0, 12);
  const last = s.frameStats.at(-1)?.msg;
  const [errors, warnings] = db.validationCounts;
  return {
    session: s.id,
    name: s.name,
    state: s.state,
    detail: s.detail || void 0,
    launched: s.launched,
    pid: s.pid ?? void 0,
    port: s.port,
    exitCode: s.exitCode ?? void 0,
    api: s.api ?? void 0,
    device: deviceName(db),
    uptimeSeconds: round((Date.now() - s.startedAt) / 1e3),
    frame: last ? {
      index: last.frame,
      frameMs: round(last.frameTimeMs),
      fps: last.frameTimeMs > 0 ? round(1e3 / last.frameTimeMs) : void 0,
      submitMs: round(last.submitMs),
      refreshMs: round(last.refreshMs) || void 0,
      presentMode: last.presentMode || void 0,
      droppedTotal: last.droppedTotal || void 0
    } : void 0,
    objects: { live: db.allObjects.size, byType: Object.fromEntries(byType.map(([type, objects]) => [type, objects.size])) },
    memoryBytes: {
      deviceMemory: db.memory.device || void 0,
      buffers: db.memory.buffers || void 0,
      images: db.memory.images || void 0,
      reportedByDriver: db.memory.reported || void 0
    },
    validation: { errors, warnings, total: db.validation.length },
    leakedObjects: db.leakCount || void 0,
    recentLog: s.log.slice(-15),
    note: s.state === "connected" && !last ? "Connected, but no frame has been reported yet: the application may not be rendering." : void 0
  };
}
var TEMP_SOURCE = /\S*vkinsp_\d+_\d+_\d+\.(glsl|hlsl|spvasm)/g;
function compilerLog(log) {
  return log.split(/\r?\n/).filter((line) => line.trim().replace(TEMP_SOURCE, "") !== "").join("\n").replace(TEMP_SOURCE, "source").trim();
}
function stageOf(s, pipelineId, stage) {
  const pipeline = s.database.getObject(pipelineId);
  if (!pipeline || pipeline.type !== "VkPipeline") throw new Error(`${refText(s.database, pipelineId) ?? `Object ${pipelineId}`} is not a live VkPipeline of ${s.id}.`);
  const stages = pipelineStages(pipeline, s.database);
  const source = stages.find((x) => x.stage === stage.toLowerCase());
  if (!source) throw new Error(`${refText(s.database, pipelineId)} has no ${stage} stage with code (it has: ${stages.map((x) => x.stage).join(", ") || "none"}).`);
  return { flag: source.stageFlag, entryPoint: source.entryPoint, source };
}
function liveDescriptor(db, d) {
  if (d.buffer !== void 0) return { buffer: refText(db, d.buffer), offset: d.offset, range: d.range };
  if (d.imageView !== void 0 || d.sampler !== void 0) {
    return {
      imageView: refText(db, d.imageView),
      image: refText(db, imageOfView(db, refId(d.imageView ?? void 0))),
      layout: d.imageLayout,
      sampler: refText(db, d.sampler),
      immutableSampler: d.immutable || void 0
    };
  }
  return compact(d, db);
}
function discardNote(db, imageId) {
  const views = /* @__PURE__ */ new Set();
  for (const v of db.objectsByType.get("VkImageView")?.values() ?? []) if (refId(v.descriptor?.image) === imageId) views.add(v.id);
  let stored = 0;
  let discarded = 0;
  for (const fb of db.objectsByType.get("VkFramebuffer")?.values() ?? []) {
    const d = fb.descriptor;
    const attachments = d && Array.isArray(d.pAttachments) ? d.pAttachments : [];
    const pass = db.getObject(refId(d?.renderPass));
    const descriptions = pass?.descriptor && Array.isArray(pass.descriptor.pAttachments) ? pass.descriptor.pAttachments : [];
    attachments.forEach((a, i) => {
      const view = refId(a);
      const description = descriptions[i];
      if (view === null || !views.has(view) || !isObject(description)) return;
      if (str(description.storeOp).endsWith("DONT_CARE")) discarded++;
      else stored++;
    });
  }
  return discarded && !stored ? "Every render pass that draws into this image discards it (storeOp DONT_CARE), so what it holds between frames is undefined: capture_frames reads attachments at the end of each pass instead." : void 0;
}
function requireConnected(s) {
  if (!s.connected) throw new Error(`${s.id} is not connected (${s.state}${s.detail ? `: ${s.detail}` : ""}).`);
}
function liveTools(sessions2, store) {
  return [
    {
      name: "launch_app",
      description: "Launch an application with GPU Inspector's capture library in it (the Vulkan layer; on macOS the Metal library) and connect to it, so its frames can be captured and its frame statistics watched while it runs. Returns the session's status: the device, live objects, the frame rate once frames arrive, and the recent log (the layer's own output is in it, which is where to look when it does not connect). The application runs until stop_app or until this server exits.",
      inputSchema: schema({
        exe: { type: "string", description: "The executable (on macOS an .app bundle works too)." },
        args: { type: "string", description: "Command line arguments, quoted as in a shell." },
        cwd: { type: "string", description: "Working directory (default the executable's directory)." },
        env: { type: "object", additionalProperties: { type: "string" }, description: "Extra environment variables." },
        validation: { type: "boolean", description: "Also enable the Khronos validation layer (Vulkan SDK) or Metal's validation, so validation messages reach the captures (default false)." },
        syncValidation: { type: "boolean", description: "With validation: synchronization validation too (default false)." },
        stacktraces: { type: "boolean", description: "Record a stack at every object creation (default true)." },
        recordAlways: { type: "boolean", description: "Record every command buffer as it is built, so buffers recorded once and reused appear in captures (default false; costs CPU time)." },
        port: { type: "integer", minimum: 1, maximum: 65535, description: "Port for the capture library (default 47531, or the next free one)." },
        layerDir: { type: "string", description: "The directory holding VK_LAYER_INSPECTOR_capture.json, when neither a GPU Inspector checkout nor an installed GPU Inspector provides it." },
        waitSeconds: { type: "number", minimum: 1, maximum: 600, description: "How long to wait for the capture library to connect (default 60)." }
      }, ["exe"]),
      handler: async (args) => {
        const env = args.env && typeof args.env === "object" ? Object.fromEntries(Object.entries(args.env).map(([k, v]) => [k, String(v)])) : void 0;
        const s = await sessions2.launch({
          exe: requireString(args, "exe"),
          args: stringArg(args, "args"),
          cwd: stringArg(args, "cwd"),
          env,
          validation: boolArg(args, "validation", false),
          syncValidation: boolArg(args, "syncValidation", false),
          stacktraces: boolArg(args, "stacktraces", true),
          recordAlways: boolArg(args, "recordAlways", false),
          port: optionalInt(args, "port"),
          layerDir: stringArg(args, "layerDir")
        }, (numberArg(args, "waitSeconds") ?? 60) * 1e3);
        const result = jsonResult({
          ...sessionStatus(s),
          problem: s.connected ? void 0 : "The capture library did not connect. recentLog (and get_session_log) has the application's and the layer's output: a crash, an application that does not use Vulkan, or a layer the loader did not load."
        });
        if (!s.connected) result.isError = true;
        return result;
      }
    },
    {
      name: "attach_app",
      description: "Connect to an application whose capture library already listens on a port: one started with GPU Inspector's implicit layer (VKINSP_ENABLE=1 and VKINSP_PORT), by hand, or by GPU Inspector itself. The capture library serves one client at a time, so attaching disconnects GPU Inspector from that application if it was connected.",
      inputSchema: schema({
        port: { type: "integer", minimum: 1, maximum: 65535, description: "The port (default 47531)." },
        waitSeconds: { type: "number", minimum: 1, maximum: 600, description: "How long to keep trying (default 10)." }
      }),
      handler: async (args) => {
        const s = await sessions2.attach(intArg(args, "port", 47531, 1, 65535), (numberArg(args, "waitSeconds") ?? 10) * 1e3);
        return jsonResult(sessionStatus(s));
      }
    },
    {
      name: "list_android_devices",
      description: "The Android devices adb sees (serial, state, model, Android API level, ABI) and whether GPU Inspector's Android layer is available; with `device`, also the third-party packages installed on that device, for launch_android_app.",
      inputSchema: schema({ device: { type: "string", description: "A device's serial: also list the packages installed on it." } }),
      readOnly: true,
      handler: async (args) => {
        const { adb: adb2, devices, layer } = await sessions2.androidDevices();
        const device = stringArg(args, "device");
        return jsonResult({
          adb: adb2 ?? "not found: install the Android SDK platform-tools, or set ANDROID_HOME or INSPECTOR_ADB",
          devices: devices.map((d) => ({ serial: d.serial, state: d.state, model: d.model || void 0, sdk: d.sdk || void 0, abi: d.abi || void 0 })),
          layer: layer ?? "not found: build it with tools/build_android.py (it needs the Android NDK), install GPU Inspector, or set INSPECTOR_ANDROID_LAYER_DIR",
          packages: adb2 && device ? await listPackages(adb2, device) : void 0,
          note: devices.some((d) => d.state === "unauthorized") ? "An unauthorized device is waiting for its USB debugging prompt to be accepted." : void 0
        });
      }
    },
    {
      name: "launch_android_app",
      description: "Launch an Android application with GPU Inspector's Vulkan layer and connect to it, for the same live tools as launch_app (get_live_frame_stats, capture_frames, read_live_image, replace_shader...). The layer is installed on the device (the layer package on Android 10+, else copied into the application's data), enabled for the package through Android's GPU debug layer settings, and reached through an adb port forward. The application must be debuggable (a development build) unless the device is rooted. Logcat's layer output and crashes go to get_session_log; stop_app ends the application and turns the debug layer settings off.",
      inputSchema: schema({
        package: { type: "string", description: "The package name (list_android_devices lists the installed ones)." },
        device: { type: "string", description: "The device's serial (default the only connected device)." },
        activity: { type: "string", description: "The activity to start (default the package's launcher activity)." },
        stacktraces: { type: "boolean", description: "Record a stack at every object creation (default true)." },
        recordAlways: { type: "boolean", description: "Record every command buffer as it is built, for applications that reuse command buffers recorded once (default false)." },
        port: { type: "integer", minimum: 1, maximum: 65535, description: "Host port for the forward (default 47531, or the next free one)." },
        waitSeconds: { type: "number", minimum: 1, maximum: 600, description: "How long to wait for the layer to connect once the application has started (default 60)." }
      }, ["package"]),
      handler: async (args) => {
        const s = await sessions2.launchAndroid({
          package: requireString(args, "package"),
          device: stringArg(args, "device"),
          activity: stringArg(args, "activity"),
          stacktraces: boolArg(args, "stacktraces", true),
          recordAlways: boolArg(args, "recordAlways", false),
          port: optionalInt(args, "port")
        }, (numberArg(args, "waitSeconds") ?? 60) * 1e3);
        const result = jsonResult({
          ...sessionStatus(s),
          problem: s.connected ? void 0 : s.state === "error" ? `The launch failed on the device: ${s.detail}` : "The layer did not connect. recentLog (and get_session_log) has logcat's layer output and crashes: an application that is not debuggable or does not use Vulkan, or a device that is asleep."
        });
        if (!s.connected) result.isError = true;
        return result;
      }
    },
    {
      name: "list_sessions",
      description: "List the live sessions this server has launched or attached to, with their state.",
      inputSchema: schema({}),
      readOnly: true,
      handler: () => jsonResult({
        sessions: sessions2.list().map((s) => ({
          session: s.id,
          name: s.name,
          state: s.state,
          pid: s.pid ?? void 0,
          port: s.port,
          api: s.api ?? void 0,
          frame: s.frameStats.at(-1)?.msg.frame
        })),
        capturesDirectory: capturesDir()
      })
    },
    {
      name: "get_session_status",
      description: "A live session's state: whether it is connected, the process, the device, the last frame report (frame time, submit time, refresh period, dropped frames), live objects by type, memory, validation counts, and the recent log.",
      inputSchema: schema({ session: SESSION_PARAM }),
      readOnly: true,
      handler: (args) => jsonResult(sessionStatus(sessions2.get(stringArg(args, "session"))))
    },
    {
      name: "get_live_frame_stats",
      description: "Watch a running application's frame reports for a few seconds, without capturing: average, shortest and longest frame time, frame rate, CPU time inside queue submission, the display refresh period and where it came from, dropped frames, and a verdict on whether the frame meets the refresh or is bound by submission. GPU time is not measured live: capture_frames with profilePasses measures each pass.",
      inputSchema: schema({
        session: SESSION_PARAM,
        seconds: { type: "number", minimum: 0.3, maximum: 30, description: "How long to watch (default 2)." }
      }),
      readOnly: true,
      handler: async (args) => {
        const s = sessions2.get(stringArg(args, "session"));
        if (!s.connected) throw new Error(`${s.id} is not connected (${s.state}).`);
        const seconds = Math.min(30, Math.max(0.3, numberArg(args, "seconds") ?? 2));
        const since = Date.now();
        await sleep2(seconds * 1e3);
        const reports = s.frameStats.filter((f) => f.at >= since).map((f) => f.msg);
        if (!reports.length) {
          return jsonResult({ session: s.id, seconds, note: "No frame reports arrived: the application is not presenting frames (minimized, paused, loading, or not rendering)." });
        }
        let frames = 0;
        let weightedMs = 0;
        let weightedSubmit = 0;
        let min = Infinity;
        let max = 0;
        let dropped = 0;
        for (const m of reports) {
          const n = Math.max(1, m.frames ?? 1);
          frames += n;
          weightedMs += m.frameTimeMs * n;
          weightedSubmit += (m.submitMs ?? 0) * n;
          min = Math.min(min, m.minMs ?? m.frameTimeMs);
          max = Math.max(max, m.maxMs ?? m.frameTimeMs);
          dropped += m.dropped ?? 0;
        }
        const last = reports[reports.length - 1];
        const frameMs = weightedMs / frames;
        const submitMs = weightedSubmit / frames;
        const refresh = last.refreshMs ?? 0;
        let verdict;
        if (refresh > 0 && frameMs <= refresh * 1.1) verdict = "Meeting the display refresh: the frame waits for vsync, so there is headroom.";
        else if (frameMs > 0 && submitMs / frameMs > 0.8) verdict = "Bound by submission: most of each frame is spent inside queue submit on the CPU.";
        else verdict = `${refresh > 0 ? "Missing the display refresh" : "Vsync is off"}, and submission is not what takes the time: the GPU, presentation, or the application's own CPU work. capture_frames with profilePasses measures the GPU passes.`;
        return jsonResult({
          session: s.id,
          seconds,
          frames,
          frame: last.frame,
          frameMs: round(frameMs),
          fps: round(1e3 / frameMs),
          shortestMs: round(min),
          longestMs: round(max),
          submitMs: round(submitMs),
          refreshMs: refresh > 0 ? round(refresh) : void 0,
          refreshSource: last.refreshSource ? REFRESH_SOURCE_NOTE[last.refreshSource] ?? last.refreshSource : void 0,
          presentMode: last.presentMode || void 0,
          frameBoundary: last.frameBoundary || void 0,
          droppedFrames: dropped || void 0,
          droppedTotal: last.droppedTotal || void 0,
          driverAllocatedBytes: last.allocatedBytes,
          verdict
        });
      }
    },
    {
      name: "capture_frames",
      description: "Capture frames of a running application: every command of the frame with its bound state, the render targets read back at the end of each pass, bound buffers and images, and (profilePasses) GPU timestamps and counters per pass. The capture is saved as a .gpucap file and opened, and its summary returned: the capture tools (get_bottlenecks, get_frame_issues, get_command, read_texture...) work on it by the returned capture id. Capture while the application shows what is slow or wrong.",
      inputSchema: schema({
        session: SESSION_PARAM,
        frames: { type: "integer", minimum: 1, maximum: 16, description: "Frames to capture (default 1)." },
        atFrame: { type: "integer", minimum: 0, description: "Capture that frame (the capture library's present counter) instead of the next one." },
        delaySeconds: { type: "number", minimum: 0, maximum: 600, description: "Wait this long before requesting the capture." },
        profilePasses: { type: "boolean", description: "GPU timestamps and counters around every pass (default true)." },
        renderTargets: { type: "boolean", description: "Read back every pass's attachments (default true)." },
        buffers: { type: "boolean", description: "Read back bound buffer ranges (default true)." },
        images: { type: "boolean", description: "Read back images bound through descriptor sets (default true)." },
        stacktraces: { type: "boolean", description: "Record the stack of every command (default false; costs CPU time in the application while capturing)." },
        overdraw: { type: "boolean", description: "Metal: draw every render pass a second time with a counting fragment shader, measuring its overdraw per pixel (get_overdraw, and get_bottlenecks' measuredOverdraw). Default false: it costs GPU and CPU time in the captured frame. Vulkan applications ignore it; vkinsp_replay --overdraw measures a Vulkan capture file." },
        pixelHistory: {
          type: "object",
          description: "Metal: follow one pixel of a render target through the captured frame, for get_pixel_history on the new capture. Every pass that renders to the texture is drawn again one draw at a time at the pixel, in the frame's own command buffers. A drawable's texture id (from an earlier capture) follows whichever drawable the captured frame renders into. Vulkan applications ignore it; get_pixel_history replays a Vulkan capture instead.",
          properties: {
            texture: { type: "integer", description: "The texture's object id (list_textures of an earlier capture)." },
            x: { type: "integer", minimum: 0, description: "The pixel's column, at the mip level." },
            y: { type: "integer", minimum: 0, description: "The pixel's row, at the mip level." },
            mip: { type: "integer", minimum: 0, description: "The mip level the passes render to (default 0)." },
            layer: { type: "integer", minimum: 0, description: "The array slice (default 0)." }
          },
          required: ["texture", "x", "y"]
        },
        maxBufferKB: { type: "integer", minimum: 1, description: "Bytes read back per bound buffer range, in KB (default 128)." },
        recordAlways: { type: "boolean", description: "Switch recording of every command buffer on (or off) first, for applications that reuse command buffers recorded before the capture." },
        timeoutSeconds: { type: "number", minimum: 5, maximum: 3600, description: "How long to wait for the capture (default 60)." },
        saveAs: { type: "string", description: `Where to save the .gpucap (default a new file in ${capturesDir()}).` }
      }),
      handler: async (args) => {
        const s = sessions2.get(stringArg(args, "session"));
        const delay = numberArg(args, "delaySeconds");
        if (delay) await sleep2(delay * 1e3);
        if (args.recordAlways !== void 0) await s.send({ action: "Settings", recordAlways: boolArg(args, "recordAlways", false) });
        const pixelArg = args.pixelHistory;
        if (pixelArg !== void 0 && (typeof pixelArg !== "object" || pixelArg === null)) throw new Error("pixelHistory must be an object: { texture, x, y }.");
        const pixel = pixelArg;
        const pixelHistory = pixel ? { texture: requireInt(pixel, "texture"), x: requireInt(pixel, "x"), y: requireInt(pixel, "y"), mip: optionalInt(pixel, "mip") ?? 0, layer: optionalInt(pixel, "layer") ?? 0 } : void 0;
        const result = await s.capture({
          frames: intArg(args, "frames", 1, 1, 16),
          atFrame: optionalInt(args, "atFrame"),
          profilePasses: boolArg(args, "profilePasses", true),
          renderTargets: boolArg(args, "renderTargets", true),
          buffers: boolArg(args, "buffers", true),
          images: boolArg(args, "images", true),
          stacktraces: boolArg(args, "stacktraces", false),
          overdraw: boolArg(args, "overdraw", false),
          pixelHistory,
          maxBufferBytes: intArg(args, "maxBufferKB", 128, 1) * 1024,
          timeoutMs: (numberArg(args, "timeoutSeconds") ?? 60) * 1e3
        });
        const file = await s.saveCapture(result.data, stringArg(args, "saveAs"));
        const { capture } = store.open(file);
        const notes = [];
        if (result.completion === "quiet") notes.push("This capture library does not mark the end of a capture (it was built before that message existed), so the capture was taken as complete once its stream went quiet.");
        if (!result.data.commands.length) notes.push("The capture has no commands. An application that records its command buffers once and resubmits them needs recordAlways: true.");
        if (pixelHistory && !result.data.pixelHistory) {
          notes.push(result.data.api === "metal" ? "No pixel history arrived: the application's capture library was built before pixel history." : "pixelHistory is followed by the Metal capture library only; get_pixel_history replays a Vulkan capture instead.");
        }
        return jsonResult({
          session: s.id,
          file,
          megabytes: round(capture.fileBytes / 1048576),
          secondsToCapture: round(result.elapsedMs / 1e3),
          captureNotes: notes.length ? notes : void 0,
          ...captureSummary(capture)
        });
      }
    },
    {
      name: "list_live_objects",
      description: "List a running application's live objects (images, buffers, pipelines, descriptor sets, Metal textures...) as the capture library tracks them now, with a one-line summary each; without a type filter it also counts them by type. Object ids are the same in the session's captures.",
      inputSchema: schema({
        session: SESSION_PARAM,
        type: { type: "string", description: 'Only this type: "VkImage", "VkDescriptorSet", "MTLTexture" (the Vk prefix may be left out).' },
        name: { type: "string", description: "Regular expression on the object's name or label." },
        ...PAGE_PARAMS
      }),
      readOnly: true,
      handler: (args) => {
        const s = sessions2.get(stringArg(args, "session"));
        const db = s.database;
        const type = stringArg(args, "type")?.toLowerCase();
        const name = regexArg(args, "name");
        const all = [...db.allObjects.values()].sort((a, b) => a.id - b.id);
        const list = all.filter((o) => (!type || o.type.toLowerCase() === type || o.shortType.toLowerCase() === type) && (!name || name.test(o.name) || name.test(o.label)));
        const types = {};
        if (!type) for (const o of all) types[o.type] = (types[o.type] ?? 0) + 1;
        const p = page(list, args, 100, 500);
        return jsonResult({
          session: s.id,
          state: s.state,
          types: type ? void 0 : types,
          total: p.total,
          offset: p.offset,
          nextOffset: p.nextOffset,
          objects: p.items.map((o) => ({
            id: o.id,
            type: o.type,
            name: o.name !== `${o.shortType} ${o.id}` ? o.name : void 0,
            summary: o.summary(db) || void 0,
            destroyed: o.isDeleted || void 0
          }))
        });
      }
    },
    {
      name: "get_live_object",
      description: "One live object of a running application in full: the call that created it with its arguments, later updates (memory bindings, descriptor contents read so far), its owner, what it depends on and what depends on it, its payloads, the validation messages naming it, and its creation stack, fetched from the capture library (launched with stack traces, the default). The stack is where a leaked or misconfigured object came from.",
      inputSchema: schema({
        session: SESSION_PARAM,
        id: { type: "integer", minimum: 0, description: "The object's id: the number after # in a reference like VkImage#12." },
        stack: { type: "boolean", description: "Fetch the creation stack (default true)." }
      }, ["id"]),
      readOnly: true,
      handler: async (args) => {
        const s = sessions2.get(stringArg(args, "session"));
        const db = s.database;
        const id = requireInt(args, "id");
        const o = db.getObject(id);
        if (!o) throw new Error(`No live object ${id} in ${s.id} (list_live_objects lists them).`);
        let stack;
        let stackNote;
        if (boolArg(args, "stack", true)) {
          stack = db.stacks.get(id) ?? (s.connected ? (await requestStacks(s, [id]))?.get(id) : void 0);
          if (!stack?.length) {
            stackNote = db.stacksAvailable === false ? "The capture library records no stacks: the application was launched without stack traces." : s.connected ? "No creation stack was recorded for this object." : "Not connected, so the creation stack cannot be fetched.";
          }
        }
        const validation = db.validationFor(id);
        const contents = ["VkImage", "VkImageView", "MTLTexture"].includes(o.type) ? "read_live_image shows its current contents." : o.type === "VkDescriptorSet" ? "get_live_descriptor_set reads what it binds now." : void 0;
        return jsonResult({
          session: s.id,
          ...objectDetail(db, o),
          validation: validation.length ? validation.slice(0, 20).map((v) => ({
            severity: v.severity,
            id: v.idName ?? void 0,
            count: v.count > 1 ? v.count : void 0,
            frame: v.frame,
            message: clip(v.message, 600)
          })) : void 0,
          creationStack: stack?.length ? stackLines(await symbolizeOnHost(stack)) : void 0,
          stackNote,
          contents
        });
      }
    },
    {
      name: "read_live_image",
      description: "Look at an image of a running application as it is now, without capturing: the capture library copies one mip level and array layer at the application's next frame (a VkImageView reads its image at the view's first mip and layer). Returns what read_texture returns: the PNG, per-channel minimum, maximum and mean, the share of zero texels, NaN and infinity counts, a 3x3 grid of texel values and exact values at requested texels. Quicker than a capture for checking a target after replace_shader. An image the library cannot copy from (transient attachments, an unknown layout) comes back with the reason; an attachment every render pass discards (storeOp DONT_CARE) holds undefined contents between frames, which the answer notes.",
      inputSchema: schema({
        session: SESSION_PARAM,
        object: { type: "integer", minimum: 1, description: "The VkImage, VkImageView or MTLTexture object id (list_live_objects lists them)." },
        mip: { type: "integer", minimum: 0, description: "Mip level (default 0, or the view's first)." },
        layer: { type: "integer", minimum: 0, description: "Array layer, or the slice of a 3D image (default 0, or the view's first layer)." },
        ...IMAGE_PARAMS,
        timeoutSeconds: { type: "number", minimum: 1, maximum: 120, description: "How long to wait for the application's next frame (default 15)." }
      }, ["object"]),
      readOnly: true,
      handler: async (args) => {
        const s = sessions2.get(stringArg(args, "session"));
        requireConnected(s);
        const db = s.database;
        const requested = requireInt(args, "object");
        let o = db.getObject(requested);
        if (!o) throw new Error(`No live object ${requested} in ${s.id} (list_live_objects lists them).`);
        let mip = optionalInt(args, "mip");
        let layer = optionalInt(args, "layer");
        if (o.type === "VkImageView") {
          const view = o.descriptor;
          const range = view && isObject(view.subresourceRange) ? view.subresourceRange : null;
          mip ??= num(range?.baseMipLevel);
          layer ??= num(range?.baseArrayLayer);
          const image = db.getObject(refId(view?.image));
          if (!image) throw new Error(`${refText(db, o.id)} names no live image.`);
          o = image;
        }
        if (o.type !== "VkImage" && o.type !== "MTLTexture") {
          throw new Error(`${refText(db, o.id)} is not an image: read_live_image takes a VkImage, a VkImageView or an MTLTexture.`);
        }
        const d = o.descriptor;
        const is3D = /3D/.test(`${str(d?.imageType)}${str(d?.textureType)}`);
        const timeoutMs = (numberArg(args, "timeoutSeconds") ?? 15) * 1e3;
        const msg = await s.readImage(o.id, mip ?? 0, is3D ? 0 : layer ?? 0, timeoutMs);
        if (!msg) {
          throw new Error(`${refText(db, o.id)} was not read back within ${timeoutMs / 1e3} s: the capture library copies images at the application's next frame, so it may not be rendering.`);
        }
        const slices = Math.max(1, msg.depth || 1);
        const slice = slices > 1 ? Math.min(layer ?? 0, slices - 1) : void 0;
        const head = {
          session: s.id,
          image: refText(db, o.id),
          format: msg.format || void 0,
          aspect: msg.aspect,
          mip: msg.mip,
          layer: slices > 1 ? void 0 : msg.layer,
          slice,
          slices: slices > 1 ? slices : void 0,
          note: o.type === "VkImage" ? discardNote(db, o.id) : void 0
        };
        if (msg.error) {
          const failed = jsonResult({ ...head, error: msg.error });
          failed.isError = true;
          return failed;
        }
        if (!msg.__binary) return jsonResult({ ...head, note: "The answer carried no pixel data." });
        if (!isFormatSupported(msg)) return jsonResult({ ...head, note: `Decoding ${msg.format} is not supported.` });
        const tex = decodeTexels(msg, msg.__binary, slice ?? 0);
        if (!tex) return jsonResult({ ...head, note: "The pixel data is shorter than the image's size says." });
        return texelAnswer(tex, args, head, msg.aspect);
      }
    },
    {
      name: "get_live_descriptor_set",
      description: "What a running application's descriptor set binds now (Vulkan): each binding's type and stages, and each descriptor's buffer with offset and range, image view with its image and layout, or sampler. A capture shows the sets bound at each draw with their buffer contents; this reads a set between captures.",
      inputSchema: schema({
        session: SESSION_PARAM,
        set: { type: "integer", minimum: 1, description: "The VkDescriptorSet's object id." }
      }, ["set"]),
      readOnly: true,
      handler: async (args) => {
        const s = sessions2.get(stringArg(args, "session"));
        requireConnected(s);
        const db = s.database;
        const id = requireInt(args, "set");
        const o = db.getObject(id);
        if (!o || o.type !== "VkDescriptorSet") throw new Error(`${o ? refText(db, id) : `Object ${id}`} is not a live VkDescriptorSet of ${s.id}.`);
        if (!await s.readDescriptorSet(id)) throw new Error("The layer did not answer within 10 s.");
        const u = o.updates;
        if (u.tracked === false) {
          return jsonResult({ session: s.id, set: refText(db, id), note: "The layer has no record of this set's contents: it was freed, or its pool was reset." });
        }
        const bindings = Array.isArray(u.bindings) ? u.bindings : [];
        return jsonResult({
          session: s.id,
          set: refText(db, id),
          layout: refText(db, u.layout),
          bindings: bindings.map((b) => {
            const shown = b.descriptors.slice(0, 32);
            return {
              binding: b.binding,
              type: b.type,
              stages: b.stages,
              descriptors: shown.map((x) => x ? liveDescriptor(db, x) : "not written"),
              more: b.descriptors.length > shown.length ? b.descriptors.length - shown.length : void 0
            };
          }),
          note: bindings.length ? void 0 : "The set has no bindings."
        });
      }
    },
    {
      name: "replace_shader",
      description: `Replace one stage of a running Vulkan pipeline: the source (GLSL, HLSL or SPIR-V assembly) is compiled with the Vulkan SDK's compilers for the stage's entry point and SPIR-V version, and the layer rebuilds the pipeline with it, binding the replacement wherever the application binds the original. get_shader with view "glsl" on a capture gives editable source for a pipeline; capture again (and compare_captures) to see the effect; restore_shader undoes it. Command buffers recorded before the edit keep the original until the application records them again.`,
      inputSchema: schema({
        session: SESSION_PARAM,
        pipeline: { type: "integer", minimum: 1, description: "The VkPipeline's object id (the same in the live session and its captures)." },
        stage: { type: "string", description: "The stage to replace: vertex, fragment, compute, geometry, tess_control, tess_eval, mesh, task, ..." },
        source: { type: "string", description: "The complete new source of the stage." },
        language: { type: "string", enum: LANGUAGES, description: "The source's language (default glsl)." },
        entryPoint: { type: "string", description: "The entry point in the source (default the stage's own)." }
      }, ["pipeline", "stage", "source"]),
      handler: async (args) => {
        const s = sessions2.get(stringArg(args, "session"));
        if (s.api === "metal") throw new Error("Shader replacement is Vulkan only.");
        if (!s.connected) throw new Error(`${s.id} is not connected (${s.state}).`);
        const pipelineId = requireInt(args, "pipeline");
        const stageName = requireString(args, "stage").toLowerCase();
        const { flag, entryPoint, source } = stageOf(s, pipelineId, stageName);
        const original = await fetchBlob(s, source.object, source.blobIndex);
        const version = original && reflectSpirv(original)?.version || "";
        const language = enumArg(args, "language", LANGUAGES, "glsl");
        const compiled = await compileShader(
          requireString(args, "source"),
          language,
          stageName,
          stringArg(args, "entryPoint") ?? entryPoint,
          version,
          { includeDirs: searchPaths("sourceRoots").dirs }
        );
        if (!compiled.ok || !compiled.spirv) {
          const failed = jsonResult({ ok: false, failedAt: "compile", compiler: compiled.tool, log: clip(compilerLog(compiled.log), 12e3) });
          failed.isError = true;
          return failed;
        }
        const reply = await s.replaceShader(pipelineId, flag, stageName, compiled.spirv);
        const result = jsonResult({
          ok: reply?.ok ?? false,
          pipeline: refText(s.database, pipelineId),
          stage: stageName,
          spirvVersion: version || void 0,
          replacement: reply?.replacement ? refText(s.database, reply.replacement) ?? `VkPipeline#${reply.replacement}` : void 0,
          error: reply ? reply.error : "The layer did not answer within 15 s.",
          layerNote: reply?.note,
          compilerLog: compilerLog(compiled.log) ? clip(compilerLog(compiled.log), 4e3) : void 0
        });
        if (!reply?.ok) result.isError = true;
        return result;
      }
    },
    {
      name: "restore_shader",
      description: "Undo replace_shader: the pipeline binds its original code again, for one stage or every replaced stage.",
      inputSchema: schema({
        session: SESSION_PARAM,
        pipeline: { type: "integer", minimum: 1, description: "The VkPipeline's object id." },
        stage: { type: "string", description: "The stage to restore (default every replaced stage)." }
      }, ["pipeline"]),
      handler: async (args) => {
        const s = sessions2.get(stringArg(args, "session"));
        if (!s.connected) throw new Error(`${s.id} is not connected (${s.state}).`);
        const pipelineId = requireInt(args, "pipeline");
        const stage = stringArg(args, "stage");
        const flag = stage ? stageOf(s, pipelineId, stage).flag : void 0;
        const reply = await s.restoreShader(pipelineId, flag);
        return jsonResult({ ok: reply?.ok ?? false, pipeline: refText(s.database, pipelineId), stage, error: reply ? reply.error : "The layer did not answer within 15 s." });
      }
    },
    {
      name: "get_session_log",
      description: "A live session's log: the application's standard output and error, the capture library's own log, connection changes, validation messages and shader edits, most recent last.",
      inputSchema: schema({
        session: SESSION_PARAM,
        lines: { type: "integer", minimum: 1, maximum: 2e3, description: "How many of the most recent lines (default 100)." },
        match: { type: "string", description: "Regular expression: only lines that match." }
      }),
      readOnly: true,
      handler: (args) => {
        const s = sessions2.get(stringArg(args, "session"));
        const match = regexArg(args, "match");
        const lines = (match ? s.log.filter((l) => match.test(l)) : s.log).slice(-intArg(args, "lines", 100, 1, 2e3));
        return jsonResult({ session: s.id, state: s.state, lines });
      }
    },
    {
      name: "stop_app",
      description: "End a live session: a launched application is terminated (with the processes it started), an attached one is disconnected.",
      inputSchema: schema({ session: SESSION_PARAM }),
      handler: async (args) => {
        const s = sessions2.get(stringArg(args, "session"));
        await s.stop();
        return jsonResult({ session: s.id, state: s.state, detail: s.detail || void 0, exitCode: s.exitCode ?? void 0 });
      }
    }
  ];
}

// src/mcp/stdio_server.ts
import { createInterface } from "node:readline";
var PROTOCOL_VERSIONS = ["2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"];
var METHOD_NOT_FOUND = -32601;
var INVALID_PARAMS = -32602;
var PARSE_ERROR = -32700;
var McpStdioServer = class {
  constructor(_info, tools, _instructions = "") {
    this._info = _info;
    this._instructions = _instructions;
    for (const t of tools) this._tools.set(t.name, t);
  }
  _tools = /* @__PURE__ */ new Map();
  /** Serves `input` until it closes, writing responses to `output`. */
  serve(input, output) {
    return new Promise((resolve) => {
      const lines = createInterface({ input, crlfDelay: Infinity });
      lines.on("line", (line) => {
        if (!line.trim()) return;
        void this._receive(line).then((reply) => {
          if (reply) output.write(`${JSON.stringify(reply)}
`);
        });
      });
      lines.on("close", () => resolve());
    });
  }
  async _receive(line) {
    let message;
    try {
      message = JSON.parse(line);
    } catch {
      return { jsonrpc: "2.0", id: null, error: { code: PARSE_ERROR, message: "Parse error" } };
    }
    if (Array.isArray(message)) {
      const replies = (await Promise.all(message.map((m) => this.handle(m)))).filter((r) => r !== null);
      return replies.length ? replies : null;
    }
    return this.handle(message);
  }
  /** The response to one message, or null for a notification (which gets none). */
  async handle(message) {
    const req = message;
    if (!req || typeof req !== "object" || req.id === void 0 || req.id === null) return null;
    const id = req.id;
    const params = req.params ?? {};
    switch (req.method) {
      case "initialize": {
        const asked = typeof params.protocolVersion === "string" ? params.protocolVersion : "";
        return {
          jsonrpc: "2.0",
          id,
          result: {
            protocolVersion: PROTOCOL_VERSIONS.includes(asked) ? asked : PROTOCOL_VERSIONS[0],
            capabilities: { tools: { listChanged: false } },
            serverInfo: this._info,
            ...this._instructions ? { instructions: this._instructions } : {}
          }
        };
      }
      case "ping":
        return { jsonrpc: "2.0", id, result: {} };
      case "tools/list":
        return {
          jsonrpc: "2.0",
          id,
          result: {
            tools: [...this._tools.values()].map((t) => ({
              name: t.name,
              description: t.description,
              inputSchema: t.inputSchema,
              ...t.readOnly ? { annotations: { readOnlyHint: true } } : {}
            }))
          }
        };
      case "tools/call": {
        const tool = this._tools.get(String(params.name));
        if (!tool) return { jsonrpc: "2.0", id, error: { code: INVALID_PARAMS, message: `Unknown tool: ${String(params.name)}` } };
        const args = params.arguments && typeof params.arguments === "object" ? params.arguments : {};
        try {
          return { jsonrpc: "2.0", id, result: await tool.handler(args) };
        } catch (e) {
          return { jsonrpc: "2.0", id, result: { content: [{ type: "text", text: `Error: ${e?.message ?? String(e)}` }], isError: true } };
        }
      }
      default:
        return { jsonrpc: "2.0", id, error: { code: METHOD_NOT_FOUND, message: `Method not found: ${String(req.method)}` } };
    }
  }
};

// src/mcp/server.ts
var INSTRUCTIONS = [
  "These tools read GPU Inspector frame captures (.gpucap) of Vulkan and Metal applications, with the analyses GPU Inspector runs, and drive running applications.",
  "Open a saved capture with open_capture (list_captures shows the files GPU Inspector saved recently), or launch_app an application (launch_android_app on Android) and capture_frames it; then start from get_capture_summary.",
  "For performance: get_bottlenecks (needs profiled passes), get_frame_issues, get_render_graph, analyze_shaders, get_shader_flame_graph, get_live_frame_stats, and compare_captures to check a fix.",
  "To debug rendering: read_texture shows what a pass wrote; list_commands finds draws by pass, label or kind; get_command shows the state a draw read (pipeline, decoded uniforms, vertex and index buffers, render targets); read_vertices, read_buffer and get_shader go deeper; get_validation lists real errors. replace_shader tries a shader fix in the running application, and read_live_image looks at an image without capturing.",
  'Object references read Type#id "name": pass the id to get_object. Cite command indices, object ids and pass labels so the user can find them in GPU Inspector.'
].join(" ");
function createServer(store = new CaptureStore(), sessions2 = new SessionManager()) {
  const version = true ? "0.10.0" : "dev";
  return new McpStdioServer({ name: "gpu-inspector", version }, [
    ...captureTools(store),
    ...commandTools(store),
    ...resourceTools(store),
    ...liveTools(sessions2, store)
  ], INSTRUCTIONS);
}

// src/mcp/main.ts
console.log = console.info = console.debug = (...parts2) => {
  process2.stderr.write(`${parts2.map(String).join(" ")}
`);
};
var sessions = new SessionManager();
var exit = (code) => {
  void sessions.stopAll().finally(() => process2.exit(code));
};
process2.on("SIGINT", () => exit(0));
process2.on("SIGTERM", () => exit(0));
createServer(void 0, sessions).serve(process2.stdin, process2.stdout).then(
  () => exit(0),
  (e) => {
    process2.stderr.write(`gpu-inspector MCP server: ${e?.stack ?? String(e)}
`);
    exit(1);
  }
);
