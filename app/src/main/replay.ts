// Running vkinsp_replay (replay/, docs/REPLAY.md) from GPU Inspector and its MCP server: finding the
// built tool, and measuring a Vulkan capture's overdraw with it. A Metal capture measures overdraw
// while it is taken (metal/src/overdraw.h); a Vulkan capture has to be replayed on this machine's GPU.
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

export interface OverdrawRun {
  /** The --overdraw-data file the replay wrote (parseOverdrawFile in renderer/overdraw.ts reads it), or null. */
  data: Uint8Array | null;
  /** Why there is no data. */
  error?: string;
  /** The end of what the tool printed. */
  output: string;
}

/** The last lines of the tool's output, for an error message. */
function tail(text: string, lines = 12): string {
  return text.trim().split(/\r?\n/).slice(-lines).join("\n");
}

/** Replays a capture file with --overdraw-data and reads what it wrote. */
export function runOverdrawReplay(tool: string, capturePath: string, timeoutMs = 10 * 60 * 1000): Promise<OverdrawRun> {
  return new Promise((resolve) => {
    const out = path.join(os.tmpdir(), `vkinsp_overdraw_${process.pid}_${Date.now()}_${Math.random().toString(36).slice(2)}.bin`);
    let output = "";
    let done = false;
    let timedOut = false;
    const child = spawn(tool, [capturePath, "--overdraw-data", out], { stdio: ["ignore", "pipe", "pipe"], windowsHide: true });
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
        error: error ?? (timedOut ? `the replay did not finish within ${Math.round(timeoutMs / 1000)} s` : `the replay wrote no overdraw data:\n${tail(output)}`),
      });
    };
    child.on("error", (e) => finish(`could not run ${tool}: ${e.message}`));
    child.on("close", () => finish(null));
  });
}

/** Writes capture bytes to a temporary file, replays it for overdraw, and removes the file. */
export async function measureOverdrawOfBytes(tool: string, bytes: Uint8Array, name = "capture"): Promise<OverdrawRun> {
  const base = name.replace(/[^\w.-]+/g, "_") || "capture";
  const file = path.join(os.tmpdir(), `vkinsp_replay_${process.pid}_${Date.now()}_${base}.gpucap`);
  try {
    fs.writeFileSync(file, Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength));
  } catch (e) {
    return { data: null, output: "", error: `could not write ${file}: ${(e as Error).message}` };
  }
  try {
    return await runOverdrawReplay(tool, file);
  } finally {
    try {
      fs.unlinkSync(file);
    } catch {
      // already gone
    }
  }
}
