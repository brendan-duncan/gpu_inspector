// Live applications the MCP server drives. An application is launched with GPU Inspector's capture
// library in it (the Vulkan layer's environment, or the Metal library injected on macOS), an
// Android package is started over adb with the layer enabled for it (main/android.ts), or an
// application already listening is attached to on its port. The capture library's socket feeds
// an ObjectDatabase as it feeds the app's session: live objects, frame statistics, validation
// messages. A capture is requested, streamed into a CaptureData and saved as a .gpucap file, which
// the capture tools then read like any other.
import { spawn, type ChildProcess } from "node:child_process";
import fs from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { AndroidTarget, disableLayer, findAdb, findAndroidLayer, listDevices, type AndroidLayerFiles } from "../main/android.js";
import { FrameReader, encodeRequest } from "../main/layer_protocol.js";
import { DEFAULT_PORT, findFreePort, findLayerDir, findValidationLayerDir, splitArgs, terminate, vulkanLayerEnvironment } from "../main/launch_env.js";
import { captureEnvironment, findCaptureLibrary, injectionBlockedReason, resolveExecutable } from "../main/metal.js";
import { CaptureData } from "../renderer/capture_data.js";
import { serializeCapture } from "../renderer/capture_file.js";
import { captureFileName } from "../renderer/capture_format.js";
import { resolveSymbols } from "../renderer/stack_requests.js";
import { ObjectDatabase } from "../renderer/vulkan/object_database.js";
import { isObject, str } from "../renderer/vulkan/vulkan_object.js";
import type { AndroidDevice, CaptureRequest, FrameStatsMessage, ImageDataMessage, LayerMessage, ShaderReplacedMessage, UiRequest } from "../shared/protocol.js";
import { symbolizeSymbolMap } from "./search_paths.js";

const MAX_LOG_LINES = 2000;
/** A minute of the capture library's frame reports (one every 100 ms). */
const MAX_FRAME_STATS = 600;
/** How long a capture's stream stays silent before it counts as complete, for a capture library that does not send CaptureComplete. */
const DEFAULT_QUIET_MS = 2000;
const SNAPSHOT_TIMEOUT_MS = 10000;
const KILL_TIMEOUT_MS = 3000;

/** The messages a capture streams, which CaptureData reassembles. */
const CAPTURE_ACTIONS = new Set(["CaptureFrameResults", "CaptureFrameCommands", "CaptureTextureFrames", "CaptureTextureData", "CaptureBuffers", "CaptureBufferData", "CapturePassTimings"]);

const sleep = (ms: number): Promise<void> => new Promise((resolve) => setTimeout(resolve, ms));

export type SessionState = "connecting" | "connected" | "disconnected" | "exited" | "error";

export interface LaunchOptions {
  exe: string;
  args?: string | string[];
  cwd?: string;
  env?: Record<string, string>;
  port?: number;
  validation?: boolean;
  syncValidation?: boolean;
  stacktraces?: boolean;
  recordAlways?: boolean;
  /** The directory holding VK_LAYER_INSPECTOR_capture.json, when it is not found by itself. */
  layerDir?: string;
}

export interface CaptureOptions {
  frames: number;
  atFrame?: number;
  profilePasses: boolean;
  renderTargets: boolean;
  buffers: boolean;
  images: boolean;
  stacktraces: boolean;
  maxBufferBytes: number;
  timeoutMs: number;
}

export interface CaptureResult {
  data: CaptureData;
  /** "marker": the capture library said the capture was complete; "quiet": its stream went silent. */
  completion: "marker" | "quiet";
  elapsedMs: number;
}

/** Where captures are saved when no path is given: GPU_INSPECTOR_CAPTURES_DIR, else a directory under the system's temporary directory. */
export function capturesDir(): string {
  return process.env.GPU_INSPECTOR_CAPTURES_DIR ?? path.join(os.tmpdir(), "gpu-inspector-captures");
}

/** Checkouts whose build tree may hold the capture libraries: GPU_INSPECTOR_ROOT, and the checkout this server was built in. */
function checkoutRoots(): string[] {
  const roots: string[] = [];
  if (process.env.GPU_INSPECTOR_ROOT) roots.push(process.env.GPU_INSPECTOR_ROOT);
  // In a checkout the bundle is claude-plugin/server/gpu-inspector-mcp.mjs.
  roots.push(path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", ".."));
  return roots;
}

/** Where installed builds of GPU Inspector keep their capture libraries (electron-builder's defaults). */
function installedLayerDirs(): string[] {
  const home = os.homedir();
  if (process.platform === "win32") {
    const local = process.env.LOCALAPPDATA ?? path.join(home, "AppData", "Local");
    const apps = [path.join(local, "Programs", "gpu-inspector"), path.join(local, "Programs", "GPU Inspector")];
    for (const programFiles of [process.env.ProgramFiles, process.env["ProgramFiles(x86)"]]) {
      if (programFiles) apps.push(path.join(programFiles, "GPU Inspector"));
    }
    return apps.map((dir) => path.join(dir, "resources", "layer"));
  }
  if (process.platform === "darwin") {
    return ["/Applications", path.join(home, "Applications")].map((dir) => path.join(dir, "GPU Inspector.app", "Contents", "Resources", "layer"));
  }
  return ["/opt/GPU Inspector/resources/layer", "/opt/gpu-inspector/resources/layer"];
}

/** The Android layer (tools/build_android.py): INSPECTOR_ANDROID_LAYER_DIR, a checkout's build tree, or an installed GPU Inspector. */
function androidLayer(): AndroidLayerFiles | null {
  const candidates = [
    process.env.INSPECTOR_ANDROID_LAYER_DIR,
    ...checkoutRoots().map((root) => path.join(root, "build", "android")),
    ...installedLayerDirs().map((dir) => path.join(dir, "android")),
  ].filter((d): d is string => !!d);
  return findAndroidLayer(candidates);
}

export interface AndroidLaunchOptions {
  package: string;
  /** The device's serial; the only connected device when absent. */
  device?: string;
  /** The activity to start; the package's launcher activity when absent. */
  activity?: string;
  port?: number;
  stacktraces?: boolean;
  recordAlways?: boolean;
}

/** The application name a Vulkan application gave its instance, when it gave one. */
function applicationName(db: ObjectDatabase): string | null {
  for (const o of db.objectsByType.get("VkInstance")?.values() ?? []) {
    const info = o.descriptor?.pApplicationInfo;
    const name = isObject(info) ? str(info.pApplicationName) : "";
    if (name) return name;
  }
  return null;
}

export class LiveSession {
  readonly database = new ObjectDatabase();
  readonly log: string[] = [];
  /** Frame reports with the time each arrived. */
  readonly frameStats: { at: number; msg: FrameStatsMessage }[] = [];
  readonly startedAt = Date.now();
  state: SessionState = "connecting";
  detail = "";
  pid: number | null = null;
  exitCode: string | null = null;
  private _proc: ChildProcess | null = null;
  private _socket: net.Socket | null = null;
  private _listeners = new Set<(msg: LayerMessage) => void>();
  private _capturing = false;
  /** Set while stop() terminates the application, so its exit reads as that rather than as a crash. */
  private _stopping = false;
  /** The launched application is gone: it exited, failed to start, or was stopped. */
  private _ended = false;
  /**
   * A launched target that is not a child process of this server (an Android application): how
   * to stop it, and how to repair the way to it when connections are refused (a lost adb forward).
   */
  remote: { stop(): Promise<void>; repair?(): Promise<unknown> } | null = null;

  constructor(readonly id: string, public name: string, readonly port: number, readonly launched: boolean) {
    const db = this.database;
    db.onFrameStats.addListener((msg) => {
      this.frameStats.push({ at: Date.now(), msg });
      if (this.frameStats.length > MAX_FRAME_STATS) this.frameStats.splice(0, this.frameStats.length - MAX_FRAME_STATS);
    });
    db.onValidationMessage.addListener((entry, isNew) => {
      if (isNew) this.appendLog(`validation ${entry.severity}${entry.idName ? ` ${entry.idName}` : ""}: ${entry.message.split("\n")[0].slice(0, 300)}`);
    });
    db.onLeakReport.addListener((r) => this.appendLog(`leak report: ${r.ownerClass} ${r.owner} destroyed with ${r.count} live objects`));
    db.onOtherMessage.addListener((msg) => {
      if (msg.action === "ShaderReplaced") {
        this.appendLog(`shader edit: pipeline ${msg.pipeline} ${msg.stage}: ${msg.ok ? (msg.replacement ? `applied as object ${msg.replacement}` : "restored") : `failed: ${msg.error ?? "unknown error"}`}`);
      }
    });
  }

  get connected(): boolean {
    return this._socket !== null && !this._socket.destroyed;
  }

  /** The API the capture library reports objects of; null before any arrived. */
  get api(): "vulkan" | "metal" | null {
    for (const type of this.database.objectsByType.keys()) if (type.startsWith("MTL")) return "metal";
    return this.database.allObjects.size ? "vulkan" : null;
  }

  appendLog(line: string): void {
    this.log.push(line);
    if (this.log.length > MAX_LOG_LINES) this.log.splice(0, this.log.length - MAX_LOG_LINES);
  }

  private setState(state: SessionState, detail = ""): void {
    this.state = state;
    this.detail = detail;
    this.appendLog(`[${state}]${detail ? ` ${detail}` : ""}`);
  }

  send(msg: UiRequest): Promise<boolean> {
    if (!this._socket || this._socket.destroyed) return Promise.resolve(false);
    this._socket.write(encodeRequest(msg));
    return Promise.resolve(true);
  }

  /** Hears every message from the capture library, after the object database; returns the unsubscribe. */
  onMessage(listener: (msg: LayerMessage) => void): () => void {
    this._listeners.add(listener);
    return () => { this._listeners.delete(listener); };
  }

  /** The first message `match` accepts (its return value), or null after `timeoutMs`. */
  waitFor<T>(match: (msg: LayerMessage) => T | undefined, timeoutMs: number): Promise<T | null> {
    return new Promise((resolve) => {
      const off = this.onMessage((msg) => {
        const hit = match(msg);
        if (hit === undefined) return;
        clearTimeout(timer);
        off();
        resolve(hit);
      });
      const timer = setTimeout(() => {
        off();
        resolve(null);
      }, timeoutMs);
    });
  }

  /** Starts the application; its output goes to the session's log. */
  startProcess(exe: string, args: string[], cwd: string, env: NodeJS.ProcessEnv): void {
    const proc = spawn(exe, args, { cwd, env, stdio: ["ignore", "pipe", "pipe"] });
    this._proc = proc;
    this.pid = proc.pid ?? null;
    for (const stream of [proc.stdout, proc.stderr]) {
      let rest = "";
      stream?.on("data", (d: Buffer) => {
        rest += d.toString("utf8");
        const lines = rest.split(/\r?\n/);
        rest = lines.pop() ?? "";
        for (const line of lines) if (line.length) this.appendLog(line);
      });
    }
    proc.on("exit", (code, signal) => {
      if (this._proc !== proc) return;
      this._proc = null;
      this.pid = null;
      this._ended = true;
      this.exitCode = String(signal ?? code);
      this._disconnect();
      this.setState("exited", this._stopping ? "terminated by stop_app" : `code ${this.exitCode}`);
    });
    proc.on("error", (e) => {
      if (this._proc !== proc) return;
      this._proc = null;
      this.pid = null;
      this._disconnect();
      this._ended = true;
      this.setState("error", e.message);
    });
  }

  /**
   * Connects to the capture library, retrying until it answers, the launched process exits, or
   * `timeoutMs` passes; resolves once the snapshot of live objects that follows a connection is in.
   */
  async connect(timeoutMs: number): Promise<boolean> {
    const deadline = Date.now() + timeoutMs;
    this.setState("connecting", `port ${this.port}`);
    let attempts = 0;
    while (Date.now() < deadline) {
      if (this._ended) return false;
      const sock = await this._tryConnect();
      attempts++;
      if (sock) {
        const snapshot = this._waitForSnapshot(SNAPSHOT_TIMEOUT_MS, sock);
        this._attach(sock);
        // Closed before the capture library said anything: adb accepts a forwarded connection
        // before the layer listens on the device, so that is not an answer yet.
        if ((await snapshot) !== "closed" || this.connected) {
          const name = applicationName(this.database);
          if (name && !this.launched) this.name = name;
          return this.connected;
        }
      } else if (this.remote?.repair && attempts % 4 === 0) {
        // Refused: adb may have lost the port forward (the device reconnected).
        await this.remote.repair().catch(() => undefined);
      }
      await sleep(this.launched && !this.remote ? 250 : 500);
    }
    this.setState("disconnected", `nothing answered on port ${this.port}`);
    return false;
  }

  private _tryConnect(): Promise<net.Socket | null> {
    return new Promise((resolve) => {
      const sock = net.createConnection({ host: "127.0.0.1", port: this.port });
      sock.once("connect", () => {
        sock.removeAllListeners("error");
        resolve(sock);
      });
      sock.once("error", () => {
        sock.destroy();
        resolve(null);
      });
    });
  }

  private _attach(sock: net.Socket): void {
    sock.setNoDelay(true);
    this._socket = sock;
    const reader = new FrameReader();
    let heard = false;
    sock.on("data", (chunk: Buffer) => {
      // Connected once the capture library speaks, not when the socket opens (adb accepts first).
      if (!heard) {
        heard = true;
        this.setState("connected", `port ${this.port}`);
      }
      const messages = reader.push(chunk, (e) => this.appendLog(`bad ${e.kind === "json" ? "JSON" : "binary header"} from the capture library: ${e.error}`));
      for (const msg of messages) {
        this.database.handleMessage(msg);
        for (const listener of [...this._listeners]) listener(msg);
      }
    });
    const gone = (): void => {
      if (this._socket !== sock) return;
      this._socket = null;
      if (this.state === "connected") this.setState("disconnected", "the connection closed (the application exited, or another client connected to it)");
    };
    sock.on("error", gone);
    sock.on("close", gone);
    void this.send({ action: "Ping" });
  }

  /**
   * Resolves when the snapshot the capture library sends on connection has arrived, when the
   * socket closes first ("closed" if no snapshot had begun), or after `timeoutMs`.
   */
  private _waitForSnapshot(timeoutMs: number, sock: net.Socket): Promise<"snapshot" | "closed" | "timeout"> {
    const db = this.database;
    return new Promise((resolve) => {
      let started = false;
      const done = (outcome: "snapshot" | "closed" | "timeout"): void => {
        clearTimeout(timer);
        db.onSnapshotBegin.disconnect(begin);
        db.onAddObject.disconnect(add);
        sock.off("close", closed);
        resolve(outcome);
      };
      const begin = (count: number): void => {
        started = true;
        if (count === 0) done("snapshot");
      };
      const add = (_object: unknown, inSnapshot: boolean): void => {
        if (started && !inSnapshot) done("snapshot");
      };
      const closed = (): void => done(started ? "snapshot" : "closed");
      const timer = setTimeout(() => done("timeout"), timeoutMs);
      db.onSnapshotBegin.addListener(begin);
      db.onAddObject.addListener(add);
      sock.once("close", closed);
    });
  }

  private _disconnect(): void {
    const sock = this._socket;
    this._socket = null;
    sock?.destroy();
  }

  /**
   * Requests a capture and waits for all of it: until the capture library marks its end, or, for
   * one built before that marker existed, until the stream has been silent for a while after the
   * commands and buffers are in.
   */
  async capture(o: CaptureOptions): Promise<CaptureResult> {
    if (!this.connected) throw new Error(`${this.id} is not connected (${this.state}${this.detail ? `: ${this.detail}` : ""}).`);
    if (this._capturing) throw new Error(`${this.id} is already capturing.`);
    this._capturing = true;
    const data = new CaptureData();
    const started = Date.now();
    let commandsComplete = false;
    let marker = false;
    let lastTraffic = 0;
    const onCommands = (): void => { commandsComplete = true; };
    data.onCommandsComplete.addListener(onCommands);
    const off = this.onMessage((msg) => {
      if (msg.action === "CaptureComplete") {
        marker = true;
      } else if (CAPTURE_ACTIONS.has(msg.action)) {
        lastTraffic = Date.now();
        data.handleMessage(msg);
      }
    });
    const quietMs = Number(process.env.GPU_INSPECTOR_CAPTURE_QUIET_MS) || DEFAULT_QUIET_MS;
    try {
      const request: CaptureRequest = {
        action: "Capture", frameCount: o.frames, ...(o.atFrame !== undefined ? { atFrame: o.atFrame } : {}),
        captureTextures: o.renderTargets, captureBuffers: o.buffers, captureImages: o.images,
        profilePasses: o.profilePasses, stacktraces: o.stacktraces, maxBufferSize: o.maxBufferBytes,
      };
      await this.send(request);
      for (;;) {
        await sleep(50);
        const now = Date.now();
        if (marker) return { data, completion: "marker", elapsedMs: now - started };
        if (commandsComplete && lastTraffic && now - lastTraffic >= quietMs && !data.buffersLoading) return { data, completion: "quiet", elapsedMs: now - started };
        if (!this.connected) {
          throw new Error(data.commands.length ? "The connection was lost while the capture was streaming." : "The connection was lost before the capture arrived.");
        }
        if (now - started > o.timeoutMs) {
          throw new Error(lastTraffic
            ? `The capture did not finish streaming within ${o.timeoutMs / 1000} s.`
            : `No capture arrived within ${o.timeoutMs / 1000} s: a capture starts at ${o.atFrame !== undefined ? `frame ${o.atFrame}` : "the next frame"}, so the application may not be rendering (minimized, paused, or waiting).`);
        }
      }
    } finally {
      off();
      data.onCommandsComplete.disconnect(onCommands);
      this._capturing = false;
    }
  }

  /** Saves a capture with the objects it references, fetching their shaders from the capture library; returns the file. */
  async saveCapture(data: CaptureData, file?: string): Promise<string> {
    // Frames the capture library names by module and offset only are resolved on this machine first.
    const bytes = await serializeCapture(this, data, {
      resolveSymbols: (addresses) => resolveSymbols(this, addresses, (frames) => symbolizeSymbolMap(this.database, frames)),
    });
    let target: string;
    if (file) {
      target = path.resolve(file);
      fs.mkdirSync(path.dirname(target), { recursive: true });
    } else {
      const dir = capturesDir();
      fs.mkdirSync(dir, { recursive: true });
      const name = captureFileName(this.name, data.frame, data.frames);
      target = path.join(dir, name);
      for (let n = 2; fs.existsSync(target); n++) target = path.join(dir, name.replace(/\.gpucap$/, `_${n}.gpucap`));
    }
    fs.writeFileSync(target, bytes);
    return target;
  }

  /**
   * Rebuilds a pipeline with one stage's code replaced (Vulkan); the layer's answer, or null without
   * one. The request names the stage by its flag, the answer by the layer's stage name ("fragment").
   */
  async replaceShader(pipeline: number, stageFlag: string, stageName: string, spirv: Uint8Array, timeoutMs = 15000): Promise<ShaderReplacedMessage | null> {
    const answer = this.waitFor((msg) => (msg.action === "ShaderReplaced" && msg.pipeline === pipeline && (msg.stage === stageName || msg.stage === stageFlag) ? msg : undefined), timeoutMs);
    await this.send({ action: "ReplaceShader", pipeline, stage: stageFlag, spirv: Buffer.from(spirv).toString("base64") });
    return answer;
  }

  /** Drops the replacement of one stage (or every stage) of a pipeline. */
  async restoreShader(pipeline: number, stageFlag: string | undefined, timeoutMs = 15000): Promise<ShaderReplacedMessage | null> {
    const answer = this.waitFor((msg) => (msg.action === "ShaderReplaced" && msg.pipeline === pipeline ? msg : undefined), timeoutMs);
    await this.send({ action: "RestoreShader", pipeline, ...(stageFlag ? { stage: stageFlag } : {}) });
    return answer;
  }

  /** One subresource of a live image, which the capture library reads back at the application's next frame; null without an answer. */
  async readImage(id: number, mip: number, layer: number, timeoutMs: number): Promise<ImageDataMessage | null> {
    const answer = this.waitFor((msg) => (msg.action === "ImageData" && msg.id === id ? msg : undefined), timeoutMs);
    await this.send({ action: "RequestImage", id, mip, layer });
    return answer;
  }

  /** Reads a descriptor set's current contents into its object's updates (`bindings`); false without an answer. */
  async readDescriptorSet(id: number, timeoutMs = 10000): Promise<boolean> {
    const answer = this.waitFor((msg) => (msg.action === "ObjectUpdate" && msg.id === id && "bindings" in msg ? true : undefined), timeoutMs);
    await this.send({ action: "RequestDescriptorSet", id });
    return (await answer) ?? false;
  }

  /** A remote target ended: it exited on its own, failed to start, or was stopped. */
  remoteEnded(state: "exited" | "error", detail: string): void {
    if (this._ended) return;
    this._ended = true;
    this.pid = null;
    this._disconnect();
    this.setState(state, detail);
  }

  /** Terminates a launched application; an attached one is only disconnected. */
  async stop(): Promise<void> {
    const proc = this._proc;
    const remote = this.remote;
    this._disconnect();
    if (remote) {
      this.remote = null;
      if (!this._ended) {
        this._stopping = true;
        await remote.stop().catch(() => undefined);
        this.remoteEnded("exited", "terminated by stop_app");
      }
      return;
    }
    if (!proc) {
      if (this.state === "connected" || this.state === "connecting") this.setState("disconnected", "detached");
      return;
    }
    this._stopping = true;
    await new Promise<void>((resolve) => {
      const timer = setTimeout(resolve, KILL_TIMEOUT_MS);
      proc.once("exit", () => {
        clearTimeout(timer);
        resolve();
      });
      try {
        terminate(proc);
      } catch {
        clearTimeout(timer);
        resolve();
      }
    });
  }
}

export class SessionManager {
  private readonly _sessions = new Map<string, LiveSession>();
  private _counter = 0;
  private _latest: LiveSession | null = null;

  /** Launches an application with the capture library in it and waits for it to connect. */
  async launch(o: LaunchOptions, waitMs: number): Promise<LiveSession> {
    const requested = path.resolve(o.exe);
    if (!fs.existsSync(requested)) throw new Error(`No executable at ${requested}.`);
    const args = Array.isArray(o.args) ? o.args : splitArgs(o.args ?? "");
    const taken = new Set([...this._sessions.values()].filter((s) => s.connected || s.pid !== null).map((s) => s.port));
    const port = await findFreePort(o.port ?? DEFAULT_PORT, (p) => taken.has(p));
    let exe = requested;
    let env: NodeJS.ProcessEnv;
    let note: string;
    if (process.platform === "darwin") {
      const library = findCaptureLibrary(checkoutRoots(), installedLayerDirs());
      if (!library) throw new Error("The Metal capture library (libmtlinsp_capture.dylib) was not found: build it in the GPU Inspector checkout, install GPU Inspector, or set INSPECTOR_METAL_LIB.");
      exe = resolveExecutable(requested);
      const blocked = injectionBlockedReason(exe);
      if (blocked) throw new Error(blocked);
      env = { ...process.env, ...o.env, ...captureEnvironment(library, port, true, !!o.validation, o.stacktraces ?? true) };
      note = `capture library: ${library}`;
    } else {
      const layerDir = o.layerDir ?? findLayerDir(checkoutRoots(), installedLayerDirs());
      if (!layerDir) {
        throw new Error("The GPU Inspector Vulkan layer was not found: build it (see GPU Inspector's README), install GPU Inspector, or pass layerDir (or set INSPECTOR_LAYER_DIR) to the directory holding VK_LAYER_INSPECTOR_capture.json.");
      }
      const validationDir = o.validation ? findValidationLayerDir() : null;
      env = {
        ...process.env, ...o.env,
        ...vulkanLayerEnvironment({
          layerDir, validationDir, port, log: true, recordAlways: !!o.recordAlways, stacktraces: o.stacktraces ?? true,
          validation: !!o.validation, syncValidation: !!o.syncValidation,
        }),
      };
      note = `layer: ${layerDir}${o.validation ? (validationDir ? `; validation layer: ${validationDir}` : "; validation layer not found (install the Vulkan SDK or set VULKAN_SDK)") : ""}`;
    }
    const session = new LiveSession(`app-${++this._counter}`, `${path.basename(requested)}${args.length ? ` ${args.join(" ")}` : ""}`, port, true);
    session.appendLog(`launching ${exe} ${args.join(" ")}`);
    session.appendLog(note);
    this._sessions.set(session.id, session);
    this._latest = session;
    session.startProcess(exe, args, o.cwd && fs.existsSync(o.cwd) ? o.cwd : path.dirname(exe), env);
    if (await session.connect(waitMs) && o.recordAlways) await session.send({ action: "Settings", recordAlways: true });
    return session;
  }

  /** The Android devices adb sees, and where the Android layer is; adb null when it was not found. */
  async androidDevices(): Promise<{ adb: string | null; devices: AndroidDevice[]; layer: string | null }> {
    const adb = findAdb();
    return { adb, devices: adb ? await listDevices(adb) : [], layer: androidLayer()?.dir ?? null };
  }

  /**
   * Starts an Android package on a device with the layer installed and enabled for it, and waits
   * for the layer to connect over the adb forward. A launch that fails on the device is reported
   * through the session's state and log rather than thrown.
   */
  async launchAndroid(o: AndroidLaunchOptions, waitMs: number): Promise<LiveSession> {
    const adb = findAdb();
    if (!adb) throw new Error("adb was not found: install the Android SDK platform-tools, or set ANDROID_HOME or INSPECTOR_ADB.");
    const layer = androidLayer();
    if (!layer) {
      throw new Error("The Android layer was not found: build it with tools/build_android.py in the GPU Inspector checkout (it needs the Android NDK), install GPU Inspector, or set INSPECTOR_ANDROID_LAYER_DIR.");
    }
    const devices = await listDevices(adb);
    const listed = devices.length ? devices.map((d) => `${d.serial} (${d.state}${d.model ? `, ${d.model}` : ""})`).join(", ") : "none";
    let device: AndroidDevice | undefined;
    if (o.device) {
      device = devices.find((d) => d.serial === o.device);
      if (!device) throw new Error(`No device ${o.device}: adb lists ${listed}.`);
      if (device.state !== "device") throw new Error(`${o.device} is ${device.state}${device.state === "unauthorized" ? ": accept the USB debugging prompt on the device" : ""}.`);
    } else {
      const usable = devices.filter((d) => d.state === "device");
      if (usable.length !== 1) {
        throw new Error(usable.length ? `${usable.length} devices are connected (${listed}): pass device.` : `No Android device is connected and authorized (adb lists ${listed}).`);
      }
      device = usable[0];
    }
    const taken = new Set([...this._sessions.values()].filter((s) => s.connected || s.pid !== null).map((s) => s.port));
    const port = await findFreePort(o.port ?? DEFAULT_PORT, (p) => taken.has(p));
    const serial = device.serial;
    const session = new LiveSession(`app-${++this._counter}`, `${o.package} (Android, ${device.model || serial})`, port, true);
    this._sessions.set(session.id, session);
    this._latest = session;
    const target = new AndroidTarget({
      adb, serial, package: o.package, activity: o.activity ?? "", port, log: true,
      recordAlways: !!o.recordAlways, stacktraces: o.stacktraces ?? true, layer,
      onLog: (line) => session.appendLog(line),
      onExit: () => session.remoteEnded("exited", "the application exited on the device"),
    });
    // Stopping also turns the debug layer settings off, or the package would load the layer again when started from the device.
    const stop = async (): Promise<void> => {
      await target.stop();
      await disableLayer(adb, serial);
    };
    session.remote = { stop, repair: () => target.ensureForward() };
    session.appendLog(`launching ${o.package} on ${serial} (${device.model || "unknown model"}, Android API ${device.sdk}, ${device.abi})`);
    try {
      await target.start();
    } catch (e) {
      session.remote = null;
      await stop().catch(() => undefined);
      session.remoteEnded("error", e instanceof Error ? e.message : String(e));
      return session;
    }
    session.pid = target.pid;
    await session.connect(waitMs);
    return session;
  }

  /** Attaches to an application whose capture library already listens on `port`. */
  async attach(port: number, waitMs: number): Promise<LiveSession> {
    const session = new LiveSession(`app-${++this._counter}`, `port ${port}`, port, false);
    if (!(await session.connect(waitMs))) {
      throw new Error(`Nothing answered on port ${port} within ${waitMs / 1000} s. An application listens there when it was started with GPU Inspector's capture library (VKINSP_PORT, or MTLINSP_PORT on macOS).`);
    }
    this._sessions.set(session.id, session);
    this._latest = session;
    return session;
  }

  /** A session by id, or the one started most recently. */
  get(id: string | undefined): LiveSession {
    if (!id) {
      if (!this._latest) throw new Error("No live session: launch_app starts an application with the capture library, attach_app connects to one already running.");
      return this._latest;
    }
    const s = this._sessions.get(id);
    if (!s) throw new Error(`No live session "${id}". ${this._sessions.size ? `Sessions: ${[...this._sessions.keys()].join(", ")}.` : "There are none."}`);
    return s;
  }

  list(): LiveSession[] {
    return [...this._sessions.values()];
  }

  async stopAll(): Promise<void> {
    await Promise.all([...this._sessions.values()].map((s) => s.stop()));
  }
}
