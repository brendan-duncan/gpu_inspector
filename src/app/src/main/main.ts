// Electron main process: windows, inspection sessions (target process launch and the TCP
// connection to its layer), and the IPC bridge to the renderer.
//
// A session is one inspected application. Sessions are created by launching an executable or by
// connecting to a running one, live here in the main process, and are displayed by exactly one
// window at a time: the main window by default, or a window of their own ("Open in New Window").
import electron from "electron";
import updater from "electron-updater";
import { spawn, type ChildProcess } from "node:child_process";
import net from "node:net";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { symbolizeFrames } from "./symbolize.js";
import {
  NO_REPLAY_TOOL, findReplayTool, releaseAllReplays, releaseReplayKey, replayKeyed, type OverdrawRun, type PixelRequest, type ReplayAnalysis, type ReplayRun,
} from "./replay.js";
import { findShaderSources, forgetSourceIndex } from "./shader_sources.js";
import { compileDxil, compileHlslForDebugging, compileShader, decompileForDebugging, shaderText } from "./shader_tools.js";
import { measureStageByAblation, type StageAblationRequest } from "./shader_ablation_run.js";
import { FrameReader, encodeRequest } from "./layer_protocol.js";
import {
  DEFAULT_PORT, findFreePort as findFreePortFrom, findLayerDir as findLayerDirIn, findValidationLayerDir, parseEnvLines, splitArgs, terminate,
  vulkanLayerEnvironment,
} from "./launch_env.js";
import { implicitLayerStatus, setImplicitLayer, setUserEnvironment, userEnvironmentStatus } from "./implicit_layer.js";
import { CAPTURE_LIBRARY, captureEnvironment, findCaptureLibrary, injectionBlockedReason, resolveExecutable } from "./metal.js";
import { WATCH_TIMED_OUT, findD3D12Tools as findD3D12ToolsIn, watchLaunch, windowsLaunch, type D3D12Tools } from "./d3d12.js";
import { AndroidTarget, disableLayer, findAdb, findAndroidLayer, listDevices, listPackages, type AndroidLayerFiles } from "./android.js";
import {
  THEMES,
  type AndroidDeviceList,
  type AppConfig, type ConnectionState, type LaunchConfig, type LaunchResult, type LayerMessage, type OpenFileOptions, type SaveFileOptions, type SessionInfo,
  type CompileShaderResult, type ShaderLanguage, type ShaderTextMode, type ShaderTextResult, type ThemeName, type UiRequest,
  type UpdateStatus, type StackFrame, type ImplicitLayerStatus,
} from "../shared/protocol.js";

const { app, BrowserWindow, ipcMain, dialog, nativeImage, shell } = electron;
const { autoUpdater } = updater;
type BrowserWindow = electron.BrowserWindow;
type WebContents = electron.WebContents;

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const MAX_LOG_LINES = 2000;
const LAUNCH_CONNECT_TIMEOUT_MS = 60000;
const ATTACH_CONNECT_TIMEOUT_MS = 5000;
/** How long a session waits for an application the implicit layer brings (see waitForApplication). */
const WAIT_CONNECT_TIMEOUT_MS = 30 * 60 * 1000;
/**
 * How long a D3D12 watch session gives an injected application to create a device before it says
 * the injection probably came too late. The capture library opens its port at D3D12CreateDevice
 * and not before (src/d3d12/README.md), so silence past this means either no device was made or
 * one was made before the hooks went in.
 */
const D3D12_DEVICE_WAIT_MS = 10000;
const KILL_TIMEOUT_MS = 3000;

let mainWin: BrowserWindow | null = null;

// Command line: --launch=<exe> [--args="..."] [--port=N] [--screenshot=<png> --screenshot-delay=<ms>]
//               --launch-android=<package> --device=<serial> [--activity=<name>]
//               --wait-for-app (the Vulkan implicit layer) | --wait-for-d3d12=<image> (Windows)
//               [--debug-select=<VkType>] [--debug-capture[=<frames>]] [--record-always]
//               [--debug-relaunch] [--debug-multi] [--debug-detach] [--debug-theme=<name>] [--debug-mouse=x,y[;x,y...]]
//               [--debug-settle=<ms>]
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
    target: c.target === "android" || c.target === "implicit" || c.target === "waitD3D12" ? c.target : "native",
    exe: c.exe ?? "",
    args: c.args ?? "",
    cwd: c.cwd ?? "",
    env: c.env ?? "",
    device: c.device ?? "",
    activity: c.activity ?? "",
    port: Number(c.port) || DEFAULT_PORT,
    log: c.log ?? true,
    recordAlways: c.recordAlways ?? false,
    breadcrumbs: c.breadcrumbs ?? false,
    shaderStatistics: c.shaderStatistics ?? false,
    validation: c.validation ?? false,
    syncValidation: c.syncValidation ?? false,
    gpuValidation: c.gpuValidation ?? false,
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
 * What to say when there is no capture library. macOS launches inject the Metal library; Windows
 * launches carry both the Vulkan layer and the D3D12 library (d3d12.ts), so there a launch is
 * possible with either and this is said only when neither is built.
 */
const NO_LAYER_ERROR = process.platform === "darwin"
  ? `capture library not found (${CAPTURE_LIBRARY}): build it first (see src/metal/README.md)`
  : process.platform === "win32"
    ? "no capture library found: build the Vulkan layer and the D3D12 library first (docs/BUILDING.md)"
    : "layer not found: build the layer first (see docs/ARCHITECTURE.md)";
/** The implicit layer and the layer registration are the Vulkan layer's alone, whatever else is built. */
const NO_VULKAN_LAYER_ERROR = "Vulkan layer not found: build the layer first (see docs/BUILDING.md)";
/** Waiting for a Direct3D 12 application needs the D3D12 tools and nothing else. */
const NO_D3D12_ERROR = "D3D12 capture library not found: build it (see src/d3d12/README.md) or set INSPECTOR_D3D12_DIR";

/** The layer of the checkout the app was built in, or of the packaged app. */
function findLayerDir(): string | null {
  return findLayerDirIn([path.resolve(__dirname, "..", "..", "..", "..")], [path.join(process.resourcesPath ?? "", "layer")]);
}

/** The D3D12 capture library and launcher (src/d3d12/README.md), from the same places; Windows only. */
function findD3D12Tools(): D3D12Tools | null {
  if (process.platform !== "win32") return null;
  return findD3D12ToolsIn([path.resolve(__dirname, "..", "..", "..", "..")], [path.join(process.resourcesPath ?? "", "layer")]);
}

/** The Android layer libraries and APK (tools/build_android.py), from the build tree or a packaged app. */
function findAndroidLayerFiles(): AndroidLayerFiles | null {
  const root = path.resolve(__dirname, "..", "..", "..", "..");
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
  if (c.target === "waitD3D12") return `${c.exe || "an application"} when it starts (D3D12)`;
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
  s.socket.write(encodeRequest(obj));
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
  const reader = new FrameReader();
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
    const messages = reader.push(chunk, (e) => {
      s.appendLog(e.kind === "json" ? `bad JSON from layer: ${e.error}` : `bad binary header from layer: ${e.error}`);
      const logFile = cliOption("debug-log");
      if (logFile && e.kind === "json") fs.appendFileSync(`${logFile}.badjson`, e.payload.toString("utf8") + "\n\n");
    });
    for (const msg of messages) s.queueMessage(msg);
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

/** The requested port, or the next one free on the machine and not used by another session. */
function findFreePort(start: number, except: Session | null): Promise<number> {
  return findFreePortFrom(start, (port) => portInUseBySession(port, except));
}

/**
 * Starts the session's configured executable with the layer enabled and connects to it. On
 * Windows the D3D12 capture library goes in too, through its launcher (d3d12.ts): whichever API
 * the application uses connects. Either of the two may be missing, but not both (validateLaunch).
 */
function spawnTarget(s: Session, layerDir: string | null, d3d12: D3D12Tools | null): LaunchResult {
  const config = s.config;
  if (!config) return { ok: false, error: "session has no launch configuration" };
  let validationDir: string | null = null;
  if (config.validation && layerDir) {
    validationDir = findValidationLayerDir();
    if (validationDir) s.appendLog(`validation layer: ${validationDir}`);
    else s.appendLog("validation layer not found: install the Vulkan SDK (or the distribution's validation layer package) or set VULKAN_SDK");
  }
  // Testing aid: with --debug-log the layer also writes its log to a file (Unity players have no
  // usable stderr).
  const debugLog = cliOption("debug-log");
  const vulkan = layerDir ? {
    layerDir, validationDir, port: s.port, log: config.log, recordAlways: config.recordAlways, breadcrumbs: config.breadcrumbs, shaderStatistics: config.shaderStatistics, stacktraces: config.stacktraces,
    validation: config.validation, syncValidation: !!config.syncValidation, gpuValidation: !!config.gpuValidation, ...(debugLog ? { logFile: `${debugLog}.layer.log` } : {}),
  } : null;
  const base: NodeJS.ProcessEnv = { ...process.env, ...parseEnvLines(config.env ?? "") };
  const args = splitArgs(config.args ?? "");
  const cwd = config.cwd && fs.existsSync(config.cwd) ? config.cwd : path.dirname(config.exe);
  if (process.platform === "win32") {
    const launch = windowsLaunch({
      exe: config.exe, args, cwd, env: base, vulkan,
      d3d12: d3d12 ? {
        tools: d3d12, port: s.port, log: config.log, recordAlways: config.recordAlways, stacktraces: config.stacktraces,
        validation: config.validation, gpuValidation: !!config.gpuValidation, ...(debugLog ? { logFile: `${debugLog}.d3d12.log` } : {}),
      } : null,
    });
    return runTarget(s, launch.exe, launch.args, cwd, launch.env, launch.notes);
  }
  if (!vulkan) return { ok: false, error: NO_LAYER_ERROR };
  return runTarget(s, config.exe, args, cwd, { ...base, ...vulkanLayerEnvironment(vulkan) }, [`layer: ${layerDir}`]);
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
  const cwd = config.cwd && fs.existsSync(config.cwd) ? config.cwd : path.dirname(exe);
  return runTarget(s, exe, splitArgs(config.args ?? ""), cwd, env, [`capture library: ${library}${config.validation ? " (Metal validation on)" : ""}`]);
}

interface RunOptions {
  /** How long to keep trying to connect; the launch deadline by default. */
  connectMs?: number;
  /** Every line the process writes, after it has gone into the session log (the D3D12 watch reads it). */
  onOutput?: (line: string) => void;
  /** What the session's status says it started; "pid N" by default. */
  started?: string;
  /** What "exited" says for an exit code of the process's own; `code N` by default. */
  exitDetail?: (code: number | null) => string | undefined;
}

/**
 * Spawns the target, pipes its output into the session's log and follows it to its exit. `exe`
 * and `args` are what is actually spawned: on Windows the D3D12 launcher with the application's
 * command line after "--" (d3d12.ts), or that same launcher watching for an application to start
 * (waitForD3D12Application), so they are taken as given rather than from the configuration.
 */
function runTarget(s: Session, exe: string, args: string[], cwd: string, env: NodeJS.ProcessEnv, notes: string[], o: RunOptions = {}): LaunchResult {
  const config = s.config;
  if (!config) return { ok: false, error: "session has no launch configuration" };
  s.appendLog(`launching ${exe} ${args.join(" ")}`);
  for (const note of notes) s.appendLog(note);
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
      for (const line of lines) {
        if (!line.length) continue;
        s.appendLog(line);
        o.onOutput?.(line);
      }
    });
  };
  pipe(proc.stdout);
  pipe(proc.stderr);
  proc.on("exit", (code, signal) => {
    if (s.target !== proc) return;  // already replaced by a relaunch
    s.target = null;
    s.pid = null;
    disconnectSession(s);
    s.setStatus("exited", s.killing ? "terminated by inspector" : o.exitDetail?.(code) ?? `code ${signal ?? code}`);
    s.killing = false;
  });
  proc.on("error", (e) => {
    if (s.target !== proc) return;
    s.target = null;
    s.pid = null;
    disconnectSession(s);
    s.setStatus("error", e.message);
  });
  s.setStatus("launched", o.started ?? `pid ${proc.pid}`);
  connectSession(s, o.connectMs ?? LAUNCH_CONNECT_TIMEOUT_MS);
  return { ok: true, sessionId: s.id, pid: proc.pid, port: s.port };
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
  /** A local process: the Vulkan layer, and on Windows the D3D12 tools too; at least one of them is there. */
  | { kind: "native"; layerDir: string | null; d3d12: D3D12Tools | null }
  /** macOS: the capture library to inject, and the binary inside the bundle to run. */
  | { kind: "metal"; library: string; exe: string }
  | { kind: "android"; adb: string; layer: AndroidLayerFiles }
  | { kind: "implicit" }
  /** Windows: the D3D12 tools, whose launcher watches for the application to start. */
  | { kind: "waitD3D12"; d3d12: D3D12Tools };

function validateLaunch(config: LaunchConfig): ValidLaunch | { error: string } {
  if (config.target === "implicit") {
    if (!findLayerDir()) return { error: NO_VULKAN_LAYER_ERROR };
    return { kind: "implicit" };
  }
  if (config.target === "waitD3D12") {
    if (process.platform !== "win32") return { error: "waiting for a Direct3D 12 application is a Windows target" };
    const image = path.basename(config.exe || "");
    if (!image) return { error: "no application to wait for: give the executable's name (TestVulkan.exe) or its full path" };
    const d3d12 = findD3D12Tools();
    if (!d3d12) return { error: NO_D3D12_ERROR };
    return { kind: "waitD3D12", d3d12 };
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
  // Windows: the Vulkan layer and the D3D12 library both go into every target, so either one
  // makes the launch possible (the session log says which is missing).
  const layerDir = findLayerDir();
  const d3d12 = findD3D12Tools();
  if (!layerDir && !d3d12) return { error: NO_LAYER_ERROR };
  return { kind: "native", layerDir, d3d12 };
}

function startTarget(s: Session, v: ValidLaunch): LaunchResult {
  if (v.kind === "implicit") return waitForApplication(s);
  if (v.kind === "waitD3D12") return waitForD3D12Application(s, v.d3d12);
  if (v.kind === "android") return launchAndroid(s, v.adb, v.layer);
  if (v.kind === "metal") return spawnMetalTarget(s, v.library, v.exe);
  return spawnTarget(s, v.layerDir, v.d3d12);
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

/**
 * D3D12's counterpart of the implicit layer (src/d3d12/README.md, "Getting in"): nothing can be
 * registered with the D3D12 runtime, so the session's process is dxinsp_launch.exe --watch, which
 * polls for a process with the configured image name and injects the capture library into it as it
 * starts. The watcher then stands in for the application the way the launcher does for one it
 * started, so the session's log, status and Stop all work unchanged — except that stopping ends
 * the watch, never an application the inspector did not start.
 *
 * The library opens its port only once a D3D12 device exists, so a connection is also the proof
 * that the injection was in time; when none comes within D3D12_DEVICE_WAIT_MS of an injection the
 * session says what that means.
 */
function waitForD3D12Application(s: Session, d3d12: D3D12Tools): LaunchResult {
  const config = s.config;
  if (!config) return { ok: false, error: "session has no launch configuration" };
  const debugLog = cliOption("debug-log");
  const watch = watchLaunch(d3d12, {
    image: config.exe, timeoutSeconds: WAIT_CONNECT_TIMEOUT_MS / 1000, once: true,
    port: s.port, log: config.log, recordAlways: config.recordAlways, stacktraces: config.stacktraces,
    validation: config.validation, gpuValidation: !!config.gpuValidation, ...(debugLog ? { logFile: `${debugLog}.d3d12.log` } : {}),
  });
  const image = path.basename(config.exe);
  let timer: NodeJS.Timeout | null = null;
  return runTarget(s, watch.exe, watch.args, d3d12.dir, { ...process.env, ...parseEnvLines(config.env ?? "") }, [
    `D3D12 capture library: ${d3d12.library}${config.validation ? " (D3D12 debug layer on)" : ""}`,
    `waiting for ${image} to start: run it now, from wherever it is normally started`,
    "only the D3D12 library goes in this way; a Vulkan application is waited for with the implicit layer instead",
  ], {
    connectMs: WAIT_CONNECT_TIMEOUT_MS,
    started: `waiting for ${image} on port ${s.port}`,
    exitDetail: (code) => code === WATCH_TIMED_OUT ? `${image} did not start within ${WAIT_CONNECT_TIMEOUT_MS / 60000} minutes` : undefined,
    onOutput: (line) => {
      // The watcher says which process it got into; from there the library has a few seconds to
      // create a device, which is the only sign it was in the process early enough.
      if (!/^dxinsp: injected /.test(line) || timer) return;
      timer = setTimeout(() => {
        timer = null;
        if (s.connected || s.state === "exited") return;
        s.appendLog(`no D3D12 device was created in ${image} within ${D3D12_DEVICE_WAIT_MS / 1000} s: either it does not use `
          + "Direct3D 12, or it already had its device when the library went in (the watch has to be running before the "
          + "application starts). The session keeps waiting.");
      }, D3D12_DEVICE_WAIT_MS);
    },
  });
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
        // Synchronously: this runs on the way out of the process, where a kill left to a callback
        // never happens and the inspected application outlives the inspector.
        terminate(s.target, true);
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

// ------------------------------------------------------------------------------------------
// IPC

ipcMain.handle("inspector:compileShader", (_e, source: string, language: ShaderLanguage, stage: string, entryPoint: string, spirvVersion: string) =>
  // #include is resolved against the session's source roots, the same ones the Source view reads
  // the files a module's debug information names from.
  compileShader(source, language, stage, entryPoint, spirvVersion, { includeDirs: sourceRootDirs() }));

// A D3D12 pipeline's stage: HLSL to DXIL with dxc, includes from the same roots.
ipcMain.handle("inspector:compileDxil", (_e, source: string, stage: string, entryPoint: string, shaderModel?: string) =>
  compileDxil(source, stage, entryPoint, shaderModel || "6_0", { includeDirs: sourceRootDirs() }));

ipcMain.handle("inspector:decompileForDebugging", (_e, spirv: Uint8Array, stage: string, entryPoint: string) =>
  decompileForDebugging(spirv, stage, entryPoint));

// A D3D12 stage for the shader debugger: its HLSL (from the container, or a PDB under the symbol
// directories, as for inspector:shaderText) compiled to SPIR-V, includes from the source roots.
ipcMain.handle("inspector:compileHlslForDebugging", (_e, bytecode: Uint8Array, stage: string, entryPoint: string, target?: string, pdbDirs?: string[]) =>
  compileHlslForDebugging(bytecode, stage, entryPoint, { pdbDirs: symbolDirsWith(pdbDirs), includeDirs: sourceRootDirs(), target }));

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

// The user documentation (docs/) on GitHub, opened in the browser. The renderer names a page and
// an anchor, never a URL, so nothing else can be opened through this.
const DOCS_URL = "https://github.com/brendan-duncan/gpu_inspector/blob/main/docs/";
ipcMain.handle("inspector:openDocs", (_e, page?: string) => {
  const target = typeof page === "string" && /^[A-Z_]+\.md(#[a-z0-9-]+)?$/.test(page) ? page : "README.md";
  return shell.openExternal(DOCS_URL + target).then(() => true, () => false);
});
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
  return dir ? implicitLayerStatus(dir) : { registered: false, manifest: "", error: NO_VULKAN_LAYER_ERROR };
});
ipcMain.handle("inspector:setImplicitLayer", async (_e, on: boolean): Promise<ImplicitLayerStatus> => {
  const dir = findLayerDir();
  return dir ? setImplicitLayer(dir, !!on) : { registered: false, manifest: "", error: NO_VULKAN_LAYER_ERROR };
});
// The implicit layer's variables for the whole account, for applications started by a launcher.
ipcMain.handle("inspector:userEnvironment", () => userEnvironmentStatus());
ipcMain.handle("inspector:setUserEnvironment", (_e, port: number | null) => setUserEnvironment(port === null ? null : Number(port)));
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
/** The source roots last set, for the Source view's lookups and the shader editor's includes. */
function sourceRootDirs(): string[] {
  return (loadSettings().sourceRoots ?? "").split(";").map((d) => d.trim()).filter(Boolean);
}

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
// Vulkan overdraw: the capture replayed on this machine's GPU (src/main/replay.ts, docs/REPLAY.md).
/** What every replay request names: the renderer's key for its capture, and the capture's bytes when the main process asked for them. */
interface ReplayRequest { key: string; data?: Uint8Array; name?: string }

/** Runs an analysis in the replay kept alive for a renderer's capture (replayKeyed in replay.ts). */
function replayFor(opts: ReplayRequest, analysis: ReplayAnalysis): Promise<ReplayRun> {
  const tool = findReplayTool([path.resolve(__dirname, "..", "..", "..", "..")], [path.join(process.resourcesPath ?? "", "layer")]);
  if (!tool) return Promise.resolve({ data: null, output: "", error: NO_REPLAY_TOOL });
  return replayKeyed(tool, opts.key, opts.data, analysis, opts.name);
}

ipcMain.handle("inspector:measureOverdraw", (_e, opts: ReplayRequest): Promise<OverdrawRun> => replayFor(opts, { kind: "overdraw" }));
// A capture closed, or changed so that it is serialized again: its replay stops and its file goes.
ipcMain.handle("inspector:releaseReplay", (_e, key: string) => releaseReplayKey(key));
// Vulkan per-draw timing and counters: the frame replayed with queries around each draw
// (src/replay/src/draw_stats.cpp), for the Shader Flame Graph.
ipcMain.handle("inspector:measureDraws", (_e, opts: ReplayRequest): Promise<ReplayRun> => replayFor(opts, { kind: "draws" }));
// Vulkan hardware counters: the GPU's own counters around each render pass, collected by replaying
// the frame once per collection pass (src/replay/src/hw_counters.cpp).
ipcMain.handle("inspector:measureHwCounters", (_e, opts: ReplayRequest & { perDraw?: boolean }): Promise<ReplayRun> =>
  replayFor(opts, { kind: "counters", perDraw: opts.perDraw }));
// Vulkan draw-call overlays: where some draws landed, drawn again on their own (src/replay/src/overlay.cpp).
ipcMain.handle("inspector:drawOverlay", (_e, opts: ReplayRequest & { commands: number[] }): Promise<ReplayRun> =>
  replayFor(opts, { kind: "overlay", commands: opts.commands }));
// Vulkan mesh output: what some draws' vertex shaders wrote, through transform feedback (src/replay/src/mesh.cpp).
ipcMain.handle("inspector:meshOutput", (_e, opts: ReplayRequest & { commands: number[] }): Promise<ReplayRun> =>
  replayFor(opts, { kind: "mesh", commands: opts.commands }));
// Vulkan pixel history: one pixel followed through the replayed frame (src/replay/src/history.cpp).
ipcMain.handle("inspector:pixelHistory", (_e, opts: ReplayRequest & { pixel: PixelRequest }): Promise<ReplayRun> =>
  replayFor(opts, { kind: "pixel", ...opts.pixel }));
// Vulkan shader cost by ablation: a stage's variants timed at one draw (src/replay/src/ablation.cpp).
ipcMain.handle("inspector:measureShader", async (_e, opts: ReplayRequest & { stage: StageAblationRequest }) => {
  let needData = false;
  try {
    const ablation = await measureStageByAblation(async (analysis) => {
      const run = await replayFor(opts, analysis);
      if (run.needData) {
        needData = true;
        throw new Error("the replay needs the capture");
      }
      return run;
    }, { ...opts.stage, spirv: new Uint8Array(opts.stage.spirv) });
    return { ablation };
  } catch (e) {
    return needData ? { needData: true } : { error: (e as Error).message };
  }
});
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
// A D3D12 shader built with dxc -Zs keeps its HLSL out of the container and in a PDB beside the
// build, so the tool is given directories to look in: the session's symbol directories, which
// already name unstripped build output, then the last ones a launch used and the MCP server's
// environment variable, so a capture file opened later still finds them.
ipcMain.handle("inspector:shaderText", (_e, spirv: Uint8Array, mode: ShaderTextMode, pdbDirs?: string[]) =>
  shaderText(spirv, mode, { pdbDirs: symbolDirsWith(pdbDirs) }));

/** The session's symbol directories with the saved and the environment's ones, for a shader PDB. */
function symbolDirsWith(pdbDirs?: string[]): string[] {
  const dirs = [...(pdbDirs ?? []), ...(loadSettings().symbolDirs ?? "").split(";"), ...(process.env.GPU_INSPECTOR_SYMBOL_DIRS ?? "").split(";")]
    .map((d) => d.trim()).filter(Boolean);
  return [...new Set(dirs)];
}
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

// Testing aid: a run that takes screenshots (tools/ui_tests.py) keeps rendering while other windows
// cover it. Chromium stops the rendering steps of a window it finds occluded (requestAnimationFrame,
// ResizeObserver), so a tab that fits its image once its pane has a size never refitted, and a
// scripted click missed the image: whether a case passed depended on what was in front of the window.
if (cliOption("screenshot")) {
  app.commandLine.appendSwitch("disable-backgrounding-occluded-windows");
  app.commandLine.appendSwitch("disable-renderer-backgrounding");
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
        // --args is the launch dialog's field; --launch-args is the spelling src/metal/README.md uses.
        args: cliOption("args") ?? cliOption("launch-args") ?? "",
        port: Number(cliOption("port")) || DEFAULT_PORT,
        recordAlways: cliFlag("record-always"),
        validation: cliFlag("validation"),
        syncValidation: cliFlag("sync-validation"),
        gpuValidation: cliFlag("gpu-validation"),
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
      const done = dir ? setImplicitLayer(dir, implicit === "on") : Promise.resolve({ registered: false, manifest: "", error: NO_VULKAN_LAYER_ERROR } as ImplicitLayerStatus);
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
    // --wait-for-d3d12=<image name or path>: the same for Direct3D 12, where there is no implicit
    // layer: the session watches for the application to start and injects the library into it.
    const waitD3D12 = cliOption("wait-for-d3d12");
    if (waitD3D12) {
      void launch({ ...normalizeLaunch({} as LaunchConfig), target: "waitD3D12", exe: waitD3D12, port: Number(cliOption("port")) || DEFAULT_PORT,
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
        // Testing aid: --debug-settle=<ms> waits before the dump, for work a click set going that
        // finishes on its own: the pixel history's replay, the overdraw measurement.
        const settle = Math.min(Number(cliOption("debug-settle")) || 0, 60000);
        if (settle > 0) await new Promise((r) => setTimeout(r, settle));
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
  releaseAllReplays();
  killAllTargets();
  for (const f of tempCaptures) { try { fs.unlinkSync(f); } catch { /* already gone */ } }
});

app.on("window-all-closed", () => {
  killAllTargets();
  app.quit();
});
