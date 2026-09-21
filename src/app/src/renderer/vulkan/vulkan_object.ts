// A tracked Vulkan object as reported by the layer's AddObject message.
//
// Unlike WebGPU Inspector's per-class object model, Vulkan objects are represented by one
// generic class plus a per-type summary table: the layer sends the creating call's full
// argument list as the descriptor, so everything the UI needs is in `args`.
import { estimateImageBytes } from "./vk_format.js";
import { d3d12PipelineKind, d3d12ResourceBytes, d3d12TextureShape, isD3D12Type } from "../d3d12/d3d12_object.js";
import { dxgiFormatShort } from "../d3d12/dxgi_format.js";
import { backendForObjectType, shortTypeName } from "../backend.js";
import type { AddObjectMessage, ArgObject, ArgValue, BlobInfo, HandleRef } from "../../shared/protocol.js";

export interface ObjectLookup {
  getObject(id: number | undefined | null): VulkanObject | null;
}

/** The members of a graphics pipeline create info each graphics pipeline library part holds (VK_GRAPHICS_PIPELINE_LIBRARY_*). */
const LIBRARY_PARTS: [string, string[]][] = [
  ["VERTEX_INPUT_INTERFACE", ["pVertexInputState", "pInputAssemblyState"]],
  ["PRE_RASTERIZATION_SHADERS", ["pViewportState", "pRasterizationState", "pTessellationState"]],
  ["FRAGMENT_SHADER", ["pDepthStencilState", "pMultisampleState"]],
  ["FRAGMENT_OUTPUT_INTERFACE", ["pColorBlendState", "pMultisampleState"]],
];

function pNextEntry(info: ArgObject, sType: string): ArgObject | null {
  const chain = info.pNext;
  const list = Array.isArray(chain) ? chain : isObject(chain) ? [chain] : [];
  return (list.find((e) => isObject(e) && e.sType === sType) as ArgObject | undefined) ?? null;
}

/**
 * A graphics pipeline create info with what the pipeline libraries it links hold filled in (libraries
 * linked from libraries included): the stages, the state members of the parts each library holds,
 * and every library's dynamic states. A linked pipeline's own create info names none of that, so
 * everything that reads a pipeline's state reads this. Null when the create info links no libraries,
 * or a library is not in `db`.
 */
export function withLibraries(info: ArgObject, db: ObjectLookup, depth = 0): ArgObject | null {
  const link = pNextEntry(info, "VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR");
  const libraries = link && Array.isArray(link.pLibraries) ? link.pLibraries : null;
  if (!libraries || depth > 8) return null;
  const out: ArgObject = { ...info };
  const stages: ArgValue[] = Array.isArray(info.pStages) ? [...info.pStages] : [];
  const dynamic: ArgValue[] = isObject(info.pDynamicState) && Array.isArray(info.pDynamicState.pDynamicStates) ? [...info.pDynamicState.pDynamicStates] : [];
  for (const ref of libraries) {
    const library = db.getObject(isObject(ref) && typeof ref.__id === "number" ? ref.__id : null);
    const raw = library?.args && Array.isArray(library.args.pCreateInfos) ? library.args.pCreateInfos[library.index] : null;
    if (!isObject(raw)) return null;
    const own = withLibraries(raw, db, depth + 1) ?? raw;
    const flags = String(pNextEntry(raw, "VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT")?.flags ?? "");
    // A library linked from others holds what they hold; one made directly holds the parts its flags name.
    const holds = (part: string): boolean => (own !== raw ? true : flags.includes(part));
    for (const [part, members] of LIBRARY_PARTS) {
      if (!holds(part)) continue;
      for (const m of members) if (!isObject(out[m]) && isObject(own[m])) out[m] = own[m];
    }
    for (const s of Array.isArray(own.pStages) ? own.pStages : []) {
      if (!isObject(s)) continue;
      const fragment = s.stage === "VK_SHADER_STAGE_FRAGMENT_BIT";
      if (!holds(fragment ? "FRAGMENT_SHADER" : "PRE_RASTERIZATION_SHADERS")) continue;
      if (!stages.some((x) => isObject(x) && x.stage === s.stage)) stages.push(s);
    }
    if (isObject(own.pDynamicState) && Array.isArray(own.pDynamicState.pDynamicStates)) {
      for (const d of own.pDynamicState.pDynamicStates) if (!dynamic.includes(d)) dynamic.push(d);
    }
    if (!out.layout && own.layout) out.layout = own.layout;
    if (!out.renderPass && own.renderPass) out.renderPass = own.renderPass;
  }
  out.stageCount = stages.length;
  out.pStages = stages;
  if (dynamic.length) out.pDynamicState = { ...(isObject(info.pDynamicState) ? info.pDynamicState : {}), dynamicStateCount: dynamic.length, pDynamicStates: dynamic };
  return out;
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
  /** Where a pipeline linked from libraries finds them (set by the object database), and its create info with theirs filled in. */
  libraryLookup: ObjectLookup | null = null;
  private _linkedDescriptor: ArgObject | null = null;

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

  /** The type without its API's prefix: "VkImage" -> "Image", "ID3D12Resource" -> "Resource" (backend.ts). */
  get shortType(): string {
    return shortTypeName(this.type);
  }

  get name(): string {
    if (this.label) return this.label;
    // Metal objects with a name of their own: a function is its entry point's name, the device
    // the GPU's. Better than a number in every list they appear in.
    const own = this.args?.name;
    if (typeof own === "string" && own && this.type.startsWith("MTL")) {
      // A function's stage tells its entry points apart in a list: "main (vertex)".
      const kind = this.type === "MTLFunction" ? this.args?.functionType : undefined;
      return typeof kind === "string" && kind ? `${own} (${kind})` : own;
    }
    return `${this.shortType} ${this.id}`;
  }

  get isInvalid(): boolean {
    return !!this.invalidReason;
  }

  /**
   * The create-info struct for this object, when the creating call has one. A Metal object's
   * arguments are its descriptor (src/metal/src/tracker.h): the library flattens the creating
   * call's descriptor into `args`, reflection included.
   */
  get descriptor(): ArgObject | null {
    const a = this.args;
    if (!a) return null;
    if (this.type.startsWith("MTL")) return a;
    // A plugin's objects are described the same way: the library sends what describes them as `args`.
    if (backendForObjectType(this.type)?.builtin === false) return a;
    // A D3D12 object's descriptor is the creating call's pDesc (src/d3d12/README.md, "Talking to the
    // UI"); a call without one (GetBuffer, CreateFence) has only its parameters.
    if (isD3D12Type(this.type)) return isObject(a.pDesc) ? a.pDesc : a;
    if (isObject(a.pCreateInfo)) return a.pCreateInfo;
    if (isObject(a.pAllocateInfo)) return a.pAllocateInfo;
    if (Array.isArray(a.pCreateInfos)) {
      const d = a.pCreateInfos[this.index];
      if (!isObject(d)) return null;
      // A pipeline linked from graphics pipeline libraries: its state is what they hold.
      if (this.libraryLookup && this.type === "VkPipeline") {
        this._linkedDescriptor ??= withLibraries(d, this.libraryLookup);
        if (this._linkedDescriptor) return this._linkedDescriptor;
      }
      return d;
    }
    if (isObject(a.pBeginInfo)) return a.pBeginInfo;
    return null;
  }

  /** One-line summary shown next to the name in the object list. */
  summary(db: ObjectLookup | null): string {
    const d = this.descriptor;
    const a = this.args ?? {};
    switch (this.type) {
      case "VkAccelerationStructureKHR":
      case "ID3D12RaytracingAccelerationStructure":
        return structureSummary(this);
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
      // D3D12 objects, from their pDesc (d3d12/d3d12_object.ts reads the resource shapes).
      case "ID3D12Resource": {
        const shape = d3d12TextureShape(this, db);
        if (shape) {
          const dims = shape.dimension === "3d" ? `${shape.width}x${shape.height}x${shape.depth}` : `${shape.width}x${shape.height}`;
          const layers = shape.layers > 1 ? ` [${shape.layers}]` : "";
          const mips = shape.mips > 1 ? ` ${shape.mips} mips` : "";
          const samples = shape.samples > 1 ? ` ${shape.samples}x` : "";
          return `${this.cmd === "GetBuffer" ? "back buffer " : ""}${dxgiFormatShort(shape.format)} ${dims}${layers}${mips}${samples}`;
        }
        return d && d.Width !== undefined ? `buffer ${formatBytes(num(d.Width))}${d.Flags && d.Flags !== "0" ? `  ${fmtFlags(d.Flags)}` : ""}` : "";
      }
      case "ID3D12Heap":
        return d ? `${formatBytes(num(d.SizeInBytes))}${isObject(d.Properties) ? `  ${fmt(d.Properties.Type)}` : ""}` : "";
      case "ID3D12PipelineState": {
        const refl = d && isObject(d.reflection) ? Object.keys(d.reflection) : [];
        const stages = refl.length ? refl : this.blobs.map((b) => b.name.split(":")[0]);
        return stages.length ? stages.map((s) => s.replace(/_/g, " ")).join(" + ") : d3d12PipelineKind(this);
      }
      case "ID3D12DescriptorHeap":
        return d ? `${fmt(d.Type).replace(/^DESCRIPTOR_HEAP_TYPE_/, "")} x${num(d.NumDescriptors)}${str(d.Flags).includes("SHADER_VISIBLE") ? "  shader visible" : ""}` : "";
      case "ID3D12CommandQueue":
        return d ? fmt(d.Type).replace(/^COMMAND_LIST_TYPE_/, "").toLowerCase() : "";
      case "ID3D12GraphicsCommandList":
      case "ID3D12CommandList":
      case "ID3D12CommandAllocator":
        return a.type !== undefined ? fmt(a.type).replace(/^COMMAND_LIST_TYPE_/, "").toLowerCase() : "";
      case "ID3D12RootSignature": {
        const params = d && Array.isArray(d.pParameters) ? d.pParameters.length : num(d?.NumParameters);
        return d ? `${params} parameter${params === 1 ? "" : "s"}` : "";
      }
      case "ID3D12QueryHeap":
        return d ? `${fmt(d.Type).replace(/^QUERY_HEAP_TYPE_/, "")} x${num(d.Count)}` : "";
      case "ID3D12CommandSignature":
        return d ? `${Array.isArray(d.pArgumentDescs) ? d.pArgumentDescs.length : num(d.NumArgumentDescs)} args, stride ${num(d.ByteStride)}` : "";
      case "IDXGISwapChain": {
        if (!d) return "";
        const bd = isObject(d.BufferDesc) ? d.BufferDesc : d;
        return `${dxgiFormatShort(str(bd.Format))} ${num(bd.Width)}x${num(bd.Height)} x${num(d.BufferCount)}`;
      }
      case "IDXGIAdapter": {
        const desc = isObject(this.updates.Desc) ? this.updates.Desc : isObject(a.Desc) ? a.Desc : null;
        return desc ? str(desc.Description) : "";
      }
      case "ID3D12Device": {
        const adapter = db?.getObject(this.parentId);
        const level = str(a.MinimumFeatureLevel ?? a.featureLevel).replace(/^D3D_FEATURE_LEVEL_/, "").replace("_", ".");
        return `${adapter?.summary(db) ?? ""}${level ? `  feature level ${level}` : ""}`.trim();
      }
      default:
        return backendForObjectType(this.type)?.objectSummary?.(this) ?? "";
    }
  }
}

/**
 * Bytes an object occupies on the GPU: a VkDeviceMemory's allocation, a VkBuffer's size, and an
 * estimate for a VkImage from its format, size, mips, layers and samples (swapchain images from
 * their swapchain). A Metal heap's size, and a Metal buffer's or texture's allocatedSize, what
 * the driver set aside (a texture view and a buffer-backed texture share their parent's storage
 * and count 0). 0 for everything else.
 */
export function objectMemoryBytes(o: VulkanObject, db: ObjectLookup | null): number {
  const d = o.descriptor;
  switch (o.type) {
    // D3D12: a heap's size, and a resource's estimate from its description (a placed resource
    // lives inside a heap, and is counted in both like a Vulkan image inside its VkDeviceMemory).
    case "ID3D12Heap":
      return num(d?.SizeInBytes);
    case "ID3D12Resource":
      return d3d12ResourceBytes(o, db);
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
        const e = sd && isObject(sd.imageExtent) ? sd.imageExtent : null;
        return sd ? estimateImageBytes(str(sd.imageFormat), num(e?.width), num(e?.height), 1, 1, num(sd.imageArrayLayers) || 1, 1) : 0;
      }
      if (!d) return 0;
      const e = isObject(d.extent) ? d.extent : null;
      const samples = Number(/VK_SAMPLE_COUNT_(\d+)_BIT/.exec(str(d.samples))?.[1] ?? 1);
      return estimateImageBytes(str(d.format), num(e?.width), num(e?.height), num(e?.depth) || 1, num(d.mipLevels) || 1, num(d.arrayLayers) || 1, samples);
    }
    default:
      return backendForObjectType(o.type)?.objectBytes?.(o) ?? 0;
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
  if (m) return m[1];
  // D3D12 and DXGI enums: "DXGI_FORMAT_R8G8B8A8_UNORM" -> "R8G8B8A8_UNORM", "D3D12_CULL_MODE_BACK" -> "BACK".
  for (const p of D3D12_PREFIXES) if (v.startsWith(p)) return v.substring(p.length);
  return v;
}

const D3D12_PREFIXES = [
  "DXGI_FORMAT_", "D3D12_DESCRIPTOR_RANGE_TYPE_", "D3D12_ROOT_PARAMETER_TYPE_", "D3D12_SHADER_VISIBILITY_",
  "D3D12_PRIMITIVE_TOPOLOGY_TYPE_", "D3D_PRIMITIVE_TOPOLOGY_", "D3D12_PRIMITIVE_TOPOLOGY_", "D3D12_CULL_MODE_", "D3D12_FILL_MODE_",
  "D3D12_COMPARISON_FUNC_", "D3D12_DEPTH_WRITE_MASK_", "D3D12_BLEND_OP_", "D3D12_BLEND_", "D3D12_LOGIC_OP_", "D3D12_STENCIL_OP_",
  "D3D12_RESOURCE_STATE_", "D3D12_RESOURCE_DIMENSION_", "D3D12_RESOURCE_FLAG_", "D3D12_HEAP_TYPE_", "D3D12_HEAP_FLAG_",
  "D3D12_TEXTURE_LAYOUT_", "D3D12_FILTER_", "D3D12_TEXTURE_ADDRESS_MODE_", "D3D12_SRV_DIMENSION_", "D3D12_UAV_DIMENSION_",
  "D3D12_RTV_DIMENSION_", "D3D12_DSV_DIMENSION_", "D3D12_DSV_FLAG_", "D3D12_CLEAR_FLAG_", "D3D12_RESOURCE_BARRIER_TYPE_",
  "D3D12_RESOURCE_BARRIER_FLAG_", "D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_", "D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_",
  "D3D12_RENDER_PASS_FLAG_", "D3D12_COMMAND_LIST_TYPE_", "D3D12_DESCRIPTOR_HEAP_TYPE_", "D3D12_DESCRIPTOR_HEAP_FLAG_",
  "D3D12_QUERY_HEAP_TYPE_", "D3D12_QUERY_TYPE_", "D3D12_INDIRECT_ARGUMENT_TYPE_", "D3D12_INPUT_CLASSIFICATION_",
  "D3D12_PIPELINE_STATE_FLAG_", "D3D12_BARRIER_LAYOUT_", "D3D12_BARRIER_SYNC_", "D3D12_BARRIER_ACCESS_", "D3D12_BARRIER_TYPE_",
  "D3D12_FEATURE_", "D3D_FEATURE_LEVEL_", "D3D_SHADER_MODEL_", "D3D12_STATIC_BORDER_COLOR_", "D3D12_STATE_OBJECT_TYPE_",
  "D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_", "D3D12_CONSERVATIVE_RASTERIZATION_MODE_", "D3D12_COLOR_WRITE_ENABLE_",
  "DXGI_SWAP_EFFECT_", "DXGI_SCALING_", "DXGI_ALPHA_MODE_", "DXGI_MODE_SCANLINE_ORDER_", "DXGI_MODE_SCALING_", "DXGI_USAGE_",
];

/** "VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT" -> "VERTEX_BUFFER | TRANSFER_DST" */
export function fmtFlags(v: ArgValue | undefined): string {
  if (typeof v !== "string") return v === undefined ? "" : String(v);
  return v.split(" | ").map((s) => (s.startsWith("VK_")
    ? s.replace(/^VK_[A-Z0-9]+?_(USAGE_|CREATE_|STAGE_|ACCESS_|ASPECT_)?/, "").replace(/_BIT(_[A-Z]+)?$/, "$1")
    : fmt(s))).join(" | ");
}

export function formatBytes(bytes: number): string {
  if (!isFinite(bytes)) return "";
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  if (bytes < 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
  return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} GB`;
}

/**
 * What an acceleration structure is, from the build the capture library recorded on it: its level
 * and how much is in it. "not built while watching" is a structure the library never saw a build
 * of — made before it attached, or read by the frame without being written in it.
 */
function structureSummary(o: VulkanObject): string {
  const build = isObject(o.updates.build) ? o.updates.build : null;
  const d = o.descriptor;
  const type = str(build?.Type ?? build?.type ?? d?.type);
  const level = type.includes("TOP_LEVEL") ? "top level" : type.includes("BOTTOM_LEVEL") ? "bottom level" : "";
  if (!build) return level ? `${level}, not built while watching` : "not built while watching";
  const geometries = Array.isArray(build.geometries) ? build.geometries.filter(isObject) : [];
  let what = "";
  if (level === "top level") {
    const n = num(build.NumDescs ?? build.primitiveCount);
    what = `${n.toLocaleString()} instance${n === 1 ? "" : "s"}`;
  } else if (geometries.length) {
    const kind = str(geometries[0].Type ?? geometries[0].geometryType);
    const n = num(build.primitiveCount);
    what = kind.includes("AABB") ? `${n.toLocaleString()} box${n === 1 ? "" : "es"}` : `${n.toLocaleString()} triangle${n === 1 ? "" : "s"}`;
  }
  return [level, what].filter(Boolean).join(", ");
}
