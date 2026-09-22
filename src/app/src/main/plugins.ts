// Plugins: graphics APIs added to the inspector from outside the app (docs/PLUGINS.md).
//
// A plugin is a directory holding a plugin.json, which names
//   - a backend module (JavaScript, ES module), which the renderer and the MCP server import and whose
//     `activate` returns the Backend that says how the API's captures read (renderer/backend.ts);
//   - per platform, how the plugin's capture library gets into an application the inspector launches:
//     libraries for the launcher to inject (Windows), libraries to preload (Linux, macOS), and the
//     environment the library reads its settings from.
// The capture library speaks the protocol every built-in one does (shared/protocol.ts) over the same
// TCP framing, so once it is in the process the session is an ordinary session.
//
// This module only finds and reads plugins; main.ts serves their files to the renderer and puts their
// libraries into launched processes, and the MCP server (src/mcp/) imports their backends itself.
// Node only (no Electron), so the MCP server can use it.
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { PLUGIN_SDK_VERSION, type PluginInfo } from "../shared/protocol.js";

export { PLUGIN_SDK_VERSION };

/** How a plugin's capture library gets into an application started on one platform. */
export interface PluginPlatformCapture {
  /**
   * Windows: libraries the launcher (dxinsp_launch.exe) injects into the target as it starts, before
   * its first instruction runs, the way it injects the D3D12 library. Each is loaded, then its
   * `GpuInspectorInitialize` export (or `DxinspInitialize`) is called with the settings. Relative
   * to the plugin's directory.
   */
  inject?: string[];
  /** Linux and macOS: libraries to preload (LD_PRELOAD, DYLD_INSERT_LIBRARIES), relative to the plugin's directory. */
  preload?: string[];
  /**
   * Environment variables the library reads its settings from. Values may use ${port} (the session's
   * port), ${log} ("1" or "0"), ${recordAlways}, ${stacktraces} and ${pluginDir}.
   */
  env?: Record<string, string>;
}

/**
 * How a plugin's capture library gets into an Android application: as an OpenGL ES layer, which
 * Android (10 and later) loads into a debuggable application from its data directory.
 */
export interface PluginAndroidCapture {
  /**
   * The OpenGL ES layer library, relative to the plugin's directory, with ${abi} for the device's ABI
   * ("android/lib/${abi}/libglesinsp_capture.so"). Its file name is what gpu_debug_layers_gles names.
   */
  glesLayer: string;
  /** The abstract socket the library listens on, which adb forwards the session's port to: ${port} and ${package} expanded. */
  socket: string;
  /** System properties (adb shell setprop) the library reads its settings from; values as for `env`. */
  properties?: Record<string, string>;
}

export interface PluginManifest {
  /** Unique, lower case: "gles". */
  id: string;
  /** "OpenGL ES". */
  name: string;
  version: string;
  /** The plugin contract it was written for (PLUGIN_SDK_VERSION). */
  sdk: number;
  /** The `api` its captures carry, which the backend is registered under. The plugin's id by default. */
  api?: string;
  description?: string;
  /** The backend module, relative to the plugin's directory. */
  backend?: string;
  /** How the capture library goes into a launched application, per platform (process.platform). */
  capture?: Partial<Record<"win32" | "linux" | "darwin", PluginPlatformCapture>> & { android?: PluginAndroidCapture };
}

export interface Plugin {
  manifest: PluginManifest;
  /** The directory plugin.json is in. */
  dir: string;
  /** The backend module's absolute path, when the manifest names one and it exists. */
  backend: string | null;
  /** Why the plugin is not usable; null when it is. A plugin with an error is listed but not loaded. */
  error: string | null;
}

/** The user's own plugins directory: beside the app's settings (Electron's userData), or $GPU_INSPECTOR_HOME/plugins. */
export function userPluginDir(): string {
  if (process.env.GPU_INSPECTOR_HOME) return path.join(process.env.GPU_INSPECTOR_HOME, "plugins");
  const home = os.homedir();
  if (process.platform === "win32") return path.join(process.env.APPDATA ?? path.join(home, "AppData", "Roaming"), "gpu-inspector", "plugins");
  if (process.platform === "darwin") return path.join(home, "Library", "Application Support", "gpu-inspector", "plugins");
  return path.join(process.env.XDG_CONFIG_HOME ?? path.join(home, ".config"), "gpu-inspector", "plugins");
}

/**
 * Where plugins are looked for, first match of an id winning: GPU_INSPECTOR_PLUGINS (a path list of
 * plugin directories or directories of them), the user's plugins directory, then each checkout's
 * build/plugins (what the build writes, src/plugins/), then the installed app's resources/plugins.
 */
export function pluginSearchDirs(checkoutRoots: string[], packaged: string[] = []): string[] {
  const dirs: string[] = [];
  for (const d of (process.env.GPU_INSPECTOR_PLUGINS ?? "").split(path.delimiter)) if (d.trim()) dirs.push(d.trim());
  dirs.push(userPluginDir());
  for (const root of checkoutRoots) dirs.push(path.join(root, "build", "plugins"));
  dirs.push(...packaged);
  return dirs;
}

function readManifest(dir: string): Plugin | null {
  const file = path.join(dir, "plugin.json");
  if (!fs.existsSync(file)) return null;
  let manifest: PluginManifest;
  try {
    manifest = JSON.parse(fs.readFileSync(file, "utf8")) as PluginManifest;
  } catch (e) {
    const id = path.basename(dir);
    return { manifest: { id, name: id, version: "", sdk: 0 }, dir, backend: null, error: `plugin.json does not parse: ${(e as Error).message}` };
  }
  const plugin: Plugin = { manifest, dir, backend: null, error: null };
  if (typeof manifest.id !== "string" || !/^[a-z][a-z0-9_-]*$/.test(manifest.id)) {
    plugin.error = "plugin.json needs an id: lower case letters, digits, - and _";
    manifest.id = typeof manifest.id === "string" && manifest.id ? manifest.id : path.basename(dir);
    return plugin;
  }
  manifest.name ||= manifest.id;
  manifest.version ||= "";
  manifest.api ||= manifest.id;
  if (typeof manifest.sdk !== "number" || manifest.sdk > PLUGIN_SDK_VERSION) {
    plugin.error = `written for plugin SDK ${String(manifest.sdk)}; this GPU Inspector implements ${PLUGIN_SDK_VERSION}`;
    return plugin;
  }
  if (manifest.backend) {
    const backend = path.resolve(dir, manifest.backend);
    if (!isInside(dir, backend)) plugin.error = "the backend module is outside the plugin's directory";
    else if (!fs.existsSync(backend)) plugin.error = `the backend module ${manifest.backend} is missing (is the plugin built?)`;
    else plugin.backend = backend;
  }
  return plugin;
}

/** Whether `file` is `dir` or inside it. */
export function isInside(dir: string, file: string): boolean {
  const rel = path.relative(path.resolve(dir), path.resolve(file));
  return rel === "" || (!rel.startsWith("..") && !path.isAbsolute(rel));
}

/**
 * Every plugin in `dirs`: a directory holding plugin.json is a plugin, and so is each directory
 * directly inside one that does not. The first of each id wins, so a plugin in the user's directory
 * overrides the same plugin shipped with the app.
 */
export function findPlugins(dirs: string[]): Plugin[] {
  const found = new Map<string, Plugin>();
  const consider = (dir: string): void => {
    const p = readManifest(dir);
    if (p && !found.has(p.manifest.id)) found.set(p.manifest.id, p);
  };
  for (const dir of dirs) {
    let entries: fs.Dirent[];
    try {
      if (!fs.statSync(dir).isDirectory()) continue;
      if (fs.existsSync(path.join(dir, "plugin.json"))) {
        consider(dir);
        continue;
      }
      entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
      continue;
    }
    for (const e of entries) if (e.isDirectory()) consider(path.join(dir, e.name));
  }
  return [...found.values()];
}

export interface CaptureSettings {
  port: number;
  log: boolean;
  recordAlways: boolean;
  stacktraces: boolean;
}

function expand(value: string, plugin: Plugin, s: CaptureSettings): string {
  return value
    .replace(/\$\{port\}/g, String(s.port))
    .replace(/\$\{log\}/g, s.log ? "1" : "0")
    .replace(/\$\{recordAlways\}/g, s.recordAlways ? "1" : "0")
    .replace(/\$\{stacktraces\}/g, s.stacktraces ? "1" : "0")
    .replace(/\$\{pluginDir\}/g, plugin.dir);
}

/** What goes into a process started on this platform for one plugin: libraries and variables. */
export interface PluginLaunch {
  plugin: Plugin;
  /** Windows: libraries for the launcher to inject, absolute. */
  inject: string[];
  /** Linux / macOS: libraries to preload, absolute. */
  preload: string[];
  env: Record<string, string>;
  /** Libraries the manifest names that are not there (the plugin is then skipped, and the log says so). */
  missing: string[];
}

/** How each usable plugin with a capture library for this platform goes into a launched process. */
export function pluginLaunches(plugins: Plugin[], settings: CaptureSettings, platform: string = process.platform): PluginLaunch[] {
  const out: PluginLaunch[] = [];
  for (const plugin of plugins) {
    if (plugin.error) continue;
    const c = plugin.manifest.capture?.[platform as "win32" | "linux" | "darwin"];
    if (!c) continue;
    const resolve = (files: string[] | undefined): string[] => (files ?? []).map((f) => path.resolve(plugin.dir, f));
    const inject = platform === "win32" ? resolve(c.inject) : [];
    const preload = platform === "win32" ? [] : resolve(c.preload);
    const env: Record<string, string> = {};
    for (const [k, v] of Object.entries(c.env ?? {})) env[k] = expand(String(v), plugin, settings);
    const missing = [...inject, ...preload].filter((f) => !fs.existsSync(f));
    if (!inject.length && !preload.length && !Object.keys(env).length) continue;
    out.push({ plugin, inject, preload, env, missing });
  }
  return out;
}

/**
 * Linux and macOS: the plugins' libraries preloaded into a launch, in front of whatever the
 * environment already preloads (`variable` is LD_PRELOAD or DYLD_INSERT_LIBRARIES), and their
 * settings. Returns the lines for the session's log; a plugin whose library is not built is left out.
 */
export function applyPreloads(env: NodeJS.ProcessEnv, launches: PluginLaunch[], variable: "LD_PRELOAD" | "DYLD_INSERT_LIBRARIES"): string[] {
  const notes: string[] = [];
  const preload: string[] = [];
  for (const p of launches) {
    if (p.missing.length) {
      notes.push(`${p.plugin.manifest.name} capture library not found (${p.missing.join(", ")}): build the plugin`);
      continue;
    }
    if (!p.preload.length) continue;
    Object.assign(env, p.env);
    preload.push(...p.preload);
    notes.push(`${p.plugin.manifest.name} capture library: ${p.preload.join(", ")} (plugin ${p.plugin.dir})`);
  }
  if (preload.length) env[variable] = [...preload, ...(env[variable] ? [env[variable]] : [])].join(":");
  return notes;
}

/** What an Android launch of a plugin's API puts on the device: the layer library for the device's ABI, its socket and its settings. */
export interface PluginAndroidLaunch {
  plugin: Plugin;
  /** The library for the first of the device's ABIs the plugin has one for; null when it has none. */
  library: string | null;
  abi: string;
  /** The layer's name for gpu_debug_layers_gles: the library's file name. */
  layerName: string;
  socket: string;
  properties: Record<string, string>;
  /** Why the plugin cannot go onto the device, for the session's log; null when it can. */
  error: string | null;
}

/** The usable plugins with an Android capture library: the APIs an Android launch can choose besides Vulkan. */
export function androidPlugins(plugins: Plugin[]): Plugin[] {
  return plugins.filter((p) => !p.error && p.manifest.capture?.android?.glesLayer && p.manifest.capture.android.socket);
}

/** How plugin `plugin` goes into package `pkg` on a device with ABIs `abilist` (its preferred first). */
export function pluginAndroidLaunch(plugin: Plugin, abilist: string[], pkg: string, settings: CaptureSettings): PluginAndroidLaunch {
  const a = plugin.manifest.capture!.android!;
  const expandAndroid = (v: string): string => expand(v, plugin, settings).replace(/\$\{package\}/g, pkg);
  const properties: Record<string, string> = {};
  for (const [k, v] of Object.entries(a.properties ?? {})) properties[k] = expandAndroid(String(v));
  const socket = expandAndroid(a.socket);
  const layerName = path.basename(a.glesLayer);
  for (const abi of abilist) {
    const library = path.resolve(plugin.dir, a.glesLayer.replace(/\$\{abi\}/g, abi));
    if (isInside(plugin.dir, library) && fs.existsSync(library)) return { plugin, library, abi, layerName, socket, properties, error: null };
  }
  return {
    plugin, library: null, abi: "", layerName, socket, properties,
    error: `${plugin.manifest.name} has no Android library for ${abilist.join(", ") || "the device"}: build it with tools/build_android.py`,
  };
}

/** The renderer's view of a plugin, its backend addressed through the scheme main.ts serves plugin files on. */
export function pluginInfo(p: Plugin): PluginInfo {
  const rel = p.backend ? path.relative(p.dir, p.backend).split(path.sep).map(encodeURIComponent).join("/") : null;
  return {
    id: p.manifest.id, name: p.manifest.name, version: p.manifest.version, api: p.manifest.api ?? p.manifest.id, dir: p.dir,
    backendUrl: rel ? `${PLUGIN_SCHEME}://${p.manifest.id}/${rel}` : null,
    error: p.error,
  };
}

/** The URL scheme the renderer loads plugin modules through (main.ts registers it). */
export const PLUGIN_SCHEME = "gpuinsp-plugin";
