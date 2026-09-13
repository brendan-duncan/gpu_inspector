// The Vulkan SDK's shader tools: where they are, spirv-dis / spirv-cross turning a SPIR-V payload
// into text, and the compilers turning edited source back into SPIR-V. No Electron here, so the
// MCP server (src/mcp/) shows and replaces shaders the way the Inspect tab does.
import { execFile } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import type { CompileShaderResult, DebugTranslationResult, ShaderLanguage, ShaderTextMode, ShaderTextResult } from "../shared/protocol.js";

let tempCounter = 0;

function tempBase(): string {
  return path.join(os.tmpdir(), `vkinsp_${process.pid}_${Date.now()}_${++tempCounter}`);
}

/** A tool from INSPECTOR_TOOLS_DIR or the Vulkan SDK, else the bare name for PATH to resolve. */
export function findTool(name: string): string {
  const exe = process.platform === "win32" ? `${name}.exe` : name;
  const candidates: string[] = [];
  if (process.env.INSPECTOR_TOOLS_DIR) candidates.push(path.join(process.env.INSPECTOR_TOOLS_DIR, exe));
  if (process.env.VULKAN_SDK) candidates.push(path.join(process.env.VULKAN_SDK, "Bin", exe), path.join(process.env.VULKAN_SDK, "bin", exe));
  for (const c of candidates) if (fs.existsSync(c)) return c;
  return exe; // hope it is on PATH
}

export interface ShaderTextOptions {
  /** spirv-cross: the entry point to translate, of a module with several ("vertex", "main"). */
  entry?: { stage: string; name: string };
  /**
   * spirv-cross: a variable for every value instead of expressions folded into one statement, so
   * a debugger stepping the translation by line stops about once per SPIR-V instruction.
   */
  forceTemporary?: boolean;
}

/** SPIR-V as assembly (spirv-dis) or as GLSL, HLSL or MSL (spirv-cross). */
export function shaderText(spirv: Uint8Array, mode: ShaderTextMode, options: ShaderTextOptions = {}): Promise<ShaderTextResult> {
  return new Promise((resolve) => {
    const tmp = `${tempBase()}.spv`;
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
      if (options.entry) args.push("--entry", options.entry.name, "--stage", GLSL_STAGES[options.entry.stage] ?? "frag");
      if (options.forceTemporary) args.push("--force-temporary");
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

// Shader editing: compiles a source language to SPIR-V with the SDK's compilers. `stage` uses the
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

export interface CompileOptions {
  /**
   * Directories `#include` is resolved against: the session's source roots. The edited source is
   * compiled from a temporary file, so an include is found by its path under a root, the way an
   * engine writes them ("common/lighting.glsl"), not relative to the shader's own file.
   */
  includeDirs?: string[];
  /** GLSL: embed the source text and a line per instruction (glslang `-g`), under this file name. */
  debugFileName?: string;
}

/** Whether the source has an `#include` glslang would need the Google include extension for. */
function needsIncludeExtension(source: string): boolean {
  return /^[ \t]*#[ \t]*include/m.test(source)
    && !/GL_GOOGLE_include_directive|GL_ARB_shading_language_include/.test(source);
}

/** GLSL (glslangValidator), HLSL (dxc) or SPIR-V assembly (spirv-as) compiled to SPIR-V for one stage. */
export function compileShader(source: string, language: ShaderLanguage, stage: string, entryPoint: string, spirvVersion: string,
                              options: CompileOptions = {}): Promise<CompileShaderResult> {
  return new Promise((resolve) => {
    const base = tempBase();
    const includeDirs = (options.includeDirs ?? []).filter((d) => d && fs.existsSync(d));
    // Debug information names the file as the compiler was given it: a file of that name in a
    // directory of its own, compiled from inside it, so the name carries no temporary path.
    const debugName = language === "glsl" ? options.debugFileName : undefined;
    const dir = debugName ? fs.mkdtempSync(`${base}_`) : null;
    const src = dir ? path.join(dir, debugName!) : base + (language === "hlsl" ? ".hlsl" : language === "spirv-asm" ? ".spvasm" : ".glsl");
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
      for (const dir of includeDirs) args.push("-I", dir);
    } else {
      tool = findTool("glslangValidator");
      // The decompiled source declares main(); the pipeline expects the original entry point name.
      args = ["-V", "-S", GLSL_STAGES[stage] ?? "frag", "--target-env", targetEnv(spirvVersion, "glslang"),
        "--source-entrypoint", "main", "-e", entry, "-o", out];
      if (debugName) args.push("-g", debugName);
      else args.push(src);
      for (const dir of includeDirs) args.push(`-I${dir}`);
      // glslang rejects #include unless the source asks for the extension. The preamble goes in
      // after the #version line and is counted separately, so the error lines stay the user's.
      if (needsIncludeExtension(source)) args.push("-P#extension GL_GOOGLE_include_directive : require");
    }
    execFile(tool, args, { maxBuffer: 64 * 1024 * 1024, cwd: dir ?? undefined }, (err, stdout, stderr) => {
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
      if (dir) fs.rmSync(dir, { recursive: true, force: true });
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

/** The name the decompiled source has in the recompiled module's debug information. */
const DECOMPILED_FILE = "decompiled.glsl";

/**
 * A module without source made steppable by line: spirv-cross decompiles the entry point to GLSL,
 * one statement per value, and glslang compiles that back with the text and a line per instruction
 * embedded. The result computes what the original does, but is not the module the GPU ran, so the
 * debugger checks it against the original. `stage` is the layer's stage name, as for compileShader.
 */
export async function decompileForDebugging(spirv: Uint8Array, stage: string, entryPoint: string): Promise<DebugTranslationResult> {
  const glsl = await shaderText(spirv, "glsl", { entry: { stage, name: entryPoint || "main" }, forceTemporary: true });
  if (!glsl.ok) return { ok: false, log: glsl.text, tool: "spirv-cross" };
  // The recompiled module targets the version the original was built for (header word 1: 0x00010500 is 1.5).
  const version = spirv.byteLength >= 8 ? new DataView(spirv.buffer, spirv.byteOffset, 8).getUint32(4, true) : 0;
  const spirvVersion = version ? `${(version >> 16) & 0xff}.${(version >> 8) & 0xff}` : "1.5";
  const compiled = await compileShader(glsl.text, "glsl", stage, entryPoint, spirvVersion, { debugFileName: DECOMPILED_FILE });
  if (!compiled.ok) {
    // glslang prints the file's name before its messages.
    const log = compiled.log.split(/\r?\n/).filter((l) => l.trim() && l.trim() !== DECOMPILED_FILE).join("\n");
    return { ok: false, log: `the decompiled GLSL did not compile: ${log}`, tool: compiled.tool, source: glsl.text };
  }
  return { ok: true, spirv: compiled.spirv, log: compiled.log, tool: compiled.tool, source: glsl.text };
}
