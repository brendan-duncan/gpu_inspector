// DXR in captures (src/renderer/d3d12/raytracing.ts): a state object's exports and hit groups, a
// trace's binding table regions and the records matched to the identifiers the runtime handed out,
// and a build turned into the shape the acceleration structure views take.
//
// The shapes here are what the capture library writes (src/d3d12/src/raytracing.cpp), taken from a
// real dxinsp_triangle --ray-tracing capture.
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
const dir = mkdtempSync(join(tmpdir(), "dxr-"));
const bundle = (path, name) => buildSync({
  entryPoints: [path], bundle: true, format: "esm", platform: "node", outfile: join(dir, name), logLevel: "silent",
});
bundle(join(here, "..", "src", "renderer", "d3d12", "raytracing.ts"), "raytracing.js");
bundle(join(here, "..", "src", "renderer", "acceleration_structure.ts"), "acceleration_structure.js");
const {
  buildCapture, buildTarget, d3d12BindingTableRegions, d3d12ShaderGroups, d3d12StructureAddresses,
  d3d12TableRecords, exportWithIdentifier, parseD3D12Build, stateObjectInfo, traceStateObjectId,
} = await import(pathToFileURL(join(dir, "raytracing.js")).href);
const { parseInstances } = await import(pathToFileURL(join(dir, "acceleration_structure.js")).href);

/** 32 identifier bytes whose first byte is `first`, as the library writes them: lowercase hex. */
const identifier = (first) => first.toString(16).padStart(2, "0") + "00".repeat(31);

const stateObject = {
  type: "ID3D12StateObject",
  descriptor: {
    Type: "D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE",
    pSubobjects: [
      { Type: "D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY", NumExports: 0, pExports: null },
      { Type: "D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP", HitGroupExport: "HitGroup", HitGroupType: "D3D12_HIT_GROUP_TYPE_TRIANGLES", ClosestHitShaderImport: "ClosestHit", AnyHitShaderImport: null, IntersectionShaderImport: null },
      { Type: "D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP", HitGroupExport: "HitGroupTinted", HitGroupType: "D3D12_HIT_GROUP_TYPE_TRIANGLES", ClosestHitShaderImport: "ClosestHitTinted", AnyHitShaderImport: null, IntersectionShaderImport: null },
      { Type: "D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG", MaxPayloadSizeInBytes: 12, MaxAttributeSizeInBytes: 8 },
      { Type: "D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG", MaxTraceRecursionDepth: 1 },
    ],
  },
  updates: {
    shaderIdentifiers: {
      size: 32,
      pipelineStackSize: 0,
      unlistedExports: true,
      exports: [
        { name: "HitGroup", identifier: identifier(0x11), stackSize: null },
        { name: "HitGroupTinted", identifier: identifier(0x10), stackSize: null },
        { name: "RayGen", identifier: identifier(0x13), stackSize: 0 },
        { name: "Miss", identifier: identifier(0x12), stackSize: 0 },
      ],
    },
  },
  blobs: [{ name: "library:0" }],
};

test("a state object's exports, configs and the library it could not enumerate", () => {
  const info = stateObjectInfo(stateObject);
  assert.equal(info.exports.length, 4);
  assert.equal(info.maxRecursionDepth, 1);
  assert.equal(info.maxPayloadBytes, 12);
  assert.equal(info.maxAttributeBytes, 8);
  // A library with NumExports 0 exports everything in it, so the list may be short.
  assert.equal(info.unlistedExports, true);
  // A hit group has no stack of its own; the runtime answers 0xffffffff and the library writes null.
  assert.equal(info.exports[0].stackSize, null);
  assert.equal(stateObjectInfo({ type: "ID3D12PipelineState", descriptor: {}, updates: {} }), null);
});

test("shader groups: the hit groups first, then every other export the runtime named", () => {
  const groups = d3d12ShaderGroups(stateObject);
  assert.deepEqual(groups.map((g) => [g.name, g.type, g.closestHitName]), [
    ["HitGroup", "triangles hit", "ClosestHit"],
    ["HitGroupTinted", "triangles hit", "ClosestHitTinted"],
    ["RayGen", "general", undefined],
    ["Miss", "general", undefined],
  ]);
  assert.deepEqual(d3d12ShaderGroups({ type: "ID3D12PipelineState", descriptor: {}, updates: {} }), []);
});

test("an identifier names the export it was given for, and nothing else", () => {
  const info = stateObjectInfo(stateObject);
  assert.equal(exportWithIdentifier(info, identifier(0x13)), "RayGen");
  // Case does not matter: the bytes do.
  assert.equal(exportWithIdentifier(info, identifier(0x10).toUpperCase()), "HitGroupTinted");
  assert.equal(exportWithIdentifier(info, identifier(0x77)), null);
});

const trace = {
  index: 8,
  method: "DispatchRays",
  args: {
    pDesc: {
      RayGenerationShaderRecord: { StartAddress: { address: "0x8dfb000" }, SizeInBytes: 32 },
      MissShaderTable: { StartAddress: { address: "0x8dfb040" }, SizeInBytes: 32, StrideInBytes: 32 },
      HitGroupTable: { StartAddress: { address: "0x8dfb080" }, SizeInBytes: 128, StrideInBytes: 64 },
      CallableShaderTable: { StartAddress: null, SizeInBytes: 0, StrideInBytes: 0 },
      Width: 256, Height: 256, Depth: 1,
    },
  },
  stateObject: { __id: 41, __class: "ID3D12StateObject" },
  bindingTableData: [
    { region: "RayGeneration", capture: 9, stride: 32 },
    { region: "Miss", capture: 10, stride: 32 },
    { region: "HitGroup", capture: 11, stride: 64 },
  ],
};

test("a trace's regions are counted in records, and the raygen region is always one", () => {
  const regions = d3d12BindingTableRegions(trace.args);
  assert.deepEqual(regions, [
    // The raygen record has no stride of its own: its size is the record.
    { region: "RayGeneration", size: 32, stride: 32, records: 1 },
    { region: "Miss", size: 32, stride: 32, records: 1 },
    { region: "HitGroup", size: 128, stride: 64, records: 2 },
    { region: "Callable", size: 0, stride: 0, records: 0 },
  ]);
  assert.equal(traceStateObjectId(trace), 41);
});

/** A region's bytes: one record per identifier, padded to `stride`. */
function table(stride, firsts) {
  const bytes = new Uint8Array(stride * firsts.length);
  firsts.forEach((f, i) => { bytes[i * stride] = f; });
  return bytes;
}

test("each record of the table resolves to the export whose identifier it holds", () => {
  const contents = new Map([
    [9, table(32, [0x13])],
    [10, table(32, [0x12])],
    [11, table(64, [0x11, 0x10])],
  ]);
  const records = d3d12TableRecords({ command: trace, stateObject, bytesOf: (id) => contents.get(id) ?? null });
  assert.deepEqual(records.map((r) => [r.region, r.index, r.name, r.dataBytes]), [
    ["RayGeneration", 0, "RayGen", 0],
    ["Miss", 0, "Miss", 0],
    // A 64-byte stride over a 32-byte identifier leaves 32 bytes of the application's own.
    ["HitGroup", 0, "HitGroup", 32],
    ["HitGroup", 1, "HitGroupTinted", 32],
  ]);
});

test("a record holding an identifier the state object never gave out resolves to nothing", () => {
  const contents = new Map([[11, table(64, [0x11, 0x77])]]);
  const command = { ...trace, bindingTableData: [{ region: "HitGroup", capture: 11, stride: 64 }] };
  const records = d3d12TableRecords({ command, stateObject, bytesOf: (id) => contents.get(id) ?? null });
  assert.equal(records[0].name, "HitGroup");
  assert.equal(records[1].name, undefined, "no export has that identifier");
  assert.equal(records[1].group, null);
  assert.equal(records[1].handle.slice(0, 2), "77", "the handle is kept, which is all there is to show");
});

test("a table the capture did not read back has no records at all", () => {
  const records = d3d12TableRecords({ command: { ...trace, bindingTableData: [] }, stateObject, bytesOf: () => null });
  assert.deepEqual(records, []);
});

// ------------------------------------------------------------------------------------------
// Builds

const bottomLevel = {
  index: 3,
  method: "BuildRaytracingAccelerationStructure",
  args: {
    pDesc: {
      DestAccelerationStructureData: { address: "0x8df8000" },
      Inputs: {
        Type: "D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL",
        Flags: "D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE",
        NumDescs: 1,
        DescsLayout: "D3D12_ELEMENTS_LAYOUT_ARRAY",
        pGeometryDescs: [{
          Type: "D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES",
          Flags: "D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE",
          Triangles: {
            Transform3x4: null, IndexFormat: "DXGI_FORMAT_UNKNOWN", VertexFormat: "DXGI_FORMAT_R32G32B32_FLOAT",
            IndexCount: 0, VertexCount: 3, IndexBuffer: null,
            VertexBuffer: { StartAddress: { address: "0x8df2000" }, StrideInBytes: 12 },
          },
        }],
      },
    },
  },
  destStructure: 50,
  buildData: [{ geometry: 0, field: "VertexBuffer", buffer: 42, offset: 0, capture: 7 }],
};

const topLevel = {
  index: 3,
  method: "BuildRaytracingAccelerationStructure",
  args: {
    pDesc: {
      DestAccelerationStructureData: { address: "0x8df9000" },
      Inputs: {
        Type: "D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL",
        Flags: "D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE",
        NumDescs: 2, DescsLayout: "D3D12_ELEMENTS_LAYOUT_ARRAY",
        InstanceDescs: { address: "0x8df3000" },
      },
    },
  },
  destStructure: 51,
  buildData: [{ field: "InstanceDescs", buffer: 44, offset: 0, capture: 8 }],
};

test("a bottom level build becomes the shape the views take, with its format translated", () => {
  const build = parseD3D12Build(bottomLevel);
  assert.equal(build.target, 50);
  assert.equal(build.topLevel, false);
  assert.equal(build.mode, "BUILD", "an update is a build flag in D3D12, not a mode of its own");
  assert.equal(build.primitives, 1, "three vertices with no indices is one triangle");
  const g = build.geometries[0];
  assert.equal(g.kind, "triangles");
  // The mesh reader works in VK_FORMAT names, which is the one thing that has to be translated.
  assert.equal(g.vertexFormat, "R32G32B32_SFLOAT");
  assert.equal(g.vertexStride, 12);
  assert.equal(g.maxVertex, 2, "the highest vertex the build may read");
  assert.equal(g.indexType, "NONE");
  assert.equal(g.vertexData, 7);
  assert.equal(g.indexData, undefined);
});

test("a top level build is one geometry of instances, read back from the command", () => {
  const build = parseD3D12Build(topLevel);
  assert.equal(build.target, 51);
  assert.equal(build.topLevel, true);
  assert.equal(build.primitives, 2);
  assert.deepEqual(build.geometries.map((g) => [g.kind, g.primitiveCount, g.instanceData]), [["instances", 2, 8]]);
  // A top level's one input carries no geometry index, so it is asked for with -1.
  assert.equal(buildCapture(topLevel, -1, "InstanceDescs"), 8);
  assert.equal(buildTarget(topLevel), 51);
  assert.equal(parseD3D12Build({ method: "DispatchRays", args: {} }), null);
});

test("an update is spelled in the build flags", () => {
  const updating = structuredClone(topLevel);
  updating.args.pDesc.Inputs.Flags = "D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE";
  assert.equal(parseD3D12Build(updating).mode, "UPDATE");
});

test("a structure's address is what an instance's reference names", () => {
  const structures = [
    { id: 50, descriptor: { Address: { address: "0x8df8000" } } },
    { id: 51, descriptor: { Address: { address: "0x8df9000" } } },
    { id: 52, descriptor: {} },
  ];
  const addresses = d3d12StructureAddresses(structures);
  // Keyed by the decimal spelling, which is what parseInstances reads out of an instance.
  assert.equal(addresses.get(String(0x8df8000)), 50);
  assert.equal(addresses.get(String(0x8df9000)), 51);
  assert.equal(addresses.size, 2, "a structure with no address resolves nothing");
});

test("a D3D12 instance buffer parses as a Vulkan one: the layouts are identical", () => {
  // D3D12_RAYTRACING_INSTANCE_DESC and VkAccelerationStructureInstanceKHR are byte for byte the
  // same, and the instance flag bits have the same values, so nothing is translated.
  const bytes = new Uint8Array(64);
  const view = new DataView(bytes.buffer);
  // A row-major 3x4 identity translated by 0.45 in x.
  view.setFloat32(0, 1, true); view.setFloat32(12, 0.45, true);
  view.setFloat32(20, 1, true);
  view.setFloat32(40, 1, true);
  view.setUint32(48, (0xff << 24) | 7, true);          // InstanceMask 0xFF, InstanceID 7
  view.setUint32(52, (0x1 << 24) | 1, true);           // TRIANGLE_CULL_DISABLE, hit group +1
  view.setBigUint64(56, BigInt(0x8df8000), true);      // the bottom level it names
  const [i] = parseInstances(bytes, d3d12StructureAddresses([{ id: 50, descriptor: { Address: { address: "0x8df8000" } } }]));
  assert.equal(i.customIndex, 7);
  assert.equal(i.mask, 0xff);
  assert.equal(i.bindingTableOffset, 1);
  assert.deepEqual(i.flagNames, ["TRIANGLE_FACING_CULL_DISABLE"]);
  assert.equal(i.blas, 50, "the reference resolved to the structure at that address");
  assert.equal(i.transform[3].toFixed(2), "0.45");
});
