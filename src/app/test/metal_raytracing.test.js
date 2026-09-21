// Metal ray tracing in captures (src/renderer/metal/raytracing.ts): a build turned into the shape
// the acceleration structure views take, and the five instance descriptor layouts decoded.
//
// The instance layouts are what these tests are really for. The D3D12 module gets away with
// reusing parseInstances because D3D12_RAYTRACING_INSTANCE_DESC is byte for byte
// VkAccelerationStructureInstanceKHR; Metal's is not, in four separate ways (a transposed
// transform, four unpacked uint32s instead of two 24/8 words, an index instead of an address, and
// five layouts instead of one), so every one of them is pinned here. A wrong stride or offset
// reads an instance out of the middle of its neighbour and still produces plausible numbers, which
// is exactly the kind of bug a test has to catch rather than a screenshot.
//
// The descriptor shapes are what the capture library writes (src/metal/src/raytracing.mm), taken
// from a real mtlinsp_path_tracer --rebuild capture.
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
const dir = mkdtempSync(join(tmpdir(), "mtlrt-"));
const bundle = (path, name) => buildSync({
  entryPoints: [path], bundle: true, format: "esm", platform: "node", outfile: join(dir, name), logLevel: "silent",
});
bundle(join(here, "..", "src", "renderer", "metal", "raytracing.ts"), "raytracing.js");
bundle(join(here, "..", "src", "renderer", "acceleration_structure.ts"), "acceleration_structure.js");
bundle(join(here, "..", "src", "renderer", "metal", "frame_analysis.ts"), "frame_analysis.js");
const { analyzeMetalFrame } = await import(pathToFileURL(join(dir, "frame_analysis.js")).href);
const {
  functionTableView, instancedStructures, linkedFunctionNames, metalBuild, metalBuildCapture,
  metalDescriptorOf, parseMetalBuild, parseMetalInstances,
} = await import(pathToFileURL(join(dir, "raytracing.js")).href);
const { aabbBoxes, instanceScene, transformPoint, triangleMesh, unresolvedReference } =
  await import(pathToFileURL(join(dir, "acceleration_structure.js")).href);

const ref = (id) => ({ __id: id, __class: "MTLAccelerationStructure" });
const bufferRef = (id) => ({ __id: id, __class: "MTLBuffer" });

// ---------------------------------------------------------------------------------------------
// Instance descriptors

/**
 * One MTLAccelerationStructureInstanceDescriptor, written the way Metal lays it out: a
 * MTLPackedFloat4x3 of four columns of three floats, then four uint32s.
 *
 * `rows` is the 3x4 row-major transform the *views* use, so the writer transposes it on the way in
 * — which is what makes the parser's transpose testable rather than self-confirming.
 */
function defaultInstance({ rows, options = 0, mask = 0xff, tableOffset = 0, structureIndex = 0 }) {
  const b = new ArrayBuffer(64);
  const v = new DataView(b);
  for (let row = 0; row < 3; row++) {
    for (let column = 0; column < 4; column++) {
      v.setFloat32(column * 12 + row * 4, rows[row * 4 + column], true);
    }
  }
  v.setUint32(48, options, true);
  v.setUint32(52, mask, true);
  v.setUint32(56, tableOffset, true);
  v.setUint32(60, structureIndex, true);
  return new Uint8Array(b);
}

function concat(...parts) {
  const total = parts.reduce((n, p) => n + p.byteLength, 0);
  const out = new Uint8Array(total);
  let at = 0;
  for (const p of parts) { out.set(p, at); at += p.byteLength; }
  return out;
}

const IDENTITY_ROWS = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0];

test("a default instance descriptor decodes its fields and resolves its bottom level by index", () => {
  // A translation, so a transposed read would put the instance somewhere else entirely rather than
  // agreeing with the right answer the way the identity does.
  const rows = [1, 0, 0, 7, 0, 1, 0, -3, 0, 0, 1, 11];
  const bytes = defaultInstance({ rows, options: 0x4, mask: 0x0f, tableOffset: 2, structureIndex: 1 });
  const [i] = parseMetalInstances(bytes, { instanceDescriptorType: "MTLAccelerationStructureInstanceDescriptorTypeDefault" },
                                  [101, 202, 303]);
  assert.deepEqual(i.transform, rows, "the 4x3 column-major transform reads back as the 3x4 row-major one");
  assert.equal(i.mask, 0x0f);
  assert.equal(i.bindingTableOffset, 2);
  assert.equal(i.flags, 0x4);
  assert.deepEqual(i.flagNames, ["Opaque"], "Metal's name for the bit Vulkan calls FORCE_OPAQUE");
  assert.equal(i.reference, "1");
  assert.equal(i.referenceKind, "index");
  assert.equal(i.blas, 202, "index 1 of instancedAccelerationStructures");
  // The transform places a point where the row-major convention says it does.
  assert.deepEqual(transformPoint(i.transform, 0, 0, 0), [7, -3, 11]);
});

test("the transform is read as the transpose of what Metal stores, not reinterpreted", () => {
  // A rotation, which is the case an untransposed read gets wrong in a way a translation would not:
  // the off-diagonal terms swap.
  const rows = [0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0];
  const bytes = defaultInstance({ rows });
  const [i] = parseMetalInstances(bytes, {}, []);
  assert.deepEqual(i.transform, rows);
  // (1,0,0) rotated by a quarter turn about Z is (0,1,0).
  assert.deepEqual(transformPoint(i.transform, 1, 0, 0).map(Math.round), [0, 1, 0]);
});

test("several instances are walked at the descriptor's stride", () => {
  const bytes = concat(
    defaultInstance({ rows: IDENTITY_ROWS, structureIndex: 0, mask: 1 }),
    defaultInstance({ rows: IDENTITY_ROWS, structureIndex: 1, mask: 2 }),
    defaultInstance({ rows: IDENTITY_ROWS, structureIndex: 2, mask: 3 }),
  );
  const got = parseMetalInstances(bytes, {}, [10, 20, 30]);
  assert.deepEqual(got.map((i) => [i.index, i.mask, i.blas]), [[0, 1, 10], [1, 2, 20], [2, 3, 30]]);
});

test("an application's own padding between instances is honoured over the struct size", () => {
  // instanceDescriptorStride larger than the struct: the build walks by the stride, so reading at
  // the struct size would take the second instance out of the first one's padding.
  const padded = new Uint8Array(160);
  padded.set(defaultInstance({ rows: IDENTITY_ROWS, mask: 7, structureIndex: 0 }), 0);
  padded.set(defaultInstance({ rows: IDENTITY_ROWS, mask: 9, structureIndex: 1 }), 80);
  const got = parseMetalInstances(padded, { instanceDescriptorStride: 80 }, [1, 2]);
  assert.equal(got.length, 2);
  assert.deepEqual(got.map((i) => i.mask), [7, 9]);
});

test("a partial instance at the end is left out rather than guessed at", () => {
  const bytes = concat(defaultInstance({ rows: IDENTITY_ROWS }), new Uint8Array(20));
  assert.equal(parseMetalInstances(bytes, {}, [1]).length, 1);
});

test("instanceCount bounds the parse, so padding past the instances is not read as more", () => {
  const bytes = concat(defaultInstance({ rows: IDENTITY_ROWS }), defaultInstance({ rows: IDENTITY_ROWS }));
  assert.equal(parseMetalInstances(bytes, { instanceCount: 1 }, [1]).length, 1);
});

test("a UserID instance descriptor is 68 bytes and carries a user id", () => {
  const b = new ArrayBuffer(68);
  const v = new DataView(b);
  for (let row = 0; row < 3; row++) {
    for (let column = 0; column < 4; column++) v.setFloat32(column * 12 + row * 4, IDENTITY_ROWS[row * 4 + column], true);
  }
  v.setUint32(48, 0x1, true);
  v.setUint32(52, 0xff, true);
  v.setUint32(56, 5, true);
  v.setUint32(60, 2, true);
  v.setUint32(64, 4242, true);   // userID
  const [i] = parseMetalInstances(new Uint8Array(b),
    { instanceDescriptorType: "MTLAccelerationStructureInstanceDescriptorTypeUserID" }, [7, 8, 9]);
  assert.equal(i.customIndex, 4242, "the user id is what a shader reads, and what Vulkan calls the custom index");
  assert.equal(i.bindingTableOffset, 5);
  assert.equal(i.blas, 9);
  assert.deepEqual(i.flagNames, ["DisableTriangleCulling"]);
});

test("a Motion instance descriptor has no transform of its own and is drawn at the origin", () => {
  // 60 bytes, and the fields start at 0 because there is no matrix in front of them: the transform
  // comes from a range of keyframes in the build's motionTransformBuffer instead.
  const b = new ArrayBuffer(60);
  const v = new DataView(b);
  v.setUint32(0, 0x8, true);     // options: NonOpaque
  v.setUint32(4, 0x3, true);     // mask
  v.setUint32(8, 1, true);       // intersectionFunctionTableOffset
  v.setUint32(12, 1, true);      // accelerationStructureIndex
  v.setUint32(16, 77, true);     // userID
  const [i] = parseMetalInstances(new Uint8Array(b),
    { instanceDescriptorType: "MTLAccelerationStructureInstanceDescriptorTypeMotion" }, [11, 22]);
  assert.deepEqual(i.transform, IDENTITY_ROWS, "no transform in the descriptor: the identity, not garbage read off the fields");
  assert.equal(i.mask, 3);
  assert.equal(i.customIndex, 77);
  assert.equal(i.blas, 22);
  assert.deepEqual(i.flagNames, ["NonOpaque"]);
});

test("an Indirect instance descriptor names its bottom level by resource id, which is not resolved", () => {
  const b = new ArrayBuffer(80);
  const v = new DataView(b);
  for (let row = 0; row < 3; row++) {
    for (let column = 0; column < 4; column++) v.setFloat32(column * 12 + row * 4, IDENTITY_ROWS[row * 4 + column], true);
  }
  v.setUint32(48, 0, true);
  v.setUint32(52, 0xff, true);
  v.setUint32(56, 0, true);
  v.setUint32(60, 9, true);                       // userID
  v.setBigUint64(64, 0xabcdn, true);              // accelerationStructureID
  const [i] = parseMetalInstances(new Uint8Array(b),
    { instanceDescriptorType: "MTLAccelerationStructureInstanceDescriptorTypeIndirect" }, [1, 2, 3]);
  assert.equal(i.referenceKind, "resourceId");
  assert.equal(i.reference, "0xabcd");
  assert.equal(i.blas, undefined, "a resource id is deliberately not matched: the driver's ids collide across objects");
  assert.equal(i.customIndex, 9);
  assert.equal(unresolvedReference(i), "resource 0xabcd");
});

test("an index past the end of instancedAccelerationStructures resolves to nothing, and says so", () => {
  const bytes = defaultInstance({ rows: IDENTITY_ROWS, structureIndex: 5 });
  const [i] = parseMetalInstances(bytes, {}, [1, 2]);
  assert.equal(i.blas, undefined);
  assert.equal(unresolvedReference(i), "bottom level #5 (not in this capture)");
});

// ---------------------------------------------------------------------------------------------
// Builds
//
// The descriptors below are what src/metal/src/raytracing.mm writes, copied from a real capture.

const boundingBoxBuild = {
  index: 0, frame: 0,
  method: "buildAccelerationStructure:descriptor:scratchBuffer:scratchBufferOffset:",
  args: {
    accelerationStructure: ref(12),
    descriptor: {
      usage: "None", kind: "primitive",
      geometries: [{
        intersectionFunctionTableOffset: 0, opaque: true,
        allowDuplicateIntersectionFunctionInvocation: true,
        boundingBoxCount: 2, boundingBoxStride: 24,
        boundingBoxBuffer: { buffer: bufferRef(10), offset: 0, size: 48, capture: 1 },
        kind: "boundingBoxes", primitiveCount: 2,
      }],
      primitiveCount: 2,
    },
    scratchBuffer: bufferRef(19), scratchBufferOffset: 0,
    buildData: [{ geometry: 0, field: "boundingBoxBuffer", buffer: 10, offset: 0, capture: 1 }],
  },
};

const triangleBuild = {
  index: 1, frame: 0,
  method: "buildAccelerationStructure:descriptor:scratchBuffer:scratchBufferOffset:",
  args: {
    accelerationStructure: ref(20),
    descriptor: {
      usage: "Refit|PreferFastBuild", kind: "primitive",
      geometries: [{
        intersectionFunctionTableOffset: 0, opaque: false,
        allowDuplicateIntersectionFunctionInvocation: false,
        triangleCount: 1, vertexStride: 12,
        vertexFormat: "MTLAttributeFormatFloat3", vkFormat: "VK_FORMAT_R32G32B32_SFLOAT",
        indexType: "MTLIndexTypeUInt32",
        vertexBuffer: { buffer: bufferRef(21), offset: 0, capture: 5 },
        indexBuffer: { buffer: bufferRef(22), offset: 0, size: 12, capture: 6 },
        kind: "triangles", primitiveCount: 1,
      }],
      primitiveCount: 1,
    },
    buildData: [
      { geometry: 0, field: "vertexBuffer", buffer: 21, offset: 0, capture: 5 },
      { geometry: 0, field: "indexBuffer", buffer: 22, offset: 0, capture: 6 },
    ],
  },
};

const instanceBuild = {
  index: 2, frame: 0,
  method: "buildAccelerationStructure:descriptor:scratchBuffer:scratchBufferOffset:",
  args: {
    accelerationStructure: ref(18),
    descriptor: {
      usage: "None", kind: "instance", instanceCount: 3,
      instanceDescriptorType: "MTLAccelerationStructureInstanceDescriptorTypeDefault",
      instanceDescriptorStride: 64,
      instanceTransformationMatrixLayout: "MTLMatrixLayoutColumnMajor",
      instancedAccelerationStructures: [ref(12), ref(14), ref(16)],
      instanceDescriptorBuffer: { buffer: bufferRef(17), offset: 0, size: 192, capture: 4 },
      primitiveCount: 3,
    },
    buildData: [{ geometry: 0, field: "instanceDescriptorBuffer", buffer: 17, offset: 0, capture: 4 }],
  },
};

test("a bounding box build becomes an aabbs geometry the shared views can draw", () => {
  const build = parseMetalBuild(boundingBoxBuild);
  assert.equal(build.target, 12);
  assert.equal(build.topLevel, false);
  assert.equal(build.primitives, 2);
  assert.equal(build.geometries.length, 1);
  const g = build.geometries[0];
  assert.equal(g.kind, "aabbs", "Metal's boundingBoxes is what the views call aabbs");
  assert.equal(g.aabbStride, 24);
  assert.equal(g.flags, "Opaque | AllowDuplicateIntersectionFunctionInvocation");
  // And the shared box decoder reads it: two boxes, twelve edges each, two positions per edge.
  const boxes = new Float32Array([0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3]);
  const drawn = aabbBoxes(g, new Uint8Array(boxes.buffer));
  assert.equal(drawn.length, 2 * 12 * 2 * 3);
});

test("a triangle build carries the canonical vertex format, and decodes without a maxVertex", () => {
  const build = parseMetalBuild(triangleBuild);
  const g = build.geometries[0];
  assert.equal(g.kind, "triangles");
  assert.equal(g.vertexFormat, "R32G32B32_SFLOAT", "the VK name the shared vertex decoder knows");
  assert.equal(g.vertexStride, 12);
  assert.equal(g.indexType, "UINT32");
  assert.equal(g.maxVertex, undefined, "Metal gives no vertex count, so the read-back bytes are the bound");
  assert.equal(build.flags, "Refit|PreferFastBuild");

  // Three vertices, indexed 0,1,2: the mesh comes out whole even though nothing said how many
  // vertices there are. A maxVertex defaulted to 0 would have produced one vertex and two zeroes.
  const vertices = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]);
  const indices = new Uint32Array([0, 1, 2]);
  const mesh = triangleMesh(g, new Uint8Array(vertices.buffer), new Uint8Array(indices.buffer));
  assert.deepEqual([...mesh], [0, 0, 0, 1, 0, 0, 0, 1, 0]);
});

test("an instance build is a top level with one implicit instances geometry", () => {
  const build = parseMetalBuild(instanceBuild);
  assert.equal(build.target, 18);
  assert.equal(build.topLevel, true);
  assert.equal(build.primitives, 3);
  assert.deepEqual(build.geometries.map((g) => g.kind), ["instances"]);
  assert.deepEqual(instancedStructures(metalDescriptorOf(instanceBuild.args)), [12, 14, 16]);
});

test("a build's capture ids are found by geometry and field", () => {
  assert.equal(metalBuildCapture(triangleBuild.args, 0, "vertexBuffer"), 5);
  assert.equal(metalBuildCapture(triangleBuild.args, 0, "indexBuffer"), 6);
  assert.equal(metalBuildCapture(triangleBuild.args, 1, "vertexBuffer"), 0, "no such geometry");
  assert.equal(metalBuildCapture(triangleBuild.args, 0, "transformationMatrixBuffer"), 0, "not in this build");
});

test("a refit is named as one, and a build with no structure is not a build", () => {
  const refit = {
    ...triangleBuild,
    method: "refitAccelerationStructure:descriptor:destination:scratchBuffer:scratchBufferOffset:",
  };
  assert.equal(parseMetalBuild(refit).mode, "REFIT");
  assert.equal(parseMetalBuild({ method: boundingBoxBuild.method, args: { descriptor: {} } }), null);
  assert.equal(parseMetalBuild({ method: "dispatchThreads:threadsPerThreadgroup:", args: {} }), null);
});

test("metalBuild takes a descriptor straight, which is how a structure built before the capture is read", () => {
  // src/metal/src/raytracing.mm writes the same descriptor onto the structure as into the command,
  // so the earlier-structure path needs no separate shape (acceleration_scene.ts, earlierBuilds).
  const build = metalBuild(12, boundingBoxBuild.args.descriptor, "BUILD");
  assert.equal(build.target, 12);
  assert.equal(build.geometries[0].kind, "aabbs");
  assert.equal(build.primitives, 2);
});

test("a scene places each instance's geometry by its own transform", () => {
  const instances = parseMetalInstances(concat(
    defaultInstance({ rows: [1, 0, 0, 10, 0, 1, 0, 0, 0, 0, 1, 0], structureIndex: 0 }),
    defaultInstance({ rows: [1, 0, 0, -10, 0, 1, 0, 0, 0, 0, 1, 0], structureIndex: 0 }),
  ), {}, [12]);
  // One triangle at the origin, instanced twice ten units either side.
  const mesh = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]);
  const scene = instanceScene(instances, () => mesh);
  assert.equal(scene.placed, 2);
  assert.equal(scene.drawn, "triangles");
  const xs = [];
  for (let i = 0; i < scene.mesh.length; i += 3) xs.push(scene.mesh[i]);
  assert.ok(xs.some((x) => x >= 10) && xs.some((x) => x <= -10), "both instances are where their transforms put them");
});

// ---------------------------------------------------------------------------------------------
// Function tables

const table = {
  type: "MTLIntersectionFunctionTable",
  parentId: 6,
  descriptor: { functionCount: 2, pipeline: { __id: 6, __class: "MTLComputePipelineState" } },
  updates: {
    table: {
      functionCount: 2,
      entries: [
        { index: 0, function: "sphereIntersection" },
        { index: 1, empty: true },
      ],
      buffers: [{ index: 0, buffer: 8, offset: 0 }, { index: 1, buffer: 9, offset: 16 }],
      visibleFunctionTables: [],
    },
  },
};

test("an intersection function table reads its entries, its own buffers, and its empty slots", () => {
  const view = functionTableView(table);
  assert.equal(view.functionCount, 2);
  assert.deepEqual(view.entries.map((e) => [e.index, e.function, e.empty]),
                   [[0, "sphereIntersection", false], [1, "", true]]);
  assert.deepEqual(view.buffers, [{ index: 0, buffer: 8, offset: 0 }, { index: 1, buffer: 9, offset: 16 }]);
});

test("a built-in opaque entry names the built-in and its signature rather than a function", () => {
  const withOpaque = {
    ...table,
    updates: { table: { functionCount: 1, entries: [{ index: 0, opaque: "triangle", signature: "Instancing|TriangleData" }] } },
  };
  const [e] = functionTableView(withOpaque).entries;
  assert.equal(e.opaque, "triangle");
  assert.equal(e.signature, "Instancing|TriangleData");
  assert.equal(e.function, "");
});

test("a table with no update at all falls back to the count its descriptor asked for", () => {
  const view = functionTableView({ ...table, updates: {} });
  assert.equal(view.functionCount, 2);
  assert.deepEqual(view.entries, [], "nothing was set while the inspector was watching");
});

test("anything that is not a function table has no table view", () => {
  assert.equal(functionTableView({ type: "MTLBuffer", updates: {} }), null);
  assert.equal(functionTableView({ type: "MTLAccelerationStructure", updates: {} }), null);
});

test("a pipeline's linked functions are what its table's entries can hold", () => {
  const pipeline = {
    type: "MTLComputePipelineState",
    descriptor: {
      linkedFunctions: {
        functions: [{ name: "sphereIntersection", function: { __id: 5, __class: "MTLFunction" } }],
        binaryFunctions: [],
        privateFunctions: [{ name: "helper" }],
      },
    },
  };
  assert.deepEqual(linkedFunctionNames(pipeline), ["sphereIntersection", "helper"]);
  assert.deepEqual(linkedFunctionNames({ type: "MTLComputePipelineState", descriptor: {} }), [],
                   "a pipeline made without linkedFunctions can hold no intersection function");
  assert.deepEqual(linkedFunctionNames(null), []);
});

// ---------------------------------------------------------------------------------------------
// Frame Issues rules (src/renderer/metal/frame_analysis.ts)
//
// Metal reports none of these itself: an intersection function table offset is not bounds-checked,
// and an offset on opaque geometry is ignored rather than refused. So a capture is the only place
// they can be caught, which is what makes them worth a rule.

/** The minimum a rule needs: the frame's commands, and a database to name a structure through. */
function frame(commands, objects = []) {
  const byId = new Map(objects.map((o) => [o.id, o]));
  const db = {
    allObjects: byId,
    getObject: (id) => (id === undefined || id === null ? null : byId.get(id) ?? null),
    getObjectsOfType: () => null,
  };
  const data = { commands: commands.map((c, index) => ({ index, frame: 0, ...c })) };
  return analyzeMetalFrame(data, db).findings;
}

const structure = (id, name) => ({ id, name, type: "MTLAccelerationStructure", updates: {}, descriptor: {} });

/** A build of `target` whose single geometry has these fields. */
const buildOf = (target, geometry, kind = "primitive", extra = {}) => ({
  method: "buildAccelerationStructure:descriptor:scratchBuffer:scratchBufferOffset:",
  args: {
    accelerationStructure: ref(target),
    descriptor: { kind, usage: "None", primitiveCount: 1, geometries: [geometry], ...extra },
  },
});

/** A compute encoder binding an intersection function table of `entries` entries. */
const bindTable = (id, entries) => ({
  method: "setIntersectionFunctionTable:atBufferIndex:",
  args: { intersectionFunctionTable: { __id: id, __class: "MTLIntersectionFunctionTable" }, index: 0 },
});
const tableObject = (id, entries) => ({
  id, name: `table ${id}`, type: "MTLIntersectionFunctionTable",
  descriptor: { functionCount: entries }, updates: { table: { functionCount: entries, entries: [] } },
});

test("a table offset past the end of every table bound in the frame is reported", () => {
  const findings = frame([
    bindTable(7, 2),
    buildOf(12, { kind: "boundingBoxes", opaque: false, intersectionFunctionTableOffset: 5, primitiveCount: 1 }),
  ], [structure(12, "spheres"), tableObject(7, 2)]);
  const f = findings.find((x) => x.rule === "accel-table-offset-range");
  assert.ok(f, `expected accel-table-offset-range, got ${findings.map((x) => x.rule)}`);
  assert.equal(f.severity, "high");
  assert.match(f.message, /offset 5, past the 2 entries/);
});

test("an offset inside the table is not reported", () => {
  const findings = frame([
    bindTable(7, 4),
    buildOf(12, { kind: "boundingBoxes", opaque: false, intersectionFunctionTableOffset: 3, primitiveCount: 1 }),
  ], [structure(12, "spheres"), tableObject(7, 4)]);
  assert.equal(findings.find((x) => x.rule === "accel-table-offset-range"), undefined);
});

test("with no table bound at all, an offset is not judged", () => {
  // Nothing to compare against: the table may be bound in a frame this capture does not hold, and
  // guessing would produce a confident finding about something unknown.
  const findings = frame([
    buildOf(12, { kind: "boundingBoxes", opaque: false, intersectionFunctionTableOffset: 99, primitiveCount: 1 }),
  ], [structure(12, "spheres")]);
  assert.equal(findings.find((x) => x.rule === "accel-table-offset-range"), undefined);
});

test("opaque geometry that also names an intersection function is reported, gently", () => {
  const findings = frame([
    buildOf(12, { kind: "boundingBoxes", opaque: true, intersectionFunctionTableOffset: 1, primitiveCount: 1 }),
  ], [structure(12, "spheres")]);
  const f = findings.find((x) => x.rule === "accel-opaque-with-function");
  assert.ok(f);
  assert.equal(f.severity, "low", "it wastes nothing at run time; it just does not do what it looks like");
  assert.match(f.message, /never calls an intersection function for opaque geometry/);
});

test("opaque geometry with offset zero says nothing: that is every opaque geometry", () => {
  const findings = frame([
    buildOf(12, { kind: "triangles", opaque: true, intersectionFunctionTableOffset: 0, primitiveCount: 1 }),
  ], [structure(12, "triangle")]);
  assert.equal(findings.find((x) => x.rule === "accel-opaque-with-function"), undefined);
});

test("a top level built from no instances is reported", () => {
  const findings = frame([{
    method: "buildAccelerationStructure:descriptor:scratchBuffer:scratchBufferOffset:",
    args: {
      accelerationStructure: ref(18),
      descriptor: { kind: "instance", usage: "None", instanceCount: 0, instancedAccelerationStructures: [] },
    },
  }], [structure(18, "scene TLAS")]);
  const f = findings.find((x) => x.rule === "accel-empty-top-level");
  assert.ok(f);
  assert.match(f.message, /^scene TLAS was built from zero instances/);
});

test("a structure built twice in one frame is reported, with the count", () => {
  const g = { kind: "triangles", opaque: true, intersectionFunctionTableOffset: 0, primitiveCount: 1 };
  const findings = frame([buildOf(12, g), buildOf(12, g)], [structure(12, "triangle BLAS")]);
  const f = findings.find((x) => x.rule === "accel-rebuilt-twice");
  assert.ok(f);
  assert.equal(f.count, 2);
  assert.match(f.message, /triangle BLAS was built 2 times/);
});

test("two structures each built once is not a rebuild", () => {
  const g = { kind: "triangles", opaque: true, intersectionFunctionTableOffset: 0, primitiveCount: 1 };
  const findings = frame([buildOf(12, g), buildOf(14, g)],
                         [structure(12, "a"), structure(14, "b")]);
  assert.equal(findings.find((x) => x.rule === "accel-rebuilt-twice"), undefined);
});

test("a frame with no ray tracing in it raises none of these", () => {
  const findings = frame([{ method: "dispatchThreads:threadsPerThreadgroup:", args: {} }]);
  assert.deepEqual(findings.filter((f) => f.rule.startsWith("accel-")), []);
});
