// The acceleration structure view's logic (src/renderer/acceleration_scene.ts): what a structure can
// be drawn as and why not when it cannot, and which structures a command names — whichever of the
// several ways it names them.
//
//     cd src/app && npm test
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const out = join(mkdtempSync(join(tmpdir(), "accel-")), "acceleration_scene.mjs");
buildSync({ entryPoints: [join(here, "..", "src", "renderer", "acceleration_scene.ts")], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
const { structureDrawing, structuresOfCommand, STRUCTURE_TYPES } = await import(pathToFileURL(out).href);

// ------------------------------------------------------------------------------------------
// A capture of one D3D12 bottom level of a triangle, and a top level of two instances of it.

const BLAS = 50, TLAS = 51, BLAS_BUFFER = 45, TLAS_BUFFER = 46, VERTICES = 42, INSTANCES = 44;
const blasAddress = 0x8df8000, tlasAddress = 0x8df9000;

const structure = (id, address, buffer, build) => ({
  id, type: "ID3D12RaytracingAccelerationStructure", name: `structure ${id}`,
  descriptor: { Address: { address: `0x${address.toString(16)}`, buffer: { __id: buffer, __class: "ID3D12Resource" }, offset: 0 } },
  updates: build ? { build } : {},
});

const blasBuild = {
  index: 3, method: "BuildRaytracingAccelerationStructure",
  args: { pDesc: {
    DestAccelerationStructureData: { address: `0x${blasAddress.toString(16)}`, buffer: { __id: BLAS_BUFFER }, offset: 0 },
    Inputs: {
      Type: "D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL", Flags: "", NumDescs: 1, DescsLayout: "D3D12_ELEMENTS_LAYOUT_ARRAY",
      pGeometryDescs: [{ Type: "D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES", Flags: "",
        Triangles: { IndexFormat: "DXGI_FORMAT_UNKNOWN", VertexFormat: "DXGI_FORMAT_R32G32B32_FLOAT", IndexCount: 0, VertexCount: 3,
          VertexBuffer: { StartAddress: { address: "0x8df2000", buffer: { __id: VERTICES }, offset: 0 }, StrideInBytes: 12 } } }],
    },
  } },
  destStructure: BLAS,
  buildData: [{ geometry: 0, field: "VertexBuffer", buffer: VERTICES, offset: 0, capture: 7 }],
};

const tlasBuild = {
  index: 5, method: "BuildRaytracingAccelerationStructure",
  args: { pDesc: {
    DestAccelerationStructureData: { address: `0x${tlasAddress.toString(16)}`, buffer: { __id: TLAS_BUFFER }, offset: 0 },
    Inputs: { Type: "D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL", Flags: "", NumDescs: 2,
      DescsLayout: "D3D12_ELEMENTS_LAYOUT_ARRAY", InstanceDescs: { address: "0x8df3000", buffer: { __id: INSTANCES }, offset: 0 } },
  } },
  destStructure: TLAS,
  buildData: [{ field: "InstanceDescs", buffer: INSTANCES, offset: 0, capture: 8 }],
};

/** The triangle, three float3 positions. */
function triangleBytes() {
  const f = new Float32Array([-0.5, -0.5, 0, 0.5, -0.5, 0, 0, 0.5, 0]);
  return new Uint8Array(f.buffer);
}

/** Two instances of the bottom level, the second moved 2 along x. */
function instanceBytes() {
  const bytes = new Uint8Array(128);
  const view = new DataView(bytes.buffer);
  for (let i = 0; i < 2; i++) {
    const at = i * 64;
    view.setFloat32(at + 0, 1, true); view.setFloat32(at + 20, 1, true); view.setFloat32(at + 40, 1, true);
    view.setFloat32(at + 12, i * 2, true);
    view.setUint32(at + 48, 0xff << 24, true);
    view.setBigUint64(at + 56, BigInt(blasAddress), true);
  }
  return bytes;
}

function capture(commands, contents) {
  return { commands, buffer: (id) => (contents.has(id) ? { info: { id }, data: contents.get(id) } : null) };
}

function database(structures) {
  const byId = new Map(structures.map((o) => [o.id, o]));
  return {
    getObject: (id) => byId.get(id) ?? null,
    getObjectsOfType: (type) => (type === "ID3D12RaytracingAccelerationStructure" ? byId : null),
  };
}

const structures = [structure(BLAS, blasAddress, BLAS_BUFFER, { Type: "BOTTOM_LEVEL" }), structure(TLAS, tlasAddress, TLAS_BUFFER, { Type: "TOP_LEVEL" })];
const db = database(structures);

test("a bottom level whose build is in the capture draws its own triangles", () => {
  const data = capture([blasBuild], new Map([[7, triangleBytes()]]));
  const d = structureDrawing(data, db, BLAS);
  assert.equal(d.shape, "triangles");
  assert.equal(d.kind, "triangles");
  assert.equal(d.positions.length, 9, "one triangle, three positions");
  assert.equal(d.note, "");
  assert.deepEqual(d.instances, []);
});

test("a top level draws its instances placed with their bottom level's geometry", () => {
  const data = capture([blasBuild, tlasBuild], new Map([[7, triangleBytes()], [8, instanceBytes()]]));
  const d = structureDrawing(data, db, TLAS);
  assert.equal(d.instances.length, 2);
  assert.equal(d.placed, 2, "both drawn with the triangle rather than a box");
  assert.equal(d.shape, "triangles");
  assert.equal(d.positions.length, 2 * 9);
  // The second instance's triangle sits 2 along x from the first's.
  assert.equal(d.positions[9] - d.positions[0], 2);
});

test("a structure with no build in the capture has nothing to draw, and says why", () => {
  // The usual case: the bottom level was built at load, the frame only reads it.
  const data = capture([tlasBuild], new Map([[8, instanceBytes()]]));
  const blas = structureDrawing(data, db, BLAS);
  assert.equal(blas.positions.length, 0);
  assert.equal(blas.shape, "none");
  assert.match(blas.note, /no build of this structure/);
  // The top level still draws, as a stand-in box per instance where its geometry would be.
  const tlas = structureDrawing(data, db, TLAS);
  assert.equal(tlas.instances.length, 2);
  assert.equal(tlas.placed, 0);
  assert.equal(tlas.shape, "instances");
  assert.ok(tlas.positions.length > 0, "boxes where the instances are");
});

test("a build whose geometry was not read back says so, not that it was never built", () => {
  const data = capture([blasBuild], new Map());
  const d = structureDrawing(data, db, BLAS);
  assert.equal(d.positions.length, 0);
  assert.match(d.note, /geometry was not read back/);
});

test("a top level whose instances were not read back has no scene", () => {
  const data = capture([tlasBuild], new Map());
  const d = structureDrawing(data, db, TLAS);
  assert.equal(d.positions.length, 0);
  assert.match(d.note, /instances this top level was built from are not in the capture/);
});

// ------------------------------------------------------------------------------------------
// Which structures a command names

test("a D3D12 build names the structure it wrote", () => {
  const refs = structuresOfCommand(tlasBuild, structures);
  assert.deepEqual(refs, [{ id: TLAS, role: "builds" }]);
});

test("a copy names both ends, by role", () => {
  const copy = {
    method: "CopyRaytracingAccelerationStructure",
    args: {
      DestAccelerationStructureData: { address: `0x${tlasAddress.toString(16)}`, buffer: { __id: TLAS_BUFFER }, offset: 0 },
      SourceAccelerationStructureData: { address: `0x${blasAddress.toString(16)}`, buffer: { __id: BLAS_BUFFER }, offset: 0 },
    },
  };
  assert.deepEqual(structuresOfCommand(copy, structures), [{ id: TLAS, role: "copies to" }, { id: BLAS, role: "copies from" }]);
});

test("a trace names its scene through a root SRV, which gives a buffer and an offset rather than an address", () => {
  // test/path_tracer/d3d12 binds its top level this way: nothing in the trace's own arguments names it.
  const trace = { method: "DispatchRays", args: { pDesc: { Width: 960, Height: 540, Depth: 1 } } };
  const bound = [{ set: 1, bindings: [{ binding: 0, descriptors: [{ buffer: { __id: TLAS_BUFFER, __class: "ID3D12Resource" }, offset: 0, range: 4096 }] }] }];
  assert.deepEqual(structuresOfCommand(trace, structures, bound), [{ id: TLAS, role: "traces" }]);
  // And through a descriptor table's SRV, which gives the address and the minted object.
  const table = [{ bindings: [{ descriptors: [{ accelerationStructure: { address: `0x${tlasAddress.toString(16)}`, structure: { __id: TLAS } } }] }] }];
  assert.deepEqual(structuresOfCommand(trace, structures, table), [{ id: TLAS, role: "traces" }]);
});

test("a buffer that is not a structure's is not mistaken for one", () => {
  // The instance buffer and the scratch are addressed exactly like a structure, and are not one.
  const refs = structuresOfCommand({ method: "DispatchRays", args: {} }, structures,
    [{ bindings: [{ descriptors: [{ buffer: { __id: INSTANCES }, offset: 0 }] }] }]);
  assert.deepEqual(refs, []);
});

test("a Vulkan build names its structures by handle", () => {
  const vk = [
    { id: 70, type: "VkAccelerationStructureKHR", name: "blas", descriptor: {}, updates: {} },
    { id: 71, type: "VkAccelerationStructureKHR", name: "tlas", descriptor: {}, updates: {} },
  ];
  const build = { method: "vkCmdBuildAccelerationStructuresKHR", args: { pInfos: [
    { dstAccelerationStructure: { __id: 71, __class: "VkAccelerationStructureKHR" },
      srcAccelerationStructure: { __id: 70, __class: "VkAccelerationStructureKHR" } },
  ] } };
  assert.deepEqual(structuresOfCommand(build, vk), [{ id: 71, role: "builds" }, { id: 70, role: "updates from" }]);
});

test("each structure is named once, however many times the command mentions it", () => {
  const refs = structuresOfCommand(
    { ...tlasBuild, args: { pDesc: { ...tlasBuild.args.pDesc, SourceAccelerationStructureData: tlasBuild.args.pDesc.DestAccelerationStructureData } } },
    structures);
  assert.equal(refs.length, 1);
});

test("every API's structure type is listed", () => {
  assert.ok(STRUCTURE_TYPES.includes("VkAccelerationStructureKHR"));
  assert.ok(STRUCTURE_TYPES.includes("ID3D12RaytracingAccelerationStructure"));
});

// ------------------------------------------------------------------------------------------
// A structure built before the capture began: no build command, but its last build recorded on it
// and its inputs read back as the capture started (captureInputs).

test("a bottom level built before the capture is drawn from what was read back at its start", () => {
  const early = structure(BLAS, blasAddress, BLAS_BUFFER, {
    Type: "D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL", Flags: "", NumDescs: 1,
    geometries: blasBuild.args.pDesc.Inputs.pGeometryDescs,
  });
  early.updates.captureInputs = { serial: 1, inputs: [{ geometry: 0, field: "VertexBuffer", buffer: VERTICES, offset: 0, capture: 9 }] };
  const earlyDb = database([early, structures[1]]);
  const data = { commands: [], buffer: (id) => (id === 9 ? { info: { id: 9, buffer: VERTICES }, data: triangleBytes() } : null) };
  const d = structureDrawing(data, earlyDb, BLAS);
  assert.equal(d.shape, "triangles");
  assert.equal(d.positions.length, 9);
  assert.equal(d.fromCaptureStart, true, "the view has to say the geometry was read at the capture's start");
});

test("a read-back id from an earlier capture is not taken for this one's", () => {
  // captureInputs is overwritten by each capture; an id this capture read back from another
  // buffer is some other range entirely.
  const early = structure(BLAS, blasAddress, BLAS_BUFFER, {
    Type: "D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL", Flags: "", NumDescs: 1,
    geometries: blasBuild.args.pDesc.Inputs.pGeometryDescs,
  });
  early.updates.captureInputs = { serial: 1, inputs: [{ geometry: 0, field: "VertexBuffer", buffer: VERTICES, offset: 0, capture: 9 }] };
  const data = { commands: [], buffer: (id) => (id === 9 ? { info: { id: 9, buffer: 999 }, data: triangleBytes() } : null) };
  const d = structureDrawing(data, database([early]), BLAS);
  assert.equal(d.positions.length, 0);
});

test("a build in the frame wins over what was read back at the capture's start", () => {
  const early = structure(BLAS, blasAddress, BLAS_BUFFER, {
    Type: "D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL", Flags: "", NumDescs: 1,
    geometries: blasBuild.args.pDesc.Inputs.pGeometryDescs,
  });
  early.updates.captureInputs = { serial: 1, inputs: [{ geometry: 0, field: "VertexBuffer", buffer: VERTICES, offset: 0, capture: 9 }] };
  const contents = new Map([[7, triangleBytes()], [9, triangleBytes()]]);
  const data = { commands: [blasBuild], buffer: (id) => (contents.has(id) ? { info: { id, buffer: VERTICES }, data: contents.get(id) } : null) };
  const d = structureDrawing(data, database([early]), BLAS);
  assert.equal(d.fromCaptureStart, false);
});

test("a Vulkan bottom level built before the capture is drawn from what the layer read back at its start", () => {
  const VK_BLAS = 70, VK_VERTICES = 71;
  const early = {
    id: VK_BLAS, type: "VkAccelerationStructureKHR", name: "blas",
    updates: {
      // The layer's build update writes a geometry's fields flat, not nested as the command's are.
      build: {
        method: "vkCmdBuildAccelerationStructuresKHR", type: "VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR",
        mode: "VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR", flags: "",
        geometries: [{ geometryType: "VK_GEOMETRY_TYPE_TRIANGLES_KHR", flags: "", primitiveCount: 1,
          vertexFormat: "VK_FORMAT_R32G32B32_SFLOAT", vertexStride: 12, maxVertex: 2, indexType: "VK_INDEX_TYPE_NONE_KHR",
          vertexData: { deviceAddress: 1234, buffer: { __id: VK_VERTICES }, offset: 0 } }],
        primitiveCount: 1,
      },
      captureInputs: { serial: 2, inputs: [{ info: 0, geometry: 0, field: "vertexData", buffer: VK_VERTICES, offset: 0, capture: 4 }] },
    },
  };
  const byId = new Map([[VK_BLAS, early]]);
  const vkDb = { getObject: (id) => byId.get(id) ?? null, getObjectsOfType: (type) => (type === "VkAccelerationStructureKHR" ? byId : null) };
  const data = { commands: [], buffer: (id) => (id === 4 ? { info: { id: 4, buffer: VK_VERTICES }, data: triangleBytes() } : null) };
  const d = structureDrawing(data, vkDb, VK_BLAS);
  assert.equal(d.shape, "triangles");
  assert.equal(d.positions.length, 9);
  assert.equal(d.fromCaptureStart, true);
});
