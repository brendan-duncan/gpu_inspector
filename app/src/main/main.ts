// Electron main process: windows, inspection sessions (target process launch and the TCP
// connection to its layer), and the IPC bridge to the renderer.
//
// A session is one inspected application. Sessions are created by launching an executable or by
// connecting to a running one, live here in the main process, and are displayed by exactly one
// window at a time: the main window by default, or a window of their own ("Open in New Window").
import electron from "electron";
import { spawn, execFile, type ChildProcess } from "node:child_process";
import net from "node:net";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import {
  THEMES,
  type AppConfig, type ConnectionState, type LaunchConfig, type LaunchResult, type LayerMessage, type SessionInfo,
  type ShaderTextMode, type ShaderTextResult, type ThemeName, type UiRequest,
} from "../shared/protocol.js";

const { app, BrowserWindow, ipcMain, dialog, nativeImage } = electron;
type BrowserWindow = electron.BrowserWindow;
type WebContents = electron.WebContents;

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const LAYER_NAME = "VK_LAYER_INSPECTOR_capture";
const DEFAULT_PORT = 47531;
const MAX_LOG_LINES = 2000;
const LAUNCH_CONNECT_TIMEOUT_MS = 60000;
const ATTACH_CONNECT_TIMEOUT_MS = 5000;
const KILL_TIMEOUT_MS = 3000;

let mainWin: BrowserWindow | null = null;

// Command line: --launch=<exe> [--args="..."] [--port=N] [--screenshot=<png> --screenshot-delay=<ms>]
//               [--debug-select=<VkType>] [--debug-capture[=<frames>]] [--record-always]
//               [--debug-relaunch] [--debug-multi] [--debug-detach] [--debug-theme=<name>] [--debug-mouse=x,y]
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
  theme?: ThemeName;
}

const MAX_RECENTS = 12;

function normalizeLaunch(c: Partial<LaunchConfig>): LaunchConfig {
  return {
    exe: c.exe ?? "",
    args: c.args ?? "",
    cwd: c.cwd ?? "",
    env: c.env ?? "",
    port: Number(c.port) || DEFAULT_PORT,
    log: c.log ?? true,
    recordAlways: c.recordAlways ?? false,
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
// by executable + arguments so relaunching with different options updates the existing entry.
function addRecent(config: LaunchConfig): LaunchConfig[] {
  const recents = loadRecents().filter((r) => !(r.exe === config.exe && r.args === config.args));
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

// ------------------------------------------------------------------------------------------
// Sessions

function launchDisplayName(c: LaunchConfig): string {
  const base = path.basename(c.exe) || c.exe;
  return c.args ? `${base} ${c.args}` : base;
}

class Session {
  readonly id: number;
  name: string;
  config: LaunchConfig | null;
  port: number;
  target: ChildProcess | null = null;
  pid: number | null = null;
  /** The established connection to the layer. */
  socket: net.Socket | null = null;
  /** A connection attempt in progress. */
  connecting: net.Socket | null = null;
  connectTimer: NodeJS.Timeout | null = null;
  connectDeadline = 0;
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
  // briefly reach a previous process on the same port that is still shutting down.
  const retry = (why: string): void => {
    const alive = s.config ? s.target !== null : true;
    if (alive && Date.now() < s.connectDeadline) {
      if (!s.connectTimer) {
        s.connectTimer = setTimeout(() => {
          s.connectTimer = null;
          attemptConnect(s);
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

  sock.on("error", () => gone(s.socket === sock ? "connection lost" : "could not connect"));
  sock.on("close", () => gone(""));
}

// ------------------------------------------------------------------------------------------
// Target process

function portInUseBySession(port: number, except: Session | null): boolean {
  for (const s of sessions.values()) {
    if (s !== except && s.port === port && (s.target || s.connected)) return true;
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

/** Starts the session's configured executable with the layer enabled and connects to it. */
function spawnTarget(s: Session, layerDir: string): LaunchResult {
  const config = s.config;
  if (!config) return { ok: false, error: "session has no launch configuration" };
  const env: NodeJS.ProcessEnv = {
    ...process.env,
    ...parseEnvLines(config.env ?? ""),
    VK_ADD_LAYER_PATH: layerDir,
    VK_LOADER_LAYERS_ENABLE: LAYER_NAME,
    // Older loaders:
    VK_LAYER_PATH: process.env.VK_LAYER_PATH ? `${layerDir}${path.delimiter}${process.env.VK_LAYER_PATH}` : layerDir,
    VK_INSTANCE_LAYERS: process.env.VK_INSTANCE_LAYERS ? `${LAYER_NAME}${path.delimiter}${process.env.VK_INSTANCE_LAYERS}` : LAYER_NAME,
    VKINSP_PORT: String(s.port),
    VKINSP_LOG: config.log ? "1" : "0",
    // Testing aid: with --debug-log the layer also writes its log to a file (Unity players have
    // no usable stderr).
    ...(cliOption("debug-log") ? { VKINSP_LOG_FILE: `${cliOption("debug-log")}.layer.log` } : {}),
    // Testing aid: with --debug-log the layer also writes its log to a file (Unity players have
    // no usable stderr).
    ...(cliOption("debug-log") ? { VKINSP_LOG_FILE: `${cliOption("debug-log")}.layer.log` } : {}),
    VKINSP_RECORD_ALWAYS: config.recordAlways ? "1" : "0",
  };
  const args = splitArgs(config.args ?? "");
  const cwd = config.cwd && fs.existsSync(config.cwd) ? config.cwd : path.dirname(config.exe);
  s.appendLog(`launching ${config.exe} ${args.join(" ")}`);
  s.appendLog(`layer: ${layerDir}`);
  if (s.port !== config.port) s.appendLog(`port ${config.port} is in use; using ${s.port}`);
  let proc: ChildProcess;
  try {
    proc = spawn(config.exe, args, { cwd, env, stdio: ["ignore", "pipe", "pipe"] });
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

/** Terminates the target and resolves once it has exited (or after a timeout). */
function killTarget(s: Session): Promise<void> {
  const proc = s.target;
  disconnectSession(s);
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

function validateLaunch(config: LaunchConfig): { layerDir: string } | { error: string } {
  const layerDir = findLayerDir();
  if (!layerDir) return { error: "layer not found: build the layer first (see docs/ARCHITECTURE.md)" };
  if (!config.exe || !fs.existsSync(config.exe)) return { error: `executable not found: ${config.exe}` };
  return { layerDir };
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
  const result = spawnTarget(s, v.layerDir);
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
  return spawnTarget(s, v.layerDir);
}

async function closeSession(s: Session): Promise<void> {
  await killTarget(s);
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

function findTool(name: string): string {
  const exe = process.platform === "win32" ? `${name}.exe` : name;
  const candidates: string[] = [];
  if (process.env.INSPECTOR_TOOLS_DIR) candidates.push(path.join(process.env.INSPECTOR_TOOLS_DIR, exe));
  if (process.env.VULKAN_SDK) candidates.push(path.join(process.env.VULKAN_SDK, "Bin", exe), path.join(process.env.VULKAN_SDK, "bin", exe));
  for (const c of candidates) if (fs.existsSync(c)) return c;
  return exe; // hope it is on PATH
}

function shaderText(spirv: Uint8Array, mode: ShaderTextMode): Promise<ShaderTextResult> {
  return new Promise((resolve) => {
    const tmp = path.join(os.tmpdir(), `vkinsp_${process.pid}_${Date.now()}.spv`);
    fs.writeFileSync(tmp, Buffer.from(spirv));
    let tool: string;
    let args: string[];
    if (mode === "dis") {
      tool = findTool("spirv-dis");
      args = ["--comment", "--no-color", tmp];
    } else {
      tool = findTool("spirv-cross");
      args = [tmp];
      if (mode === "hlsl") args.push("--hlsl", "--shader-model", "60");
      else if (mode === "msl") args.push("--msl");
      else args.push("--vulkan-semantics", "--version", "460");
    }
    execFile(tool, args, { maxBuffer: 64 * 1024 * 1024 }, (err, stdout, stderr) => {
      try {
        fs.unlinkSync(tmp);
      } catch {
        // ignore
      }
      if (err) resolve({ ok: false, text: `${path.basename(tool)} failed: ${stderr || err.message}` });
      else resolve({ ok: true, text: stdout });
    });
  });
}

// ------------------------------------------------------------------------------------------
// IPC

ipcMain.handle("inspector:getConfig", (e): AppConfig => {
  const win = windowOf(e.sender);
  return {
    recents: loadRecents(),
    layerDir: findLayerDir(),
    theme: appTheme(),
    windowMode: win === mainWin ? "main" : "session",
    sessions: sessionsOf(win).map((s) => s.info()),
    debug: {
      select: cliOption("debug-select"),
      capture: cliFlag("debug-capture"),
      captureFrames: Number(cliOption("debug-capture")) || 1,
      launchDialog: cliFlag("debug-launch-dialog"),
    },
  };
});
ipcMain.handle("inspector:setTheme", (_e, theme: ThemeName) => {
  setTheme(theme);
  return true;
});
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
ipcMain.handle("inspector:chooseFile", async (e, opts?: { title?: string; directory?: boolean }) => {
  const win = windowOf(e.sender) ?? mainWin;
  if (!win) return null;
  const r = await dialog.showOpenDialog(win, {
    title: opts?.title ?? "Choose executable",
    properties: opts?.directory ? ["openDirectory"] : ["openFile"],
    filters: opts?.directory ? [] : (process.platform === "win32" ? [{ name: "Executables", extensions: ["exe"] }, { name: "All files", extensions: ["*"] }] : []),
  });
  return r.canceled ? null : r.filePaths[0];
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
    const img = await win.webContents.capturePage();
    // Main window -> <file>; other windows -> <file minus extension>.<n>.png
    const target = win === mainWin ? file : file.replace(/(\.[^.]+)?$/, `.${++index}$1`);
    fs.writeFileSync(target, img.toPNG());
    console.log(`screenshot written: ${target}`);
  }
}

void app.whenReady().then(() => {
  migrateSettings();
  if (process.platform === "darwin") app.dock?.setIcon(appIconPath());
  mainWin = createMainWindow();
  mainWin.webContents.on("did-finish-load", () => {
    const exe = cliOption("launch");
    if (exe) {
      const config = normalizeLaunch({
        exe,
        args: cliOption("args") ?? "",
        port: Number(cliOption("port")) || DEFAULT_PORT,
        recordAlways: cliFlag("record-always"),
      });
      setTimeout(() => void launch(config), 300);
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
    // Testing aid: switch the theme through the same path the picker uses.
    const debugTheme = cliOption("debug-theme");
    if (isTheme(debugTheme)) setTimeout(() => setTheme(debugTheme), 1000);
    const shot = cliOption("screenshot");
    if (shot) {
      setTimeout(async () => {
        // Testing aid: --debug-mouse=x,y moves the mouse over the main window before the shot.
        const mouse = cliOption("debug-mouse")?.split(",").map(Number);
        if (mouse && mouse.length === 2 && mainWin) {
          mainWin.webContents.sendInputEvent({ type: "mouseEnter", x: mouse[0], y: mouse[1] });
          mainWin.webContents.sendInputEvent({ type: "mouseMove", x: mouse[0], y: mouse[1] });
          await new Promise((r) => setTimeout(r, 100));
          mainWin.webContents.sendInputEvent({ type: "mouseDown", x: mouse[0], y: mouse[1], button: "left", clickCount: 1 });
          mainWin.webContents.sendInputEvent({ type: "mouseUp", x: mouse[0], y: mouse[1], button: "left", clickCount: 1 });
          mainWin.webContents.sendInputEvent({ type: "mouseMove", x: mouse[0] + 2, y: mouse[1] + 2 });
          await new Promise((r) => setTimeout(r, 300));
        }
        await writeScreenshots(shot);
        if (cliFlag("quit-after-screenshot")) {
          killAllTargets();
          app.quit();
        }
      }, Number(cliOption("screenshot-delay")) || 4000);
    }
  });
});

app.on("before-quit", () => killAllTargets());

app.on("window-all-closed", () => {
  killAllTargets();
  app.quit();
});
