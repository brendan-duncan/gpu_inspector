// The Vulkan SDK's shader tools: where they are, and spirv-dis / spirv-cross turning a SPIR-V
// payload into text. No Electron here, so the MCP server (src/mcp/) shows shaders the way the
// Inspect tab does.
import { execFile } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import type { ShaderTextMode, ShaderTextResult } from "../shared/protocol.js";

let tempCounter = 0;

/** A tool from INSPECTOR_TOOLS_DIR or the Vulkan SDK, else the bare name for PATH to resolve. */
export function findTool(name: string): string {
  const exe = process.platform === "win32" ? `${name}.exe` : name;
  const candidates: string[] = [];
  if (process.env.INSPECTOR_TOOLS_DIR) candidates.push(path.join(process.env.INSPECTOR_TOOLS_DIR, exe));
  if (process.env.VULKAN_SDK) candidates.push(path.join(process.env.VULKAN_SDK, "Bin", exe), path.join(process.env.VULKAN_SDK, "bin", exe));
  for (const c of candidates) if (fs.existsSync(c)) return c;
  return exe; // hope it is on PATH
}

/** SPIR-V as assembly (spirv-dis) or as GLSL, HLSL or MSL (spirv-cross). */
export function shaderText(spirv: Uint8Array, mode: ShaderTextMode): Promise<ShaderTextResult> {
  return new Promise((resolve) => {
    const tmp = path.join(os.tmpdir(), `vkinsp_${process.pid}_${Date.now()}_${++tempCounter}.spv`);
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
