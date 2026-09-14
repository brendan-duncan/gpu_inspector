// Implicit registration of the capture layer, for applications the inspector does not launch
// itself (an editor, a game started by its launcher): the loader then loads the layer into any
// process started with VKINSP_ENABLE=1 (the manifest's enable_environment), and the inspector
// waits for it to connect. Windows keeps implicit layers in the registry (per user under HKCU,
// no elevation needed); Linux in $XDG_DATA_HOME/vulkan/implicit_layer.d as a manifest file.
import { execFile } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import type { UserEnvironmentStatus } from "../shared/protocol.js";

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

// ---------------------------------------------------------------------------------------------
// The account's environment: VKINSP_ENABLE and VKINSP_PORT for every process the user starts, for
// an application behind a launcher that cannot be given variables of its own. Windows keeps them
// under HKCU\Environment, set through .NET so running programs are told (WM_SETTINGCHANGE) and a
// launcher started afterwards sees them; Linux in ~/.config/environment.d, which a graphical
// session reads when the user logs in.

const ENV_NAMES = ["VKINSP_ENABLE", "VKINSP_PORT"] as const;

function linuxEnvironmentFile(): string {
  const configHome = process.env.XDG_CONFIG_HOME || path.join(os.homedir(), ".config");
  return path.join(configHome, "environment.d", "gpu-inspector.conf");
}

function powershell(script: string): Promise<{ ok: boolean; out: string }> {
  return new Promise((resolve) => {
    execFile("powershell.exe", ["-NoProfile", "-NonInteractive", "-Command", script], { windowsHide: true }, (err, stdout, stderr) =>
      resolve({ ok: !err, out: `${stdout}${stderr}` }));
  });
}

export async function userEnvironmentStatus(): Promise<UserEnvironmentStatus> {
  if (process.platform === "win32") {
    const r = await powershell(ENV_NAMES.map((n) => `[Environment]::GetEnvironmentVariable('${n}', 'User')`).join("; "));
    const [enable, port] = r.out.split(/\r?\n/);
    return { set: enable?.trim() === "1", port: Number(port?.trim()) || null, location: "HKCU\\Environment", ...(r.ok ? {} : { error: r.out.trim() }) };
  }
  const file = linuxEnvironmentFile();
  try {
    const values = new Map(fs.readFileSync(file, "utf8").split("\n").map((l) => l.split("=")).filter((p) => p.length === 2).map(([k, v]) => [k.trim(), v.trim()]));
    return { set: values.get("VKINSP_ENABLE") === "1", port: Number(values.get("VKINSP_PORT")) || null, location: file };
  } catch {
    return { set: false, port: null, location: file };
  }
}

/** Sets VKINSP_ENABLE=1 and VKINSP_PORT for the account, or removes both (`port` null). */
export async function setUserEnvironment(port: number | null): Promise<UserEnvironmentStatus> {
  if (port !== null && (!Number.isInteger(port) || port <= 0 || port > 65535)) {
    return { ...(await userEnvironmentStatus()), error: `not a port: ${port}` };
  }
  if (process.platform === "win32") {
    const value = (n: string): string => (port === null ? "$null" : n === "VKINSP_ENABLE" ? "'1'" : `'${port}'`);
    const r = await powershell(ENV_NAMES.map((n) => `[Environment]::SetEnvironmentVariable('${n}', ${value(n)}, 'User')`).join("; "));
    const status = await userEnvironmentStatus();
    if (!r.ok) status.error = r.out.trim() || "powershell failed";
    return status;
  }
  const file = linuxEnvironmentFile();
  try {
    if (port === null) {
      if (fs.existsSync(file)) fs.unlinkSync(file);
    } else {
      fs.mkdirSync(path.dirname(file), { recursive: true });
      fs.writeFileSync(file, `# Written by GPU Inspector: loads its implicit capture layer into Vulkan applications.\nVKINSP_ENABLE=1\nVKINSP_PORT=${port}\n`);
    }
    return { ...(await userEnvironmentStatus()), needsLogin: true };
  } catch (e) {
    return { ...(await userEnvironmentStatus()), error: e instanceof Error ? e.message : String(e) };
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
