// A tracked Vulkan object as reported by the layer's AddObject message.
//
// Unlike WebGPU Inspector's per-class object model, Vulkan objects are represented by one
// generic class plus a per-type summary table: the layer sends the creating call's full
// argument list as the descriptor, so everything the UI needs is in `args`.
import { estimateImageBytes } from "./vk_format.js";
import type { AddObjectMessage, ArgObject, ArgValue, BlobInfo, HandleRef } from "../../shared/protocol.js";

export interface ObjectLookup {
  getObject(id: number | undefined | null): VulkanObject | null;
}

export class VulkanObject {
  readonly id: number;
  readonly type: string;        // "VkImage"
  readonly parentId: number;    // id of owning object (device, pool, swapchain, ...)
  readonly cmd: string;         // "vkCreateImage"
  readonly index: number;
  readonly handle: string;      // "0x..." raw handle for correlation with app logs
  label: string;
  args: ArgObject | null;
  blobs: BlobInfo[];            // binary payloads available from the layer
  updates: Record<string, ArgValue>;  // fields from ObjectUpdate messages (memory, memoryOffset, ...)

  dependencies = new Set<VulkanObject>();  // objects referenced by args
  dependents = new Set<VulkanObject>();    // objects whose args reference this one
  invalidReason: string | null = null;
  widget: unknown = null;                  // list entry in the inspect panel
  isDeleted = false;
  /** A shader of this pipeline / this module has been replaced by the shader editor. */
  edited = false;

  constructor(msg: AddObjectMessage) {
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

  get shortType(): string {
    return this.type.startsWith("Vk") ? this.type.substring(2) : this.type;
  }

  get name(): string {
    return this.label || `${this.shortType} ${this.id}`;
  }

  get isInvalid(): boolean {
    return !!this.invalidReason;
  }

  /** The create-info struct for this object, when the creating call has one. */
  get descriptor(): ArgObject | null {
    const a = this.args;
    if (!a) return null;
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
  summary(db: ObjectLookup | null): string {
    const d = this.descriptor;
    const a = this.args ?? {};
    switch (this.type) {
      case "VkDevice": {
        const props = a.properties;
        return isObject(props) ? str(props.deviceName) : "";
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
        // The layer reports the module's entry point stage(s) (ObjectUpdate "stage").
        const stage = str(this.updates.stage);
        return `${stage ? `${stage} shader, ` : ""}${d ? formatBytes(num(d.codeSize)) : ""} SPIR-V`;
      }
      case "VkPipeline": {
        if (!d) return "";
        if (Array.isArray(d.pStages)) {
          const names = d.pStages.map((s) => (isObject(s) ? stageWord(str(s.stage)) : "")).filter((s) => s);
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
        return a.queueFamilyIndex !== undefined ? `family ${num(a.queueFamilyIndex)} index ${num(a.queueIndex)}` : "";
      case "VkQueryPool":
        return d ? `${fmt(d.queryType)} x${num(d.queryCount)}` : "";
      default:
        return "";
    }
  }
}

/**
 * Bytes an object occupies on the GPU: a VkDeviceMemory's allocation, a VkBuffer's size, and an
 * estimate for a VkImage from its format, size, mips, layers and samples (swapchain images from
 * their swapchain). 0 for everything else.
 */
export function objectMemoryBytes(o: VulkanObject, db: ObjectLookup | null): number {
  const d = o.descriptor;
  switch (o.type) {
    case "VkDeviceMemory":
      return num(d?.allocationSize);
    case "VkBuffer":
      return num(d?.size);
    case "VkImage": {
      if (o.cmd === "vkGetSwapchainImagesKHR") {
        const sd = db?.getObject(o.parentId)?.descriptor;
        const e = sd && isObject(sd.imageExtent) ? sd.imageExtent : null;
        return sd ? estimateImageBytes(str(sd.imageFormat), num(e?.width), num(e?.height), 1, 1, num(sd.imageArrayLayers) || 1, 1) : 0;
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

/** "VK_SHADER_STAGE_FRAGMENT_BIT" -> "fragment"; "VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT" -> "tess control". */
export function stageWord(flag: string): string {
  const m = /^VK_SHADER_STAGE_(.+?)_BIT/.exec(flag);
  if (!m) return flag;
  return m[1].toLowerCase().replace("tessellation_control", "tess control").replace("tessellation_evaluation", "tess eval").replace(/_/g, " ");
}

export function isObject(v: ArgValue | undefined): v is ArgObject {
  return typeof v === "object" && v !== null && !Array.isArray(v);
}

export function isHandleRef(v: ArgValue | undefined): v is HandleRef {
  return isObject(v) && typeof (v as HandleRef).__id === "number";
}

export function refId(v: ArgValue | undefined): number | null {
  return isHandleRef(v) ? v.__id : null;
}

export function num(v: ArgValue | undefined): number {
  return typeof v === "number" ? v : typeof v === "string" ? Number(v) || 0 : 0;
}

export function str(v: ArgValue | undefined): string {
  return typeof v === "string" ? v : v === undefined || v === null ? "" : String(v);
}

/** "VK_FORMAT_R8G8B8A8_UNORM" -> "R8G8B8A8_UNORM"; "VK_SAMPLE_COUNT_4_BIT" -> "4x" */
export function fmt(v: ArgValue | undefined): string {
  if (typeof v !== "string") return v === undefined || v === null ? "" : String(v);
  if (v.startsWith("VK_SAMPLE_COUNT_")) return v.replace("VK_SAMPLE_COUNT_", "").replace("_BIT", "x");
  const prefixes = [
    "VK_FORMAT_", "VK_IMAGE_VIEW_TYPE_", "VK_IMAGE_TYPE_", "VK_SHADER_STAGE_", "VK_COMMAND_BUFFER_LEVEL_",
    "VK_FILTER_", "VK_SAMPLER_ADDRESS_MODE_", "VK_QUERY_TYPE_", "VK_DESCRIPTOR_TYPE_", "VK_IMAGE_LAYOUT_",
    "VK_PIPELINE_BIND_POINT_", "VK_INDEX_TYPE_", "VK_PRIMITIVE_TOPOLOGY_", "VK_ATTACHMENT_LOAD_OP_",
    "VK_ATTACHMENT_STORE_OP_", "VK_COMPARE_OP_", "VK_CULL_MODE_", "VK_FRONT_FACE_", "VK_POLYGON_MODE_",
    "VK_BLEND_FACTOR_", "VK_BLEND_OP_", "VK_STRUCTURE_TYPE_",
  ];
  for (const p of prefixes) if (v.startsWith(p)) return v.substring(p.length).replace(/_BIT$/, "");
  const m = /^VK_[A-Z0-9]+_(.+)$/.exec(v);
  return m ? m[1] : v;
}

/** "VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT" -> "VERTEX_BUFFER | TRANSFER_DST" */
export function fmtFlags(v: ArgValue | undefined): string {
  if (typeof v !== "string") return v === undefined ? "" : String(v);
  return v.split(" | ").map((s) => s.replace(/^VK_[A-Z0-9]+?_(USAGE_|CREATE_|STAGE_|ACCESS_|ASPECT_)?/, "").replace(/_BIT(_[A-Z]+)?$/, "$1")).join(" | ");
}

export function formatBytes(bytes: number): string {
  if (!isFinite(bytes)) return "";
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  if (bytes < 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
  return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} GB`;
}
