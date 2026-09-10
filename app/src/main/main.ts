// Electron main process: windows, inspection sessions (target process launch and the TCP
// connection to its layer), and the IPC bridge to the renderer.
//
// A session is one inspected application. Sessions are created by launching an executable or by
// connecting to a running one, live here in the main process, and are displayed by exactly one
// window at a time: the main window by default, or a window of their own ("Open in New Window").
import electron from "electron";
import updater from "electron-updater";
import { spawn, execFile, type ChildProcess } from "node:child_process";
import net from "node:net";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { symbolizeFrames } from "./symbolize.js";
import { findShaderSources, forgetSourceIndex } from "./shader_sources.js";
import { findTool, shaderText } from "./shader_tools.js";
import { implicitLayerStatus, setImplicitLayer } from "./implicit_layer.js";
import { CAPTURE_LIBRARY, captureEnvironment, findCaptureLibrary, injectionBlockedReason, resolveExecutable } from "./metal.js";
import { AndroidTarget, disableLayer, findAdb, findAndroidLayer, listDevices, listPackages, type AndroidLayerFiles } from "./android.js";
import {
  THEMES,
  type AndroidDeviceList,
  type AppConfig, type ConnectionState, type LaunchConfig, type LaunchResult, type LayerMessage, type OpenFileOptions, type SaveFileOptions, type SessionInfo,
  type CompileShaderResult, type ShaderLanguage, type ShaderTextMode, type ShaderTextResult, type ThemeName, type UiRequest,
  type UpdateStatus, type StackFrame, type ImplicitLayerStatus,
} from "../shared/protocol.js";

const { app, BrowserWindow, ipcMain, dialog, nativeImage } = electron;
const { autoUpdater } = updater;
type BrowserWindow = electron.BrowserWindow;
type WebContents = electron.WebContents;

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const LAYER_NAME = "VK_LAYER_INSPECTOR_capture";
const VALIDATION_LAYER_NAME = "VK_LAYER_KHRONOS_validation";
const DEFAULT_PORT = 47531;
const MAX_LOG_LINES = 2000;
const LAUNCH_CONNECT_TIMEOUT_MS = 60000;
const ATTACH_CONNECT_TIMEOUT_MS = 5000;
/** How long a session waits for an application the implicit layer brings (see waitForApplication). */
const WAIT_CONNECT_TIMEOUT_MS = 30 * 60 * 1000;
const KILL_TIMEOUT_MS = 3000;

let mainWin: BrowserWindow | null = null;

// Command line: --launch=<exe> [--args="..."] [--port=N] [--screenshot=<png> --screenshot-delay=<ms>]
//               --launch-android=<package> --device=<serial> [--activity=<name>]
//               [--debug-select=<VkType>] [--debug-capture[=<frames>]] [--record-always]
//               [--debug-relaunch] [--debug-multi] [--debug-detach] [--debug-theme=<name>] [--debug-mouse=x,y[;x,y...]]
function cliOption(name: string): string | null {
  const prefix = `--${name}=`;
  const a = process.argv.find((x) => x.startsWith(prefix));
  return a ? a.substring(prefix.length) : null;
}

function cliFlag(name: string): boolean {
  return process.argv.includes(`--${name}`) || cliOption(name) !== null;
}

// ------------------------------------------------------------------------------------------
// Settings (recent launch configurations)

interface Settings {
  recents?: LaunchConfig[];
  /** Capture files saved or opened, most recent first. */
  recentCaptures?: string[];
  /** The last symbol directories a launch used (";"-separated), for stack traces of capture files. */
  symbolDirs?: string;
  /** The last source roots a launch used (";"-separated), for the shaders of capture files. */
  sourceRoots?: string;
  theme?: ThemeName;
}

const MAX_RECENTS = 12;

function loadRecentCaptures(): string[] {
  const list = loadSettings().recentCaptures ?? [];
  return list.filter((p) => typeof p === "string" && p);
}

/** Moves (or inserts) a capture file to the front of the recent captures and tells every window. */
function addRecentCapture(file: string): string[] {
  const list = loadRecentCaptures().filter((p) => p !== file);
  list.unshift(file);
  list.length = Math.min(list.length, MAX_RECENTS);
  const settings = loadSettings();
  settings.recentCaptures = list;
  saveSettings(settings);
  broadcast("inspector:recentCaptures", list);
  return list;
}

function removeRecentCapture(index: number): string[] {
  const list = loadRecentCaptures();
  list.splice(index, 1);
  const settings = loadSettings();
  settings.recentCaptures = list;
  saveSettings(settings);
  broadcast("inspector:recentCaptures", list);
  return list;
}

function normalizeLaunch(c: Partial<LaunchConfig>): LaunchConfig {
  return {
    target: c.target === "android" ? "android" : c.target === "implicit" ? "implicit" : "native",
    exe: c.exe ?? "",
    args: c.args ?? "",
    cwd: c.cwd ?? "",
    env: c.env ?? "",
    device: c.device ?? "",
    activity: c.activity ?? "",
    port: Number(c.port) || DEFAULT_PORT,
    log: c.log ?? true,
    recordAlways: c.recordAlways ?? false,
    validation: c.validation ?? false,
    syncValidation: c.syncValidation ?? false,
    symbolDirs: c.symbolDirs ?? "",
    sourceRoots: c.sourceRoots ?? "",
    stacktraces: c.stacktraces ?? true,
    capture: c.capture && (c.capture.mode === "frame" || c.capture.mode === "time")
      ? { mode: c.capture.mode, value: Math.max(0, Number(c.capture.value) || 0) }
      : { mode: "none", value: 0 },
  };
}

function loadRecents(): LaunchConfig[] {
  return (loadSettings().recents ?? []).map(normalizeLaunch);
}

function saveRecents(recents: LaunchConfig[]): void {
  const settings = loadSettings();
  settings.recents = recents;
  saveSettings(settings);
}

// Moves (or inserts) a configuration to the front of the recents list. Entries are identified
// by target, executable (or package + device) and arguments so relaunching with different
// options updates the existing entry.
function addRecent(config: LaunchConfig): LaunchConfig[] {
  const recents = loadRecents().filter((r) =>
    !(r.target === config.target && r.exe === config.exe && r.args === config.args && r.device === config.device));
  recents.unshift(config);
  recents.length = Math.min(recents.length, MAX_RECENTS);
  saveRecents(recents);
  return recents;
}

// "KEY=VALUE" lines -> environment entries. Blank lines and lines starting with # are ignored.
function parseEnvLines(text: string): Record<string, string> {
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

function settingsPath(): string {
  return path.join(app.getPath("userData"), "settings.json");
}

/** Copies settings saved by the app under its previous name (vulkan-inspector) on first run. */
function migrateSettings(): void {
  const current = settingsPath();
  if (fs.existsSync(current)) return;
  const previous = path.join(path.dirname(app.getPath("userData")), "vulkan-inspector", "settings.json");
  try {
    if (fs.existsSync(previous)) {
      fs.mkdirSync(path.dirname(current), { recursive: true });
      fs.copyFileSync(previous, current);
    }
  } catch {
    // start with defaults
  }
}

function loadSettings(): Settings {
  try {
    return JSON.parse(fs.readFileSync(settingsPath(), "utf8")) as Settings;
  } catch {
    return {};
  }
}

function saveSettings(settings: Settings): void {
  try {
    fs.mkdirSync(path.dirname(settingsPath()), { recursive: true });
    fs.writeFileSync(settingsPath(), JSON.stringify(settings, null, 2));
  } catch (e) {
    console.error("failed to save settings", e);
  }
}

// ------------------------------------------------------------------------------------------
// Layer location

/**
 * What to say when there is no layer directory. macOS builds are the UI only — the capture layer
 * does not build on Apple — so there the local targets (a program launched here, or the implicit
 * layer) are simply unavailable, and telling the user to go build one would send them nowhere.
 */
const NO_LAYER_ERROR = process.platform === "darwin"
  ? `capture library not found (${CAPTURE_LIBRARY}): build it first (see metal/README.md)`
  : "layer not found: build the layer first (see docs/ARCHITECTURE.md)";

function findLayerDir(): string | null {
  if (process.env.INSPECTOR_LAYER_DIR) return process.env.INSPECTOR_LAYER_DIR;
  const root = path.resolve(__dirname, "..", "..", "..");
  const candidates = [
    path.join(root, "build", "bin", "Release"),
    path.join(root, "build", "bin", "RelWithDebInfo"),
    path.join(root, "build", "bin", "Debug"),
    path.join(root, "build", "bin"),
    path.join(process.resourcesPath ?? "", "layer"),
  ];
  for (const dir of candidates) {
    if (fs.existsSync(path.join(dir, `${LAYER_NAME}.json`))) return dir;
  }
  return null;
}

/** The Android layer libraries and APK (tools/build_android.py), from the build tree or a packaged app. */
function findAndroidLayerFiles(): AndroidLayerFiles | null {
  const root = path.resolve(__dirname, "..", "..", "..");
  const candidates = [
    process.env.INSPECTOR_ANDROID_LAYER_DIR,
    path.join(root, "build", "android"),
    path.join(process.resourcesPath ?? "", "layer", "android"),
  ].filter((d): d is string => !!d);
  return findAndroidLayer(candidates);
}

// ------------------------------------------------------------------------------------------
// Sessions

function launchDisplayName(c: LaunchConfig): string {
  if (c.target === "android") return `${c.exe} (Android)`;
  if (c.target === "implicit") return `any application (port ${c.port})`;
  const base = path.basename(c.exe) || c.exe;
  return c.args ? `${base} ${c.args}` : base;
}

class Session {
  readonly id: number;
  name: string;
  config: LaunchConfig | null;
  port: number;
  target: ChildProcess | null = null;
  /** The launched Android application, for Android targets (see android.ts). */
  android: AndroidTarget | null = null;
  pid: number | null = null;
  /** The established connection to the layer. */
  socket: net.Socket | null = null;
  /** A connection attempt in progress. */
  connecting: net.Socket | null = null;
  connectTimer: NodeJS.Timeout | null = null;
  connectDeadline = 0;
  connectAttempts = 0;
  state: ConnectionState = "disconnected";
  detail = "";
  recordAlways = false;
  /** Set while the inspector itself is terminating the target, so its exit is reported as such. */
  killing = false;
  log: string[] = [];
  /** The window currently displaying this session. */
  viewer: BrowserWindow | null = null;
  private _pending: LayerMessage[] = [];
  private _flushScheduled = false;

  constructor(id: number, name: string, config: LaunchConfig | null, port: number) {
    this.id = id;
    this.name = name;
    this.config = config;
    this.port = port;
  }

  info(): SessionInfo {
    return {
      id: this.id, name: this.name, config: this.config, port: this.port, pid: this.pid,
      state: this.state, detail: this.detail, recordAlways: this.recordAlways, log: this.log.slice(),
    };
  }

  get connected(): boolean {
    return this.socket !== null && !this.socket.destroyed;
  }

  send(channel: string, payload: unknown): void {
    const win = this.viewer && !this.viewer.isDestroyed() ? this.viewer : mainWin;
    if (win && !win.isDestroyed()) win.webContents.send(channel, payload);
  }

  setStatus(state: ConnectionState, detail = ""): void {
    this.state = state;
    this.detail = detail;
    this.send("inspector:status", { sessionId: this.id, state, detail });
    this.appendLog(`[${state}] ${detail}`);
  }

  appendLog(line: string): void {
    // Testing aid: --debug-log=<file> mirrors every session log line to a file.
    const logFile = cliOption("debug-log");
    if (logFile) {
      try {
        fs.appendFileSync(logFile, `[${this.id}] ${line}
`);
      } catch {
        // ignore
      }
    }
    this.log.push(line);
    if (this.log.length > MAX_LOG_LINES) this.log.splice(0, this.log.length - MAX_LOG_LINES);
    this.send("inspector:log", { sessionId: this.id, line });
  }

  queueMessage(msg: LayerMessage): void {
    this._pending.push(msg);
    if (!this._flushScheduled) {
      this._flushScheduled = true;
      setImmediate(() => {
        this._flushScheduled = false;
        const batch = this._pending;
        this._pending = [];
        this.send("inspector:messages", { sessionId: this.id, messages: batch });
      });
    }
  }
}

const sessions = new Map<number, Session>();
let nextSessionId = 1;

function getSession(id: number): Session | null {
  return sessions.get(Number(id)) ?? null;
}

function sessionsOf(win: BrowserWindow | null): Session[] {
  if (!win) return [];
  return [...sessions.values()].filter((s) => s.viewer === win);
}

function broadcast(channel: string, payload: unknown): void {
  for (const win of BrowserWindow.getAllWindows()) {
    if (!win.isDestroyed()) win.webContents.send(channel, payload);
  }
}

/** Shows a session in a window (which must have finished loading). */
function attachSession(s: Session, win: BrowserWindow): void {
  s.viewer = win;
  if (!win.isDestroyed()) win.webContents.send("inspector:sessionAdded", s.info());
}

// ------------------------------------------------------------------------------------------
// TCP client

function sendJson(s: Session, obj: UiRequest): boolean {
  if (!s.socket || s.socket.destroyed) return false;
  const payload = Buffer.from(JSON.stringify(obj), "utf8");
  const header = Buffer.alloc(5);
  header.writeUInt32LE(payload.length, 0);
  header.writeUInt8(0, 4);
  s.socket.write(Buffer.concat([header, payload]));
  return true;
}

function disconnectSession(s: Session): void {
  if (s.connectTimer) {
    clearTimeout(s.connectTimer);
    s.connectTimer = null;
  }
  if (s.connecting) {
    s.connecting.destroy();
    s.connecting = null;
  }
  if (s.socket) {
    s.socket.destroy();
    s.socket = null;
  }
}

function connectSession(s: Session, deadlineMs: number): void {
  disconnectSession(s);
  s.connectDeadline = Date.now() + deadlineMs;
  s.connectAttempts = 0;
  s.setStatus("connecting", `port ${s.port}`);
  attemptConnect(s);
}

function attemptConnect(s: Session): void {
  const sock = net.createConnection({ host: "127.0.0.1", port: s.port });
  s.connecting = sock;
  let buffered: Buffer = Buffer.alloc(0);
  sock.setNoDelay(true);

  // Retries while the deadline has not passed and, for launched applications, the process is
  // still alive: right after a launch the layer is not listening yet, and a connection can
  // briefly reach a previous process on the same port that is still shutting down. For an
  // Android target a refused connection can also mean adb lost the port forward (the device
  // reconnected): every couple of seconds the forward is checked and re-created.
  let refused = false;
  const retry = (why: string): void => {
    const alive = s.config ? s.target !== null || s.android !== null || s.config.target === "implicit" : true;
    if (alive && Date.now() < s.connectDeadline) {
      if (s.state === "connected") s.setStatus("connecting", `port ${s.port}`);   // adb accepted, the device dropped it
      if (!s.connectTimer) {
        s.connectTimer = setTimeout(() => {
          s.connectTimer = null;
          const android = s.android;
          s.connectAttempts++;
          if (android && refused && s.connectAttempts % 4 === 0) {
            android.ensureForward().catch(() => false).then(() => { if (s.android === android && !s.socket && !s.connecting) attemptConnect(s); });
          } else {
            attemptConnect(s);
          }
        }, s.target ? 250 : 500);
      }
    } else if (s.state !== "exited" && s.state !== "error") {
      s.setStatus("disconnected", why);
    }
  };

  const gone = (why: string): void => {
    if (s.socket === sock) {
      s.socket = null;
      retry(why);
    } else if (s.connecting === sock) {
      s.connecting = null;
      retry(why);
    }
    // Otherwise the socket was replaced or explicitly closed; nothing to do.
  };

  sock.on("connect", () => {
    if (s.connecting !== sock) return;  // disconnected while connecting
    s.connecting = null;
    s.socket = sock;
    s.setStatus("connected", `port ${s.port}`);
    sendJson(s, { action: "Ping" });
    if (s.recordAlways) sendJson(s, { action: "Settings", recordAlways: true });
  });

  sock.on("data", (chunk: Buffer) => {
    if (s.socket !== sock) return;
    buffered = buffered.length ? Buffer.concat([buffered, chunk]) : chunk;
    while (buffered.length >= 5) {
      const len = buffered.readUInt32LE(0);
      const kind = buffered.readUInt8(4);
      if (buffered.length < 5 + len) break;
      const payload = buffered.subarray(5, 5 + len);
      buffered = buffered.subarray(5 + len);
      if (kind === 0) {
        try {
          s.queueMessage(JSON.parse(payload.toString("utf8")) as LayerMessage);
        } catch (e) {
          s.appendLog(`bad JSON from layer: ${e}`);
          const logFile = cliOption("debug-log");
          if (logFile) fs.appendFileSync(`${logFile}.badjson`, payload.toString("utf8") + "\n\n");
        }
      } else if (kind === 1) {
        const hl = payload.readUInt32LE(0);
        let header: Record<string, unknown> = {};
        try {
          header = JSON.parse(payload.subarray(4, 4 + hl).toString("utf8")) as Record<string, unknown>;
        } catch (e) {
          s.appendLog(`bad binary header from layer: ${e}`);
        }
        // Copy so the renderer gets a standalone ArrayBuffer.
        const data = new Uint8Array(payload.subarray(4 + hl));
        s.queueMessage({ ...header, __binary: data } as unknown as LayerMessage);
      }
    }
  });

  sock.on("error", (e: NodeJS.ErrnoException) => {
    refused = e.code === "ECONNREFUSED";
    gone(s.socket === sock ? "connection lost" : "could not connect");
  });
  sock.on("close", () => gone(""));
}

// ------------------------------------------------------------------------------------------
// Target process

function portInUseBySession(port: number, except: Session | null): boolean {
  for (const s of sessions.values()) {
    if (s !== except && s.port === port && (s.target || s.android || s.connected)) return true;
  }
  return false;
}

function portFree(port: number): Promise<boolean> {
  return new Promise((resolve) => {
    const srv = net.createServer();
    srv.once("error", () => resolve(false));
    srv.listen({ port, host: "127.0.0.1", exclusive: true }, () => srv.close(() => resolve(true)));
  });
}

/**
 * The requested port, or the next free one after it when it is taken: by another session (so
 * several launches of the same configuration can run side by side) or by anything else on the
 * machine, such as a previous instance of the target that has not finished exiting.
 */
async function findFreePort(start: number, except: Session | null): Promise<number> {
  for (let port = start; port < start + 100 && port < 65536; port++) {
    if (portInUseBySession(port, except)) continue;
    if (await portFree(port)) return port;
  }
  return start;
}

function splitArgs(s: string): string[] {
  const out: string[] = [];
  const re = /"([^"]*)"|'([^']*)'|(\S+)/g;
  let m: RegExpExecArray | null;
  while ((m = re.exec(s))) out.push(m[1] ?? m[2] ?? m[3]);
  return out;
}

/**
 * The directory holding the Khronos validation layer's manifest: the Vulkan SDK (VULKAN_SDK, or
 * the default install locations) or a distribution's layer directory. Needed because the launch
 * sets VK_LAYER_PATH, which replaces the loader's own explicit-layer search.
 */
function findValidationLayerDir(): string | null {
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

/** Starts the session's configured executable with the layer enabled and connects to it. */
function spawnTarget(s: Session, layerDir: string): LaunchResult {
  const config = s.config;
  if (!config) return { ok: false, error: "session has no launch configuration" };
  // With "Validation layer" the Khronos validation layer is enabled too; its messages reach the
  // inspector's debug-utils messenger (layer/src/validation.cpp).
  const layers = [LAYER_NAME];
  const layerPaths = [layerDir];
  if (config.validation) {
    const dir = findValidationLayerDir();
    if (dir) {
      layers.push(VALIDATION_LAYER_NAME);
      layerPaths.push(dir);
      s.appendLog(`validation layer: ${dir}`);
    } else {
      s.appendLog("validation layer not found: install the Vulkan SDK (or the distribution's validation layer package) or set VULKAN_SDK");
    }
  }
  const env: NodeJS.ProcessEnv = {
    ...process.env,
    ...parseEnvLines(config.env ?? ""),
    VK_ADD_LAYER_PATH: layerPaths.join(path.delimiter),
    VK_LOADER_LAYERS_ENABLE: layers.join(","),
    // Older loaders:
    VK_LAYER_PATH: [...layerPaths, ...(process.env.VK_LAYER_PATH ? [process.env.VK_LAYER_PATH] : [])].join(path.delimiter),
    VK_INSTANCE_LAYERS: [...layers, ...(process.env.VK_INSTANCE_LAYERS ? [process.env.VK_INSTANCE_LAYERS] : [])].join(path.delimiter),
    VKINSP_PORT: String(s.port),
    VKINSP_LOG: config.log ? "1" : "0",
    // Testing aid: with --debug-log the layer also writes its log to a file (Unity players have
    // no usable stderr).
    ...(cliOption("debug-log") ? { VKINSP_LOG_FILE: `${cliOption("debug-log")}.layer.log` } : {}),
    VKINSP_RECORD_ALWAYS: config.recordAlways ? "1" : "0",
    VKINSP_STACKTRACES: config.stacktraces ? "1" : "0",
    // The validation layer stops reporting a message after a few repeats (its
    // duplicate_message_limit, 10 by default); the inspector's layer counts repeats itself and
    // attaches a message to the captured command it fired on, which needs every occurrence.
    ...(config.validation && !process.env.VK_LAYER_DUPLICATE_MESSAGE_LIMIT ? { VK_LAYER_DUPLICATE_MESSAGE_LIMIT: "0" } : {}),
    // Synchronization validation: the settings-file name for current layers, the enable list for older ones.
    ...(config.validation && config.syncValidation ? { VK_LAYER_VALIDATE_SYNC: "true", VK_LAYER_ENABLES: "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT" } : {}),
  };
  return runTarget(s, config.exe, env, `layer: ${layerDir}`);
}

/**
 * macOS: the same, with the Metal capture library injected instead of a Vulkan layer registered.
 *
 * `exe` is the binary dyld runs, which for a bundle is inside it, while `config.exe` stays the
 * .app the user picked so the session and the recent list name it the way they do.
 */
function spawnMetalTarget(s: Session, library: string, exe: string): LaunchResult {
  const config = s.config;
  if (!config) return { ok: false, error: "session has no launch configuration" };
  const env: NodeJS.ProcessEnv = {
    ...process.env,
    ...parseEnvLines(config.env ?? ""),
    ...captureEnvironment(library, s.port, config.log ?? true, config.validation, config.stacktraces),
  };
  return runTarget(s, exe, env, `capture library: ${library}${config.validation ? " (Metal validation on)" : ""}`);
}

/** Spawns the target, pipes its output into the session's log and follows it to its exit. */
function runTarget(s: Session, exe: string, env: NodeJS.ProcessEnv, note: string): LaunchResult {
  const config = s.config;
  if (!config) return { ok: false, error: "session has no launch configuration" };
  const args = splitArgs(config.args ?? "");
  const cwd = config.cwd && fs.existsSync(config.cwd) ? config.cwd : path.dirname(exe);
  s.appendLog(`launching ${exe} ${args.join(" ")}`);
  s.appendLog(note);
  if (s.port !== config.port) s.appendLog(`port ${config.port} is in use; using ${s.port}`);
  let proc: ChildProcess;
  try {
    proc = spawn(exe, args, { cwd, env, stdio: ["ignore", "pipe", "pipe"] });
  } catch (e) {
    const message = e instanceof Error ? e.message : String(e);
    s.setStatus("error", `launch failed: ${message}`);
    return { ok: false, error: message };
  }
  s.target = proc;
  s.pid = proc.pid ?? null;
  s.killing = false;
  const pipe = (stream: NodeJS.ReadableStream | null): void => {
    if (!stream) return;
    let rest = "";
    stream.on("data", (d: Buffer) => {
      rest += d.toString("utf8");
      const lines = rest.split(/\r?\n/);
      rest = lines.pop() ?? "";
      for (const line of lines) if (line.length) s.appendLog(line);
    });
  };
  pipe(proc.stdout);
  pipe(proc.stderr);
  proc.on("exit", (code, signal) => {
    if (s.target !== proc) return;  // already replaced by a relaunch
    s.target = null;
    s.pid = null;
    disconnectSession(s);
    s.setStatus("exited", s.killing ? "terminated by inspector" : `code ${signal ?? code}`);
    s.killing = false;
  });
  proc.on("error", (e) => {
    if (s.target !== proc) return;
    s.target = null;
    s.pid = null;
    disconnectSession(s);
    s.setStatus("error", e.message);
  });
  s.setStatus("launched", `pid ${proc.pid}`);
  connectSession(s, LAUNCH_CONNECT_TIMEOUT_MS);
  return { ok: true, sessionId: s.id, pid: proc.pid, port: s.port };
}

/** Terminates a process and, on Windows, everything it spawned (Unity's crash handler, launchers). */
function terminate(proc: ChildProcess): void {
  if (process.platform === "win32" && proc.pid) {
    execFile("taskkill", ["/PID", String(proc.pid), "/T", "/F"], () => {
      // If taskkill is unavailable or the process is already gone, fall back to a plain kill.
      try { proc.kill(); } catch { /* already gone */ }
    });
    return;
  }
  proc.kill();
}

/**
 * Starts the session's package on its Android device with the layer enabled (see android.ts).
 * The layer listens on the device; adb forwards the session's port to it, so the connection
 * is made to 127.0.0.1 like for a local process. Installation and start take a while and can
 * fail (device gone, package not debuggable): that is reported through the session status.
 */
function launchAndroid(s: Session, adb: string, layer: AndroidLayerFiles): LaunchResult {
  const config = s.config;
  if (!config) return { ok: false, error: "session has no launch configuration" };
  const target = new AndroidTarget({
    adb, serial: config.device, package: config.exe, activity: config.activity, port: s.port,
    log: config.log, recordAlways: config.recordAlways, stacktraces: config.stacktraces, layer,
    onLog: (line) => s.appendLog(line),
    onExit: () => {
      if (s.android !== target) return;
      s.android = null;
      s.pid = null;
      disconnectSession(s);
      s.setStatus("exited", s.killing ? "terminated by inspector" : "process exited");
      s.killing = false;
    },
  });
  s.android = target;
  s.pid = null;
  s.killing = false;
  s.appendLog(`launching ${config.exe} on ${config.device}`);
  if (s.port !== config.port) s.appendLog(`port ${config.port} is in use; using ${s.port}`);
  s.setStatus("launched", `starting on ${config.device}`);
  target.start().then(() => {
    if (s.android !== target) return;
    s.pid = target.pid;
    s.setStatus("launched", `pid ${target.pid}`);
    connectSession(s, LAUNCH_CONNECT_TIMEOUT_MS);
  }, (e: unknown) => {
    if (s.android !== target) return;
    s.android = null;
    void target.stop();
    s.setStatus("error", e instanceof Error ? e.message : String(e));
  });
  return { ok: true, sessionId: s.id, port: s.port };
}

/** Terminates the target and resolves once it has exited (or after a timeout). */
function killTarget(s: Session): Promise<void> {
  const proc = s.target;
  disconnectSession(s);
  if (s.android) {
    const target = s.android;
    s.android = null;
    s.pid = null;
    s.killing = true;
    return target.stop().then(() => {
      s.setStatus("exited", "terminated by inspector");
      s.killing = false;
    });
  }
  if (!proc) return Promise.resolve();
  s.killing = true;
  return new Promise((resolve) => {
    let done = false;
    const finish = (): void => {
      if (done) return;
      done = true;
      resolve();
    };
    proc.once("exit", finish);
    setTimeout(finish, KILL_TIMEOUT_MS);
    try {
      terminate(proc);
    } catch {
      finish();
    }
  });
}

type ValidLaunch =
  | { kind: "native"; layerDir: string }
  /** macOS: the capture library to inject, and the binary inside the bundle to run. */
  | { kind: "metal"; library: string; exe: string }
  | { kind: "android"; adb: string; layer: AndroidLayerFiles }
  | { kind: "implicit" };

function validateLaunch(config: LaunchConfig): ValidLaunch | { error: string } {
  if (config.target === "implicit") {
    if (!findLayerDir()) return { error: NO_LAYER_ERROR };
    return { kind: "implicit" };
  }
  if (config.target === "android") {
    const adb = findAdb();
    if (!adb) return { error: "adb not found: install the Android SDK platform-tools, or set ANDROID_HOME or INSPECTOR_ADB" };
    const layer = findAndroidLayerFiles();
    if (!layer) return { error: "Android layer not found: build it with tools/build_android.py (see docs/ARCHITECTURE.md)" };
    if (!config.device) return { error: "no Android device selected" };
    if (!config.exe) return { error: "no package name given" };
    return { kind: "android", adb, layer };
  }
  if (!config.exe || !fs.existsSync(config.exe)) return { error: `executable not found: ${config.exe}` };
  if (process.platform === "darwin") {
    const library = findCaptureLibrary();
    if (!library) return { error: NO_LAYER_ERROR };
    // A .app is a directory; dyld needs the binary inside it.
    const exe = resolveExecutable(config.exe);
    if (!fs.existsSync(exe) || fs.statSync(exe).isDirectory()) {
      return { error: `no executable found inside ${path.basename(config.exe)}` };
    }
    // Checked before spawning: a hardened target starts fine and simply never connects, which is
    // a much worse thing to debug than a message.
    const blocked = injectionBlockedReason(exe);
    if (blocked) return { error: blocked };
    return { kind: "metal", library, exe };
  }
  const layerDir = findLayerDir();
  if (!layerDir) return { error: NO_LAYER_ERROR };
  return { kind: "native", layerDir };
}

function startTarget(s: Session, v: ValidLaunch): LaunchResult {
  if (v.kind === "implicit") return waitForApplication(s);
  if (v.kind === "android") return launchAndroid(s, v.adb, v.layer);
  if (v.kind === "metal") return spawnMetalTarget(s, v.library, v.exe);
  return spawnTarget(s, v.layerDir);
}

/**
 * Nothing to start: the implicit layer is registered, so an application started with
 * VKINSP_ENABLE=1 (and VKINSP_PORT set to the session's port) loads the layer and listens; the
 * session keeps trying to connect for a good while.
 */
function waitForApplication(s: Session): LaunchResult {
  s.appendLog(`waiting for an application started with VKINSP_ENABLE=1 VKINSP_PORT=${s.port} (the implicit layer)`);
  connectSession(s, WAIT_CONNECT_TIMEOUT_MS);
  s.setStatus("connecting", `waiting for an application with VKINSP_ENABLE=1 on port ${s.port}`);
  return { ok: true, sessionId: s.id, port: s.port };
}

async function launch(config: LaunchConfig): Promise<LaunchResult> {
  const v = validateLaunch(config);
  if ("error" in v) return { ok: false, error: v.error };
  config = normalizeLaunch(config);
  const port = await findFreePort(config.port, null);
  const s = new Session(nextSessionId++, launchDisplayName(config), config, port);
  s.recordAlways = config.recordAlways;
  sessions.set(s.id, s);
  if (mainWin) attachSession(s, mainWin);
  const result = startTarget(s, v);
  addRecent(config);
  broadcast("inspector:recents", loadRecents());
  return result;
}

function connectOnly(port: number): LaunchResult {
  port = Number(port) || DEFAULT_PORT;
  const s = new Session(nextSessionId++, `port ${port}`, null, port);
  sessions.set(s.id, s);
  if (mainWin) attachSession(s, mainWin);
  connectSession(s, ATTACH_CONNECT_TIMEOUT_MS);
  return { ok: true, sessionId: s.id, port };
}

/** Terminates the session's application and launches it again, in the same session. */
async function restartSession(s: Session): Promise<LaunchResult> {
  if (!s.config) return { ok: false, error: "session was not launched by the inspector" };
  const v = validateLaunch(s.config);
  if ("error" in v) {
    s.setStatus("error", v.error);
    return { ok: false, error: v.error };
  }
  await killTarget(s);
  s.port = await findFreePort(s.config.port, s);
  return startTarget(s, v);
}

async function closeSession(s: Session): Promise<void> {
  await killTarget(s);
  // The last Android session on a device turns the device's debug layer settings off again, so
  // the package runs without the layer when started from the device itself.
  const config = s.config;
  if (config?.target === "android" &&
      ![...sessions.values()].some((o) => o !== s && o.config?.target === "android" && o.config.device === config.device)) {
    const adb = findAdb();
    if (adb) void disableLayer(adb, config.device);
  }
  sessions.delete(s.id);
  const viewer = s.viewer;
  s.send("inspector:sessionRemoved", s.id);
  s.viewer = null;
  // A session window with nothing left to show closes itself.
  if (viewer && viewer !== mainWin && !viewer.isDestroyed() && sessionsOf(viewer).length === 0) viewer.close();
}

function killAllTargets(): void {
  for (const s of sessions.values()) {
    disconnectSession(s);
    if (s.android) {
      s.android.stopSync();
      s.android = null;
    }
    if (s.target) {
      try {
        terminate(s.target);
      } catch {
        // already gone
      }
      s.target = null;
    }
  }
}

// ------------------------------------------------------------------------------------------
// Self-update (electron-updater)
//
// Installed builds check the GitHub releases of the repository named in electron-builder.yml
// (electron-builder writes the provider into resources/app-update.yml) shortly after startup,
// and again when the user clicks the version label. Downloads only start when the user asks;
// a downloaded update is installed on quit or when the user asks to restart.

const UPDATE_CHECK_DELAY_MS = 3000;
/** How long --screenshot waits for a window to paint before giving up on it (see writeScreenshots). */
const SCREENSHOT_TIMEOUT_MS = 10000;
const canUpdate = app.isPackaged;

function sendUpdate(status: UpdateStatus): void {
  broadcast("inspector:update", status);
}

function configureUpdater(): void {
  autoUpdater.autoDownload = false;
  autoUpdater.autoInstallOnAppQuit = true;
  autoUpdater.on("checking-for-update", () => sendUpdate({ state: "checking" }));
  autoUpdater.on("update-available", (info) => sendUpdate({ state: "available", version: info.version }));
  autoUpdater.on("update-not-available", (info) => sendUpdate({ state: "up-to-date", version: info.version }));
  autoUpdater.on("download-progress", (p) => sendUpdate({ state: "downloading", percent: p.percent }));
  autoUpdater.on("update-downloaded", (info) => sendUpdate({ state: "downloaded", version: info.version }));
  autoUpdater.on("error", (err) => sendUpdate({ state: "error", message: err.message }));
}

async function checkForUpdates(): Promise<boolean> {
  if (!canUpdate) {
    sendUpdate({ state: "error", message: "Updates are only available in installed builds." });
    return false;
  }
  try {
    await autoUpdater.checkForUpdates();
    return true;
  } catch (e) {
    // The "error" event already reported it to the windows.
    console.error("update check failed:", e);
    return false;
  }
}

async function downloadUpdate(): Promise<boolean> {
  if (!canUpdate) return false;
  try {
    await autoUpdater.downloadUpdate();
    return true;
  } catch (e) {
    console.error("update download failed:", e);
    return false;
  }
}

function installUpdate(): boolean {
  if (!canUpdate) return false;
  // before-quit stops the inspected applications.
  autoUpdater.quitAndInstall(false, true);
  return true;
}

// ------------------------------------------------------------------------------------------
// Windows

// UI theme: a persisted user setting (Theme picker in the main window). INSPECTOR_THEME in the
// environment overrides it for the run, which the screenshot test aids use.
function isTheme(v: unknown): v is ThemeName {
  return typeof v === "string" && (THEMES as readonly string[]).includes(v);
}

function appTheme(): ThemeName {
  const env = process.env.INSPECTOR_THEME;
  if (isTheme(env)) return env;
  const saved = loadSettings().theme;
  return isTheme(saved) ? saved : "dark";
}

function windowBackground(theme: ThemeName): string {
  return theme === "light" ? "#ffffff" : "#1e1e1e";
}

/** Saves the theme and applies it to every open window. */
function setTheme(theme: ThemeName): void {
  if (!isTheme(theme)) return;
  const settings = loadSettings();
  settings.theme = theme;
  saveSettings(settings);
  for (const win of BrowserWindow.getAllWindows()) {
    if (!win.isDestroyed()) win.setBackgroundColor(windowBackground(theme));
  }
  broadcast("inspector:theme", theme);
}

// Window / taskbar icon, rendered from assets/icon.svg by `npm run icons`.
function appIconPath(): string {
  const file = process.platform === "win32" ? "icon.ico" : "icon.png";
  const candidates = [path.join(__dirname, "..", "..", "assets", file), path.join(process.resourcesPath ?? "", "assets", file)];
  return candidates.find((p) => fs.existsSync(p)) ?? candidates[0];
}

// X11 puts the window icon in the _NET_WM_ICON property, which cannot exceed the server's maximum
// request size (256 KB). assets/icon.png is 512x512, four bytes a pixel: 1 MB, so Chromium drops
// it without a word and the desktop falls back to a placeholder icon (a gear, on GNOME). Scale it
// down for X11; 128x128 (64 KB) is what the taskbar and window list actually display.
const X11_ICON_SIZE = 128;
let scaledIcon: electron.NativeImage | null = null;

function appIcon(): string | electron.NativeImage {
  if (process.platform !== "linux") return appIconPath();
  if (!scaledIcon) {
    const image = nativeImage.createFromPath(appIconPath());
    if (image.isEmpty()) return appIconPath();
    const { width } = image.getSize();
    scaledIcon = width > X11_ICON_SIZE ? image.resize({ width: X11_ICON_SIZE, height: X11_ICON_SIZE }) : image;
  }
  return scaledIcon;
}

const windowPrefs = (): electron.BrowserWindowConstructorOptions => ({
  width: 1500,
  height: 950,
  backgroundColor: windowBackground(appTheme()),
  title: "GPU Inspector",
  icon: appIcon(),
  webPreferences: {
    preload: path.join(__dirname, "preload.cjs"),
    contextIsolation: true,
    nodeIntegration: false,
    sandbox: false,
  },
});

const rendererHtml = (): string => path.join(__dirname, "..", "renderer", "index.html");

function createMainWindow(): BrowserWindow {
  const win = new BrowserWindow(windowPrefs());
  win.setMenuBarVisibility(false);
  void win.loadFile(rendererHtml(), { query: { theme: appTheme() } });
  if (process.env.INSPECTOR_DEVTOOLS) win.webContents.openDevTools({ mode: "detach" });
  win.on("closed", () => {
    // The main window is the application: closing it ends every session.
    mainWin = null;
    app.quit();
  });
  return win;
}

/** Moves a session out of its current window into a window of its own. */
function openSessionWindow(s: Session): void {
  const previous = s.viewer;
  const win = new BrowserWindow({ ...windowPrefs(), title: `${s.name} - GPU Inspector` });
  win.setMenuBarVisibility(false);
  // The renderer asks for its sessions with getConfig once loaded, so nothing is pushed here.
  s.viewer = win;
  if (previous && !previous.isDestroyed()) previous.webContents.send("inspector:sessionRemoved", s.id);
  void win.loadFile(rendererHtml(), { query: { session: String(s.id), theme: appTheme() } });
  if (process.env.INSPECTOR_DEVTOOLS) win.webContents.openDevTools({ mode: "detach" });
  win.on("closed", () => {
    // Sessions shown in a closed window return to the main window rather than being killed.
    for (const sess of sessionsOf(win)) {
      if (mainWin && !mainWin.isDestroyed()) attachSession(sess, mainWin);
      else void closeSession(sess);
    }
  });
}

/**
 * Opens a capture file in a window of its own (the renderer there opens the path as a file
 * session and has no launcher). Bytes handed over from a live capture go to a temporary file
 * that is removed when the application quits; the window is told not to list it as recent.
 */
const tempCaptures: string[] = [];
function openCaptureWindow(opts: { path?: string; data?: Uint8Array; name?: string }): boolean {
  let file = opts.path ?? null;
  let temp = false;
  if (!file && opts.data) {
    const base = (opts.name ?? "capture").replace(/[^\w.-]+/g, "_") || "capture";
    file = path.join(os.tmpdir(), `vkinsp_${process.pid}_${tempCaptures.length}_${base}.gpucap`);
    try {
      fs.writeFileSync(file, Buffer.from(opts.data.buffer, opts.data.byteOffset, opts.data.byteLength));
    } catch (err) {
      console.error(`capture window: ${file}: ${err}`);
      return false;
    }
    tempCaptures.push(file);
    temp = true;
  }
  if (!file) return false;
  const win = new BrowserWindow({ ...windowPrefs(), title: `${path.basename(file)} - GPU Inspector` });
  win.setMenuBarVisibility(false);
  void win.loadFile(rendererHtml(), { query: { capture: file, ...(temp ? { temp: "1" } : {}), theme: appTheme() } });
  if (process.env.INSPECTOR_DEVTOOLS) win.webContents.openDevTools({ mode: "detach" });
  return true;
}

function moveSessionToMain(s: Session): void {
  const previous = s.viewer;
  if (!mainWin || mainWin.isDestroyed() || previous === mainWin) return;
  if (previous && !previous.isDestroyed()) previous.webContents.send("inspector:sessionRemoved", s.id);
  attachSession(s, mainWin);
  if (previous && !previous.isDestroyed() && sessionsOf(previous).length === 0) previous.close();
}

function windowOf(sender: WebContents): BrowserWindow | null {
  return BrowserWindow.fromWebContents(sender);
}

// ------------------------------------------------------------------------------------------
// Shader text (SPIR-V disassembly / cross compilation) using the Vulkan SDK's tools until the
// project ships its own SPIRV-Tools/SPIRV-Cross build.

// Shader editor: compiles a source language to SPIR-V with the SDK's compilers. `stage` uses the
// layer's stage names ("vertex", "fragment", ...); `spirvVersion` ("1.5") picks the target
// environment so the module matches what the application's driver accepts.
const GLSL_STAGES: Record<string, string> = {
  vertex: "vert", tess_control: "tesc", tess_eval: "tese", geometry: "geom", fragment: "frag", compute: "comp",
  task: "task", mesh: "mesh", raygen: "rgen", intersection: "rint", any_hit: "rahit", closest_hit: "rchit", miss: "rmiss", callable: "rcall",
};
const HLSL_PROFILES: Record<string, string> = {
  vertex: "vs_6_0", tess_control: "hs_6_0", tess_eval: "ds_6_0", geometry: "gs_6_0", fragment: "ps_6_0", compute: "cs_6_0",
  task: "as_6_5", mesh: "ms_6_5", raygen: "lib_6_3", intersection: "lib_6_3", any_hit: "lib_6_3", closest_hit: "lib_6_3", miss: "lib_6_3", callable: "lib_6_3",
};

function targetEnv(spirvVersion: string, tool: "glslang" | "dxc" | "spirv-as"): string {
  const v = spirvVersion || "1.5";
  if (tool === "spirv-as") return `spv${v}`;
  const glslang: Record<string, string> = { "1.0": "vulkan1.0", "1.3": "vulkan1.1", "1.4": "vulkan1.1spirv1.4", "1.5": "vulkan1.2", "1.6": "vulkan1.3" };
  const env = glslang[v] ?? "vulkan1.2";
  return tool === "dxc" ? env : env.replace("spirv", "spv");
}

function compileShader(source: string, language: ShaderLanguage, stage: string, entryPoint: string, spirvVersion: string): Promise<CompileShaderResult> {
  return new Promise((resolve) => {
    const base = path.join(os.tmpdir(), `vkinsp_${process.pid}_${Date.now()}`);
    const src = base + (language === "hlsl" ? ".hlsl" : language === "spirv-asm" ? ".spvasm" : ".glsl");
    const out = base + ".spv";
    fs.writeFileSync(src, source);
    const entry = entryPoint || "main";
    let tool: string;
    let args: string[];
    if (language === "spirv-asm") {
      tool = findTool("spirv-as");
      args = ["--target-env", targetEnv(spirvVersion, "spirv-as"), "-o", out, src];
    } else if (language === "hlsl") {
      tool = findTool("dxc");
      args = ["-spirv", "-T", HLSL_PROFILES[stage] ?? "ps_6_0", "-E", entry, `-fspv-target-env=${targetEnv(spirvVersion, "dxc")}`, "-Fo", out, src];
    } else {
      tool = findTool("glslangValidator");
      // The decompiled source declares main(); the pipeline expects the original entry point name.
      args = ["-V", "-S", GLSL_STAGES[stage] ?? "frag", "--target-env", targetEnv(spirvVersion, "glslang"),
        "--source-entrypoint", "main", "-e", entry, "-o", out, src];
    }
    execFile(tool, args, { maxBuffer: 64 * 1024 * 1024 }, (err, stdout, stderr) => {
      const log = `${stdout ?? ""}${stderr ?? ""}`.trim();
      let spirv: Uint8Array | undefined;
      try {
        if (fs.existsSync(out)) spirv = new Uint8Array(fs.readFileSync(out));
      } catch {
        spirv = undefined;
      }
      for (const f of [src, out]) {
        try {
          fs.unlinkSync(f);
        } catch {
          // ignore
        }
      }
      const name = path.basename(tool);
      if (err || !spirv || spirv.byteLength < 20) {
        const reason = log || (err && "code" in err && err.code === "ENOENT" ? `${name} not found: install the Vulkan SDK or set VULKAN_SDK` : err?.message ?? `${name} produced no output`);
        resolve({ ok: false, log: reason, tool: name });
      } else {
        resolve({ ok: true, spirv, log, tool: name });
      }
    });
  });
}

// ------------------------------------------------------------------------------------------
// IPC

ipcMain.handle("inspector:compileShader", (_e, source: string, language: ShaderLanguage, stage: string, entryPoint: string, spirvVersion: string) =>
  compileShader(source, language, stage, entryPoint, spirvVersion));

ipcMain.handle("inspector:getConfig", (e): AppConfig => {
  const win = windowOf(e.sender);
  return {
    recents: loadRecents(),
    recentCaptures: loadRecentCaptures(),
    layerDir: findLayerDir(),
    platform: process.platform,
    theme: appTheme(),
    windowMode: win === mainWin ? "main" : "session",
    sessions: sessionsOf(win).map((s) => s.info()),
    version: app.getVersion(),
    canUpdate,
    debug: {
      select: cliOption("debug-select"),
      capture: cliFlag("debug-capture"),
      captureFrames: Number(cliOption("debug-capture")) || 1,
      captureStacks: cliFlag("debug-capture-stacks"),
      expandStacks: cliFlag("debug-expand-stacks"),
      selectCommand: cliOption("debug-command") ? Number(cliOption("debug-command")) : null,
      showView: cliOption("debug-view"),
      expandSection: cliOption("debug-expand"),
      waitForApp: cliFlag("wait-for-app"),
      launchDialog: cliFlag("debug-launch-dialog") ? cliOption("debug-launch-dialog") ?? "native" : null,
      openCapture: cliOption("debug-open"),
      saveCapture: cliOption("debug-save"),
    },
  };
});
ipcMain.handle("inspector:setTheme", (_e, theme: ThemeName) => {
  setTheme(theme);
  return true;
});
ipcMain.handle("inspector:checkForUpdates", () => checkForUpdates());
ipcMain.handle("inspector:downloadUpdate", () => downloadUpdate());
ipcMain.handle("inspector:installUpdate", () => installUpdate());
ipcMain.handle("inspector:getRecents", () => loadRecents());
ipcMain.handle("inspector:removeRecent", (_e, index: number) => {
  const recents = loadRecents();
  recents.splice(index, 1);
  saveRecents(recents);
  return recents;
});
ipcMain.handle("inspector:clearRecents", () => {
  saveRecents([]);
  return [];
});
ipcMain.handle("inspector:launch", (_e, config: LaunchConfig) => launch(config));
ipcMain.handle("inspector:connect", (_e, port: number) => connectOnly(port));
ipcMain.handle("inspector:implicitLayer", async (): Promise<ImplicitLayerStatus> => {
  const dir = findLayerDir();
  return dir ? implicitLayerStatus(dir) : { registered: false, manifest: "", error: NO_LAYER_ERROR };
});
ipcMain.handle("inspector:setImplicitLayer", async (_e, on: boolean): Promise<ImplicitLayerStatus> => {
  const dir = findLayerDir();
  return dir ? setImplicitLayer(dir, !!on) : { registered: false, manifest: "", error: NO_LAYER_ERROR };
});
ipcMain.handle("inspector:androidDevices", async (): Promise<AndroidDeviceList> => {
  const adb = findAdb();
  const layer = findAndroidLayerFiles() !== null;
  if (!adb) return { adb: null, devices: [], layer, error: "adb not found: install the Android SDK platform-tools, or set ANDROID_HOME or INSPECTOR_ADB" };
  try {
    return { adb, devices: await listDevices(adb), layer, error: null };
  } catch (e) {
    return { adb, devices: [], layer, error: e instanceof Error ? e.message : String(e) };
  }
});
ipcMain.handle("inspector:androidPackages", async (_e, serial: string): Promise<string[]> => {
  const adb = findAdb();
  if (!adb || !serial) return [];
  try {
    return await listPackages(adb, serial);
  } catch {
    return [];
  }
});
ipcMain.handle("inspector:kill", async (_e, id: number) => {
  const s = getSession(id);
  if (!s) return false;
  await killTarget(s);
  if (!s.config) s.setStatus("disconnected", "");
  return true;
});
ipcMain.handle("inspector:restart", (_e, id: number): Promise<LaunchResult> => {
  const s = getSession(id);
  return s ? restartSession(s) : Promise.resolve({ ok: false, error: "no such session" });
});
ipcMain.handle("inspector:closeSession", async (_e, id: number) => {
  const s = getSession(id);
  if (!s) return false;
  await closeSession(s);
  return true;
});
ipcMain.handle("inspector:openSessionWindow", (_e, id: number) => {
  const s = getSession(id);
  if (!s) return false;
  openSessionWindow(s);
  return true;
});
// Stack frames the layer named by module and offset only, resolved with the unstripped libraries
// under the session's symbol directories (or the last ones used, for capture files).
ipcMain.handle("inspector:symbolize", (_e, frames: StackFrame[], dirs: string[]) => {
  const list = (dirs.length ? dirs : (loadSettings().symbolDirs ?? "").split(";")).map((d) => d.trim()).filter(Boolean);
  if (dirs.length) {
    const settings = loadSettings();
    if (settings.symbolDirs !== dirs.join(";")) {
      settings.symbolDirs = dirs.join(";");
      saveSettings(settings);
    }
  }
  return symbolizeFrames(frames, list);
});

// Shader source files named by a module's debug information, found under the session's source
// roots (or the last ones used, for capture files).
ipcMain.handle("inspector:shaderSource", (_e, names: string[], roots: string[]) => {
  const list = (roots.length ? roots : (loadSettings().sourceRoots ?? "").split(";")).map((d) => d.trim()).filter(Boolean);
  if (roots.length) {
    const settings = loadSettings();
    if (settings.sourceRoots !== roots.join(";")) {
      settings.sourceRoots = roots.join(";");
      saveSettings(settings);
      forgetSourceIndex();
    }
  }
  return findShaderSources(names, list);
});

ipcMain.handle("inspector:openCaptureWindow", (_e, opts: { path?: string; data?: Uint8Array; name?: string }) => openCaptureWindow(opts));
// A capture window's "Move to Main Window": the main window opens the file and this one closes.
ipcMain.handle("inspector:openCaptureInMain", (e, filePath: string) => {
  if (!mainWin || mainWin.isDestroyed()) return false;
  mainWin.webContents.send("inspector:openCapture", filePath);
  mainWin.focus();
  const win = windowOf(e.sender);
  if (win && win !== mainWin) win.close();
  return true;
});
ipcMain.handle("inspector:moveSessionToMain", (_e, id: number) => {
  const s = getSession(id);
  if (!s) return false;
  moveSessionToMain(s);
  return true;
});
ipcMain.handle("inspector:refresh", (_e, id: number) => {
  const s = getSession(id);
  return s ? sendJson(s, { action: "RequestSnapshot" }) : false;
});
ipcMain.handle("inspector:send", (_e, id: number, msg: UiRequest) => {
  const s = getSession(id);
  if (!s) return false;
  if (msg.action === "Settings" && msg.recordAlways !== undefined) s.recordAlways = msg.recordAlways;
  return sendJson(s, msg);
});
ipcMain.handle("inspector:shaderText", (_e, spirv: Uint8Array, mode: ShaderTextMode) => shaderText(spirv, mode));
ipcMain.handle("inspector:chooseFile", async (e, opts?: OpenFileOptions) => {
  const win = windowOf(e.sender) ?? mainWin;
  if (!win) return null;
  const defaultFilters = process.platform === "win32" ? [{ name: "Executables", extensions: ["exe"] }, { name: "All files", extensions: ["*"] }] : [];
  // macOS: an application is a .app bundle, which is a directory. treatPackageAsDirectory would
  // make the panel descend into it; without it the bundle is chosen as one item, which is what
  // the launch path wants (metal.ts resolves the executable inside).
  const properties: Array<"openFile" | "openDirectory"> = opts?.directory ? ["openDirectory"] : ["openFile"];
  const r = await dialog.showOpenDialog(win, {
    title: opts?.title ?? "Choose executable",
    properties,
    filters: opts?.directory ? [] : opts?.filters ?? defaultFilters,
  });
  return r.canceled ? null : r.filePaths[0];
});

// Capture files (renderer/capture_file.ts): the renderer serializes, the main process owns the
// dialogs and the filesystem.
ipcMain.handle("inspector:saveFile", async (e, opts: SaveFileOptions, data: Uint8Array): Promise<string | null> => {
  let target = opts.path ?? null;
  if (!target) {
    const win = windowOf(e.sender) ?? mainWin;
    if (!win) return null;
    const r = await dialog.showSaveDialog(win, { title: opts.title ?? "Save", defaultPath: opts.defaultPath, filters: opts.filters });
    if (r.canceled || !r.filePath) return null;
    target = r.filePath;
  }
  try {
    fs.writeFileSync(target, Buffer.from(data.buffer, data.byteOffset, data.byteLength));
    return target;
  } catch (err) {
    console.error(`saveFile ${target}: ${err}`);
    return null;
  }
});
ipcMain.handle("inspector:addRecentCapture", (_e, file: string) => addRecentCapture(file));
ipcMain.handle("inspector:removeRecentCapture", (_e, index: number) => removeRecentCapture(index));
ipcMain.handle("inspector:readFile", (_e, file: string): Uint8Array | null => {
  try {
    return new Uint8Array(fs.readFileSync(file));
  } catch (err) {
    console.error(`readFile ${file}: ${err}`);
    return null;
  }
});

// ------------------------------------------------------------------------------------------
// App lifecycle

function firstSession(): Session | null {
  return sessions.values().next().value ?? null;
}

async function writeScreenshots(file: string): Promise<void> {
  const windows = BrowserWindow.getAllWindows().filter((w) => !w.isDestroyed());
  let index = 0;
  for (const win of windows) {
    // capturePage() asks the compositor for a frame, and that can both fail and never answer:
    // it rejects with UnknownVizError when the GPU process will not produce one (which happens
    // when the app runs with its output redirected, as the UI tests run it), and it has no
    // timeout of its own. Either way an unhandled rejection here would skip the --screenshot
    // caller's app.quit() and hang the run, so give up on a shot rather than on the run.
    const img = await Promise.race([
      win.webContents.capturePage().catch((e: unknown) => {
        console.error(`screenshot failed: ${e instanceof Error ? e.message : String(e)}`);
        return null;
      }),
      new Promise<null>((r) => setTimeout(() => r(null), SCREENSHOT_TIMEOUT_MS)),
    ]);
    if (!img) continue;
    // Main window -> <file>; other windows -> <file minus extension>.<n>.png
    const target = win === mainWin ? file : file.replace(/(\.[^.]+)?$/, `.${++index}$1`);
    fs.writeFileSync(target, img.toPNG());
    console.log(`screenshot written: ${target}`);
  }
}

void app.whenReady().then(() => {
  migrateSettings();
  if (canUpdate) {
    configureUpdater();
    setTimeout(() => void checkForUpdates(), UPDATE_CHECK_DELAY_MS);
  }
  if (process.platform === "darwin") app.dock?.setIcon(appIconPath());
  mainWin = createMainWindow();
  mainWin.webContents.on("did-finish-load", () => {
    const exe = cliOption("launch");
    const androidPackage = cliOption("launch-android");
    if (exe || androidPackage) {
      const config = normalizeLaunch({
        target: androidPackage ? "android" : "native",
        exe: androidPackage ?? exe ?? "",
        device: cliOption("device") ?? "",
        activity: cliOption("activity") ?? "",
        // --args is the launch dialog's field; --launch-args is the spelling metal/README.md uses.
        args: cliOption("args") ?? cliOption("launch-args") ?? "",
        port: Number(cliOption("port")) || DEFAULT_PORT,
        recordAlways: cliFlag("record-always"),
        validation: cliFlag("validation"),
        syncValidation: cliFlag("sync-validation"),
        symbolDirs: cliOption("symbol-dirs") ?? "",
        sourceRoots: cliOption("source-roots") ?? "",
        // --capture-frame=N / --capture-after=SECONDS queue a capture like the launch dialog does.
        capture: cliOption("capture-frame") !== null ? { mode: "frame", value: Number(cliOption("capture-frame")) || 0 }
          : cliOption("capture-after") !== null ? { mode: "time", value: Number(cliOption("capture-after")) || 0 }
          : { mode: "none", value: 0 },
      });
      // --launch=<path> / --launch-android=<package>: the command-line form of the launch dialog,
      // for scripting and for the UI tests. Everything not given takes its default from
      // normalizeLaunch. This is the only place a launch happens: launching here *and* from a
      // second handler started the application twice, which left a stray process behind every run
      // and, with --args, a first session that had none of them.
      setTimeout(() => {
        void launch(config)
          .then((r) => { if (!r.ok) console.error(`--launch failed: ${r.error}`); })
          .catch((e) => console.error(`--launch threw: ${e instanceof Error ? e.stack : String(e)}`));
      }, 300);
      // Testing aids for the session handling.
      if (cliFlag("debug-multi")) setTimeout(() => void launch(config), 1500);
      if (cliFlag("debug-relaunch")) {
        setTimeout(() => {
          const s = firstSession();
          if (s) void restartSession(s);
        }, 4000);
      }
      if (cliFlag("debug-detach")) {
        setTimeout(() => {
          const s = firstSession();
          if (s) openSessionWindow(s);
        }, 3000);
      }
    }
    // --implicit-layer=on|off registers or unregisters the implicit layer for this user and quits.
    const implicit = cliOption("implicit-layer");
    if (implicit === "on" || implicit === "off") {
      const dir = findLayerDir();
      const done = dir ? setImplicitLayer(dir, implicit === "on") : Promise.resolve({ registered: false, manifest: "", error: NO_LAYER_ERROR } as ImplicitLayerStatus);
      void done.then((status) => {
        console.log(status.error ? `implicit layer: ${status.error}` : `implicit layer ${status.registered ? "registered" : "not registered"}: ${status.manifest}`);
        app.quit();
      });
      return;
    }
    // --connect=<port>: attach to an application that is already listening, the command-line form
    // of the Connect button. Unlike --wait-for-app it needs no layer of ours in the process, so it
    // is the way in for a capture library the inspector did not launch — the Metal one today.
    const connectPort = cliOption("connect");
    if (connectPort) connectOnly(Number(connectPort) || DEFAULT_PORT);
    // --wait-for-app: a session that waits for an application started with VKINSP_ENABLE=1.
    if (cliFlag("wait-for-app")) {
      void launch({ ...normalizeLaunch({} as LaunchConfig), target: "implicit", port: Number(cliOption("port")) || DEFAULT_PORT,
        recordAlways: cliFlag("record-always"), log: true });
    }
    // Testing aid: switch the theme through the same path the picker uses.
    const debugTheme = cliOption("debug-theme");
    if (isTheme(debugTheme)) setTimeout(() => setTheme(debugTheme), 1000);
    // Testing aid: --debug-open-window=<file> opens a capture file in a window of its own.
    const openWindow = cliOption("debug-open-window");
    if (openWindow) setTimeout(() => openCaptureWindow({ path: openWindow }), 500);
    const shot = cliOption("screenshot");
    if (shot) {
      setTimeout(async () => {
        // Testing aid: --debug-mouse=x,y[;x,y...] clicks each point on the main window (in
        // order, with a pause between) before the shot; the last one leaves the mouse there.
        for (const point of cliOption("debug-mouse")?.split(";") ?? []) {
          const mouse = point.split(",").map(Number);
          if (mouse.length !== 2 || !mainWin) continue;
          mainWin.webContents.sendInputEvent({ type: "mouseEnter", x: mouse[0], y: mouse[1] });
          mainWin.webContents.sendInputEvent({ type: "mouseMove", x: mouse[0], y: mouse[1] });
          await new Promise((r) => setTimeout(r, 100));
          mainWin.webContents.sendInputEvent({ type: "mouseDown", x: mouse[0], y: mouse[1], button: "left", clickCount: 1 });
          mainWin.webContents.sendInputEvent({ type: "mouseUp", x: mouse[0], y: mouse[1], button: "left", clickCount: 1 });
          mainWin.webContents.sendInputEvent({ type: "mouseMove", x: mouse[0] + 2, y: mouse[1] + 2 });
          await new Promise((r) => setTimeout(r, 400));
        }
        // Testing aid: --debug-expand=<text> opens the selected command's section with that text
        // in its title (a shader's source, a buffer's contents), then waits for what it fetches.
        const expand = cliOption("debug-expand");
        if (expand && mainWin) {
          const opened = await mainWin.webContents.executeJavaScript(
            `window.__inspectorDebugExpand ? window.__inspectorDebugExpand(${JSON.stringify(expand)}) : false`);
          if (!opened) console.error(`--debug-expand: no section titled ${JSON.stringify(expand)}`);
          // The section fetches what it shows (a shader's source from a source root, a buffer's
          // bytes from the layer) after it opens, and the dump below has to see the result.
          await new Promise((r) => setTimeout(r, 1500));
        }
        // Testing aid: --debug-dump=<json> writes what the renderer knows (sessions, captures,
        // findings, validation) for tools/ui_tests.py to check.
        const dump = cliOption("debug-dump");
        if (dump && mainWin) {
          try {
            const state = await mainWin.webContents.executeJavaScript("window.__inspectorDebugState ? window.__inspectorDebugState() : null");
            fs.writeFileSync(dump, JSON.stringify(state, null, 1));
          } catch (e) {
            fs.writeFileSync(dump, JSON.stringify({ error: String(e) }));
          }
        }
        // Whatever the screenshots did, the quit has to happen: the harness's only other way out
        // of a run is its timeout.
        await writeScreenshots(shot).catch((e: unknown) => console.error(`screenshots failed: ${String(e)}`));
        if (cliFlag("quit-after-screenshot")) {
          killAllTargets();
          app.quit();
        }
      }, Number(cliOption("screenshot-delay")) || 4000);
    }
  });
});

app.on("before-quit", () => {
  killAllTargets();
  for (const f of tempCaptures) { try { fs.unlinkSync(f); } catch { /* already gone */ } }
});

app.on("window-all-closed", () => {
  killAllTargets();
  app.quit();
});
