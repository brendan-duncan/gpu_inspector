// Running vkinsp_replay (replay/, docs/REPLAY.md) from GPU Inspector and its MCP server: finding the
// built tool, and replaying a Vulkan capture for its overdraw or a pixel's history. A Metal capture
// measures overdraw while it is taken (metal/src/overdraw.h); a Vulkan capture has to be replayed on
// this machine's GPU.
import { spawn } from "node:child_process";
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

/** What to replay for: every pass's overdraw, or one pixel's history. */
export type ReplayAnalysis = { kind: "overdraw" } | { kind: "draws" } | ({ kind: "pixel" } & PixelRequest);

/** The last lines of the tool's output, for an error message. */
function tail(text: string, lines = 12): string {
  return text.trim().split(/\r?\n/).slice(-lines).join("\n");
}

function analysisArgs(analysis: ReplayAnalysis, out: string): string[] {
  if (analysis.kind === "overdraw") return ["--overdraw-data", out];
  if (analysis.kind === "draws") return ["--draw-data", out];
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

/** Replays a capture file with --overdraw-data. */
export function runOverdrawReplay(tool: string, capturePath: string, timeoutMs?: number): Promise<OverdrawRun> {
  return runReplay(tool, capturePath, { kind: "overdraw" }, timeoutMs);
}

/** Writes capture bytes to a temporary file, replays it for an analysis, and removes the file. */
export async function replayBytes(tool: string, bytes: Uint8Array, analysis: ReplayAnalysis, name = "capture"): Promise<ReplayRun> {
  const base = name.replace(/[^\w.-]+/g, "_") || "capture";
  const file = path.join(os.tmpdir(), `vkinsp_replay_${process.pid}_${Date.now()}_${base}.gpucap`);
  try {
    fs.writeFileSync(file, Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength));
  } catch (e) {
    return { data: null, output: "", error: `could not write ${file}: ${(e as Error).message}` };
  }
  try {
    return await runReplay(tool, file, analysis);
  } finally {
    try {
      fs.unlinkSync(file);
    } catch {
      // already gone
    }
  }
}

/** Writes capture bytes to a temporary file, replays it for overdraw, and removes the file. */
export function measureOverdrawOfBytes(tool: string, bytes: Uint8Array, name = "capture"): Promise<OverdrawRun> {
  return replayBytes(tool, bytes, { kind: "overdraw" }, name);
}
