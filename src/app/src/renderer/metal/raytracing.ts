// Metal ray tracing in the shapes the ray tracing views already draw.
//
// The counterpart of ../d3d12/raytracing.ts, and the third API to be turned into the one
// AccelerationBuild that ../acceleration_structure.ts and ../acceleration_scene.ts take. What the
// capture library records is in src/metal/src/raytracing.h.
//
// Where Metal makes this easier than the other two:
//
//   A build names its buffers. `descriptor.geometries[n].vertexBuffer` is `{buffer, offset,
//   capture}` as recorded, because a Metal geometry descriptor holds an `id<MTLBuffer>` rather than
//   a device address. There is no address to match, and no resolution step to reproduce here.
//
//   A top level names its bottom levels. `instancedAccelerationStructures` is an array of tracked
//   objects on the descriptor, and an instance's `accelerationStructureIndex` indexes it. Resolving
//   that is an array lookup, where Vulkan and D3D12 need a map from device address to object.
//
// Where it is harder, and where the D3D12 module's "nothing to translate" does **not** carry over.
// A D3D12_RAYTRACING_INSTANCE_DESC is byte for byte a VkAccelerationStructureInstanceKHR; a Metal
// instance descriptor is not:
//
//   * the transform is an MTLPackedFloat4x3 — four columns of three floats — which is the
//     *transpose* of Vulkan's row-major float[3][4], the same 48 bytes in the other order;
//   * the index, mask, table offset and options are four separate uint32 fields, not two 24/8
//     bitpacked words;
//   * the bottom level is an index (or, for the two Indirect types, an MTLResourceID), not an
//     8-byte address;
//   * there are five layouts, not one, chosen by `instanceDescriptorType`.
//
// So parseMetalInstances is a parser of its own rather than a reinterpret. Only the four option
// bits happen to line up, with the same values under Metal's names (METAL_INSTANCE_FLAGS).
import {
  METAL_INSTANCE_FLAGS, flagNamesOf,
  type AccelerationBuild, type AccelerationGeometry, type AccelerationInstance,
} from "../acceleration_structure.js";
import { isObject, num, refId, str, type VulkanObject } from "../vulkan/vulkan_object.js";
import type { ArgObject, ArgValue, CaptureCommand } from "../../shared/protocol.js";

/** Whether an object is a Metal acceleration structure. */
export function isMetalStructure(object: { type?: string } | null | undefined): boolean {
  return object?.type === "MTLAccelerationStructure";
}

/** The selectors the capture library records a build or refit under. */
export const METAL_BUILD_METHODS = new Set([
  "buildAccelerationStructure:descriptor:scratchBuffer:scratchBufferOffset:",
  "refitAccelerationStructure:descriptor:destination:scratchBuffer:scratchBufferOffset:",
  "refitAccelerationStructure:descriptor:destination:scratchBuffer:scratchBufferOffset:options:",
]);

/** ...and a copy, which gives the destination the source's contents. */
export const METAL_COPY_METHODS = new Set([
  "copyAccelerationStructure:toAccelerationStructure:",
  "copyAndCompactAccelerationStructure:toAccelerationStructure:",
]);

// ---------------------------------------------------------------------------------------------
// Instance descriptors
//
// Five layouts. Each is laid out here as the field offsets the parser reads, because the strides
// are not derivable from anything the capture carries and getting one wrong reads an instance out
// of the middle of its neighbor.

interface InstanceLayout {
  stride: number;
  /** Byte offset of the MTLPackedFloat4x3, or -1 for the Motion types, which carry no transform. */
  transform: number;
  options: number;
  mask: number;
  tableOffset: number;
  /** Byte offset of the uint32 index into instancedAccelerationStructures, or -1. */
  structureIndex: number;
  /** Byte offset of the MTLResourceID (8 bytes), for the Indirect types, or -1. */
  resourceId: number;
  /** Byte offset of the uint32 userID, or -1 for the Default type, which has none. */
  userId: number;
}

/**
 * The layout of each MTLAccelerationStructureInstanceDescriptorType, by its enum value.
 *
 * Taken from the struct definitions in MTLAccelerationStructure.h. The Motion forms drop the
 * transform — they name a range of keyframes in the build's motionTransformBuffer instead — so
 * their instances have no transform of their own to draw with, which the scene has to allow for.
 */
const INSTANCE_LAYOUTS: Record<number, InstanceLayout> = {
  // MTLAccelerationStructureInstanceDescriptor
  0: { stride: 64, transform: 0, options: 48, mask: 52, tableOffset: 56, structureIndex: 60, resourceId: -1, userId: -1 },
  // MTLAccelerationStructureUserIDInstanceDescriptor
  1: { stride: 68, transform: 0, options: 48, mask: 52, tableOffset: 56, structureIndex: 60, resourceId: -1, userId: 64 },
  // MTLAccelerationStructureMotionInstanceDescriptor
  2: { stride: 60, transform: -1, options: 0, mask: 4, tableOffset: 8, structureIndex: 12, resourceId: -1, userId: 16 },
  // MTLIndirectAccelerationStructureInstanceDescriptor
  3: { stride: 80, transform: 0, options: 48, mask: 52, tableOffset: 56, structureIndex: -1, resourceId: 64, userId: 60 },
  // MTLIndirectAccelerationStructureMotionInstanceDescriptor
  4: { stride: 76, transform: -1, options: 0, mask: 4, tableOffset: 8, structureIndex: -1, resourceId: 16, userId: 12 },
};

/** The identity, for a Motion instance that has no transform of its own in its descriptor. */
const IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0];

/** Which of the five layouts an `instanceDescriptorType` names. */
function layoutOf(type: string | number | undefined, stride: number): InstanceLayout {
  const byName: Record<string, number> = {
    MTLAccelerationStructureInstanceDescriptorTypeDefault: 0,
    MTLAccelerationStructureInstanceDescriptorTypeUserID: 1,
    MTLAccelerationStructureInstanceDescriptorTypeMotion: 2,
    MTLAccelerationStructureInstanceDescriptorTypeIndirect: 3,
    MTLAccelerationStructureInstanceDescriptorTypeIndirectMotion: 4,
  };
  const kind = typeof type === "string" ? byName[type] ?? 0 : typeof type === "number" ? type : 0;
  const layout = INSTANCE_LAYOUTS[kind] ?? INSTANCE_LAYOUTS[0];
  // The application may set a stride larger than the struct — padding of its own between instances
  // — and the build walks by that, so it wins over the struct's size.
  return stride > layout.stride ? { ...layout, stride } : layout;
}

/**
 * The instances a Metal top level was built from, parsed out of the buffer the build read.
 *
 * `structures` is the build's `instancedAccelerationStructures` as object ids, which is what an
 * instance's index resolves through. Returns as many whole instances as the bytes hold: a partial
 * one at the end is a read-back cut short, and half an instance is not worth guessing at.
 */
export function parseMetalInstances(bytes: Uint8Array, descriptor: ArgObject,
                                    structures: number[]): AccelerationInstance[] {
  const layout = layoutOf(
    typeof descriptor.instanceDescriptorType === "string" ? descriptor.instanceDescriptorType : undefined,
    num(descriptor.instanceDescriptorStride),
  );
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const declared = num(descriptor.instanceCount);
  const out: AccelerationInstance[] = [];
  for (let at = 0; at + layout.stride <= bytes.byteLength; at += layout.stride) {
    if (declared > 0 && out.length >= declared) break;
    // MTLPackedFloat4x3 is four columns of three, so element (row, column) is at
    // column * 12 + row * 4 — the transpose of the row-major 3x4 the views and transformPoint use.
    const transform: number[] = [];
    if (layout.transform >= 0) {
      for (let row = 0; row < 3; row++) {
        for (let column = 0; column < 4; column++) {
          transform.push(view.getFloat32(at + layout.transform + column * 12 + row * 4, true));
        }
      }
    } else {
      transform.push(...IDENTITY);
    }
    const flags = view.getUint32(at + layout.options, true);
    let reference = "";
    let referenceKind: AccelerationInstance["referenceKind"];
    let blas: number | undefined;
    if (layout.structureIndex >= 0) {
      const index = view.getUint32(at + layout.structureIndex, true);
      reference = String(index);
      referenceKind = "index";
      if (index < structures.length && structures[index]) blas = structures[index];
    } else if (layout.resourceId >= 0) {
      // An MTLResourceID, which is what the application put in the descriptor rather than anything
      // the capture handed out. It is deliberately not matched against the structures' own
      // gpuResourceID: the driver reports small values that collide across objects, so a lookup
      // would resolve confidently to the wrong structure. Shown as itself instead.
      reference = `0x${view.getBigUint64(at + layout.resourceId, true).toString(16)}`;
      referenceKind = "resourceId";
    }
    out.push({
      index: out.length,
      transform,
      customIndex: layout.userId >= 0 ? view.getUint32(at + layout.userId, true) : 0,
      mask: view.getUint32(at + layout.mask, true),
      bindingTableOffset: view.getUint32(at + layout.tableOffset, true),
      flags,
      flagNames: flagNamesOf(flags, METAL_INSTANCE_FLAGS),
      reference,
      ...(referenceKind ? { referenceKind } : {}),
      ...(blas !== undefined ? { blas } : {}),
    });
  }
  return out;
}

// ---------------------------------------------------------------------------------------------
// Builds

/** Metal's geometry kinds as the shared views name them. */
const GEOMETRY_KINDS: Record<string, string> = {
  triangles: "triangles",
  motionTriangles: "triangles",
  boundingBoxes: "aabbs",
  motionBoundingBoxes: "aabbs",
  curves: "curves",
  motionCurves: "curves",
};

/**
 * The capture id the library put on one of a build's inputs, from `args.buildData`.
 *
 * On the command's own arguments rather than beside them: a Metal descriptor already says `buffer`
 * and `offset`, so the capture id belongs there, and putting it on the command rather than on the
 * structure is what keeps a per-frame rebuild from overwriting the captured build's ids with a
 * later build's (src/metal/src/raytracing.h).
 */
export function metalBuildCapture(source: ArgValue | undefined, geometry: number, field: string): number {
  const list = isObject(source) && Array.isArray(source.buildData) ? source.buildData
             : Array.isArray(source) ? source : [];
  for (const e of list as ArgValue[]) {
    if (!isObject(e)) continue;
    if (num(e.geometry) === geometry && e.field === field) return num(e.capture);
  }
  return 0;
}

/** The field a shared view asks for, as a Metal descriptor spells it, per geometry kind. */
export const METAL_FIELDS: Record<string, string[]> = {
  vertexData: ["vertexBuffer"],
  indexData: ["indexBuffer"],
  transformData: ["transformationMatrixBuffer"],
  aabbData: ["boundingBoxBuffer"],
  data: ["instanceDescriptorBuffer"],
};

/**
 * The object ids of a build's `instancedAccelerationStructures`, in order; 0 where the entry names
 * a structure the capture does not hold, so the array's indices still line up with the instances'.
 */
export function instancedStructures(descriptor: ArgObject): number[] {
  const list = Array.isArray(descriptor.instancedAccelerationStructures)
    ? descriptor.instancedAccelerationStructures : [];
  return list.map((s) => refId(s) ?? 0);
}

/** A Metal build's descriptor, from the command's arguments or from a structure's `build` update. */
export function metalDescriptorOf(value: ArgValue | undefined): ArgObject | null {
  if (!isObject(value)) return null;
  const d = value.descriptor;
  return isObject(d) ? d : null;
}

/**
 * One build, refit or copy command as an AccelerationBuild. Null when the command is not one, so a
 * caller can map over commands without filtering first.
 *
 * `target` is the structure the build wrote, which the library names outright — there is no address
 * to resolve, and a refit with no destination writes its source, which the library already
 * resolved to the one structure the command actually built.
 */
export function parseMetalBuild(command: CaptureCommand): AccelerationBuild | null {
  const args = isObject(command.args) ? command.args : null;
  if (!args) return null;
  const target = refId(args.accelerationStructure ?? args.destinationAccelerationStructure);
  if (!target) return null;
  const descriptor = metalDescriptorOf(args);
  if (!descriptor) return null;
  return metalBuild(target, descriptor, str(args.mode) || (command.method.startsWith("refit") ? "REFIT" : "BUILD"));
}

/** An AccelerationBuild from a descriptor and the structure it built, however that was found. */
export function metalBuild(target: number, descriptor: ArgObject, mode: string): AccelerationBuild {
  const topLevel = str(descriptor.kind) === "instance";
  const geometries: AccelerationGeometry[] = [];
  let primitives = 0;

  if (topLevel) {
    // A top level has one implicit geometry — its instances — where Vulkan spells it as a geometry
    // of type INSTANCES. Made explicit here so the shared views, which walk geometries, see it.
    const count = num(descriptor.instanceCount);
    primitives = count;
    geometries.push({ index: 0, kind: "instances", flags: "", primitiveCount: count });
  } else {
    const raw = Array.isArray(descriptor.geometries) ? descriptor.geometries : [];
    raw.forEach((g, index) => {
      if (!isObject(g)) return;
      const count = num(g.primitiveCount);
      primitives += count;
      const kind = GEOMETRY_KINDS[str(g.kind)] ?? str(g.kind);
      geometries.push({
        index,
        kind,
        // Metal has no per-geometry flag set; `opaque` and the duplicate-invocation switch are the
        // two booleans that stand in for one, and they read better named than as a bit string.
        flags: [
          g.opaque === true ? "Opaque" : "",
          g.allowDuplicateIntersectionFunctionInvocation === true ? "AllowDuplicateIntersectionFunctionInvocation" : "",
        ].filter(Boolean).join(" | "),
        primitiveCount: count,
        ...(kind === "triangles" ? {
          // The library writes the same layout twice: Metal's own name for display, and the
          // canonical VK_FORMAT_* the vertex decoder knows (MTLAttributeFormat and MTLVertexFormat
          // are the same values, so this is the same mapping a draw's attributes get).
          vertexFormat: str(g.vkFormat).replace(/^VK_FORMAT_/, ""),
          vertexStride: num(g.vertexStride),
          indexType: metalIndexType(str(g.indexType)),
          // No maxVertex on purpose: a Metal triangle geometry carries a triangle count and no
          // vertex count, so what was read back is the only bound there is (triangleMesh).
        } : {}),
        ...(kind === "aabbs" ? { aabbStride: num(g.boundingBoxStride) } : {}),
      });
    });
  }

  return { target, topLevel, mode, flags: str(descriptor.usage), geometries, primitives };
}

/** "UINT16"/"UINT32"/"NONE", as the shared triangle decoder spells an index type. */
function metalIndexType(name: string): string {
  if (name.endsWith("UInt16")) return "UINT16";
  if (name.endsWith("UInt32")) return "UINT32";
  return "NONE";
}

// ---------------------------------------------------------------------------------------------
// Structures, and what a capture holds of them

/** A structure's last build, from the `build` update the library put on it. */
export function metalStructureBuild(object: VulkanObject): ArgObject | null {
  return isObject(object.updates.build) ? object.updates.build as ArgObject : null;
}

/**
 * What the library read back of this structure's inputs when the capture began
 * (`captureInputs`), or null.
 *
 * A structure built before the capture has no build command in it, and this is the only account of
 * what is in one. The descriptor is the shape; `inputs` holds the capture ids, in the same
 * `{geometry, field, buffer, capture}` shape a build command's `buildData` uses, so one lookup
 * serves both.
 */
export function metalCaptureInputs(object: VulkanObject): ArgObject | null {
  return isObject(object.updates.captureInputs) ? object.updates.captureInputs as ArgObject : null;
}

// ---------------------------------------------------------------------------------------------
// Intersection function tables
//
// Metal's answer to a shader binding table, and not much like one: there are no records in GPU
// memory and no opaque handles. A pipeline hands out a table, the application sets an entry per
// index, and the traversal calls entry N when it reaches a primitive whose
// `intersectionFunctionTableOffset` is N. So the capture knows the table exactly, and the
// interesting question is not "what does this record hold" but "does the offset a geometry declares
// land on a function at all".

export interface FunctionTableEntry {
  index: number;
  /** The MTLFunction the handle named, or "" for an entry that holds a built-in or nothing. */
  function: string;
  /** "triangle" or "curve" for setOpaque…IntersectionFunctionWithSignature:, else "". */
  opaque: string;
  /** The signature flags of a built-in entry, e.g. "Instancing|TriangleData". */
  signature: string;
  /** Never set: a ray reaching this entry calls nothing. */
  empty: boolean;
}

export interface FunctionTableView {
  functionCount: number;
  entries: FunctionTableEntry[];
  /** Buffers bound to the table's own argument slots, which its functions read through. */
  buffers: { index: number; buffer: number; offset: number }[];
  visibleFunctionTables: { index: number; table: number }[];
}

/** A tracked MTLIntersectionFunctionTable or MTLVisibleFunctionTable as the view draws it; null for anything else. */
export function functionTableView(object: VulkanObject): FunctionTableView | null {
  if (object.type !== "MTLIntersectionFunctionTable" && object.type !== "MTLVisibleFunctionTable") return null;
  const update = isObject(object.updates.table) ? object.updates.table as ArgObject : null;
  const declared = num(object.descriptor?.functionCount);
  const rawEntries = update && Array.isArray(update.entries) ? update.entries : [];
  const entries: FunctionTableEntry[] = rawEntries.filter(isObject).map((e, i) => ({
    index: e.index !== undefined ? num(e.index) : i,
    function: str(e.function),
    opaque: str(e.opaque),
    signature: str(e.signature),
    empty: e.empty === true,
  }));
  const buffers = (update && Array.isArray(update.buffers) ? update.buffers : []).filter(isObject).map((b) => ({
    index: num(b.index), buffer: num(b.buffer), offset: num(b.offset),
  })).sort((a, b) => a.index - b.index);
  const visible = (update && Array.isArray(update.visibleFunctionTables) ? update.visibleFunctionTables : [])
    .filter(isObject).map((v) => ({ index: num(v.index), table: num(v.table) }))
    .sort((a, b) => a.index - b.index);
  return {
    functionCount: update ? num(update.functionCount) || declared : declared,
    entries, buffers, visibleFunctionTables: visible,
  };
}

/** The linked functions a pipeline was built with, by name: what its table's entries can hold. */
export function linkedFunctionNames(pipeline: VulkanObject | null | undefined): string[] {
  const linked = isObject(pipeline?.descriptor?.linkedFunctions) ? pipeline!.descriptor!.linkedFunctions as ArgObject : null;
  if (!linked) return [];
  const names: string[] = [];
  for (const key of ["functions", "binaryFunctions", "privateFunctions"]) {
    const list = Array.isArray(linked[key]) ? linked[key] : [];
    for (const f of list) {
      if (isObject(f) && str(f.name)) names.push(str(f.name));
    }
  }
  return names;
}
