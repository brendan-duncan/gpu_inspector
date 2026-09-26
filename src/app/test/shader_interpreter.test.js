// The shader debugger's SPIR-V interpreter (src/renderer/spirv/): real compiler output run with
// known inputs, checked against what the shader computes by hand. The vectors in
// test/vectors/interpreter are built from the sources beside them:
//
//     glslangValidator -V -g -o basic.frag.spv basic.frag          (and .vert, .comp, derivatives.frag)
//     glslangValidator -V -gVS -o basic.frag.nsdi.spv basic.frag   (NonSemantic.Shader.DebugInfo.100)
//     spirv-opt -O basic.frag.spv -o basic.frag.opt.spv            (and basic.comp: phis, inlined calls)
//     dxc -spirv -T ps_6_0 -E main -fspv-target-env=vulkan1.1 -Fo basic.hlsl.spv basic.hlsl
//     glslangValidator -V -g -o stages.geom.spv stages.geom                (and stages.tesc, stages.tese)
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "interp-"));
const out = join(dir, "interp.mjs");
buildSync({
  stdin: { contents: `export * from "./module.js"; export * from "./interpreter.js"; export * from "./values.js"; export * from "./group.js";`, resolveDir: join(here, "..", "src", "renderer", "spirv"), loader: "ts" },
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const { SpirvModule, Invocation, InvocationGroup } = await import(pathToFileURL(out).href);

const vector = (name) => new SpirvModule(new Uint8Array(readFileSync(join(here, "vectors", "interpreter", name))));

/** Little-endian bytes written by a callback given a DataView. */
function bytes(size, write) {
  const b = new Uint8Array(size);
  write(new DataView(b.buffer));
  return b;
}

/** A 2x2 RGBA texture: red, green / blue, white, with alphas 0.5, 0.25, 1, 0.75. */
const TEXTURE = {
  width: 2, height: 2, depth: 1, layers: 1, mips: 1, baseMip: 0, format: "VK_FORMAT_R8G8B8A8_UNORM", integer: false,
  level: () => ({ width: 2, height: 2, texels: new Float32Array([1, 0, 0, 0.5, 0, 1, 0, 0.25, 0, 0, 1, 1, 1, 1, 1, 0.75]) }),
};

function bindings({ buffers = {}, push = null, textures = {} } = {}) {
  return {
    buffer: (set, binding) => buffers[`${set}/${binding}`] ?? null,
    texture: (set, binding) => textures[`${set}/${binding}`] ?? null,
    sampler: () => null,
    pushConstants: push,
    specialization: new Map(),
  };
}

function inputs(locations = {}, builtins = {}) {
  return {
    locations: new Map(Object.entries(locations).map(([k, v]) => [Number(k), v])),
    builtins: new Map(Object.entries(builtins).map(([k, v]) => [Number(k), v])),
  };
}

function close(actual, expected, message) {
  const a = actual.flat(Infinity);
  const e = expected.flat(Infinity);
  assert.equal(a.length, e.length, `${message}: ${JSON.stringify(actual)}`);
  a.forEach((x, i) => assert.ok(Math.abs(x - e[i]) < 1e-4, `${message}: ${JSON.stringify(actual)} is not ${JSON.stringify(expected)}`));
}

// basic.frag's uniform block (std140) and push constants.
function fragmentBindings(mode, translateX = 1) {
  return bindings({
    buffers: {
      "0/0": bytes(136, (v) => {
        [2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0, translateX, 2, 3, 1].forEach((x, i) => v.setFloat32(i * 4, x, true));
        [1, 2, 0.5].forEach((x, i) => v.setFloat32(64 + i * 4, x, true));
        v.setFloat32(76, 1.5, true);
        [0.5, 1, 2].forEach((x, i) => v.setFloat32(80 + i * 16, x, true));
        v.setInt32(128, mode, true);
      }),
    },
    push: bytes(8, (v) => {
      v.setFloat32(0, 0.125, true);
      v.setUint32(4, 1, true);
    }),
    textures: { "0/1": TEXTURE },
  });
}

const FRAGMENT_INPUTS = () => inputs({ 0: [0.5, 0.25, 1.0], 1: [0.75, 0.25] }, { 15: [10.5, 20.5, 0.5, 1] });

function runFragment(name, mode, translateX) {
  const invocation = new Invocation(vector(name), { bindings: fragmentBindings(mode, translateX), inputs: FRAGMENT_INPUTS() });
  invocation.run();
  return invocation;
}

for (const name of ["basic.frag.spv", "basic.frag.opt.spv", "basic.frag.nsdi.spv"]) {
  test(`${name}: a fragment's uniforms, push constants, loop, switch, call and texture read`, () => {
    // mode 0: the light shades (0.5, 0.5, 0.5) by 1.5, the push constant adds 0.125, the transform scales and translates.
    let inv = runFragment(name, 0);
    assert.equal(inv.status, "returned", inv.error);
    close(inv.outputs()[0].value, [2.75, 4.625, 6.5, 0.25], "mode 0");
    // mode 1: the texel at (0.75, 0.25) is exactly the green one.
    inv = runFragment(name, 1);
    close(inv.outputs()[0].value, [1.25, 5.375, 3.5, 0.25], "mode 1");
    // mode 2: the loop sums 0.5 + 2 + 6.
    inv = runFragment(name, 2);
    close(inv.outputs()[0].value, [18.25, 27.875, 37.5, 0.25], "mode 2");
    // A translation far to the left discards.
    inv = runFragment(name, 0, -1000);
    assert.equal(inv.status, "discarded");
    assert.deepEqual([...inv.warnings], [], "every input and resource had a value");
  });
}

test("stepping reports source lines, locals and the call stack", () => {
  const m = vector("basic.frag.spv");
  const inv = new Invocation(m, { bindings: fragmentBindings(0), inputs: FRAGMENT_INPUTS() });
  const lines = new Set();
  let deepest = 0;
  let sawLocals = false;
  while (!inv.finished) {
    inv.step();
    const inst = inv.current;
    const loc = inst ? m.debug.locations[inst.index] : null;
    if (loc) lines.add(loc.line);
    deepest = Math.max(deepest, inv.frames.length);
    if (inv.frames.length === 1 && inv.locals().some((l) => l.name === "sum" && Math.abs(l.value - 8.5) < 1e-6)) sawLocals = true;
  }
  assert.ok(lines.has(34) && lines.has(27), `the loop body and the called function's lines were stepped: ${[...lines].sort((a, b) => a - b)}`);
  assert.equal(deepest, 2, "shade() ran in a frame of its own");
  assert.ok(sawLocals, "the loop's sum is visible as a local once computed");
});

test("NonSemantic debug info names the locals too", () => {
  const m = vector("basic.frag.nsdi.spv");
  const inv = new Invocation(m, { bindings: fragmentBindings(2), inputs: FRAGMENT_INPUTS() });
  let names = [];
  while (!inv.finished) {
    inv.step();
    if (inv.frames.length === 1) names = inv.locals().map((l) => l.name);
  }
  assert.ok(names.includes("sum") && names.includes("color"), names.join(", "));
});

test("basic.vert: attributes, built-ins, a std140 mat3 and the gl_PerVertex output", () => {
  const camera = bytes(112, (v) => {
    [2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1].forEach((x, i) => v.setFloat32(i * 4, x, true));
    // mat3 in std140: three columns of 16 bytes. Column 0 is (0, 1, 0).
    [0, 1, 0].forEach((x, i) => v.setFloat32(64 + i * 4, x, true));
    [1, 0, 0].forEach((x, i) => v.setFloat32(80 + i * 4, x, true));
    [0, 0, 1].forEach((x, i) => v.setFloat32(96 + i * 4, x, true));
  });
  const inv = new Invocation(vector("basic.vert.spv"), {
    bindings: bindings({ buffers: { "0/0": camera } }),
    inputs: inputs({ 0: [1, 2, 3], 1: [0.1, 0.9] }, { 42: 5, 43: 2 }),
  });
  inv.run();
  assert.equal(inv.status, "returned", inv.error);
  const outputs = Object.fromEntries(inv.outputs().map((o) => [o.location ?? o.name, o.value]));
  const perVertex = inv.outputs().find((o) => Array.isArray(o.value) && Array.isArray(o.value[0]));
  close(perVertex.value[0], [2, 14, 6, 1], "gl_Position: (1, 2, 3) + 5 * (0, 1, 0), scaled by 2");
  close(outputs[0], [0.9, 0.1], "vUV swapped");
  assert.equal(outputs[1], 7, "vIndex = 5 + 2");
});

for (const name of ["basic.comp.spv", "basic.comp.opt.spv"]) {
  test(`${name}: storage buffers, runtime arrays, integer and bit operations`, () => {
    const values = [0.35, 1.26, 2.5, 7.9];
    const data = bytes(4 + values.length * 4, (v) => {
      v.setUint32(0, 10, true);
      values.forEach((x, i) => v.setFloat32(4 + i * 4, x, true));
    });
    const results = new Uint8Array(values.length * 4);
    const run = (x) => {
      const inv = new Invocation(vector(name), {
        bindings: bindings({ buffers: { "0/0": data, "0/1": results } }),
        inputs: inputs({}, { 28: [x, 0, 0] }),
      });
      inv.run();
      assert.equal(inv.status, "returned", inv.error);
      return inv;
    };
    const hash = (x) => {
      x ^= x >>> 16;
      x = Math.imul(x, 0x7feb352d) >>> 0;
      x ^= x >>> 15;
      x = Math.imul(x, 0x846ca68b) >>> 0;
      x ^= x >>> 16;
      return x >>> 0;
    };
    for (let i = 0; i < values.length; i++) {
      const inv = run(i);
      const block = (n) => inv.resourceVariables().find((r) => r.binding === n).value;
      const v = Math.fround(values[i]);
      const expected = (((Math.floor(v * 10) % 7) - i * 3) ^ (hash(i) & 0xff)) + 10;
      assert.equal(block(1)[0][i], expected, `results[${i}]`);
      close([block(0)[1][i]], [v * 2], `values[${i}] doubled, in the interpreter's copy`);
    }
    const past = run(9);
    assert.deepEqual(past.resourceVariables().find((r) => r.binding === 1).value[0], [0, 0, 0, 0], "an invocation past the array writes nothing");
    assert.equal(data[4], new Uint8Array(new Float32Array([0.35]).buffer)[0], "the captured bytes are left alone");
  });
}

test("basic.hlsl (dxc): a cbuffer matrix, lerp and saturate, a separate texture and sampler", () => {
  const params = bytes(80, (v) => {
    // Columns: HLSL packs matrices column-major by default. The last column translates.
    [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 10, 20, 30, 1].forEach((x, i) => v.setFloat32(i * 4, x, true));
    [0.2, 0.4, 0.6].forEach((x, i) => v.setFloat32(64 + i * 4, x, true));
    v.setFloat32(76, 2, true);
  });
  const inv = new Invocation(vector("basic.hlsl.spv"), {
    bindings: bindings({ buffers: { "0/0": params }, textures: { "0/1": TEXTURE } }),
    inputs: inputs({ 0: [0.5, 0.25] }, { 15: [100.5, 50.5, 0.5, 1] }),
  });
  inv.run();
  assert.equal(inv.status, "returned", inv.error);
  // c = (0.4, 0.8, 1.2) half way to red, translated by (10, 20, 30); the green channel half way between the top two texels.
  close(inv.outputs().find((o) => o.location === 0).value, [10.7, 20.4, 30.6, 0.5], "SV_Target");
});

test("derivatives.frag: dFdx, dFdy, fwidth and implicit-LOD sampling across the pixel quad", async () => {
  const quadOut = join(dir, "quad.mjs");
  buildSync({ entryPoints: [join(here, "..", "src", "renderer", "debug", "quad.ts")], bundle: true, format: "esm", platform: "node", outfile: quadOut, logLevel: "silent" });
  const { PixelQuad } = await import(pathToFileURL(quadOut).href);
  const m = vector("derivatives.frag.spv");
  // inUV varies by 0.01 a pixel across and 0.02 down; the debugged pixel is the quad's bottom-right one.
  const { x0, y0, target } = PixelQuad.place(11, 21);
  assert.deepEqual([x0, y0, target], [10, 20, 3]);
  const quad = new PixelQuad((dx, dy, source) => new Invocation(m, {
    bindings: bindings({ textures: { "0/0": TEXTURE } }),
    inputs: inputs({ 0: [(x0 + dx) * 0.01, (y0 + dy) * 0.02] }, { 15: [x0 + dx + 0.5, y0 + dy + 0.5, 0.5, 1] }),
    derivatives: source,
  }), target);
  quad.run();
  const inv = quad.invocation;
  assert.equal(inv.status, "returned", inv.error);
  const out = inv.outputs()[0].value;
  close(out.slice(0, 3), [10, 20, 40], "dFdx(uv).x, dFdy(uv).y and fwidth(4 uv.x), times 1000");
  // uv (0.11, 0.42): texel centers at 0.25 and 0.75 put it between the two columns, in the top row.
  assert.ok(out[3] > 0 && out[3] <= 1, `the red channel of a magnified sample: ${out[3]}`);
  assert.deepEqual([...inv.warnings], []);
});

// BuiltIn numbers the stage tests give values to.
const POSITION = 0, PRIMITIVE_ID = 7, INVOCATION_ID = 8, TESS_LEVEL_OUTER = 11, TESS_LEVEL_INNER = 12, TESS_COORD = 13, PATCH_VERTICES = 14;
const GEOMETRY = 3, TESS_CONTROL = 1, TESS_EVAL = 2;

/** Input vertices: each a position and one located value. */
function vertices(list) {
  return list.map(([position, value]) => ({ builtins: new Map([[POSITION, position]]), locations: new Map([[0, value]]) }));
}

test("stages.geom: gl_in[] and arrayed inputs per vertex, and every EmitVertex kept with its primitive", () => {
  const inv = new Invocation(vector("stages.geom.spv"), {
    model: GEOMETRY, bindings: bindings(),
    inputs: {
      ...inputs({}, { [PRIMITIVE_ID]: 4, [INVOCATION_ID]: 1 }),
      vertices: vertices([[[0, 0, 0, 1], [1, 0, 0]], [[1, 0, 0, 1], [0, 1, 0]], [[0, 1, 0, 1], [0, 0, 1]]]),
    },
  });
  assert.equal(inv.run(), "returned", inv.error);
  const emitted = inv.emittedVertices();
  assert.equal(emitted.length, 7);
  assert.deepEqual(emitted.map((e) => e.primitive), [0, 0, 0, 1, 1, 1, 1]);
  const out = (k, name) => emitted[k].outputs.find((o) => o.name === name).value;
  close(out(1, "color"), [0, 1, 0], "the second vertex's color");
  close([out(2, "tag")], [41], "gl_PrimitiveIDIn * 10 + gl_InvocationID");
  close(out(3, "gl_PerVertex")[0], [1, 0, 0, 1], "the strip's first position, shifted");
  close(out(6, "color"), [2, 0, 0], "the strip's fourth vertex wraps to the first input");
  assert.deepEqual([...inv.warnings], []);
});

test("stages.tesc: a patch's invocations share outputs, and barrier() lets each read its neighbour's", () => {
  const module = vector("stages.tesc.spv");
  const patch = vertices([[[1, 2, 3, 1], [1]], [[4, 5, 6, 1], [2]], [[7, 8, 9, 1], [3]]]);
  const group = new InvocationGroup(3, 1, (i, g, shared) => new Invocation(module, {
    model: TESS_CONTROL, bindings: bindings(),
    inputs: { ...inputs({}, { [INVOCATION_ID]: i, [PRIMITIVE_ID]: 0, [PATCH_VERTICES]: 3 }), vertices: patch },
    sharedOutputs: shared?.outputCells, barrier: g,
  }));
  // Invocation 1 alone reaches the barrier first; its neighbour's value is only there once the others catch up.
  assert.equal(group.run(), "returned", group.invocation.error);
  const out = (name) => group.invocation.outputs().find((o) => o.name === name).value;
  close(out("doubled"), [2, 4, 6], "each invocation's own vertex");
  close(out("neighbour"), [4, 6, 2], "each reads the next one's after the barrier");
  close([out("total")], [12], "a patch output invocation 0 wrote");
  close(out("gl_TessLevelOuter"), [3, 4, 5, 0], "the tessellation levels");
  close(out("gl_out").map((v) => v[0]), [[1, 2, 3, 1], [4, 5, 6, 1], [7, 8, 9, 1]], "gl_out[].gl_Position");
  assert.ok(group.lanes.every((l) => l.status === "returned"), "every lane finished with the debugged one");
});

test("stages.tese: per-vertex and patch inputs, the tessellation levels and gl_TessCoord", () => {
  const inv = new Invocation(vector("stages.tese.spv"), {
    model: TESS_EVAL, bindings: bindings(),
    inputs: {
      ...inputs({ 2: [12] }, { [TESS_COORD]: [0.5, 0.25, 0.25], [PRIMITIVE_ID]: 7, [TESS_LEVEL_OUTER]: [3, 4, 5, 0], [TESS_LEVEL_INNER]: [6, 0] }),
      vertices: vertices([[[0, 0, 0, 1], [2]], [[4, 0, 0, 1], [4]], [[0, 8, 0, 1], [6]]]),
    },
  });
  assert.equal(inv.run(), "returned", inv.error);
  const out = (name) => inv.outputs().find((o) => o.name === name).value;
  close(out("gl_PerVertex")[0], [1, 2, 0, 1], "the position at the coordinate");
  close(out("result"), [0.5 * 2 + 0.25 * 4 + 0.25 * 6, 12, 4, 7], "doubled at the coordinate, the patch total, a level, the patch");
  assert.deepEqual([...inv.warnings], []);
});
