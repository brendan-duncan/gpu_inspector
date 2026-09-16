// Tying a capture's read-back buffers to the acceleration structure they were built into.
//
// acceleration_structure.ts knows the formats; this knows where the bytes are. A build names its
// geometry by device address, and the layer resolved those addresses and read the contents back,
// leaving the capture ids on the recorded build command (src/vulkan/src/hooks.cpp). So finding what
// a structure holds means finding the build command that targeted it and following those ids.
import {
  instanceScene, parseBuild, parseInstances, triangleMesh,
  type AccelerationBuild, type AccelerationInstance,
} from "./acceleration_structure.js";
import type { AccelerationScene } from "./ray_tracing_view.js";
import type { CaptureData } from "./capture_data.js";
import type { CaptureCommand } from "../shared/protocol.js";
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
  const list = Array.isArray(command.buildData) ? command.buildData : [];
  for (const e of list) {
    if (!isObject(e)) continue;
    if (num(e.info) === info && num(e.geometry) === geometry && e.field === field) return num(e.capture);
  }
  return 0;
}

/** The bytes of a capture's buffer read-back, or null when it holds none. */
function bytesOf(data: CaptureData, captureId: number): Uint8Array | null {
  if (!captureId) return null;
  for (const b of data.buffers.values()) {
    if (b.info.id === captureId) return b.data ?? null;
  }
  return null;
}

/** Addresses the layer recorded on the structures, so an instance's reference names an object. */
function addressesOf(db: StructureDatabase): Map<string, number> {
  const out = new Map<string, number>();
  for (const o of db.getObjectsOfType("VkAccelerationStructureKHR")?.values() ?? []) {
    const a = o.updates.deviceAddress;
    if (a !== undefined && a !== null) out.set(String(a), o.id);
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
