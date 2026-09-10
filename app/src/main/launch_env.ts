// Starting an application with a capture library in it, without Electron: where the Vulkan layer
// and the Khronos validation layer are, the environment that enables them, free ports, and ending a
// process tree. The app's sessions (main.ts) and the MCP server's live sessions launch this way.
import { execFile, type ChildProcess } from "node:child_process";
import fs from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";

export const LAYER_NAME = "VK_LAYER_INSPECTOR_capture";
export const VALIDATION_LAYER_NAME = "VK_LAYER_KHRONOS_validation";
export const DEFAULT_PORT = 47531;

/**
 * The directory holding the layer's manifest: INSPECTOR_LAYER_DIR, else a build tree under one of
 * `roots` (Release, RelWithDebInfo, Debug, then build/bin itself), else one of `packaged`, the
 * layer directories of installed apps.
 */
export function findLayerDir(roots: string[], packaged: string[] = []): string | null {
  if (process.env.INSPECTOR_LAYER_DIR) return process.env.INSPECTOR_LAYER_DIR;
  const candidates: string[] = [];
  for (const root of roots) {
    const bin = path.join(root, "build", "bin");
    candidates.push(path.join(bin, "Release"), path.join(bin, "RelWithDebInfo"), path.join(bin, "Debug"), bin);
  }
  candidates.push(...packaged);
  for (const dir of candidates) {
    if (fs.existsSync(path.join(dir, `${LAYER_NAME}.json`))) return dir;
  }
  return null;
}

/**
 * The directory holding the Khronos validation layer's manifest: the Vulkan SDK (VULKAN_SDK, or
 * the default install locations) or a distribution's layer directory. Needed because the launch
 * sets VK_LAYER_PATH, which replaces the loader's own explicit-layer search.
 */
export function findValidationLayerDir(): string | null {
  const manifest = "VkLayer_khronos_validation.json";
  const candidates: string[] = [];
  const sdk = process.env.VULKAN_SDK;
  if (sdk) candidates.push(path.join(sdk, "Bin"), path.join(sdk, "share", "vulkan", "explicit_layer.d"), path.join(sdk, "etc", "vulkan", "explicit_layer.d"));
  if (process.platform === "win32") {
    // Installed SDKs without VULKAN_SDK in this process's environment: newest first.
    for (const root of ["C:\\VulkanSDK", path.join(os.homedir(), "VulkanSDK")]) {
      try {
        const versions = fs.readdirSync(root).filter((v) => /^\d/.test(v)).sort().reverse();
        for (const v of versions) candidates.push(path.join(root, v, "Bin"));
      } catch {
        // no SDK there
      }
    }
  } else {
    candidates.push("/usr/share/vulkan/explicit_layer.d", "/usr/local/share/vulkan/explicit_layer.d", "/etc/vulkan/explicit_layer.d",
      path.join(os.homedir(), ".local", "share", "vulkan", "explicit_layer.d"));
  }
  for (const c of candidates) if (fs.existsSync(path.join(c, manifest))) return c;
  return null;
}

export interface VulkanLayerOptions {
  layerDir: string;
  /** The Khronos validation layer's directory, to enable it beside ours; null leaves it off. */
  validationDir: string | null;
  port: number;
  log: boolean;
  recordAlways: boolean;
  stacktraces: boolean;
  /** "Validation layer" was asked for: its settings apply even when no validation layer was found. */
  validation: boolean;
  syncValidation: boolean;
  /** Also append the layer's log to this file (a GUI application has no usable stderr). */
  logFile?: string;
}

/** The variables that load the layer into a process started with them, on top of its environment. */
export function vulkanLayerEnvironment(o: VulkanLayerOptions): NodeJS.ProcessEnv {
  // With "Validation layer" the Khronos validation layer is enabled too; its messages reach the
  // inspector's debug-utils messenger (layer/src/validation.cpp).
  const layers = [LAYER_NAME, ...(o.validationDir ? [VALIDATION_LAYER_NAME] : [])];
  const layerPaths = [o.layerDir, ...(o.validationDir ? [o.validationDir] : [])];
  return {
    VK_ADD_LAYER_PATH: layerPaths.join(path.delimiter),
    VK_LOADER_LAYERS_ENABLE: layers.join(","),
    // Older loaders:
    VK_LAYER_PATH: [...layerPaths, ...(process.env.VK_LAYER_PATH ? [process.env.VK_LAYER_PATH] : [])].join(path.delimiter),
    VK_INSTANCE_LAYERS: [...layers, ...(process.env.VK_INSTANCE_LAYERS ? [process.env.VK_INSTANCE_LAYERS] : [])].join(path.delimiter),
    VKINSP_PORT: String(o.port),
    VKINSP_LOG: o.log ? "1" : "0",
    ...(o.logFile ? { VKINSP_LOG_FILE: o.logFile } : {}),
    VKINSP_RECORD_ALWAYS: o.recordAlways ? "1" : "0",
    VKINSP_STACKTRACES: o.stacktraces ? "1" : "0",
    // The validation layer stops reporting a message after a few repeats (its
    // duplicate_message_limit, 10 by default); the inspector's layer counts repeats itself and
    // attaches a message to the captured command it fired on, which needs every occurrence.
    ...(o.validation && !process.env.VK_LAYER_DUPLICATE_MESSAGE_LIMIT ? { VK_LAYER_DUPLICATE_MESSAGE_LIMIT: "0" } : {}),
    // Synchronization validation: the settings-file name for current layers, the enable list for older ones.
    ...(o.validation && o.syncValidation ? { VK_LAYER_VALIDATE_SYNC: "true", VK_LAYER_ENABLES: "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT" } : {}),
  };
}

/** "KEY=VALUE" lines -> environment entries. Blank lines and lines starting with # are ignored. */
export function parseEnvLines(text: string): Record<string, string> {
  const out: Record<string, string> = {};
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || line.startsWith("#")) continue;
    const eq = line.indexOf("=");
    if (eq <= 0) continue;
    out[line.substring(0, eq).trim()] = line.substring(eq + 1);
  }
  return out;
}

/** A command line split into arguments, with single or double quotes grouping. */
export function splitArgs(s: string): string[] {
  const out: string[] = [];
  const re = /"([^"]*)"|'([^']*)'|(\S+)/g;
  let m: RegExpExecArray | null;
  while ((m = re.exec(s))) out.push(m[1] ?? m[2] ?? m[3]);
  return out;
}

export function portFree(port: number): Promise<boolean> {
  return new Promise((resolve) => {
    const srv = net.createServer();
    srv.once("error", () => resolve(false));
    srv.listen({ port, host: "127.0.0.1", exclusive: true }, () => srv.close(() => resolve(true)));
  });
}

/**
 * The requested port, or the next free one after it when it is taken: by `taken` (another session,
 * so several launches of the same configuration run side by side) or by anything else on the
 * machine, such as a previous instance of the target that has not finished exiting.
 */
export async function findFreePort(start: number, taken: (port: number) => boolean = () => false): Promise<number> {
  for (let port = start; port < start + 100 && port < 65536; port++) {
    if (taken(port)) continue;
    if (await portFree(port)) return port;
  }
  return start;
}

/** Terminates a process and, on Windows, everything it spawned (Unity's crash handler, launchers). */
export function terminate(proc: ChildProcess): void {
  if (process.platform === "win32" && proc.pid) {
    execFile("taskkill", ["/PID", String(proc.pid), "/T", "/F"], () => {
      // If taskkill is unavailable or the process is already gone, fall back to a plain kill.
      try { proc.kill(); } catch { /* already gone */ }
    });
    return;
  }
  proc.kill();
}
