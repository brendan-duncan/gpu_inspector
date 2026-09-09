// Implicit registration of the capture layer, for applications the inspector does not launch
// itself (an editor, a game started by its launcher): the loader then loads the layer into any
// process started with VKINSP_ENABLE=1 (the manifest's enable_environment), and the inspector
// waits for it to connect. Windows keeps implicit layers in the registry (per user under HKCU,
// no elevation needed); Linux in $XDG_DATA_HOME/vulkan/implicit_layer.d as a manifest file.
import { execFile } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

export const LAYER_NAME = "VK_LAYER_INSPECTOR_capture";
const REGISTRY_KEY = "HKCU\\SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers";

export interface ImplicitLayerStatus {
  registered: boolean;
  /** The manifest the registration points at (or would). */
  manifest: string;
  error?: string;
}

function reg(args: string[]): Promise<{ ok: boolean; out: string }> {
  return new Promise((resolve) => {
    execFile("reg.exe", args, { windowsHide: true }, (err, stdout, stderr) => resolve({ ok: !err, out: `${stdout}${stderr}` }));
  });
}

function linuxManifest(): string {
  const dataHome = process.env.XDG_DATA_HOME || path.join(os.homedir(), ".local", "share");
  return path.join(dataHome, "vulkan", "implicit_layer.d", `${LAYER_NAME}.json`);
}

/** Whether the layer in `layerDir` is registered as an implicit layer for this user. */
export async function implicitLayerStatus(layerDir: string): Promise<ImplicitLayerStatus> {
  const manifest = path.join(layerDir, `${LAYER_NAME}.json`);
  if (process.platform === "win32") {
    const r = await reg(["query", REGISTRY_KEY, "/v", manifest]);
    return { registered: r.ok && r.out.toLowerCase().includes(manifest.toLowerCase()), manifest };
  }
  const target = linuxManifest();
  try {
    const text = fs.readFileSync(target, "utf8");
    return { registered: text.includes(layerDir), manifest: target };
  } catch {
    return { registered: false, manifest: target };
  }
}

/** Registers (or unregisters) the layer in `layerDir` as an implicit layer for this user. */
export async function setImplicitLayer(layerDir: string, on: boolean): Promise<ImplicitLayerStatus> {
  const manifest = path.join(layerDir, `${LAYER_NAME}.json`);
  if (process.platform === "win32") {
    const r = on
      ? await reg(["add", REGISTRY_KEY, "/v", manifest, "/t", "REG_DWORD", "/d", "0", "/f"])
      : await reg(["delete", REGISTRY_KEY, "/v", manifest, "/f"]);
    const status = await implicitLayerStatus(layerDir);
    if (!r.ok && status.registered === !on) status.error = r.out.trim() || "reg.exe failed";
    return status;
  }
  const target = linuxManifest();
  try {
    if (on) {
      // A copy of the manifest with the library's absolute path, since the loader resolves a
      // relative library_path against the manifest's own directory.
      const source = JSON.parse(fs.readFileSync(manifest, "utf8")) as { layer?: { library_path?: string } };
      const lib = source.layer?.library_path ?? `lib${LAYER_NAME}.so`;
      if (source.layer) source.layer.library_path = path.isAbsolute(lib) ? lib : path.join(layerDir, lib);
      fs.mkdirSync(path.dirname(target), { recursive: true });
      fs.writeFileSync(target, JSON.stringify(source, null, 4));
    } else if (fs.existsSync(target)) {
      fs.unlinkSync(target);
    }
    return await implicitLayerStatus(layerDir);
  } catch (e) {
    return { registered: false, manifest: target, error: e instanceof Error ? e.message : String(e) };
  }
}
