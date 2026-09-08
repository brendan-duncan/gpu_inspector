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

const candidates = process.env.INSPECTOR_LAYER_DIR
  ? [process.env.INSPECTOR_LAYER_DIR]
  : ["Release", "RelWithDebInfo", ""].map((c) => path.join(root, "build", "bin", c));
const src = candidates.find((d) => fs.existsSync(path.join(d, manifest)) && fs.existsSync(path.join(d, library)));
if (!src) {
  console.error(`layer not found (${library} + ${manifest}) in:\n  ${candidates.join("\n  ")}\nBuild it first (see README.md) or set INSPECTOR_LAYER_DIR.`);
  process.exit(1);
}

const dst = path.join(appDir, "dist", "layer");
fs.rmSync(dst, { recursive: true, force: true });
fs.mkdirSync(dst, { recursive: true });
for (const f of [library, manifest]) fs.copyFileSync(path.join(src, f), path.join(dst, f));
console.log(`staged layer from ${src} -> ${dst}`);

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
