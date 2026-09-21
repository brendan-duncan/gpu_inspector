// Tying a capture's read-back buffers to the acceleration structure they were built into.
//
// acceleration_structure.ts knows the formats; this knows where the bytes are. A build names its
// geometry by device address, and the capture library resolved those addresses and read the
// contents back, leaving the capture ids on the recorded build command (src/vulkan/src/hooks.cpp,
// src/d3d12/src/raytracing.cpp). So finding what a structure holds means finding the build command
// that targeted it and following those ids.
//
// Both APIs land in the same AccelerationBuild, so only the two parsers and the two ways a build
// names its destination differ; everything after that is shared. D3D12's instance buffers need no
// translation at all, because a D3D12_RAYTRACING_INSTANCE_DESC is byte for byte a
// VkAccelerationStructureInstanceKHR (d3d12/raytracing.ts).
import {
  aabbBoxes, instanceScene, parseBuild, parseInstances, triangleMesh,
  type AccelerationBuild, type AccelerationInstance,
} from "./acceleration_structure.js";
import type { AccelerationScene } from "./ray_tracing_view.js";
import type { CaptureData } from "./capture_data.js";
import type { CaptureCommand } from "../shared/protocol.js";
import { buildCapture, d3d12StructureAddresses, parseD3D12Build } from "./d3d12/raytracing.js";
import { isObject, num, type VulkanObject } from "./vulkan/vulkan_object.js";

/** What this needs of an object database: every acceleration structure, to resolve an instance's reference. */
export interface StructureDatabase {
  getObjectsOfType(type: string): Map<number, VulkanObject> | null;
}

/** One build in a capture: the command that made it and the build it describes. */
interface CapturedBuild {
  command: CaptureCommand;
  info: number;
  build: AccelerationBuild;
}

/** Every acceleration structure build the capture recorded, in command order. */
function buildsIn(data: CaptureData): CapturedBuild[] {
  const out: CapturedBuild[] = [];
  for (const c of data.commands) {
    // D3D12 builds one structure per command; Vulkan's takes an array of them.
    if (c.method === "BuildRaytracingAccelerationStructure") {
      const build = parseD3D12Build(c);
      if (build) out.push({ command: c, info: 0, build });
      continue;
    }
    if (!c.method.includes("BuildAccelerationStructures")) continue;
    const args = c.args;
    if (!isObject(args)) continue;
    const infos = Array.isArray(args.pInfos) ? args.pInfos : [];
    const ranges = Array.isArray(args.ppBuildRangeInfos) ? args.ppBuildRangeInfos : [];
    infos.forEach((info, i) => {
      const build = parseBuild(info, ranges[i]);
      if (build) out.push({ command: c, info: i, build });
    });
  }
  return out;
}

/**
 * The capture id the layer attached to one of a build's addresses, from the command's `buildData`.
 * The ids are on the command rather than on the structure because a structure's update is
 * last-write-wins, and an application that rebuilds its top level every frame would overwrite the
 * captured build's ids with a later build's.
 */
function captureIdOf(command: CaptureCommand, info: number, geometry: number, field: string): number {
  // D3D12's inputs are named the way its structures are (VertexBuffer, InstanceDescs) and carry no
  // info index, since one command builds one structure.
  if (command.method === "BuildRaytracingAccelerationStructure") {
    return buildCapture(command, D3D12_FIELDS[field] === "InstanceDescs" ? -1 : geometry, D3D12_FIELDS[field] ?? field);
  }
  const list = Array.isArray(command.buildData) ? command.buildData : [];
  for (const e of list) {
    if (!isObject(e)) continue;
    if (num(e.info) === info && num(e.geometry) === geometry && e.field === field) return num(e.capture);
  }
  return 0;
}

/** The Vulkan field names this module asks for, as D3D12 records them. */
const D3D12_FIELDS: Record<string, string> = {
  data: "InstanceDescs",
  vertexData: "VertexBuffer",
  indexData: "IndexBuffer",
  transformData: "Transform3x4",
  aabbData: "AABBs",
};

/** The bytes of a capture's buffer read-back, or null when it holds none. */
function bytesOf(data: CaptureData, captureId: number): Uint8Array | null {
  return data.buffer(captureId)?.data ?? null;
}

/** Addresses the capture library recorded on the structures, so an instance's reference names an object. */
function addressesOf(db: StructureDatabase): Map<string, number> {
  const out = new Map<string, number>();
  for (const o of db.getObjectsOfType("VkAccelerationStructureKHR")?.values() ?? []) {
    const a = o.updates.deviceAddress;
    if (a !== undefined && a !== null) out.set(String(a), o.id);
  }
  // D3D12's structures are named by the address a build wrote them to, which is their identity.
  for (const [address, id] of d3d12StructureAddresses(db.getObjectsOfType("ID3D12RaytracingAccelerationStructure")?.values() ?? [])) {
    out.set(address, id);
  }
  return out;
}

/**
 * The scene a top level describes, or null when this capture holds no build of it. A bottom level
 * has no scene: its geometry is shown against the build itself.
 */
export function accelerationScene(data: CaptureData, db: StructureDatabase, structureId: number): AccelerationScene | null {
  const builds = buildsIn(data);
  // The last build of this structure in the capture is the one that decided its contents.
  const target = [...builds].reverse().find((b) => b.build.target === structureId && b.build.topLevel);
  if (!target) return null;

  const instances: AccelerationInstance[] = [];
  const addresses = addressesOf(db);
  target.build.geometries.forEach((g, index) => {
    if (g.kind !== "instances") return;
    const bytes = bytesOf(data, captureIdOf(target.command, target.info, index, "data"));
    if (bytes) instances.push(...parseInstances(bytes, addresses));
  });
  if (!instances.length) return null;

  // A bottom level's geometry is only here when its build was captured too, which for most
  // applications it was not: a bottom level is built once, before anything is capturing.
  const meshes = new Map<number, Float32Array | null>();
  const meshOf = (blas: number): Float32Array | null => {
    const hit = meshes.get(blas);
    if (hit !== undefined) return hit;
    let mesh: Float32Array | null = null;
    const source = [...builds].reverse().find((b) => b.build.target === blas && !b.build.topLevel);
    if (source) {
      for (let g = 0; g < source.build.geometries.length && !mesh; g++) {
        const geometry = source.build.geometries[g];
        mesh = triangleMesh(
          geometry,
          bytesOf(data, captureIdOf(source.command, source.info, g, "vertexData")),
          bytesOf(data, captureIdOf(source.command, source.info, g, "indexData")),
        );
      }
    }
    meshes.set(blas, mesh);
    return mesh;
  };

  // A procedural bottom level has no triangles; its boxes are the whole of its shape outside the
  // intersection shader, and they are what the traversal tests against.
  const boxes = new Map<number, Float32Array | null>();
  const boxesOf = (blas: number): Float32Array | null => {
    const hit = boxes.get(blas);
    if (hit !== undefined) return hit;
    let mesh: Float32Array | null = null;
    const source = [...builds].reverse().find((b) => b.build.target === blas && !b.build.topLevel);
    if (source) {
      const parts: Float32Array[] = [];
      for (let g = 0; g < source.build.geometries.length; g++) {
        const geometry = source.build.geometries[g];
        const part = aabbBoxes(geometry, bytesOf(data, captureIdOf(source.command, source.info, g, "aabbData")));
        if (part) parts.push(part);
      }
      if (parts.length) {
        const total = parts.reduce((n, p) => n + p.length, 0);
        mesh = new Float32Array(total);
        let at = 0;
        for (const p of parts) {
          mesh.set(p, at);
          at += p.length;
        }
      }
    }
    boxes.set(blas, mesh);
    return mesh;
  };

  return { instances, meshOf, boxesOf };
}

/** Re-exported so a caller can draw a scene without importing both modules. */
export { instanceScene };

// ---------------------------------------------------------------------------------------------
// One answer for "what can be shown of this structure, and if nothing, why not"
//
// The Inspect panel, the tab and every button that offers to open one all ask this, so they all
// agree. A structure with nothing to draw is the common case rather than a fault — an engine builds
// its bottom levels once, at load, and a capture of a later frame holds no build of them — so the
// reason is part of the answer rather than something a caller has to work out.

export type StructureShape = "instances" | "triangles" | "aabbs" | "none";

export interface StructureDrawing {
  /** What the preview draws: positions per vertex, already expanded into primitives. Empty when there is nothing. */
  positions: Float32Array;
  kind: "triangles" | "lines";
  /** What those positions are, which decides what the view may claim about them. */
  shape: StructureShape;
  /** A top level's instances, for the table beside the preview; empty for a bottom level. */
  instances: AccelerationInstance[];
  /** How many instances were drawn with their own geometry rather than a stand-in box. */
  placed: number;
  /** Why there is nothing to draw, or "" when there is. */
  note: string;
}

const NOTHING = (note: string): StructureDrawing =>
  ({ positions: new Float32Array(0), kind: "lines", shape: "none", instances: [], placed: 0, note });

/**
 * What a structure can be shown as. A top level is its instances placed in the world; a bottom
 * level is its own geometry, which is triangles or the boxes of a procedural one.
 */
export function structureDrawing(data: CaptureData, db: StructureDatabase, structureId: number): StructureDrawing {
  const builds = buildsIn(data);
  const target = [...builds].reverse().find((b) => b.build.target === structureId);
  if (!target) {
    return NOTHING("This capture holds no build of this structure. A bottom level is usually built once, at load, "
      + "and a frame captured later reads it without ever writing it — capture a frame that rebuilds it to see what is in it.");
  }

  if (target.build.topLevel) {
    const scene = accelerationScene(data, db, structureId);
    if (!scene || !scene.instances.length) {
      return NOTHING("The instances this top level was built from are not in the capture, so the scene it describes is not known.");
    }
    const drawn = instanceScene(scene.instances, scene.meshOf, scene.boxesOf);
    return {
      positions: drawn.mesh, kind: drawn.kind,
      shape: drawn.drawn === "none" ? "instances" : drawn.drawn === "triangles" ? "triangles" : "aabbs",
      instances: scene.instances, placed: drawn.placed, note: "",
    };
  }

  // A bottom level: its own geometry, in its own space. Every geometry of the build together, so a
  // structure of several reads as the one thing it is.
  const triangles: Float32Array[] = [];
  const boxes: Float32Array[] = [];
  target.build.geometries.forEach((g, index) => {
    const mesh = triangleMesh(
      g,
      bytesOf(data, captureIdOf(target.command, target.info, index, "vertexData")),
      bytesOf(data, captureIdOf(target.command, target.info, index, "indexData")),
    );
    if (mesh) triangles.push(mesh);
    const box = aabbBoxes(g, bytesOf(data, captureIdOf(target.command, target.info, index, "aabbData")));
    if (box) boxes.push(box);
  });
  const join = (parts: Float32Array[]): Float32Array => {
    const out = new Float32Array(parts.reduce((n, p) => n + p.length, 0));
    let at = 0;
    for (const p of parts) {
      out.set(p, at);
      at += p.length;
    }
    return out;
  };
  // Triangles win, as they do in a scene: a box around known geometry says less than the geometry.
  if (triangles.length) return { positions: join(triangles), kind: "triangles", shape: "triangles", instances: [], placed: 0, note: "" };
  if (boxes.length) return { positions: join(boxes), kind: "lines", shape: "aabbs", instances: [], placed: 0, note: "" };
  return NOTHING("This build's geometry was not read back, so what the structure holds is not known. "
    + "A build reads its vertices by GPU address, and an address the capture could not tie to a buffer has no contents to fetch.");
}

/** Every type a capture's acceleration structures go under, whichever API took it. */
export const STRUCTURE_TYPES = ["VkAccelerationStructureKHR", "VkAccelerationStructureNV", "ID3D12RaytracingAccelerationStructure"];

// ---------------------------------------------------------------------------------------------
// Which structures a command names
//
// A build, a copy, a postbuild query and a trace all name acceleration structures, and none of them
// the same way: Vulkan by handle, D3D12 by the GPU address a build wrote the structure to — in its
// arguments, in a descriptor's view, or as a root SRV the trace reads. Every one of those addresses
// is written by the capture library as {address, buffer, offset}, and every structure it minted
// carries the same, so matching on the address finds them all whatever field they are in.

export interface StructureReference {
  id: number;
  /** What the command does with it: "builds", "updates from", "copies to", "reads", "traces". */
  role: string;
}

/** The role a structure plays in a command, from the field that names it. */
function roleOf(key: string, method: string): string {
  if (/^(DestAccelerationStructureData|dstAccelerationStructure|destStructure)$/.test(key)) {
    return /Copy/.test(method) ? "copies to" : "builds";
  }
  if (/^(SourceAccelerationStructureData|srcAccelerationStructure)$/.test(key)) {
    return /Copy/.test(method) ? "copies from" : "updates from";
  }
  if (/^pSourceAccelerationStructureData$/.test(key)) return "queries";
  return "traces";
}

/**
 * Every acceleration structure `command` names, each once, in the order found. `bound` is what the
 * command read through its bindings (a trace's descriptor tables and root views), which the command
 * itself does not carry.
 */
export function structuresOfCommand(command: { method: string; args?: unknown; [key: string]: unknown },
                                    structures: VulkanObject[], bound: unknown = null): StructureReference[] {
  const byId = new Map(structures.map((o) => [o.id, o]));
  // Every structure's captured address, lower-cased hex: the one thing every way of naming it shares.
  const byAddress = new Map<string, number>();
  const byPlace = new Map<string, number>();
  for (const o of structures) {
    const a = o.descriptor && typeof o.descriptor === "object" ? (o.descriptor as Record<string, unknown>).Address : null;
    if (!a || typeof a !== "object") continue;
    const rec = a as Record<string, unknown>;
    if (typeof rec.address === "string") byAddress.set(rec.address.toLowerCase(), o.id);
    const buffer = rec.buffer && typeof rec.buffer === "object" ? (rec.buffer as Record<string, unknown>).__id : null;
    if (typeof buffer === "number") byPlace.set(`${buffer}:${Number(rec.offset ?? 0)}`, o.id);
  }
  const out: StructureReference[] = [];
  const seen = new Set<number>();
  const add = (id: number | undefined, key: string): void => {
    if (id === undefined || seen.has(id) || !byId.has(id)) return;
    seen.add(id);
    out.push({ id, role: roleOf(key, command.method) });
  };
  const walk = (v: unknown, key: string, depth: number): void => {
    if (depth > 12 || v === null || typeof v !== "object") return;
    if (Array.isArray(v)) {
      for (const item of v) walk(item, key, depth + 1);
      return;
    }
    const rec = v as Record<string, unknown>;
    // A handle reference to a structure (Vulkan, and the D3D12 descriptor's minted object).
    if (typeof rec.__id === "number" && byId.has(rec.__id)) add(rec.__id, key);
    // An address the library resolved, in an argument or a view.
    if (typeof rec.address === "string") add(byAddress.get(rec.address.toLowerCase()), key);
    // A root view names the buffer and offset rather than the address.
    const buffer = rec.buffer && typeof rec.buffer === "object" ? (rec.buffer as Record<string, unknown>).__id : null;
    if (typeof buffer === "number") add(byPlace.get(`${buffer}:${Number(rec.offset ?? 0)}`), key);
    for (const [k, child] of Object.entries(rec)) {
      if (k === "__id" || k === "__class") continue;
      walk(child, k, depth + 1);
    }
  };
  // The structure a D3D12 build wrote, as the library resolved it.
  if (typeof command.destStructure === "number") add(command.destStructure, "destStructure");
  walk(command.args, "", 0);
  walk(bound, "bound", 0);
  return out;
}
