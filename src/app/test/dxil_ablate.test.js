// Shader cost by ablation on Direct3D 12: the variants src/renderer/d3d12/dxil_ablate.ts plans of a
// DXIL module, written as its disassembly. What they are timed by is `dxinsp_replay --ablate`, and
// tools/ui_tests.py's d3d12-measure-shader runs the whole of it on a GPU; this is the part that
// needs none.
//
// vectors/ablation/heavy_ps.ll is test/d3d12_triangle/heavy.hlsl's PSMain (`dxc -T ps_6_0 -Zi`,
// then `dxinsp_shader --disassemble`): Fbm (a loop of Hash calls), Blurred (a loop of texture
// reads), and PSMain combining them. wave_cs.ll is wave.hlsl's CSMain: one branch, one buffer store.
// Every function is inlined by then and every vector is scalars, which is what the cases are about.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const out = join(mkdtempSync(join(tmpdir(), "dxilablate-")), "dxil_ablate.mjs");
buildSync({ entryPoints: [join(here, "..", "src", "renderer", "d3d12", "dxil_ablate.ts")], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
const { planDxilAblation } = await import(pathToFileURL(out).href);

const vector = (name) => readFileSync(join(here, "vectors", "ablation", name), "utf8");
const heavy = vector("heavy_ps.ll");
const plan = planDxilAblation(heavy, "fragment", "PSMain");
const variant = (kind, name) => plan.variants.find((v) => v.kind === kind && v.name === name);
/** The instructions of a module, without the comments the disassembler writes after them. */
const code = (text) => text.split(/\r?\n/).filter((l) => /^\s+\S/.test(l) && !l.startsWith(";")).map((l) => l.replace(/\s*;.*$/, ""));

test("a stage's functions, lines and textures each get a variant", () => {
  const names = plan.variants.map((v) => `${v.kind} ${v.name}`);
  for (const expected of ["stage fragment: PSMain", "function Fbm", "function Hash", "function Blurred", "texture checker", "line heavy.hlsl:20", "line heavy.hlsl:47"]) {
    assert.ok(names.includes(expected), `${expected} in ${names.join(", ")}`);
  }
  // The texture is named as the HLSL binds it, which is what tells two of them apart.
  assert.deepEqual([variant("texture", "checker").set, variant("texture", "checker").binding], [0, 0]);
  // Lines know the function they were inlined from, which is what finds their frame in the flame graph.
  assert.equal(variant("line", "heavy.hlsl:20").functionName, "Hash");
  assert.equal(variant("line", "heavy.hlsl:47").functionName, "Blurred");
});

test("what control flow needs, what a loop carries and what only reads have no variant", () => {
  const skipped = new Map(plan.skipped.map((s) => [s.name, s.reason]));
  // The loop tests: `i < 480`, and Blurred's two.
  for (const line of ["heavy.hlsl:27", "heavy.hlsl:45", "heavy.hlsl:46"]) assert.match(skipped.get(line) ?? "", /control flow/, line);
  // `p = p * 2.03 + ...` and `amplitude *= 0.5`: every iteration reads what the last one left.
  for (const line of ["heavy.hlsl:36", "heavy.hlsl:37"]) assert.match(skipped.get(line) ?? "", /loop/, line);
  // The entry point's signature is where dxc puts the input loads, and a load is not work: a
  // variant of it would be the shader with every input replaced, which is the whole shader.
  assert.match(skipped.get("heavy.hlsl:53") ?? "", /nothing that can be replaced/);
});

test("a variant changes operands and adds the stand-in, and renumbers nothing", () => {
  const original = code(heavy);
  const defined = (lines) => lines.map((l) => /^\s+(%\d+) = /.exec(l)?.[1]).filter(Boolean);
  for (const v of plan.variants) {
    const lines = code(v.text);
    const added = lines.filter((l) => /^\s+%ablate\./.test(l));
    assert.equal(lines.length, original.length + added.length, `${v.name}: only the stand-in's lines are new`);
    // Unnamed values are numbered in order of appearance: one taken out would shift every one after it.
    assert.deepEqual(defined(lines), defined(original), `${v.name}: the numbered values are the ones the module had`);
    assert.ok(v.edits > 0);
  }
  // The stand-in is an input the shader already loads, loaded again under a name at the top of
  // the function: something the compiler cannot fold, where a constant would let it fold the rest.
  const fbm = code(variant("function", "Fbm").text);
  const first = fbm.findIndex((l) => /^\s+%\S+ = call /.test(l));
  assert.match(fbm[first], /^\s+%ablate\.src = call float @dx\.op\.loadInput\.f32\(/);
  // The stage variant needs none: what it writes is made constant, and nothing is computed from an output.
  const stage = variant("stage", "fragment: PSMain");
  assert.ok(!stage.text.includes("%ablate."));
  assert.equal((stage.text.match(/@dx\.op\.storeOutput\.f32\([^)]*float 5\.000000e-01\)/g) ?? []).length, 3);
});

test("a function is not charged with work the optimizer moved across its boundary", () => {
  // `tex * (0.5 + n)`, with tex = Blurred's sum / 16: dxc folds the 1/16 into a multiply of
  // (0.5 + n) and leaves it Blurred's line. It reads nothing Blurred computed and everything Fbm
  // did, so replacing it would leave Fbm unread and charge Blurred with the whole stage.
  const moved = code(heavy).find((l) => l.includes("6.250000e-02"));
  const name = /^\s+(%\S+) = /.exec(moved)[1];
  const readers = (text) => code(text).filter((l) => l.includes(`${name},`) || l.endsWith(name) || l.includes(`${name} `)).filter((l) => !l.includes(`${name} =`) && !l.includes("llvm.dbg"));
  assert.ok(readers(heavy).length > 0);
  assert.equal(readers(variant("function", "Blurred").text).length, readers(heavy).length, "what Fbm's result feeds is still read");
  // Blurred's own values are what is replaced: the three sums the loops accumulate.
  assert.ok(variant("function", "Blurred").edits >= 3);
});

test("upstream says which parts' values reach a line", () => {
  const index = (kind, name) => plan.variants.findIndex((v) => v.kind === kind && v.name === name);
  // `sum / 16.0` is computed from what the sampling line accumulated.
  assert.ok(variant("line", "heavy.hlsl:57").upstream.includes(index("function", "Fbm")));
  assert.ok(variant("line", "heavy.hlsl:47").upstream.includes(index("texture", "checker")));
  for (const v of plan.variants) for (const k of v.upstream) assert.ok(k >= 0 && k < plan.variants.length && plan.variants[k] !== v);
});

test("a module with no debug information still has its textures and its stage", () => {
  const stripped = heavy.split(/\r?\n/).filter((l) => !l.includes("@llvm.dbg.") && !/^!\d+ = (distinct )?!DI/.test(l)).join("\n").replace(/, !dbg !\d+/g, "");
  const p = planDxilAblation(stripped, "fragment", "PSMain");
  assert.deepEqual(p.variants.map((v) => `${v.kind} ${v.name}`), ["stage fragment: PSMain", "texture checker"]);
  assert.match(p.skipped.find((s) => s.kind === "line")?.reason ?? "", /no line information/);
});

test("a compute stage is its writes, and the thread id stands in", () => {
  const p = planDxilAblation(vector("wave_cs.ll"), "compute", "CSMain");
  const stage = p.variants.find((v) => v.kind === "stage");
  assert.ok(stage, p.skipped.map((s) => `${s.name}: ${s.reason}`).join("; "));
  // The value written, not where: the coordinate and the mask stay, and an `undef` component was never written.
  const store = code(stage.text).find((l) => l.includes("@dx.op.bufferStore.f32"));
  assert.match(store, /i32 %1, i32 0, float 5\.000000e-01, float undef, float undef, float undef, i8 1\)/);
  // `wave[id.x] = sin(...)`: the line's value is replaced where the store reads it, by the thread id as a float.
  const line = p.variants.find((v) => v.kind === "line");
  assert.ok(line, "the line that computes the sine");
  const lines = code(line.text);
  assert.ok(lines.some((l) => /^\s+%ablate\.src = call i32 @dx\.op\.threadId\.i32\(i32 93, i32 0\)/.test(l)));
  assert.ok(lines.some((l) => /^\s+%ablate\.float = uitofp i32 %ablate\.src to float/.test(l)));
  assert.ok(lines.some((l) => l.includes("@dx.op.bufferStore.f32") && l.includes("float %ablate.float")));
});

test("only pixel and compute stages are measured, and a library is not", () => {
  assert.match(planDxilAblation(heavy, "vertex", "VSMain").skipped[0].reason, /only pixel and compute/);
  const library = heavy.replace("define void @PSMain()", "define void @Other() {\n  ret void\n}\n\ndefine void @PSMain()");
  assert.match(planDxilAblation(library, "fragment", "PSMain").skipped[0].reason, /more than one function/);
});
