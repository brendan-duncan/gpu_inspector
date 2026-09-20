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
  instanceScene, parseBuild, parseInstances, triangleMesh,
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

  return { instances, meshOf };
}

/** Re-exported so a caller can draw a scene without importing both modules. */
export { instanceScene };
