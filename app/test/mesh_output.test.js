// The mesh view's data (src/renderer/mesh_output.ts, mesh_input.ts): what `vkinsp_replay --mesh-data`
// writes and what is said about it, and a draw's input vertices unrolled into primitives.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "mesh-"));
const load = async (entry, name) => {
  const out = join(dir, `${name}.mjs`);
  buildSync({ entryPoints: [join(here, "..", "src", "renderer", entry)], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
  return import(pathToFileURL(out).href);
};
const { parseMeshFile, clipPositions, clipStats, meshSummary, outputValues, positionOutput } = await load("mesh_output.ts", "mesh_output");
const { listPositions } = await load("mesh_input.ts", "mesh_input");

/** A file laid out the way WriteMeshData in replay/src/main.cpp lays it out: records of gl_Position and a float colour. */
function meshFile(vertices) {
  const stride = 20;
  const data = new Uint8Array(vertices.length * stride);
  const view = new DataView(data.buffer);
  vertices.forEach(([x, y, z, w, c], i) => [x, y, z, w, c].forEach((v, k) => view.setFloat32(i * stride + k * 4, v, true)));
  const manifest = {
    format: "gpu-inspector-mesh", version: 1, device: "Test GPU", problems: [],
    draws: [{
      command: 9, method: "vkCmdDraw", frame: 0, commandBuffer: 7, passIndex: 0, measured: true,
      topology: "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST", stride, vertices: vertices.length, truncated: false,
      outputs: [
        { name: "gl_Position", offset: 0, components: 4, base: "float", builtin: "Position" },
        { name: "shade", offset: 16, components: 1, base: "float", location: 0 },
      ],
      payload: [0, data.length],
    }, {
      command: 3, method: "vkCmdBindPipeline", frame: 0, commandBuffer: 0, passIndex: 0, measured: false, topology: "", stride: 0,
      vertices: 0, truncated: false, outputs: [], note: "command 3 is vkCmdBindPipeline, not a draw",
    }],
  };
  const json = new TextEncoder().encode(JSON.stringify(manifest));
  const magic = new TextEncoder().encode("MESH 1\n");
  const bytes = new Uint8Array(magic.length + 4 + json.length + data.length);
  bytes.set(magic, 0);
  new DataView(bytes.buffer).setUint32(magic.length, json.length, true);
  bytes.set(json, magic.length + 4);
  bytes.set(data, magic.length + 4 + json.length);
  return bytes;
}

test("the replay's vertex outputs are read back, and say whether the geometry can be seen", () => {
  const file = parseMeshFile(meshFile([
    // A triangle in view.
    [-0.5, -0.5, 0.5, 1, 0.25], [0.5, -0.5, 0.5, 1, 0.5], [0, 0.5, 0.5, 1, 0.75],
    // One entirely right of the view volume.
    [2, 0, 0.5, 1, 0], [3, 0, 0.5, 1, 0], [2, 1, 0.5, 1, 0],
    // One with a vertex behind the eye.
    [0, 0, 0.5, -1, 0], [0.1, 0, 0.5, 1, 0], [0, 0.1, 0.5, 1, 0],
    // One with no area: its vertices are in a line.
    [0, 0, 0.5, 1, 0], [0.25, 0.25, 0.5, 1, 0], [0.5, 0.5, 0.5, 1, 0],
  ]));
  assert.equal(file.device, "Test GPU");
  const m = file.draws[0];
  assert.equal(positionOutput(m).name, "gl_Position");
  assert.deepEqual(outputValues(m, m.outputs[1], 2), [0.75]);
  assert.equal(clipPositions(m).length, 12 * 4);

  const stats = clipStats(m);
  assert.equal(stats.primitives, 4);
  assert.equal(stats.outside, 1, "the triangle right of x = w");
  assert.equal(stats.behind, 1, "the vertex with w = -1");
  assert.equal(stats.degenerate, 1, "the triangle along a line");
  assert.equal(stats.invalid, 0);
  assert.deepEqual(stats.ndc.min.map((v) => Math.round(v * 100) / 100), [-0.5, -0.5, 0.5]);
  assert.equal(stats.ndc.max[0], 3);
  assert.equal(meshSummary(m), "12 vertices, 4 triangles, 1 outside the view, 1 vertices behind the eye, 1 with no area");
  assert.match(meshSummary(file.draws[1]), /^Not captured: command 3 is vkCmdBindPipeline/);
  assert.throws(() => parseMeshFile(new TextEncoder().encode("OVERLAY 1\n\0\0\0\0")), /Not a mesh output file/);
});

test("strips and fans are unrolled into the lists transform feedback writes", () => {
  // Five vertices along x: positions are (i, 0, 0).
  const input = (topology) => ({
    topology, attributes: [{ name: "pos", location: 0, binding: 0, format: "VK_FORMAT_R32G32B32_SFLOAT", components: 3, perInstance: false }],
    ids: [0, 1, 2, 3, 4], indices: null, position: 0, notes: [],
    values: (order) => [order, 0, 0],
  });
  const order = (topology) => [...listPositions(input(topology)).order];
  assert.deepEqual(order("VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST"), [0, 1, 2], "the two left over make no triangle");
  assert.deepEqual(order("VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP"), [0, 1, 2, 2, 1, 3, 2, 3, 4], "every other triangle's winding is swapped back");
  assert.deepEqual(order("VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN"), [0, 1, 2, 0, 2, 3, 0, 3, 4]);
  assert.deepEqual(order("VK_PRIMITIVE_TOPOLOGY_LINE_STRIP"), [0, 1, 1, 2, 2, 3, 3, 4]);
  assert.deepEqual(order("VK_PRIMITIVE_TOPOLOGY_POINT_LIST"), [0, 1, 2, 3, 4]);
  const { positions } = listPositions(input("VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN"));
  assert.deepEqual([...positions.subarray(3, 6)], [1, 0, 0], "each listed vertex carries the position it came from");
  assert.equal(listPositions({ ...input("VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST"), position: -1 }).positions.length, 0, "no position, nothing to draw");
});
