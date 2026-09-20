// DXR in the shapes the ray tracing views already draw.
//
// The two APIs describe the same three things in different words, so rather than a second set of
// views this turns a D3D12 capture into what renderer/ray_tracing_view.ts and
// renderer/acceleration_structure.ts already take:
//
//   Shader groups. Vulkan's pGroups become D3D12's hit group subobjects, and a group's "shaders"
//   are the exports a hit group names. Where Vulkan numbers its groups and hands out a handle per
//   index, D3D12 names its exports and hands out a 32-byte identifier per name — so a D3D12 group
//   carries the name the runtime knew it by, and a binding table record is matched by identifier
//   rather than by position.
//
//   Builds. A D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC carries its primitive counts and
//   its formats inside the geometry descriptions rather than in a separate range structure, and
//   names its destination by GPU address rather than by handle — the capture library resolves that
//   address to the object it minted (src/d3d12/src/raytracing.h) and puts the id on the command.
//
//   Instances. Nothing to translate: D3D12_RAYTRACING_INSTANCE_DESC is byte for byte
//   VkAccelerationStructureInstanceKHR — a row-major 3x4 transform, a 24/8 pair for the custom
//   index and mask, another for the hit group offset and flags, and an 8-byte reference — and the
//   instance flag bits have the same values under different names. So parseInstances reads a DXR
//   instance buffer unchanged.
import type { AccelerationBuild, AccelerationGeometry } from "../acceleration_structure.js";
import { walkRecords, type BindingTableRecord } from "../binding_table.js";
import type { ShaderGroup } from "../shader_cache.js";
import type { ArgObject, ArgValue, CaptureCommand } from "../../shared/protocol.js";
import { isObject, num, str, type VulkanObject } from "../vulkan/vulkan_object.js";
import { vkFormatOfDxgi } from "./dxgi_format.js";

/** Bytes of a shader identifier, and so of the start of every binding table record. Fixed by DXR. */
export const IDENTIFIER_SIZE = 32;

/** Whether an object is a D3D12 acceleration structure, the capture library's own minted kind. */
export function isD3D12Structure(object: { type?: string } | null | undefined): boolean {
  return object?.type === "ID3D12RaytracingAccelerationStructure";
}

// ---------------------------------------------------------------------------------------------
// State objects

/** One export of a state object, with the identifier the runtime gave it. */
export interface StateObjectExport {
  name: string;
  /** Lowercase hex of the 32 identifier bytes, as the capture library wrote them. */
  identifier: string;
  /** The stack this export needs, or null where the runtime gives none (every hit group). */
  stackSize: number | null;
}

export interface StateObjectInfo {
  exports: StateObjectExport[];
  pipelineStackSize: number;
  /**
   * A DXIL library the description exported wholesale (NumExports 0). Its exports are known only
   * where the application asked the runtime for one, so a table record matching nothing may be an
   * export nobody asked about rather than a table filled wrongly.
   */
  unlistedExports: boolean;
  maxRecursionDepth: number | null;
  maxPayloadBytes: number | null;
  maxAttributeBytes: number | null;
}

/** What a state object exports and how deep it may trace, or null for an object with no DXR in it. */
export function stateObjectInfo(object: VulkanObject | null | undefined): StateObjectInfo | null {
  if (!object || object.type !== "ID3D12StateObject") return null;
  const identifiers = isObject(object.updates.shaderIdentifiers) ? object.updates.shaderIdentifiers : null;
  const raw = identifiers && Array.isArray(identifiers.exports) ? identifiers.exports : [];
  const exports: StateObjectExport[] = raw.filter(isObject).map((e) => ({
    name: str(e.name),
    identifier: str(e.identifier),
    stackSize: e.stackSize === null || e.stackSize === undefined ? null : num(e.stackSize),
  }));

  let maxRecursionDepth: number | null = null;
  let maxPayloadBytes: number | null = null;
  let maxAttributeBytes: number | null = null;
  for (const s of subobjects(object)) {
    const type = str(s.Type);
    if (type.includes("RAYTRACING_PIPELINE_CONFIG")) maxRecursionDepth = num(s.MaxTraceRecursionDepth);
    else if (type.includes("RAYTRACING_SHADER_CONFIG")) {
      maxPayloadBytes = num(s.MaxPayloadSizeInBytes);
      maxAttributeBytes = num(s.MaxAttributeSizeInBytes);
    }
  }
  if (!exports.length && maxRecursionDepth === null && maxPayloadBytes === null) return null;
  return {
    exports,
    pipelineStackSize: identifiers ? num(identifiers.pipelineStackSize) : 0,
    unlistedExports: identifiers ? identifiers.unlistedExports === true : false,
    maxRecursionDepth, maxPayloadBytes, maxAttributeBytes,
  };
}

function subobjects(object: VulkanObject): ArgObject[] {
  const desc = object.descriptor;
  const list = desc && Array.isArray(desc.pSubobjects) ? desc.pSubobjects : [];
  return list.filter(isObject);
}

/**
 * A state object's shader groups: one per hit group subobject, then one per export the runtime gave
 * an identifier for that no hit group already covers — the raygen, the misses and the callables,
 * which in D3D12 go in a table by themselves rather than in a group.
 *
 * The index is the position in this list, which is what a record of the table is reported against;
 * D3D12 has no group index of its own, so it is this list's order and nothing more.
 */
export function d3d12ShaderGroups(object: VulkanObject | null | undefined): ShaderGroup[] {
  if (!object || object.type !== "ID3D12StateObject") return [];
  const out: ShaderGroup[] = [];
  const named = new Set<string>();
  for (const s of subobjects(object)) {
    if (!str(s.Type).includes("HIT_GROUP")) continue;
    const name = str(s.HitGroupExport);
    named.add(name);
    const kind = str(s.HitGroupType).includes("PROCEDURAL") ? "procedural hit" : "triangles hit";
    out.push({
      index: out.length, type: kind, name,
      closestHitName: str(s.ClosestHitShaderImport) || undefined,
      anyHitName: str(s.AnyHitShaderImport) || undefined,
      intersectionName: str(s.IntersectionShaderImport) || undefined,
    });
  }
  const info = stateObjectInfo(object);
  for (const e of info?.exports ?? []) {
    if (named.has(e.name)) continue;
    out.push({ index: out.length, type: "general", name: e.name });
  }
  return out;
}

/** The export whose identifier is `hex`, or null: how a binding table record names what it runs. */
export function exportWithIdentifier(info: StateObjectInfo | null, hex: string): string | null {
  if (!info || !hex) return null;
  const want = hex.toLowerCase();
  for (const e of info.exports) {
    if (e.identifier.toLowerCase() === want) return e.name;
  }
  return null;
}

// ---------------------------------------------------------------------------------------------
// The shader binding table

/** A DispatchRays command's four regions, as record counts. */
export function d3d12BindingTableRegions(args: ArgObject | null | undefined): { region: string; records: number; stride: number; size: number }[] {
  const desc = args && isObject(args.pDesc) ? args.pDesc : null;
  if (!desc) return [];
  const out: { region: string; records: number; stride: number; size: number }[] = [];
  const add = (region: string, value: ArgValue | undefined, oneRecord: boolean): void => {
    if (!isObject(value)) return;
    const size = num(value.SizeInBytes);
    // The raygen region is a single record, so it has no stride of its own: its size is the record.
    const stride = oneRecord ? size : num(value.StrideInBytes);
    out.push({ region, size, stride, records: stride > 0 ? Math.floor(size / stride) : 0 });
  };
  add("RayGeneration", desc.RayGenerationShaderRecord, true);
  add("Miss", desc.MissShaderTable, false);
  add("HitGroup", desc.HitGroupTable, false);
  add("Callable", desc.CallableShaderTable, false);
  return out;
}

/** The state object a trace ran, as the capture library recorded it on the command. */
export function traceStateObjectId(command: CaptureCommand): number {
  const ref = (command as { stateObject?: ArgValue }).stateObject;
  return isObject(ref) ? num(ref.__id) : 0;
}

/** The capture id and stride of one region's read-back contents, from the command's bindingTableData. */
export function bindingTableCapture(command: CaptureCommand, region: string): { capture: number; stride: number } | null {
  const list = Array.isArray((command as { bindingTableData?: ArgValue }).bindingTableData)
    ? (command as { bindingTableData?: ArgValue[] }).bindingTableData! : [];
  for (const e of list) {
    if (isObject(e) && str(e.region) === region) return { capture: num(e.capture), stride: num(e.stride) };
  }
  return null;
}

// ---------------------------------------------------------------------------------------------
// Builds

/** The structure a BuildRaytracingAccelerationStructure wrote, as the library resolved its address. */
export function buildTarget(command: CaptureCommand): number {
  return num((command as { destStructure?: ArgValue }).destStructure);
}

/** The capture id of one of a build's inputs, from the command's `buildData`. */
export function buildCapture(command: CaptureCommand, geometry: number, field: string): number {
  const list = Array.isArray((command as { buildData?: ArgValue }).buildData)
    ? (command as { buildData?: ArgValue[] }).buildData! : [];
  for (const e of list) {
    if (!isObject(e)) continue;
    // A top level's one input has no geometry index; a bottom level's inputs carry theirs.
    const at = e.geometry === undefined ? -1 : num(e.geometry);
    if (at === geometry && str(e.field) === field) return num(e.capture);
  }
  return 0;
}

const SHORT = (s: string, prefix: RegExp): string => str(s).replace(prefix, "");

/**
 * A build's description in the shape the views take. Null when the arguments are not a build's, so
 * a caller can map over commands without filtering first.
 *
 * `command` is where the input read-backs are: the capture ids go on the command rather than on the
 * structure because a structure's update is last-write-wins, and an application that rebuilds every
 * frame would leave the captured build with a later build's ids (which have none).
 */
export function parseD3D12Build(command: CaptureCommand): AccelerationBuild | null {
  const args = command.args;
  if (!isObject(args)) return null;
  const desc = isObject(args.pDesc) ? args.pDesc : null;
  const inputs = desc && isObject(desc.Inputs) ? desc.Inputs : null;
  if (!inputs) return null;
  const target = buildTarget(command);
  if (!target) return null;
  const topLevel = str(inputs.Type).includes("TOP_LEVEL");
  const flags = str(inputs.Flags);

  const geometries: AccelerationGeometry[] = [];
  let primitives = 0;
  if (topLevel) {
    primitives = num(inputs.NumDescs);
    geometries.push({
      index: 0, kind: "instances", flags: "", primitiveCount: primitives,
      instanceData: buildCapture(command, -1, "InstanceDescs") || undefined,
    });
  } else {
    const raw = Array.isArray(inputs.pGeometryDescs) ? inputs.pGeometryDescs
              : Array.isArray(inputs.ppGeometryDescs) ? inputs.ppGeometryDescs : [];
    raw.forEach((g, index) => {
      if (!isObject(g)) return;
      const geometry = parseGeometry(command, g, index);
      primitives += geometry.primitiveCount;
      geometries.push(geometry);
    });
  }
  return {
    target, topLevel, geometries, primitives, flags,
    // Vulkan's mode is a separate enum; D3D12 folds it into the build flags.
    mode: flags.includes("PERFORM_UPDATE") ? "UPDATE" : "BUILD",
  };
}

function parseGeometry(command: CaptureCommand, g: ArgObject, index: number): AccelerationGeometry {
  const type = str(g.Type);
  const flags = str(g.Flags);
  if (type.includes("PROCEDURAL_PRIMITIVE_AABBS")) {
    const aabbs = isObject(g.AABBs) ? g.AABBs : {};
    return {
      index, kind: "aabbs", flags, primitiveCount: num(aabbs.AABBCount),
      instanceData: undefined,
      ...(buildCapture(command, index, "AABBs") ? { vertexData: buildCapture(command, index, "AABBs") } : {}),
    };
  }
  const tri = isObject(g.Triangles) ? g.Triangles : {};
  const buffer = isObject(tri.VertexBuffer) ? tri.VertexBuffer : {};
  const indexCount = num(tri.IndexCount);
  const vertexCount = num(tri.VertexCount);
  const indexFormat = str(tri.IndexFormat);
  // A D3D12 build takes counts, not a separate range structure; a triangle list with no indices
  // draws its vertices in order, so either count divided by three is the primitives.
  const primitiveCount = Math.floor((indexCount || vertexCount) / 3);
  // The mesh reader works in VK_FORMAT names, which is the one place a format has to be translated.
  const vk = vkFormatOfDxgi(str(tri.VertexFormat));
  return {
    index, kind: "triangles", flags, primitiveCount,
    vertexFormat: vk ? SHORT(vk, /^VK_FORMAT_/) : undefined,
    vertexStride: num(buffer.StrideInBytes),
    // triangleMesh reads maxVertex as the highest index the build may touch.
    maxVertex: vertexCount > 0 ? vertexCount - 1 : 0,
    indexType: indexFormat.includes("R16") ? "UINT16" : indexFormat.includes("R32") ? "UINT32" : "NONE",
    vertexData: buildCapture(command, index, "VertexBuffer") || undefined,
    indexData: buildCapture(command, index, "IndexBuffer") || undefined,
    transformData: buildCapture(command, index, "Transform3x4") || undefined,
  };
}

/**
 * The address the capture library recorded on each structure, so an instance's 8-byte reference
 * names the object it points at. The addresses are written as hex ("0x8df8000") and an instance
 * holds the number, so the map is keyed by the decimal spelling parseInstances produces.
 */
export function d3d12StructureAddresses(structures: Iterable<VulkanObject>): Map<string, number> {
  const out = new Map<string, number>();
  for (const o of structures) {
    const address = isObject(o.descriptor?.Address) ? str(o.descriptor!.Address.address) : "";
    if (!address) continue;
    try {
      out.set(BigInt(address).toString(), o.id);
    } catch {
      // An address the library could not write as a number is one nothing can match anyway.
    }
  }
  return out;
}

// ---------------------------------------------------------------------------------------------
// Records

/** What building a D3D12 trace's records needs from a capture. */
export interface D3D12TableSource {
  command: CaptureCommand;
  /** The state object bound at the trace, for the identifiers it handed out. */
  stateObject: VulkanObject | null;
  bytesOf(captureId: number): Uint8Array | null;
}

/**
 * Every record of a trace's binding table, matched to the state object's exports. Empty when the
 * capture did not read the table back.
 *
 * Unlike Vulkan's, a D3D12 record resolves to a name rather than to a group index: the runtime
 * hands identifiers out per export, so the match says which export the bytes are and nothing has
 * to be inferred from position.
 */
export function d3d12TableRecords(source: D3D12TableSource): BindingTableRecord[] {
  const info = stateObjectInfo(source.stateObject);
  const out: BindingTableRecord[] = [];
  for (const region of ["RayGeneration", "Miss", "HitGroup", "Callable"]) {
    const found = bindingTableCapture(source.command, region);
    if (!found || !found.capture) continue;
    out.push(...walkRecords(region, source.bytesOf(found.capture), found.stride, IDENTIFIER_SIZE, (handle) => {
      const name = exportWithIdentifier(info, hexOf(handle));
      return name ? { name } : null;
    }));
  }
  return out;
}

function hexOf(bytes: Uint8Array): string {
  let out = "";
  for (const b of bytes) out += b.toString(16).padStart(2, "0");
  return out;
}
