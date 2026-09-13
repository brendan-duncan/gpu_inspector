// The shader debugger's stepping (src/renderer/shader_debugger.ts): over, into and out of source
// lines, breakpoints and the values a line produced, on basic.frag (test/vectors/interpreter); and a
// captured texture as the interpreter samples it (shader_debug_setup.ts).
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "dbgctl-"));
const out = join(dir, "dbg.mjs");
buildSync({
  stdin: {
    contents: `export { SpirvModule } from "./spirv/module.js"; export { Invocation } from "./spirv/interpreter.js"; export * from "./shader_debugger.js"; export { debugTexture } from "./shader_debug_setup.js"; export { recordVertex } from "./shader_debugger_view.js";`,
    resolveDir: join(here, "..", "src", "renderer"), loader: "ts",
  },
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const { SpirvModule, Invocation, DebugController, sourceKey, valueText, executableInstructions, debugTexture, recordVertex } = await import(pathToFileURL(out).href);

const module = new SpirvModule(new Uint8Array(readFileSync(join(here, "vectors", "interpreter", "basic.frag.spv"))));

function session(mode = 0) {
  const uniforms = new Uint8Array(136);
  const v = new DataView(uniforms.buffer);
  [2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0, 1, 2, 3, 1].forEach((x, i) => v.setFloat32(i * 4, x, true));
  [1, 2, 0.5].forEach((x, i) => v.setFloat32(64 + i * 4, x, true));
  v.setFloat32(76, 1.5, true);
  [0.5, 1, 2].forEach((x, i) => v.setFloat32(80 + i * 16, x, true));
  v.setInt32(128, mode, true);
  const push = new Uint8Array(8);
  new DataView(push.buffer).setFloat32(0, 0.125, true);
  new DataView(push.buffer).setUint32(4, 1, true);
  const texture = {
    width: 2, height: 2, depth: 1, layers: 1, mips: 1, baseMip: 0, format: "VK_FORMAT_R8G8B8A8_UNORM", integer: false,
    level: () => ({ width: 2, height: 2, texels: new Float32Array(16).fill(1) }),
  };
  const bindings = {
    buffer: (set, binding) => (binding === 0 ? uniforms : null),
    texture: (set, binding) => (binding === 1 ? texture : null),
    sampler: () => null,
    pushConstants: push,
    specialization: new Map(),
  };
  const inputs = { locations: new Map([[0, [0.5, 0.25, 1]], [1, [0.75, 0.25]]]), builtins: new Map([[15, [10.5, 20.5, 0.5, 1]]]) };
  return {
    target: { stage: "fragment", command: 0, x: 10, y: 20 }, module, notes: [], limits: {}, description: "test",
    start: () => new Invocation(module, { bindings, inputs }),
  };
}

const lineOf = (ctl) => ctl.location(ctl.invocation.current)?.line;

test("starts on the first source line and steps over lines without entering calls", () => {
  const ctl = new DebugController(session(0));
  assert.equal(ctl.mode, "source");
  assert.equal(lineOf(ctl), 31);
  const lines = [31];
  while (!ctl.finished) {
    ctl.advance("over");
    assert.equal(ctl.invocation.frames.length <= 1 || ctl.finished, true, "step over stays in main");
    if (!ctl.finished) lines.push(lineOf(ctl));
  }
  assert.equal(ctl.invocation.status, "returned");
  assert.ok(!lines.includes(27), `shade() was stepped over: ${lines}`);
  assert.ok(lines.filter((l) => l === 34).length === 3, `the loop body ran three times: ${lines}`);
  assert.ok(lines.includes(40) && lines.includes(56), lines.join(","));
});

test("steps into a call and out of it again", () => {
  const ctl = new DebugController(session(0));
  ctl.breakpoints.add(sourceKey(0, 40));
  ctl.advance("continue");
  assert.equal(lineOf(ctl), 40);
  ctl.advance("into");
  assert.equal(lineOf(ctl), 27);
  assert.equal(ctl.invocation.frames.length, 2);
  ctl.advance("out");
  assert.equal(ctl.invocation.frames.length, 1);
  assert.equal(lineOf(ctl), 40, "back on the call's line, to store its result");
});

test("breakpoints stop each time their line is entered", () => {
  const ctl = new DebugController(session(2));
  assert.equal(ctl.toggleBreakpoint(sourceKey(0, 34)), true);
  let stops = 0;
  while (!ctl.finished) {
    ctl.advance("continue");
    if (!ctl.finished) {
      assert.equal(lineOf(ctl), 34);
      stops++;
    }
  }
  assert.equal(stops, 3);
  assert.equal(ctl.toggleBreakpoint(sourceKey(0, 34)), false);
});

test("a line's values, budgeted steps, restart and instruction mode", () => {
  const ctl = new DebugController(session(0));
  ctl.breakpoints.add(sourceKey(0, 37));
  ctl.advance("continue");
  ctl.advance("over");
  const values = ctl.lastLine.results.map((r) => valueText(module, r.inst.resultType, r.value));
  assert.ok(values.includes("(0.5, 0.5, 0.5)"), `color = inColor * params.tint: ${values.join(" | ")}`);

  // A continue in small budgets gets to the same place as one in a single call.
  ctl.restart();
  assert.equal(lineOf(ctl), 31);
  ctl.begin("continue");
  let rounds = 0;
  while (!ctl.proceed(5)) rounds++;
  assert.ok(rounds > 1);
  assert.equal(lineOf(ctl), 37);

  ctl.mode = "instruction";
  const at = ctl.invocation.current.index;
  ctl.advance("instruction");
  assert.ok(ctl.invocation.current.index > at);
  assert.ok(executableInstructions(module).includes(ctl.invocation.current.index));
});

test("a captured texture as a shader samples it: every mip, sRGB made linear, the view's component mapping", () => {
  // A 2x1 R8G8B8A8_SRGB image with its 1x1 mip after it.
  const data = new Uint8Array([255, 0, 128, 10, 0, 255, 0, 20, 188, 188, 188, 30]);
  const info = { id: 1, format: "VK_FORMAT_R8G8B8A8_SRGB", aspect: "color", width: 2, height: 1, depth: 1, layers: 1, mip: 0, mips: 2, size: 12 };
  const components = { r: "VK_COMPONENT_SWIZZLE_G", g: "VK_COMPONENT_SWIZZLE_R", b: "VK_COMPONENT_SWIZZLE_ONE", a: "VK_COMPONENT_SWIZZLE_IDENTITY" };
  const tex = debugTexture({ info, data }, components);
  const base = Array.from(tex.level(0, 0).texels);
  const near = (a, e) => a.every((x, i) => Math.abs(x - e[i]) < 2e-3);
  assert.ok(near(base, [0, 1, 1, 10 / 255, 1, 0, 1, 20 / 255]), `mip 0: ${base}`);
  const mip = Array.from(tex.level(1, 0).texels);
  assert.ok(near(mip, [0.5029, 0.5029, 1, 30 / 255]), `mip 1 (sRGB 188 is linear 0.503): ${mip}`);
  assert.equal(tex.level(2, 0), null);
});

test("a VS Out row names the vertex and instance it was, strips and fans written as lists", () => {
  assert.deepEqual(recordVertex(7, 6, "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST"), { vertex: 1, instance: 1 });
  // A 5-vertex strip: triangles (0,1,2), (1,3,2), (2,3,4).
  const strip = [...Array(9).keys()].map((r) => recordVertex(r, 5, "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP").vertex);
  assert.deepEqual(strip, [0, 1, 2, 1, 3, 2, 2, 3, 4]);
  assert.deepEqual(recordVertex(9, 5, "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP"), { vertex: 0, instance: 1 });
  const fan = [...Array(6).keys()].map((r) => recordVertex(r, 4, "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN").vertex);
  assert.deepEqual(fan, [1, 2, 0, 2, 3, 0]);
});
