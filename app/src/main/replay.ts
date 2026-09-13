// Running vkinsp_replay (replay/, docs/REPLAY.md) from GPU Inspector and its MCP server: finding the
// built tool, and replaying a Vulkan capture for an analysis (overdraw, a pixel's history, draw
// overlays, vertex shader outputs, per-draw timing). A Metal capture measures overdraw while it is
// taken (metal/src/overdraw.h); a Vulkan capture has to be replayed on this machine's GPU.
//
// Analyses of a capture go to a replay kept alive for it (`vkinsp_replay --serve`, ReplayServer):
// its device and objects are created once, and each analysis replays only the frame, in tens of
// milliseconds rather than the fraction of a second a fresh process takes.
import { spawn, type ChildProcess } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

export const REPLAY_TOOL = process.platform === "win32" ? "vkinsp_replay.exe" : "vkinsp_replay";

/**
 * The replay tool: INSPECTOR_REPLAY, a checkout's build tree (`roots`), or beside the layer of a
 * packaged GPU Inspector (`layerDirs`, where app/tools/stage_layer.mjs puts it).
 */
export function findReplayTool(roots: string[], layerDirs: string[]): string | null {
  const candidates = [
    process.env.INSPECTOR_REPLAY,
    ...roots.flatMap((root) => ["Release", "RelWithDebInfo", "Debug", ""].map((config) => path.join(root, "build", "bin", config, REPLAY_TOOL))),
    ...layerDirs.map((dir) => path.join(dir, REPLAY_TOOL)),
  ].filter((f): f is string => !!f);
  return candidates.find((f) => fs.existsSync(f)) ?? null;
}

export const NO_REPLAY_TOOL = `${REPLAY_TOOL} not found. Build it (cmake --build build --target vkinsp_replay), or set INSPECTOR_REPLAY to its path.`;

/** What a replay wrote for the analysis it was asked for. */
export interface ReplayRun {
  /** The data file the tool wrote, or null. */
  data: Uint8Array | null;
  /** Why there is no data. */
  error?: string;
  /** The end of what the tool printed. */
  output: string;
  /** The main process does not have the capture the request names: send it again with its bytes. */
  needData?: boolean;
}

/** A replay measuring every pass's overdraw: `data` is its --overdraw-data file (parseOverdrawFile in renderer/overdraw.ts). */
export type OverdrawRun = ReplayRun;

/** The pixel a history follows: an image by its capture id, and a subresource of it. */
export interface PixelRequest {
  image: number;
  x: number;
  y: number;
  mip?: number;
  layer?: number;
}

/** What to replay for: every pass's overdraw, every draw's timing, some draws' overlays or vertex outputs, or one pixel's history. */
export type ReplayAnalysis =
  | { kind: "overdraw" } | { kind: "draws" } | { kind: "overlay"; commands: number[] } | { kind: "mesh"; commands: number[] }
  | ({ kind: "pixel" } & PixelRequest);

/** The last lines of the tool's output, for an error message. */
function tail(text: string, lines = 12): string {
  return text.trim().split(/\r?\n/).slice(-lines).join("\n");
}

function analysisArgs(analysis: ReplayAnalysis, out: string): string[] {
  if (analysis.kind === "overdraw") return ["--overdraw-data", out];
  if (analysis.kind === "draws") return ["--draw-data", out];
  if (analysis.kind === "overlay" || analysis.kind === "mesh") {
    const flag = `--${analysis.kind}`;
    return [...analysis.commands.flatMap((c) => [flag, String(Math.max(0, Math.floor(c)))]), `${flag}-data`, out];
  }
  const n = (v: number | undefined): string => String(Math.max(0, Math.floor(v ?? 0)));
  return ["--pixel", n(analysis.image), n(analysis.x), n(analysis.y), "--mip", n(analysis.mip), "--layer", n(analysis.layer), "--pixel-data", out];
}

/** Replays a capture file for an analysis and reads the data file the tool wrote. */
export function runReplay(tool: string, capturePath: string, analysis: ReplayAnalysis, timeoutMs = 10 * 60 * 1000): Promise<ReplayRun> {
  return new Promise((resolve) => {
    const out = path.join(os.tmpdir(), `vkinsp_${analysis.kind}_${process.pid}_${Date.now()}_${Math.random().toString(36).slice(2)}.bin`);
    let output = "";
    let done = false;
    let timedOut = false;
    const child = spawn(tool, [capturePath, ...analysisArgs(analysis, out)], { stdio: ["ignore", "pipe", "pipe"], windowsHide: true });
    const timer = setTimeout(() => {
      timedOut = true;
      child.kill();
    }, timeoutMs);
    const collect = (chunk: Buffer): void => {
      output += chunk.toString();
      if (output.length > 256 * 1024) output = output.slice(-128 * 1024);
    };
    child.stdout?.on("data", collect);
    child.stderr?.on("data", collect);
    const finish = (error: string | null): void => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      let data: Uint8Array | null = null;
      try {
        data = new Uint8Array(fs.readFileSync(out));
        fs.unlinkSync(out);
      } catch {
        // not written
      }
      if (data) {
        resolve({ data, output: tail(output) });
        return;
      }
      resolve({
        data: null, output: tail(output),
        error: error ?? (timedOut ? `the replay did not finish within ${Math.round(timeoutMs / 1000)} s` : `the replay wrote no data:\n${tail(output)}`),
      });
    };
    child.on("error", (e) => finish(`could not run ${tool}: ${e.message}`));
    child.on("close", () => finish(null));
  });
}

// ---------------------------------------------------------------------------------------------
// Replays kept alive

/** One answer of `vkinsp_replay --serve` (Serve in replay/src/main.cpp). */
interface ServeAnswer {
  id?: number;
  ready?: boolean;
  ok?: boolean;
  error?: string;
  device?: string;
  ms?: number;
}

/** The request line for an analysis. */
function serveRequest(id: number, analysis: ReplayAnalysis, out: string): Record<string, unknown> {
  if (analysis.kind === "pixel") {
    return { id, kind: "pixel", image: analysis.image, x: analysis.x, y: analysis.y, mip: analysis.mip ?? 0, layer: analysis.layer ?? 0, out };
  }
  return { id, ...analysis, out };
}

function tempOutput(kind: string): string {
  return path.join(os.tmpdir(), `vkinsp_${kind}_${process.pid}_${Date.now()}_${Math.random().toString(36).slice(2)}.bin`);
}

/**
 * `vkinsp_replay <capture> --serve` for one capture file: the device and the capture's objects are
 * created once, and each analysis replays the frame again in the same process. Requests are answered
 * in the order they are sent.
 */
export class ReplayServer {
  readonly tool: string;
  readonly capturePath: string;
  lastUsed = Date.now();
  private readonly _child: ChildProcess;
  private readonly _ready: Promise<string | null>;
  private _resolveReady: (error: string | null) => void = () => {};
  private readonly _pending = new Map<number, (answer: ServeAnswer) => void>();
  private _nextId = 1;
  private _output = "";
  private _exited = false;

  constructor(tool: string, capturePath: string) {
    this.tool = tool;
    this.capturePath = capturePath;
    this._ready = new Promise((resolve) => { this._resolveReady = resolve; });
    this._child = spawn(tool, [capturePath, "--serve"], { stdio: ["pipe", "pipe", "pipe"], windowsHide: true });
    let buffered = "";
    this._child.stdout?.on("data", (chunk: Buffer) => {
      buffered += chunk.toString();
      let newline: number;
      while ((newline = buffered.indexOf("\n")) >= 0) {
        const line = buffered.slice(0, newline).replace(/\r$/, "");
        buffered = buffered.slice(newline + 1);
        this._line(line);
      }
    });
    this._child.stderr?.on("data", (chunk: Buffer) => this._collect(chunk.toString()));
    this._child.stdin?.on("error", () => {});   // a process that has exited cannot be written to
    this._child.on("error", (e) => this._exit(`could not run ${tool}: ${e.message}`));
    this._child.on("exit", (code) => this._exit(`the replay process exited (${code ?? "killed"})`));
  }

  get alive(): boolean {
    return !this._exited;
  }

  /**
   * Replays the frame for an analysis and reads the file it wrote. `fallback` is set when the answer
   * is not the analysis's: the process could not start (a tool from before --serve) or exited.
   */
  async run(analysis: ReplayAnalysis, timeoutMs = 10 * 60 * 1000): Promise<ReplayRun & { fallback?: boolean }> {
    this.lastUsed = Date.now();
    const startError = await this._ready;
    if (startError) return { data: null, output: tail(this._output), error: startError, fallback: true };
    const id = this._nextId++;
    const out = tempOutput(analysis.kind);
    const answer = await new Promise<ServeAnswer>((resolve) => {
      const timer = setTimeout(() => {
        this._pending.delete(id);
        this.dispose();
        resolve({ id, ok: false, error: `the replay did not finish within ${Math.round(timeoutMs / 1000)} s` });
      }, timeoutMs);
      this._pending.set(id, (a) => {
        clearTimeout(timer);
        resolve(a);
      });
      this._child.stdin?.write(JSON.stringify(serveRequest(id, analysis, out)) + "\n");
    });
    this.lastUsed = Date.now();
    let data: Uint8Array | null = null;
    try {
      data = new Uint8Array(fs.readFileSync(out));
      fs.unlinkSync(out);
    } catch {
      // not written
    }
    if (answer.ok && data) return { data, output: tail(this._output) };
    return { data: null, output: tail(this._output), error: answer.error ?? "the replay wrote no data", fallback: this._exited };
  }

  /** Stops the process: asked to quit, and killed if it does not. */
  dispose(): void {
    if (this._exited) return;
    try {
      this._child.stdin?.write(JSON.stringify({ kind: "quit" }) + "\n");
      this._child.stdin?.end();
    } catch {
      // already gone
    }
    setTimeout(() => { if (!this._exited) this._child.kill(); }, 2000).unref();
  }

  private _line(line: string): void {
    if (!line.startsWith("@replay ")) {
      this._collect(line + "\n");
      return;
    }
    let answer: ServeAnswer;
    try {
      answer = JSON.parse(line.slice(8)) as ServeAnswer;
    } catch {
      return;
    }
    if (answer.ready !== undefined) {
      this._resolveReady(answer.ready ? null : answer.error ?? "the replay could not start");
      return;
    }
    if (answer.id === undefined) return;
    const resolve = this._pending.get(answer.id);
    this._pending.delete(answer.id);
    resolve?.(answer);
  }

  private _collect(text: string): void {
    this._output += text;
    if (this._output.length > 256 * 1024) this._output = this._output.slice(-128 * 1024);
  }

  private _exit(message: string): void {
    if (this._exited) return;
    this._exited = true;
    this._resolveReady(message);
    for (const [id, resolve] of this._pending) resolve({ id, ok: false, error: message });
    this._pending.clear();
  }
}

/**
 * The replays kept alive, one per capture file (a file changed on disk gets a new one). The least
 * recently used is stopped past `max`, and any idle for `idleMs`: each holds its capture's objects
 * on the GPU.
 */
export class ReplayServerPool {
  private readonly _servers = new Map<string, ReplayServer>();
  private _sweep: NodeJS.Timeout | null = null;

  constructor(private readonly max = 3, private readonly idleMs = 5 * 60 * 1000) {}

  /** Runs an analysis in the capture's replay, starting one if needed; a process that cannot serve falls back to a one-shot replay. */
  async run(tool: string, capturePath: string, analysis: ReplayAnalysis, timeoutMs?: number): Promise<ReplayRun> {
    let stamp = 0;
    try {
      stamp = fs.statSync(capturePath).mtimeMs;
    } catch {
      return { data: null, output: "", error: `${capturePath} does not exist` };
    }
    const key = `${tool}\n${path.resolve(capturePath)}\n${stamp}`;
    let server = this._servers.get(key);
    if (!server || !server.alive) {
      server?.dispose();
      server = new ReplayServer(tool, capturePath);
      this._servers.set(key, server);
      this._trim();
    }
    this._scheduleSweep();
    const result = await server.run(analysis, timeoutMs);
    if (!server.alive) this._servers.delete(key);
    if (result.fallback) return runReplay(tool, capturePath, analysis, timeoutMs);
    return result;
  }

  /** Stops the replays of a capture file (it is closed, or about to be deleted). */
  release(capturePath: string): void {
    const resolved = path.resolve(capturePath);
    for (const [key, server] of this._servers) {
      if (path.resolve(server.capturePath) !== resolved) continue;
      server.dispose();
      this._servers.delete(key);
    }
  }

  disposeAll(): void {
    for (const server of this._servers.values()) server.dispose();
    this._servers.clear();
  }

  private _trim(): void {
    const live = [...this._servers.entries()].sort((a, b) => a[1].lastUsed - b[1].lastUsed);
    while (live.length > this.max) {
      const [key, server] = live.shift()!;
      server.dispose();
      this._servers.delete(key);
    }
  }

  private _scheduleSweep(): void {
    if (this._sweep) return;
    this._sweep = setInterval(() => {
      const now = Date.now();
      for (const [key, server] of this._servers) {
        if (now - server.lastUsed < this.idleMs) continue;
        server.dispose();
        this._servers.delete(key);
      }
      if (!this._servers.size && this._sweep) {
        clearInterval(this._sweep);
        this._sweep = null;
      }
    }, 30 * 1000);
    this._sweep.unref();
  }
}

/** The process's replays: GPU Inspector's main process and the MCP server each have one pool. */
export const replayServers = new ReplayServerPool();

/**
 * GPU Inspector: a renderer's capture serialized for replay, kept in a temporary file under a key the
 * renderer chose, so its replay stays alive between analyses and the bytes cross only once.
 */
const replayFiles = new Map<string, string>();

/** Runs an analysis of a renderer's capture: `needData` when the key is new and no bytes came with it. */
export async function replayKeyed(tool: string, key: string, bytes: Uint8Array | undefined, analysis: ReplayAnalysis, name = "capture"): Promise<ReplayRun> {
  let file = replayFiles.get(key);
  if (!file) {
    if (!bytes) return { data: null, output: "", needData: true };
    const base = name.replace(/[^\w.-]+/g, "_") || "capture";
    file = path.join(os.tmpdir(), `vkinsp_replay_${process.pid}_${Date.now()}_${Math.random().toString(36).slice(2)}_${base}.gpucap`);
    try {
      fs.writeFileSync(file, Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength));
    } catch (e) {
      return { data: null, output: "", error: `could not write ${file}: ${(e as Error).message}` };
    }
    replayFiles.set(key, file);
  }
  return replayServers.run(tool, file, analysis);
}

/** Stops a renderer's capture replay and removes its file (the capture was closed or changed). */
export function releaseReplayKey(key: string): void {
  const file = replayFiles.get(key);
  if (!file) return;
  replayFiles.delete(key);
  replayServers.release(file);
  // The replay read the whole file when it started, so it can go now.
  fs.promises.unlink(file).catch(() => {});
}

/** Stops every replay and removes the renderers' capture files, at quit. */
export function releaseAllReplays(): void {
  replayServers.disposeAll();
  for (const file of replayFiles.values()) {
    try {
      fs.unlinkSync(file);
    } catch {
      // gone already
    }
  }
  replayFiles.clear();
}
