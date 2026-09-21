// The shader debugger's MSL interpreter (src/renderer/msl/): the source in test/vectors/msl run
// with known inputs, checked against what the shader computes by hand.
//
// Unlike the SPIR-V vectors, these need no offline compiler — a Metal capture holds the shader's
// source, and the source is what the interpreter runs.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "msl-"));
const out = join(dir, "msl.mjs");
buildSync({
  stdin: {
    contents: `export * from "./msl/program.js"; export * from "./msl/interpreter.js"; export { PixelQuad } from "./debug/quad.js"; export { DebugController } from "./shader_debugger.js"; export { sourceKey } from "./debug/program.js";`,
    resolveDir: join(here, "..", "src", "renderer"), loader: "ts",
  },
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const { MslProgram, MslInvocation, PixelQuad, DebugController, sourceKey } = await import(pathToFileURL(out).href);

const source = (name) => readFileSync(join(here, "vectors", "msl", name), "utf8");
const basic = new MslProgram(source("basic.metal"), "basic.metal");
const derivatives = new MslProgram(source("derivatives.metal"), "derivatives.metal");
const constants = new MslProgram(source("constants.metal"), "constants.metal");

/** Little-endian bytes written by a callback given a DataView. */
function bytes(size, write) {
  const b = new Uint8Array(size);
  write(new DataView(b.buffer));
  return b;
}

/**
 * The Uniforms struct of basic.metal, laid out by hand the way MSL does: tint at 0, direction at
 * 16 (a float3 is sixteen bytes), weight at 32, rotation at 40 (a float2 column is eight-aligned),
 * mode at 56 and flag at 60, in a struct of 64 bytes. Writing these offsets out rather than asking
 * the interpreter for them is the point: it is what checks that it reads a real buffer correctly.
 */
const UNIFORM_OFFSETS = { tint: 0, direction: 16, weight: 32, rotation: 40, mode: 56, flag: 60, size: 64 };

function uniforms({ tint = [1, 1, 1, 1], direction = [0, 0, 1], weight = 0.5, rotation = [1, 0, 0, 1], mode = 0, flag = true } = {}) {
  const o = UNIFORM_OFFSETS;
  return bytes(o.size, (v) => {
    tint.forEach((x, i) => v.setFloat32(o.tint + i * 4, x, true));
    direction.forEach((x, i) => v.setFloat32(o.direction + i * 4, x, true));
    v.setFloat32(o.weight, weight, true);
    rotation.forEach((x, i) => v.setFloat32(o.rotation + i * 4, x, true));
    v.setInt32(o.mode, mode, true);
    v.setUint8(o.flag, flag ? 1 : 0);
  });
}

/** A 2x2 RGBA texture: red, green / blue, white. */
const TEXTURE = {
  width: 2, height: 2, depth: 1, layers: 1, mips: 1, baseMip: 0, format: "VK_FORMAT_R8G8B8A8_UNORM", integer: false,
  level: () => ({ width: 2, height: 2, texels: new Float32Array([1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1]) }),
};

const NEAREST = {
  magFilter: "nearest", minFilter: "nearest", mipmapMode: "nearest", address: ["clamp", "clamp", "clamp"],
  border: [0, 0, 0, 0], compareOp: null, minLod: 0, maxLod: 1000, lodBias: 0, unnormalized: false,
};

function bindings({ buffers = {}, texture = TEXTURE, sampler = NEAREST } = {}) {
  return {
    buffer: (index) => buffers[index] ?? null,
    texture: () => texture,
    sampler: () => sampler,
  };
}

function inputs({ builtins = {}, attributes = {}, varyings = {} } = {}) {
  return {
    builtins: new Map(Object.entries(builtins)),
    attributes: new Map(Object.entries(attributes).map(([k, v]) => [Number(k), v])),
    varyings: new Map(Object.entries(varyings)),
  };
}

function run(program, entryPoint, options) {
  const invocation = new MslInvocation(program, { entryPoint, ...options });
  const status = invocation.run();
  assert.equal(invocation.error, "", `${entryPoint} stopped: ${invocation.error}`);
  assert.equal(status, "returned");
  return invocation;
}

/** The bytes of the buffer bound at an index, after the invocation wrote to its copy. */
function wrote(invocation, index) {
  const param = invocation.boundParams.find((p) => p.set === 0 && p.binding === index);
  const cell = param?.value?.cell;
  return cell?.buffer ? new DataView(cell.buffer.bytes.buffer, cell.buffer.bytes.byteOffset, cell.buffer.bytes.byteLength) : null;
}

const near = (a, b, message) => assert.ok(Math.abs(a - b) < 1e-5, `${message}: ${a} is not ${b}`);

test("the vectors parse and lower without a diagnostic", () => {
  assert.deepEqual(basic.diagnostics, []);
  assert.deepEqual(derivatives.diagnostics, []);
  assert.deepEqual(constants.diagnostics, []);
  assert.deepEqual(basic.entryPoints.map((f) => `${f.qualifier} ${f.name}`), [
    "kernel arithmetic", "kernel control", "kernel layout", "vertex transform", "fragment shade",
  ]);
});

test("arithmetic: vectors, swizzles, conversions, macros and the standard library", () => {
  const u = uniforms({ tint: [0.25, 0.5, 0.75, 1], direction: [0, 0, 1], weight: 0.75 });
  const invocation = run(basic, "arithmetic", {
    bindings: bindings({ buffers: { 0: new Uint8Array(9 * 4), 1: u } }),
    inputs: inputs({ builtins: { thread_position_in_grid: 2 } }),
  });
  const view = wrote(invocation, 0);
  near(view.getFloat32(0, true), (0.25 + 0.5 + 0.75) * 2, "rgb * SCALE summed");
  near(view.getFloat32(4, true), 0.5, "t.yx.x is t.y");
  // int(0.75) is 0, plus kCounts[2] which is 3.
  near(view.getFloat32(8, true), 3, "int conversion truncates, and a constant array indexes");
  near(view.getFloat32(12, true), 0.5 + (1.5 - 0.5) * 0.25, "mix");
  near(view.getFloat32(16, true), 1, "clamp of TWICE(0.75)");
  near(view.getFloat32(20, true), 1, "a normalized vector has length 1");
  near(view.getFloat32(24, true), 0.25 * 0.5 + 1 * 0.5, "the three-argument overload");
  near(view.getFloat32(28, true), 0, "the two-argument overload: dot((0,0,1), (0,1,0))");
  near(view.getFloat32(32, true), 1, "a bool in a constant buffer is one byte");
});

test("control flow: for with a break, while, switch fall-through and default", () => {
  const check = (mode, expected) => {
    const invocation = run(basic, "control", {
      bindings: bindings({ buffers: { 0: new Uint8Array(4 * 4), 1: uniforms({ mode }) } }),
      inputs: inputs({ builtins: { thread_position_in_grid: 3 } }),
    });
    const view = wrote(invocation, 0);
    assert.equal(view.getInt32(0, true), 0 + 1 + 2 + 3 + 4, "the loop breaks at k == 5");
    assert.equal(view.getInt32(4, true), 15, "1, 3, 7, 15 leaves the while loop");
    assert.equal(view.getInt32(8, true), expected, `switch on mode ${mode}`);
    assert.equal(view.getInt32(12, true), 3, "the built-in thread id");
  };
  check(0, 100);
  check(1, 200);
  check(2, 200);
  check(7, 300);
});

test("buffer layout: packed_float3 is twelve bytes, and a write goes back through the pointer", () => {
  const input = bytes(16, (v) => {
    [1, 2, 3].forEach((x, i) => v.setFloat32(i * 4, x, true));
    v.setFloat32(12, 9, true);
  });
  const invocation = run(basic, "layout", {
    bindings: bindings({ buffers: { 0: new Uint8Array(16), 1: uniforms({ weight: 3, rotation: [7, 0, 0, 1] }), 2: input } }),
    inputs: inputs({ builtins: { thread_position_in_grid: 0 } }),
  });
  const view = wrote(invocation, 0);
  near(view.getFloat32(0, true), 3, "packed_float3.x read at offset 0 and scaled");
  near(view.getFloat32(4, true), 6, "packed_float3.y is at offset 4, not 16");
  near(view.getFloat32(8, true), 9, "packed_float3.z is at offset 8");
  near(view.getFloat32(12, true), 7, "scale follows the packed float3 at offset 12");
});

test("a vertex entry point: stage_in attributes, a matrix and a built-in", () => {
  const invocation = run(basic, "transform", {
    bindings: bindings({ buffers: { 1: uniforms({ tint: [1, 0.5, 0.25, 1], weight: 2, rotation: [0, 1, -1, 0] }) } }),
    inputs: inputs({ builtins: { vertex_id: 6 }, attributes: { 0: [1, 2], 1: [1, 1, 1] } }),
  });
  const outputs = invocation.outputs();
  const position = outputs.find((o) => o.builtin === 0);
  // Column-major: rotation is columns (0, 1) and (-1, 0), so it turns (1, 2) into (-2, 1).
  assert.deepEqual(position.value.map((v) => +v.toFixed(4)), [-2, 1, 0, 1]);
  assert.deepEqual(outputs.find((o) => o.name === "color").value.map((v) => +v.toFixed(4)), [1, 0.5, 0.25]);
  near(outputs.find((o) => o.name === "fog").value, 12, "vertex_id times the weight");
});

test("a fragment entry point: varyings by name, an explicit level and a bound texture", () => {
  const invocation = run(basic, "shade", {
    bindings: bindings({ buffers: { 1: uniforms({ tint: [1, 1, 1, 0.25] }) } }),
    inputs: inputs({ varyings: { position: [0.25, 0.25, 0, 1], color: [1, 1, 1], fog: [0] } }),
  });
  const value = invocation.outputs()[0].value;
  // The texture's top-left texel is red, and fog is 0 so the branch is not taken.
  assert.deepEqual(value.map((v) => +v.toFixed(4)), [1, 0, 0, 0.25]);
});

test("a fragment's branch is taken when its varying says so", () => {
  const invocation = run(basic, "shade", {
    bindings: bindings({ buffers: { 1: uniforms({ tint: [1, 1, 1, 1] }) } }),
    inputs: inputs({ varyings: { position: [0.25, 0.25, 0, 1], color: [1, 1, 1], fog: [1] } }),
  });
  assert.deepEqual(invocation.outputs()[0].value.map((v) => +v.toFixed(4)), [1, 0.5, 0.5, 1]);
});

test("derivatives and implicit level of detail come from the pixel quad", () => {
  // uv.x steps by 0.25 across the quad's columns and uv.y across its rows, so dfdx(uv).x and
  // dfdy(uv).y are both 0.25, and fwidth(uv).x is |0.25| + |0| — uv.x does not vary down the quad.
  const quad = new PixelQuad((dx, dy, source) => new MslInvocation(derivatives, {
    entryPoint: "gradients",
    bindings: bindings(),
    inputs: inputs({ varyings: { position: [0, 0, 0, 1], uv: [0.25 + dx * 0.25, 0.25 + dy * 0.25] } }),
    derivatives: source,
  }), 0);
  assert.equal(quad.run(), "returned");
  const value = quad.invocation.outputs()[0].value;
  near(value[0], 0.25, "dfdx");
  near(value[1], 0.25, "dfdy");
  near(value[2], 0.25, "fwidth");
  near(value[3], 1, "the implicit level of detail samples the red texel");
});

test("without a quad, derivatives are zero rather than an error", () => {
  const invocation = run(derivatives, "gradients", {
    bindings: bindings(),
    inputs: inputs({ varyings: { position: [0, 0, 0, 1], uv: [0.25, 0.25] } }),
  });
  const value = invocation.outputs()[0].value;
  near(value[0], 0, "dfdx with no neighbors");
  near(value[2], 0, "fwidth with no neighbors");
});

test("an uncaptured buffer is a warning, not a failure", () => {
  const invocation = run(basic, "arithmetic", {
    bindings: { buffer: () => null, texture: () => null, sampler: () => null },
    inputs: inputs({ builtins: { thread_position_in_grid: 0 } }),
  });
  assert.ok([...invocation.warnings].some((w) => w.includes("buffer(1)")), "the missing uniform buffer is named");
  assert.ok([...invocation.warnings].some((w) => w.includes("was not captured")));
});

test("stepping by source line, with the values each line produced", () => {
  const session = {
    target: { stage: "compute", command: 0, invocation: [0, 0, 0] },
    program: basic,
    notes: [], limits: {}, description: "test",
    start: () => new MslInvocation(basic, {
      entryPoint: "control",
      bindings: bindings({ buffers: { 0: new Uint8Array(16), 1: uniforms({ mode: 1 }) } }),
      inputs: inputs({ builtins: { thread_position_in_grid: 0 } }),
    }),
  };
  const control = new DebugController(session);
  assert.equal(control.mode, "source", "MSL is always stepped by source line");
  const lineOf = (c) => c.location(c.invocation.current)?.line;
  const first = lineOf(control);
  assert.equal(first, lineOfText(basic, "int total = 0;"), "the invocation starts on the first statement");
  control.advance("over");
  assert.notEqual(lineOf(control), first, "a step over moves to another line");

  // mode 1 takes the `case 1:` arm, so a breakpoint there is reached and one on the default is not.
  const stopped = new DebugController(session);
  stopped.toggleBreakpoint(sourceKey(0, lineOfText(basic, "picked = 200;")));
  stopped.advance("continue");
  assert.equal(lineOf(stopped), lineOfText(basic, "picked = 200;"), "the run stopped on the breakpoint");
  assert.ok(!stopped.finished, "and the invocation has not finished");

  const missed = new DebugController(session);
  missed.toggleBreakpoint(sourceKey(0, lineOfText(basic, "picked = 300;")));
  missed.advance("continue");
  assert.ok(missed.finished, "a breakpoint on a branch not taken never stops the run");

  // The values a line produced are what the watch shows.
  const values = new DebugController(session);
  values.toggleBreakpoint(sourceKey(0, lineOfText(basic, "out[1] = doubled;")));
  values.advance("continue");
  const shown = values.lastLine.results.map((r) => `${basic.nameOf(r.id)} = ${basic.valueText(basic.resultType(r), r.value)}`);
  assert.ok(shown.some((s) => s.startsWith("out = ")), `a store names the buffer it wrote: ${shown.join(", ")}`);
});

/** The 1-based line of the first line of the shader containing `text`. */
function lineOfText(program, text) {
  const lines = program.files[0].text.split("\n");
  const at = lines.findIndex((l) => l.includes(text));
  assert.ok(at >= 0, `the vector has no line containing ${text}`);
  return at + 1;
}

// ---------------------------------------------------------------------------------------------
// Function constants: what the application specialized the shader with when it built the function.

/** The `[[function_constant(n)]]` values an application set, as the Metal session gathers them. */
function specialization({ byIndex = {}, byName = {} } = {}) {
  return {
    byIndex: new Map(Object.entries(byIndex).map(([k, v]) => [Number(k), v])),
    byName: new Map(Object.entries(byName)),
  };
}

function runSpecialized(constantValues) {
  const uniforms = bytes(16, (v) => [0.25, 0.5, 0.75, 1].forEach((x, i) => v.setFloat32(i * 4, x, true)));
  const invocation = new MslInvocation(constants, {
    entryPoint: "specialized",
    bindings: bindings({ buffers: { 0: new Uint8Array(5 * 4), 1: uniforms } }),
    inputs: inputs({ builtins: { thread_position_in_grid: 4 } }),
    constants: constantValues,
  });
  assert.equal(invocation.run(), "returned", invocation.error);
  const view = wrote(invocation, 0);
  return {
    color: [0, 1, 2].map((i) => +view.getFloat32(i * 4, true).toFixed(4)),
    modeDefined: view.getFloat32(12, true),
    warnings: [...invocation.warnings],
  };
}

test("the shader is specialized with the constants the function was built with", () => {
  // kEnable mixes towards white by kAmount, then kMode 1 swizzles to bgr.
  const r = runSpecialized(specialization({ byIndex: { 0: 1, 1: 0.5, 2: true } }));
  const mixed = [0.25, 0.5, 0.75].map((c) => +(c + (1 - c) * 0.5).toFixed(4));
  assert.deepEqual(r.color, [mixed[2], mixed[1], mixed[0]]);
  assert.equal(r.modeDefined, 1, "is_function_constant_defined is true for a constant that was set");
});

test("a different specialization of the same shader takes different branches", () => {
  // kMode 2 inverts, and kEnable false skips the mix entirely.
  const r = runSpecialized(specialization({ byIndex: { 0: 2, 1: 0.5, 2: false } }));
  assert.deepEqual(r.color, [0.75, 0.5, 0.25]);
});

test("a constant set by name reaches the same global", () => {
  const r = runSpecialized(specialization({ byIndex: { 0: 0, 1: 0, 2: false }, byName: { kBias: [0.1, 0.2, 0.3] } }));
  assert.deepEqual(r.color, [0.35, 0.7, 1.05],
    "is_function_constant_defined(kBias) was true, so the bias was added");
});

test("a constant the capture has no value for reads as zero, and says so", () => {
  const r = runSpecialized(specialization());
  // Nothing set: kEnable is false, kMode is 0, and the guarded bias is not added.
  assert.deepEqual(r.color, [0.25, 0.5, 0.75]);
  assert.equal(r.modeDefined, 0, "is_function_constant_defined is false for a constant that was not set");
  assert.equal(r.warnings.length, 1, `one warning naming what is missing: ${r.warnings}`);
  assert.match(r.warnings[0], /kMode \[\[function_constant\(0\)\]\]/);
  assert.match(r.warnings[0], /specialized/);
});

test("a constant another entry point reads is not this one's to be missing", () => {
  // `specialized` reads kMode, kAmount and kEnable; a shader whose entry point reads none of them
  // is not specialized at all, and must not be reported as though it were.
  const quiet = new MslProgram(`
    #include <metal_stdlib>
    using namespace metal;
    constant int kUnused [[function_constant(0)]];
    kernel void plain(device float *out [[buffer(0)]], uint i [[thread_position_in_grid]]) {
      out[0] = float(i);
    }
  `, "quiet.metal");
  const invocation = new MslInvocation(quiet, {
    entryPoint: "plain",
    bindings: bindings({ buffers: { 0: new Uint8Array(8) } }),
    inputs: inputs({ builtins: { thread_position_in_grid: 2 } }),
  });
  assert.equal(invocation.run(), "returned", invocation.error);
  assert.deepEqual([...invocation.warnings], [], "an entry point that reads no constant warns about none");
});
