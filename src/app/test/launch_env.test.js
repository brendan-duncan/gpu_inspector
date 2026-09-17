// The environment that loads the layer into a launched process (src/main/launch_env.ts), and in
// particular the validation settings: sync and GPU-assisted validation are two settings that share
// one legacy variable, so it is easy for one to quietly overwrite the other.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname, delimiter } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const out = join(mkdtempSync(join(tmpdir(), "launchenv-")), "launch_env.mjs");
buildSync({
  entryPoints: [join(here, "..", "src", "main", "launch_env.ts")],
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const { vulkanLayerEnvironment } = await import(pathToFileURL(out).href);

const base = {
  layerDir: "/layers", validationDir: "/validation", port: 47531, log: true,
  recordAlways: false, stacktraces: true, validation: false, syncValidation: false,
};
const env = (extra) => vulkanLayerEnvironment({ ...base, ...extra });

test("without the validation layer neither validation setting is passed", () => {
  const e = env({ syncValidation: true, gpuValidation: true });
  assert.equal(e.VK_LAYER_ENABLES, undefined, "the settings mean nothing without the layer they configure");
  assert.equal(e.VK_LAYER_VALIDATE_SYNC, undefined);
  assert.equal(e.VK_LAYER_VALIDATE_GPU_BASED, undefined);
});

test("sync validation alone sets its own setting and nothing else", () => {
  const e = env({ validation: true, syncValidation: true });
  assert.equal(e.VK_LAYER_VALIDATE_SYNC, "true");
  assert.equal(e.VK_LAYER_VALIDATE_GPU_BASED, undefined);
  assert.equal(e.VK_LAYER_ENABLES, "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT");
});

test("GPU validation alone sets its own setting and nothing else", () => {
  const e = env({ validation: true, gpuValidation: true });
  assert.equal(e.VK_LAYER_VALIDATE_GPU_BASED, "GPU_BASED_GPU_ASSISTED");
  assert.equal(e.VK_LAYER_VALIDATE_SYNC, undefined);
  assert.equal(e.VK_LAYER_ENABLES, "VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT");
});

test("both together are both asked for, rather than one replacing the other", () => {
  // The legacy list takes several separated by the platform's path separator. Building it by
  // assignment twice would leave whichever came last, and silently drop the other.
  const e = env({ validation: true, syncValidation: true, gpuValidation: true });
  assert.equal(e.VK_LAYER_VALIDATE_SYNC, "true");
  assert.equal(e.VK_LAYER_VALIDATE_GPU_BASED, "GPU_BASED_GPU_ASSISTED");
  assert.deepEqual(e.VK_LAYER_ENABLES.split(delimiter), [
    "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT",
    "VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT",
  ]);
});

test("neither asked for leaves the enable list unset", () => {
  assert.equal(env({ validation: true }).VK_LAYER_ENABLES, undefined);
});

test("the inspector's layer is always enabled, the validation layer only when it was found", () => {
  assert.match(env({}).VK_LOADER_LAYERS_ENABLE, /VK_LAYER_INSPECTOR_capture/);
  const without = vulkanLayerEnvironment({ ...base, validationDir: null });
  assert.doesNotMatch(without.VK_LOADER_LAYERS_ENABLE, /KHRONOS_validation/);
});
