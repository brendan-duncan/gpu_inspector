// Debugging a shader through a translation (main/shader_tools.ts decompileForDebugging, and
// shader_debug_setup.ts compareWithOriginal): the interpreter's vectors decompiled to GLSL by
// spirv-cross and compiled back with line information by glslang, stepped by source line, and run
// beside the original with the same inputs to check the two agree. Needs the Vulkan SDK's tools;
// skipped without them.
import { test } from "node:test";
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "decompile-"));
const out = join(dir, "decompile.mjs");
buildSync({
  stdin: {
    contents: `export { SpirvModule } from "./renderer/spirv/module.js"; export { Invocation } from "./renderer/spirv/interpreter.js"; export { SpirvProgram } from "./renderer/spirv/program.js"; export { DebugController } from "./renderer/shader_debugger.js"; export { compareWithOriginal } from "./renderer/shader_debug_setup.js"; export { decompileForDebugging, findTool } from "./main/shader_tools.js";`,
    resolveDir: join(here, "..", "src"), loader: "ts",
  },
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const { SpirvModule, Invocation, SpirvProgram, DebugController, compareWithOriginal, decompileForDebugging, findTool } = await import(pathToFileURL(out).href);

function available(tool) {
  try {
    execFileSync(findTool(tool), ["--help"], { stdio: "ignore" });
    return true;
  } catch (e) {
    // glslangValidator exits non-zero for --help but still ran.
    return e.code !== "ENOENT";
  }
}
const skip = !available("spirv-cross") || !available("glslangValidator") ? "spirv-cross or glslangValidator is not installed" : false;

const bytesOf = (name) => new Uint8Array(readFileSync(join(here, "vectors", "interpreter", name)));

function bytes(size, write) {
  const b = new Uint8Array(size);
  write(new DataView(b.buffer));
  return b;
}

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

const computeBindings = () => bindings({
  buffers: {
    "0/0": bytes(20, (v) => {
      v.setUint32(0, 10, true);
      [0.35, 1.26, 2.5, 7.9].forEach((x, i) => v.setFloat32(4 + i * 4, x, true));
    }),
    "0/1": new Uint8Array(16),
  },
});

const vertexCamera = () => bytes(112, (v) => {
  [2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1].forEach((x, i) => v.setFloat32(i * 4, x, true));
  [0, 1, 0].forEach((x, i) => v.setFloat32(64 + i * 4, x, true));
  [1, 0, 0].forEach((x, i) => v.setFloat32(80 + i * 4, x, true));
  [0, 0, 1].forEach((x, i) => v.setFloat32(96 + i * 4, x, true));
});

const hlslParams = () => bytes(80, (v) => {
  [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 10, 20, 30, 1].forEach((x, i) => v.setFloat32(i * 4, x, true));
  [0.2, 0.4, 0.6].forEach((x, i) => v.setFloat32(64 + i * 4, x, true));
  v.setFloat32(76, 2, true);
});

// Each vector with the invocations to run it on: [stage, layer stage name, options per invocation].
const CASES = [
  ["basic.frag.opt.spv", "fragment", [0, 1, 2].map((mode) => ({ bindings: fragmentBindings(mode), inputs: FRAGMENT_INPUTS() }))
    .concat([{ bindings: fragmentBindings(0, -1000), inputs: FRAGMENT_INPUTS() }])],
  ["basic.hlsl.spv", "fragment", [{ bindings: bindings({ buffers: { "0/0": hlslParams() }, textures: { "0/1": TEXTURE } }), inputs: inputs({ 0: [0.5, 0.25] }, { 15: [100.5, 50.5, 0.5, 1] }) }]],
  ["basic.vert.spv", "vertex", [{ bindings: bindings({ buffers: { "0/0": vertexCamera() } }), inputs: inputs({ 0: [1, 2, 3], 1: [0.1, 0.9] }, { 42: 5, 43: 2 }) }]],
  ["basic.comp.opt.spv", "compute", [0, 1, 3, 9].map((x) => ({ bindings: computeBindings(), inputs: inputs({}, { 28: [x, 0, 0] }) }))],
];

for (const [name, stage, runs] of CASES) {
  test(`${name}: the decompiled GLSL steps by line and computes what the original does`, { skip }, async () => {
    const original = bytesOf(name);
    const r = await decompileForDebugging(original, stage, "main");
    assert.ok(r.ok, r.log);
    assert.match(r.source, /#version 460/);
    const translated = new SpirvModule(r.spirv);
    const program = SpirvProgram.of(translated);
    assert.ok(program.hasSourceText(), "the recompiled module embeds the decompiled text");
    assert.equal(program.files[program.mainFile]?.name, "decompiled.glsl");
    assert.ok(program.stopKeys("source").size >= 5, `too few lines to step: ${program.stopKeys("source").size}`);

    for (const [i, options] of runs.entries()) {
      const a = new Invocation(translated, options);
      const b = new Invocation(new SpirvModule(original), options);
      a.run();
      b.run();
      const c = compareWithOriginal(a, b, stage);
      assert.ok(c.matches, `invocation ${i}: ${JSON.stringify(c)}`);
      if (a.status === "returned") assert.ok(c.values.length > 0, `invocation ${i} compared nothing`);
    }

    // The debugger steps the translation by its lines.
    const ctl = new DebugController({ program, start: () => new Invocation(translated, runs[0]) });
    assert.equal(ctl.mode, "source");
    const lines = new Set();
    while (!ctl.finished) {
      ctl.advance("into");
      const loc = ctl.location(ctl.invocation.current);
      if (loc) lines.add(loc.line);
    }
    assert.ok(lines.size >= 3, `stepped only lines ${[...lines]}`);
  });
}

test("a mismatch is reported: a different module is not the original", { skip }, async () => {
  const options = { bindings: fragmentBindings(1), inputs: FRAGMENT_INPUTS() };
  const r = await decompileForDebugging(bytesOf("basic.frag.opt.spv"), "fragment", "main");
  assert.ok(r.ok, r.log);
  const a = new Invocation(new SpirvModule(r.spirv), options);
  const b = new Invocation(new SpirvModule(bytesOf("basic.frag.opt.spv")), { bindings: fragmentBindings(2), inputs: FRAGMENT_INPUTS() });
  a.run();
  b.run();
  const c = compareWithOriginal(a, b, "fragment");
  assert.equal(c.matches, false);
  assert.ok(c.values.some((v) => !v.matches && v.label.startsWith("location 0")), JSON.stringify(c.values));
});

test("a module spirv-cross cannot read fails with the reason", { skip }, async () => {
  const r = await decompileForDebugging(new Uint8Array(20), "fragment", "main");
  assert.equal(r.ok, false);
  assert.ok(r.log.length > 0);
});
