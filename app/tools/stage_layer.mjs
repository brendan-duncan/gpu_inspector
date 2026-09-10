// Copies the built Vulkan layer (library + manifest) into dist/layer for packaging.
// electron-builder ships dist/layer as resources/layer, which is where a packaged app looks for
// the layer (findLayerDir in src/main/main.ts). Set INSPECTOR_LAYER_DIR to use a build
// directory other than <repo>/build/bin[/Release].
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const appDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const root = path.resolve(appDir, "..");
const manifest = "VK_LAYER_INSPECTOR_capture.json";
const library = process.platform === "win32" ? "VkLayer_inspector_capture.dll" : "libVkLayer_inspector_capture.so";

const dst = path.join(appDir, "dist", "layer");
fs.rmSync(dst, { recursive: true, force: true });
// Always present, even when empty: electron-builder's extraResources entry for it is
// unconditional, and the Android layer below goes inside it.
fs.mkdirSync(dst, { recursive: true });

// macOS ships the Metal capture library instead of the Vulkan layer, which does not build for
// Apple targets (layer/CMakeLists.txt covers UNIX AND NOT APPLE). It is a plain dylib injected
// with DYLD_INSERT_LIBRARIES rather than a layer with a manifest, so there is nothing beside it
// to copy. findCaptureLibrary in app/src/main/metal.ts looks for it in resources/layer.
if (process.platform === "darwin") {
  const metalLib = "libmtlinsp_capture.dylib";
  const metalCandidates = process.env.INSPECTOR_METAL_LIB
    ? [process.env.INSPECTOR_METAL_LIB]
    : ["Release", "Debug", ""].map((c) => path.join(root, "build", "bin", c, metalLib));
  const metalSrc = metalCandidates.find((f) => fs.existsSync(f));
  if (!metalSrc) {
    console.error(`Metal capture library not found (${metalLib}) in:\n  ${metalCandidates.join("\n  ")}\n`
      + `Build it first (cmake -S . -B build && cmake --build build) or set INSPECTOR_METAL_LIB.`);
    process.exit(1);
  }
  fs.copyFileSync(metalSrc, path.join(dst, metalLib));
  console.log(`staged Metal capture library from ${metalSrc} -> ${dst}`);
} else {
  const candidates = process.env.INSPECTOR_LAYER_DIR
    ? [process.env.INSPECTOR_LAYER_DIR]
    : ["Release", "RelWithDebInfo", ""].map((c) => path.join(root, "build", "bin", c));
  const src = candidates.find((d) => fs.existsSync(path.join(d, manifest)) && fs.existsSync(path.join(d, library)));
  if (!src) {
    console.error(`layer not found (${library} + ${manifest}) in:\n  ${candidates.join("\n  ")}\nBuild it first (see README.md) or set INSPECTOR_LAYER_DIR.`);
    process.exit(1);
  }
  for (const f of [library, manifest]) fs.copyFileSync(path.join(src, f), path.join(dst, f));
  console.log(`staged layer from ${src} -> ${dst}`);
}

// Android: the layer libraries and the layer APK from tools/build_android.py, when built. The
// app looks for them in resources/layer/android (findAndroidLayerFiles in src/main/main.ts).
const androidSrc = process.env.INSPECTOR_ANDROID_LAYER_DIR ?? path.join(root, "build", "android");
if (fs.existsSync(path.join(androidSrc, "lib"))) {
  const androidDst = path.join(dst, "android");
  fs.cpSync(path.join(androidSrc, "lib"), path.join(androidDst, "lib"), { recursive: true });
  for (const f of ["gpu_inspector_layer.apk", "gpu_inspector_layer.apk.json"]) {
    if (fs.existsSync(path.join(androidSrc, f))) fs.copyFileSync(path.join(androidSrc, f), path.join(androidDst, f));
  }
  console.log(`staged Android layer from ${androidSrc} -> ${androidDst}`);
} else {
  console.log(`no Android layer in ${androidSrc} (build it with tools/build_android.py); the package will not support Android targets`);
}
