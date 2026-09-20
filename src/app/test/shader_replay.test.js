// A shader edited and run in the capture (src/renderer/shader_replay.ts): the request the replay
// tools read and the render targets they answer with. tools/ui_tests.py's shader-edit cases run the
// whole of it on a GPU; this is the two file layouts and what is said about a result.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const out = join(mkdtempSync(join(tmpdir(), "shaderreplay-")), "shader_replay.mjs");
buildSync({ entryPoints: [join(here, "..", "src", "renderer", "shader_replay.ts")], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
const { encodeReplaceRequest, parseReplayedTargets, changedTargets, replacementProblems, replayedTargetsSummary } = await import(pathToFileURL(out).href);

/** `--target-data` as the tools write it: magic, manifest length, manifest, pixels. */
function targetsFile(manifest, pixels = new Uint8Array(0)) {
  const magic = new TextEncoder().encode("TARGETS 1\n");
  const json = new TextEncoder().encode(JSON.stringify(manifest));
  const bytes = new Uint8Array(magic.length + 4 + json.length + pixels.length);
  bytes.set(magic, 0);
  new DataView(bytes.buffer).setUint32(magic.length, json.length, true);
  bytes.set(json, magic.length + 4);
  bytes.set(pixels, magic.length + 4 + json.length);
  return bytes;
}
const target = (over) => ({ image: 16, commandBuffer: 8, frame: 0, passIndex: 0, attachment: 0, aspect: "color", format: "VK_FORMAT_B8G8R8A8_UNORM",
  width: 2, height: 2, compared: true, texels: 4, differingTexels: 0, maxByteDelta: 0, ...over });

test("the request names each code by where it is after the manifest", () => {
  const a = new Uint8Array([1, 2, 3, 4]);
  const b = new Uint8Array([9, 8, 7, 6, 5, 4, 3, 2]);
  const bytes = encodeReplaceRequest([{ pipeline: 48, stage: "fragment", code: a }, { pipeline: 55, stage: "compute", code: b }]);
  const magic = "REPLACE 1\n";
  assert.equal(new TextDecoder().decode(bytes.subarray(0, magic.length)), magic);
  const length = new DataView(bytes.buffer).getUint32(magic.length, true);
  const manifest = JSON.parse(new TextDecoder().decode(bytes.subarray(magic.length + 4, magic.length + 4 + length)));
  assert.deepEqual(manifest.replacements, [{ pipeline: 48, stage: "fragment", payload: [0, 4] }, { pipeline: 55, stage: "compute", payload: [4, 8] }]);
  const base = magic.length + 4 + length;
  assert.deepEqual([...bytes.subarray(base, base + 4)], [...a]);
  assert.deepEqual([...bytes.subarray(base + 4)], [...b]);
});

test("targets come back with the pixels of the ones that differ", () => {
  const pixels = new Uint8Array(16).fill(255);
  const r = parseReplayedTargets(targetsFile({
    format: "gpu-inspector-replayed-targets", device: "A GPU", problems: [],
    targets: [target({ differingTexels: 3, maxByteDelta: 200, payload: [0, 16] }), target({ image: 22, aspect: "depth", attachment: 1 })],
  }, pixels));
  assert.equal(r.device, "A GPU");
  assert.equal(r.targets[0].pixels.byteLength, 16);
  assert.equal(r.targets[1].pixels, null);
  assert.deepEqual(changedTargets(r).map((t) => t.image), [16]);
  assert.match(replayedTargetsSummary(r, [48]), /changed 1 of 2 render targets, the most 75\.0%/);
  // Truncated, or something else entirely: said, not guessed at.
  assert.throws(() => parseReplayedTargets(new Uint8Array([1, 2, 3])), /Not the render targets/);
  const short = targetsFile({ format: "gpu-inspector-replayed-targets", targets: [target({ differingTexels: 1, payload: [0, 999] })] });
  assert.equal(parseReplayedTargets(short).targets[0].pixels, null);
});

test("an edit nothing depends on, and an edit the driver refused, are different answers", () => {
  const same = parseReplayedTargets(targetsFile({ format: "gpu-inspector-replayed-targets", device: "", problems: [], targets: [target({}), target({ image: 22 })] }));
  assert.match(replayedTargetsSummary(same, [48]), /all 2 render targets are as they were/);

  // A refused pipeline is left out of the replay, so its draws draw nothing and the target differs
  // — by the draws missing, not by what the edit computes. That has to be what is said.
  const refused = parseReplayedTargets(targetsFile({
    format: "gpu-inspector-replayed-targets", device: "", targets: [target({ differingTexels: 4 })],
    problems: ["pipeline 48: vkCreateGraphicsPipelines failed (VK_ERROR_UNKNOWN)", "pipeline 480: something about another pipeline", "command 9: left out"],
  }));
  assert.deepEqual(replacementProblems(refused, [48]), ["pipeline 48: vkCreateGraphicsPipelines failed (VK_ERROR_UNKNOWN)"]);
  assert.match(replayedTargetsSummary(refused, [48]), /was not accepted: pipeline 48/);
});
