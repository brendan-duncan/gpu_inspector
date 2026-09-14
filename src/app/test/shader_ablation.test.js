// Shader cost by ablation: the variants src/renderer/vulkan/spirv_ablate.ts plans, the request and
// answer of `vkinsp_replay --ablate` (src/renderer/shader_ablation.ts), and what a part is charged.
//
// vectors/ablation/heavy.frag.spv is test/triangle/heavy.frag (`glslc -g`): fbm (a loop of hash
// calls), blurred (a loop of texture reads), and main combining them. reuse.frag.spv (`glslc -g`,
// its source beside it) keeps two texture reads in one temporary, and branches on the first.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "ablation-"));
const load = async (entry, name) => {
  const out = join(dir, `${name}.mjs`);
  buildSync({ entryPoints: [join(here, "..", "src", entry)], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
  return import(pathToFileURL(out).href);
};
const { planAblation } = await load("renderer/vulkan/spirv_ablate.ts", "spirv_ablate");
const { analyzeSpirv } = await load("renderer/vulkan/spirv_analysis.ts", "spirv_analysis");
const { encodeAblationRequest, parseAblationResult, measuredAblation, partShare } = await load("renderer/shader_ablation.ts", "shader_ablation");
const { validateSpirv } = await load("main/shader_tools.ts", "shader_tools");

const vector = (name) => new Uint8Array(readFileSync(join(here, "vectors", "ablation", name)));
const plan = (name) => {
  const spirv = vector(name);
  return planAblation(spirv, "fragment", "main", analyzeSpirv(spirv), {});
};

test("a stage's functions, lines and textures each get a variant, and what control flow needs is left alone", () => {
  const p = plan("heavy.frag.spv");
  const names = p.variants.map((v) => `${v.kind} ${v.name}`);
  for (const expected of ["stage fragment: main", "function fbm(vf2;", "function hash(vf2;", "function blurred(vf2;", "texture checker", "line heavy.frag:18", "line heavy.frag:54"]) {
    assert.ok(names.includes(expected), `${expected} in ${names.join(", ")}`);
  }
  const skipped = new Map(p.skipped.map((s) => [s.name, s.reason]));
  // The loop tests.
  for (const line of [25, 43, 44]) assert.match(skipped.get(`heavy.frag:${line}`) ?? "", /control flow depends/, `line ${line}`);
  // The octave's `p = p * 2.03` and `amplitude *= 0.5` feed the next iteration: taking them out would
  // let the compiler hoist the loop.
  for (const line of [34, 35]) assert.match(skipped.get(`heavy.frag:${line}`) ?? "", /other lines of its loop read every iteration/, `line ${line}`);
});

test("a line's upstream names the parts whose values reach it", () => {
  const p = plan("heavy.frag.spv");
  const upstream = (name) => p.variants.find((v) => v.name === name).upstream.map((k) => p.variants[k].name);
  // `vec3 color = mix(tex, vec3(n), 0.5)` gathers fbm and blurred.
  assert.ok(upstream("heavy.frag:54").includes("fbm(vf2;"));
  assert.ok(upstream("heavy.frag:54").includes("blurred(vf2;"));
  // `vec2 u = f * f * (3.0 - 2.0 * f)` reads f.
  assert.deepEqual(upstream("heavy.frag:32"), ["heavy.frag:27"]);
  // The hash itself feeds from nothing measured.
  assert.deepEqual(upstream("heavy.frag:18"), []);
});

test("a temporary reused for another read does not pull that read into the branch on the first", () => {
  const p = plan("reuse.frag.spv");
  assert.ok(p.variants.some((v) => v.kind === "texture" && v.name === "albedo"), p.variants.map((v) => v.name).join(", "));
  const mask = p.skipped.find((s) => s.kind === "texture" && s.name === "mask");
  assert.match(mask?.reason ?? "", /control flow depends on what is read from it/);
});

test("every variant passes spirv-val", async (t) => {
  const tried = await validateSpirv(vector("reuse.frag.spv"));
  if (tried === undefined) {
    t.skip("spirv-val not found (install the Vulkan SDK)");
    return;
  }
  for (const name of ["heavy.frag.spv", "reuse.frag.spv"]) {
    for (const v of plan(name).variants) assert.equal(await validateSpirv(v.spirv), null, `${name}: ${v.kind} ${v.name}`);
  }
});

test("the request carries every variant's SPIR-V after its manifest", () => {
  const a = new Uint8Array([3, 2, 3, 7, 1, 1, 1, 1]);
  const b = new Uint8Array([9, 9, 9, 9]);
  const bytes = encodeAblationRequest([{ command: 12, stage: "fragment", repeat: 8.4, variants: [{ name: "one", spirv: a }, { name: "two", spirv: b }] }], 5);
  const magic = "ABLATE 1\n";
  assert.equal(new TextDecoder().decode(bytes.subarray(0, magic.length)), magic);
  const length = new DataView(bytes.buffer, bytes.byteOffset).getUint32(magic.length, true);
  const json = JSON.parse(new TextDecoder().decode(bytes.subarray(magic.length + 4, magic.length + 4 + length)));
  assert.equal(json.rounds, 5);
  assert.equal(json.targets[0].repeat, 8);
  const payloads = bytes.subarray(magic.length + 4 + length);
  const [off, len] = json.targets[0].variants[1].payload;
  assert.deepEqual([...payloads.subarray(off, off + len)], [...b]);
  assert.equal(payloads.byteLength, a.byteLength + b.byteLength);
});

test("what a part saved, what a line did itself, and its share of the stage", () => {
  const timing = (name, ms, samples = [ms, ms, ms]) => ({ name, measured: true, ms, samples });
  const result = parseAblationResult(JSON.stringify({
    format: "gpu-inspector-ablation", device: "Test GPU", problems: [],
    targets: [{ command: 30, stage: "fragment", pipeline: 7, frame: 0, commandBuffer: 4, passIndex: 0, rounds: 5,
      baseline: timing("baseline", 1.0, [0.99, 1.0, 1.02]),
      variants: [timing("stage", 0.2), timing("fbm", 0.4), timing("line a", 0.5), timing("line b", 0.3), { name: "line c", measured: false, ms: 0, samples: [], note: "not issued" }] }],
  }));
  assert.equal(result.device, "Test GPU");
  const part = (kind, name, upstream = []) => ({ kind, name, spirv: new Uint8Array(), edits: 1, upstream });
  const p = { variants: [part("stage", "stage"), part("function", "fbm"), part("line", "a"), part("line", "b", [1, 2]), part("line", "c")], skipped: [] };
  const a = measuredAblation(7, "fragment", "main", p, result.targets[0], result.device);
  assert.ok(Math.abs(a.stageMs - 0.8) < 1e-9);
  assert.ok(Math.abs(a.noiseMs - 0.01) < 1e-9, `the baseline's median absolute deviation: ${a.noiseMs}`);
  const byName = new Map(a.parts.map((x) => [x.name, x]));
  assert.ok(Math.abs(byName.get("fbm").savedMs - 0.6) < 1e-9);
  // b saved 0.7 but fbm, which feeds it, saved 0.6 of that.
  assert.ok(Math.abs(byName.get("b").ownMs - 0.1) < 1e-9);
  assert.equal(byName.get("c").savedMs, null);
  assert.equal(byName.get("c").note, "not issued");
  assert.ok(Math.abs(partShare(a, byName.get("fbm")) - 0.75) < 1e-9);
  assert.ok(Math.abs(partShare(a, byName.get("b")) - 0.125) < 1e-9, "a line's share is what it did itself");
  assert.equal(partShare(a, byName.get("c")), null);
});
