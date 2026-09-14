// What a D3D12 object looks like in the object database: the D3D12 capture library records each
// object under its interface name (ID3D12Resource, ID3D12PipelineState, ...) with the creating
// call's parameters as its arguments, `pDesc` being the descriptor (d3d12/README.md, "Talking to
// the UI"). The Vulkan and Metal spellings of the same questions are in ../vulkan/vulkan_object.ts;
// this answers them for D3D12 so the panels can ask without knowing which API made the object.
import { dxgiFormatIsDepth, dxgiMipBytes, vkFormatOfDxgi } from "./dxgi_format.js";
import { isObject, num, str } from "../vulkan/vulkan_object.js";
import type { ObjectLookup, VulkanObject } from "../vulkan/vulkan_object.js";
import type { ArgObject, ArgValue } from "../../shared/protocol.js";

/** Whether an object type is one the D3D12 library reports (ID3D12* or IDXGI*). */
export function isD3D12Type(type: string): boolean {
  return type.startsWith("ID3D12") || type.startsWith("IDXGI");
}

/** The D3D12_RESOURCE_DESC of a resource, or null when the object is not one / has none. */
export function d3d12ResourceDesc(obj: VulkanObject | null | undefined): ArgObject | null {
  if (!obj || obj.type !== "ID3D12Resource") return null;
  const d = obj.descriptor;
  return d && (d.Dimension !== undefined || d.Width !== undefined) ? d : null;
}

/** An ID3D12Resource that is a texture (its description's Dimension is not BUFFER). */
export function isD3D12Texture(obj: VulkanObject | null | undefined): boolean {
  const d = d3d12ResourceDesc(obj);
  if (d) return str(d.Dimension) !== "D3D12_RESOURCE_DIMENSION_BUFFER";
  // A swap chain's back buffer (GetBuffer) may carry no description of its own.
  return !!obj && obj.type === "ID3D12Resource" && obj.cmd === "GetBuffer";
}

export function isD3D12Buffer(obj: VulkanObject | null | undefined): boolean {
  const d = d3d12ResourceDesc(obj);
  return !!d && str(d.Dimension) === "D3D12_RESOURCE_DIMENSION_BUFFER";
}

export interface D3D12TextureShape {
  width: number;
  height: number;
  depth: number;
  mips: number;
  layers: number;
  /** The DXGI name ("DXGI_FORMAT_R8G8B8A8_UNORM"). */
  format: string;
  samples: number;
  dimension: "1d" | "2d" | "3d";
}

/**
 * A texture's shape from its description; a back buffer without one takes its swap chain's
 * (`db` finds the parent). Null for a buffer or an object that is not a resource.
 */
export function d3d12TextureShape(obj: VulkanObject | null | undefined, db: ObjectLookup | null = null): D3D12TextureShape | null {
  if (!obj || obj.type !== "ID3D12Resource") return null;
  const d = d3d12ResourceDesc(obj);
  if (d) {
    const dim = str(d.Dimension);
    if (dim === "D3D12_RESOURCE_DIMENSION_BUFFER") return null;
    const is3D = dim === "D3D12_RESOURCE_DIMENSION_TEXTURE3D";
    const is1D = dim === "D3D12_RESOURCE_DIMENSION_TEXTURE1D";
    const depthOrLayers = num(d.DepthOrArraySize) || 1;
    const sd = isObject(d.SampleDesc) ? d.SampleDesc : null;
    return {
      width: num(d.Width), height: is1D ? 1 : num(d.Height) || 1, depth: is3D ? depthOrLayers : 1,
      mips: num(d.MipLevels) || 1, layers: is3D ? 1 : depthOrLayers, format: str(d.Format), samples: num(sd?.Count) || 1,
      dimension: is3D ? "3d" : is1D ? "1d" : "2d",
    };
  }
  if (obj.cmd !== "GetBuffer") return null;
  const sc = db?.getObject(obj.parentId)?.descriptor ?? null;
  if (!sc) return null;
  // DXGI_SWAP_CHAIN_DESC nests the size and format in BufferDesc; DXGI_SWAP_CHAIN_DESC1 has them flat.
  const bd = isObject(sc.BufferDesc) ? sc.BufferDesc : sc;
  const sd = isObject(sc.SampleDesc) ? sc.SampleDesc : null;
  return {
    width: num(bd.Width), height: num(bd.Height) || 1, depth: 1, mips: 1, layers: 1, format: str(bd.Format),
    samples: num(sd?.Count) || 1, dimension: "2d",
  };
}

/**
 * Bytes a resource occupies, estimated: a buffer's Width; a texture's mips summed over its
 * layers, rows and row bytes from the format (a multisampled texture counts every sample).
 * 0 for anything else.
 */
export function d3d12ResourceBytes(obj: VulkanObject | null | undefined, db: ObjectLookup | null = null): number {
  const d = d3d12ResourceDesc(obj);
  if (d && str(d.Dimension) === "D3D12_RESOURCE_DIMENSION_BUFFER") return num(d.Width);
  const shape = d3d12TextureShape(obj, db);
  if (!shape) return 0;
  let total = 0;
  for (let m = 0; m < shape.mips; m++) {
    const w = Math.max(1, shape.width >> m);
    const h = Math.max(1, shape.height >> m);
    const depth = Math.max(1, shape.depth >> m);
    total += dxgiMipBytes(shape.format, w, h) * depth;
  }
  return total * shape.layers * shape.samples;
}

/** Whether a pipeline state was made for compute (a CS, or nothing but a compute stage's code). */
export function d3d12PipelineKind(obj: VulkanObject | null | undefined): "graphics" | "compute" {
  if (!obj) return "graphics";
  const d = obj.descriptor;
  if (d) {
    if (isObject(d.CS) || d.CS !== undefined) return "compute";
    if (d.VS !== undefined || d.MS !== undefined || d.PS !== undefined || isObject(d.InputLayout) || d.RTVFormats !== undefined) return "graphics";
    const refl = d.reflection;
    if (isObject(refl)) {
      const stages = Object.keys(refl);
      if (stages.length && stages.every((s) => s === "compute")) return "compute";
      if (stages.length) return "graphics";
    }
  }
  if (obj.cmd === "CreateComputePipelineState") return "compute";
  if (obj.blobs.length && obj.blobs.every((b) => b.name.startsWith("compute:"))) return "compute";
  return "graphics";
}

/** A D3D12 input element, as the pipeline's InputLayout lists them. */
export interface D3D12InputElement {
  /** Index in pInputElementDescs: the attribute location the vertex layout and the mesh view use. */
  location: number;
  name: string;            // "TEXCOORD0"
  semanticName: string;
  semanticIndex: number;
  format: string;          // DXGI name
  slot: number;
  offset: number;          // resolved (APPEND_ALIGNED_ELEMENT applied)
  perInstance: boolean;
  stepRate: number;
}

const APPEND_ALIGNED = 0xffffffff;

/** The input elements of a graphics pipeline with their offsets resolved, empty for one without an InputLayout. */
export function d3d12InputElements(pipeline: VulkanObject | null | undefined): D3D12InputElement[] {
  const d = pipeline?.descriptor;
  const layout = d && isObject(d.InputLayout) ? d.InputLayout : null;
  const elements = layout && Array.isArray(layout.pInputElementDescs) ? layout.pInputElementDescs : [];
  const out: D3D12InputElement[] = [];
  const running = new Map<number, number>();   // slot -> next appended offset
  elements.forEach((e, location) => {
    if (!isObject(e)) return;
    const slot = num(e.InputSlot);
    const format = str(e.Format);
    const size = vertexBytes(format);
    const declared = num(e.AlignedByteOffset);
    let offset = declared;
    if (declared === APPEND_ALIGNED || declared < 0) {
      // Appended elements start at the next 4-byte boundary after the previous element of the slot.
      offset = Math.ceil((running.get(slot) ?? 0) / 4) * 4;
    }
    running.set(slot, offset + size);
    const semanticName = str(e.SemanticName);
    const semanticIndex = num(e.SemanticIndex);
    out.push({
      location, name: `${semanticName}${semanticIndex}`, semanticName, semanticIndex, format, slot, offset,
      perInstance: str(e.InputSlotClass).includes("PER_INSTANCE"), stepRate: num(e.InstanceDataStepRate),
    });
  });
  return out;
}

/** Attribute names by location for the mesh view: "POSITION0", "TEXCOORD0", from the InputLayout. */
export function d3d12AttributeNames(pipeline: VulkanObject | null | undefined): Map<number, string> {
  const names = new Map<number, string>();
  for (const e of d3d12InputElements(pipeline)) names.set(e.location, e.name);
  return names;
}

/** Bytes of a vertex attribute in a DXGI format (from its bits), 0 when unknown. */
function vertexBytes(format: string): number {
  const bits = [...format.replace(/^DXGI_FORMAT_/, "").matchAll(/[RGBAX](\d+)/g)].reduce((sum, m) => sum + Number(m[1]), 0);
  return Math.ceil(bits / 8);
}

/** "D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST" (or the D3D12_ / TYPE spelling) as the VK_PRIMITIVE_TOPOLOGY name the mesh code reads. */
export function vkTopologyOfD3D(v: ArgValue | undefined): string {
  const s = str(v);
  if (!s || s.startsWith("VK_")) return s;
  const name = s.replace(/^D3D1?2?_PRIMITIVE_TOPOLOGY_(TYPE_)?/, "");
  switch (name) {
    case "POINTLIST": case "POINT": return "VK_PRIMITIVE_TOPOLOGY_POINT_LIST";
    case "LINELIST": case "LINE": return "VK_PRIMITIVE_TOPOLOGY_LINE_LIST";
    case "LINESTRIP": return "VK_PRIMITIVE_TOPOLOGY_LINE_STRIP";
    case "TRIANGLELIST": case "TRIANGLE": return "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST";
    case "TRIANGLESTRIP": return "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP";
    case "TRIANGLEFAN": return "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN";
    case "LINELIST_ADJ": return "VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY";
    case "LINESTRIP_ADJ": return "VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY";
    case "TRIANGLELIST_ADJ": return "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY";
    case "TRIANGLESTRIP_ADJ": return "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY";
    default:
      if (/CONTROL_POINT_PATCHLIST/.test(name) || name === "PATCH") return "VK_PRIMITIVE_TOPOLOGY_PATCH_LIST";
      return s;
  }
}

/** The mip level and array slice an RTV / DSV / SRV / UAV description addresses (0, 0 without one). */
export function d3d12ViewSubresource(view: ArgValue | undefined): { mip: number; slice: number } {
  if (!isObject(view)) return { mip: 0, slice: 0 };
  for (const key of ["Texture1D", "Texture1DArray", "Texture2D", "Texture2DArray", "Texture2DMS", "Texture2DMSArray", "Texture3D", "TextureCube", "TextureCubeArray"]) {
    const sub = view[key];
    if (!isObject(sub)) continue;
    const mip = sub.MipSlice !== undefined ? num(sub.MipSlice) : num(sub.MostDetailedMip);
    const slice = num(sub.FirstArraySlice) || num(sub.First2DArrayFace);
    return { mip, slice };
  }
  return { mip: 0, slice: 0 };
}

/** The VK_FORMAT spelling of a resource's format (for the image decoders); "" when it has none. */
export function d3d12ResourceVkFormat(obj: VulkanObject | null | undefined, db: ObjectLookup | null = null): string {
  const shape = d3d12TextureShape(obj, db);
  return (shape && vkFormatOfDxgi(shape.format)) || "";
}

export function d3d12IsDepthResource(obj: VulkanObject | null | undefined, db: ObjectLookup | null = null): boolean {
  const shape = d3d12TextureShape(obj, db);
  return !!shape && dxgiFormatIsDepth(shape.format);
}

/** A handle the library resolved: {heap, index, resource, view}. The resource's id, or null. */
export function handleResourceId(handle: ArgValue | undefined): number | null {
  if (!isObject(handle)) return null;
  const r = handle.resource;
  return isObject(r) && typeof r.__id === "number" ? r.__id : null;
}
