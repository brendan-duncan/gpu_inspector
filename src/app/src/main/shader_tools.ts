// The Vulkan SDK's shader tools: where they are, spirv-dis / spirv-cross turning a SPIR-V payload
// into text, and the compilers turning edited source back into SPIR-V. No Electron here, so the
// MCP server (src/mcp/) shows and replaces shaders the way the Inspect tab does.
//
// A D3D12 pipeline's shader is a DXBC/DXIL container instead: its text comes from
// dxinsp_shader.exe, built beside the D3D12 capture library (src/d3d12/README.md, "Shaders"), and an
// edited HLSL goes back through dxc to bytecode (compileDxil).
import { execFile } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { SHADER_TOOL, findD3D12ShaderTool } from "./d3d12.js";
import type { CompileShaderResult, DebugTranslationResult, ShaderLanguage, ShaderTextMode, ShaderTextResult } from "../shared/protocol.js";

let tempCounter = 0;

function tempBase(): string {
  return path.join(os.tmpdir(), `vkinsp_${process.pid}_${Date.now()}_${++tempCounter}`);
}

// The bundle is ESM, so there is no __dirname. The app's bundle is dist/main/main.js, the MCP
// server's claude-plugin/server/gpu-inspector-mcp.mjs: the checkout is two or three levels up.
const moduleDir = path.dirname(fileURLToPath(import.meta.url));

/** A tool from INSPECTOR_TOOLS_DIR or the Vulkan SDK, else the bare name for PATH to resolve. */
export function findTool(name: string): string {
  const exe = process.platform === "win32" ? `${name}.exe` : name;
  const candidates: string[] = [];
  if (process.env.INSPECTOR_TOOLS_DIR) candidates.push(path.join(process.env.INSPECTOR_TOOLS_DIR, exe));
  if (process.env.VULKAN_SDK) candidates.push(path.join(process.env.VULKAN_SDK, "Bin", exe), path.join(process.env.VULKAN_SDK, "bin", exe));
  for (const c of candidates) if (fs.existsSync(c)) return c;
  return exe; // hope it is on PATH
}

/** Whether the bytes are a DXBC/DXIL container (a D3D12 pipeline's shader) rather than SPIR-V. */
export function isDxbc(bytes: Uint8Array): boolean {
  return bytes.byteLength >= 4 && bytes[0] === 0x44 && bytes[1] === 0x58 && bytes[2] === 0x42 && bytes[3] === 0x43;  // "DXBC"
}

/**
 * dxinsp_shader.exe: from INSPECTOR_TOOLS_DIR, else where the D3D12 capture library is looked for
 * (INSPECTOR_D3D12_DIR, the build tree of this checkout or of GPU_INSPECTOR_ROOT, the packaged
 * app's layer directory); null when it is not built.
 */
export function findShaderTool(): string | null {
  if (process.env.INSPECTOR_TOOLS_DIR) {
    const c = path.join(process.env.INSPECTOR_TOOLS_DIR, SHADER_TOOL);
    if (fs.existsSync(c)) return c;
  }
  const roots = [path.resolve(moduleDir, "..", ".."), path.resolve(moduleDir, "..", "..", ".."),
                 path.resolve(moduleDir, "..", "..", "..", "..")];
  if (process.env.GPU_INSPECTOR_ROOT) roots.push(process.env.GPU_INSPECTOR_ROOT);
  const packaged = process.resourcesPath ? [path.join(process.resourcesPath, "layer")] : [];
  return findD3D12ShaderTool(roots, packaged);
}

const NO_SHADER_TOOL = `${SHADER_TOOL} not found: build the D3D12 library (src/d3d12/README.md)`;

/** One source file dxinsp_shader --sources printed, however it spelled the pair. */
function embeddedSource(entry: unknown): { name: string; text: string; from?: string } | null {
  if (Array.isArray(entry) && entry.length >= 2) return { name: String(entry[0]), text: String(entry[1]) };
  if (entry && typeof entry === "object") {
    const o = entry as Record<string, unknown>;
    const text = o.text ?? o.source ?? o.contents;
    if (typeof text === "string") return { name: String(o.name ?? o.file ?? o.path ?? ""), text, from: typeof o.from === "string" ? o.from : undefined };
  }
  return null;
}

/** What keeps a D3D12 shader's HLSL, and where GPU Inspector looks for it when the container has none. */
export const NO_HLSL_HINT = "dxc -Zi embeds the HLSL in the container; dxc -Zs keeps it out and writes it to a PDB beside the build "
  + "(-Fd <dir>\\), which GPU Inspector reads when a symbol directory names that directory.";

/**
 * A DXBC/DXIL container as its disassembly ("dis") or as its HLSL ("hlsl"), through
 * dxinsp_shader.exe: the source dxc embedded with -Zi, or, for a -Zs build that kept it out, the
 * source in the PDB found under `pdbDirs`. Nothing cross-compiles the bytecode to GLSL or MSL.
 */
function dxbcText(bytes: Uint8Array, mode: ShaderTextMode, pdbDirs: string[] = []): Promise<ShaderTextResult> {
  if (mode !== "dis" && mode !== "hlsl") return Promise.resolve({ ok: false, text: `${mode} is not available for DXBC/DXIL: a D3D12 shader has its disassembly and its HLSL source` });
  const tool = findShaderTool();
  if (!tool) return Promise.resolve({ ok: false, text: NO_SHADER_TOOL });
  return new Promise((resolve) => {
    const tmp = `${tempBase()}.dxbc`;
    fs.writeFileSync(tmp, Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength));
    const args = [mode === "dis" ? "--disassemble" : "--sources", tmp];
    if (mode === "hlsl") for (const dir of pdbDirs) if (dir && fs.existsSync(dir)) args.push("--pdb-dir", dir);
    execFile(tool, args, { maxBuffer: 64 * 1024 * 1024 }, (err, stdout, stderr) => {
      try {
        fs.unlinkSync(tmp);
      } catch {
        // ignore
      }
      if (err) {
        resolve({ ok: false, text: (err as NodeJS.ErrnoException).code === "ENOENT" ? NO_SHADER_TOOL : `${SHADER_TOOL} failed: ${stderr || err.message}` });
        return;
      }
      if (mode === "dis") {
        resolve({ ok: true, text: stdout });
        return;
      }
      let sources: { name: string; text: string; from?: string }[];
      try {
        const parsed: unknown = JSON.parse(stdout);
        sources = (Array.isArray(parsed) ? parsed : []).map(embeddedSource).filter((s): s is { name: string; text: string } => s !== null);
      } catch {
        resolve({ ok: false, text: `${SHADER_TOOL} printed no source list: ${stdout.trim().split(/\r?\n/)[0] ?? ""}` });
        return;
      }
      // An empty list is not a tool failure: the tool writes why on stderr and exits 0.
      if (!sources.length) resolve({ ok: false, text: `${(stderr || "").trim() || "no HLSL source"}. ${NO_HLSL_HINT}` });
      else resolve({ ok: true, text: sources.map((s) => `// ==== ${s.name}${s.from ? ` (from ${s.from})` : ""}\n${s.text.endsWith("\n") ? s.text : `${s.text}\n`}`).join("\n") });
    });
  });
}

export interface ShaderTextOptions {
  /** spirv-cross: the entry point to translate, of a module with several ("vertex", "main"). */
  entry?: { stage: string; name: string };
  /**
   * spirv-cross: a variable for every value instead of expressions folded into one statement, so
   * a debugger stepping the translation by line stops about once per SPIR-V instruction.
   */
  forceTemporary?: boolean;
  /**
   * D3D12: directories holding the PDBs dxc wrote for shaders built with -Zs, which keep the HLSL
   * out of the container. These are the session's symbol directories — the same build output the
   * stack traces are symbolized against (main.ts's inspector:shaderText, mcp/search_paths.ts).
   */
  pdbDirs?: string[];
}

/**
 * SPIR-V as assembly (spirv-dis) or as GLSL, HLSL or MSL (spirv-cross). A DXBC/DXIL container
 * (isDxbc) goes through dxinsp_shader.exe instead: "dis" is its disassembly, "hlsl" its HLSL
 * source (embedded, or out of a PDB under `options.pdbDirs`), and the other modes say they are
 * not available for it.
 */
export function shaderText(spirv: Uint8Array, mode: ShaderTextMode, options: ShaderTextOptions = {}): Promise<ShaderTextResult> {
  if (isDxbc(spirv)) return dxbcText(spirv, mode, options.pdbDirs);
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

/**
 * spirv-val's verdict on a module: null when it is valid, the first lines of the complaint when not, and
 * undefined when there is no spirv-val to ask (nothing is known).
 */
export function validateSpirv(spirv: Uint8Array): Promise<string | null | undefined> {
  return new Promise((resolve) => {
    const tmp = `${tempBase()}.spv`;
    fs.writeFileSync(tmp, Buffer.from(spirv.buffer, spirv.byteOffset, spirv.byteLength));
    execFile(findTool("spirv-val"), ["--target-env", "vulkan1.3", tmp], { maxBuffer: 4 * 1024 * 1024 }, (err, stdout, stderr) => {
      try {
        fs.unlinkSync(tmp);
      } catch {
        // ignore
      }
      if (!err) resolve(null);
      else if ((err as NodeJS.ErrnoException).code === "ENOENT") resolve(undefined);
      else resolve((stderr || stdout || err.message).trim().split(/\r?\n/).slice(0, 3).join(" "));
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

/** dxc's target profile prefixes by the layer's stage names, for a D3D12 pipeline's stages. */
const DXIL_PROFILES: Record<string, string> = {
  vertex: "vs", fragment: "ps", tess_control: "hs", tess_eval: "ds", geometry: "gs", compute: "cs", task: "as", mesh: "ms",
};

/**
 * HLSL compiled to DXIL bytecode with dxc for one stage of a D3D12 pipeline, the replacement the
 * D3D12 capture library's ReplaceShader takes (the result's `spirv` holds the bytecode; the field
 * keeps its name). `shaderModel` is the profile's suffix ("6_0", "6_6"): the pipeline's own,
 * read from its reflection's `target`, so the replacement stays within what the device accepts.
 * Debug information with the source embedded (-Zi; -Qembed_debug only silences dxc's warning
 * about having no -Fd to write a PDB to), so the replacement keeps a
 * source view in the Inspect panel.
 */
export function compileDxil(source: string, stage: string, entryPoint: string, shaderModel = "6_0", options: CompileOptions = {}): Promise<CompileShaderResult> {
  const prefix = DXIL_PROFILES[stage];
  if (!prefix) return Promise.resolve({ ok: false, log: `no D3D12 shader profile for the ${stage} stage`, tool: "dxc" });
  return new Promise((resolve) => {
    const base = tempBase();
    const includeDirs = (options.includeDirs ?? []).filter((d) => d && fs.existsSync(d));
    const src = `${base}.hlsl`;
    const out = `${base}.dxil`;
    fs.writeFileSync(src, source);
    const tool = findTool("dxc");
    const args = ["-T", `${prefix}_${shaderModel.replace(/^[^0-9]*/, "").replace(".", "_") || "6_0"}`, "-E", entryPoint || "main", "-Zi", "-Qembed_debug", "-Fo", out, src];
    for (const dir of includeDirs) args.push("-I", dir);
    execFile(tool, args, { maxBuffer: 64 * 1024 * 1024 }, (err, stdout, stderr) => {
      const log = `${stdout ?? ""}${stderr ?? ""}`.trim();
      let bytecode: Uint8Array | undefined;
      try {
        if (fs.existsSync(out)) bytecode = new Uint8Array(fs.readFileSync(out));
      } catch {
        bytecode = undefined;
      }
      for (const f of [src, out]) {
        try {
          fs.unlinkSync(f);
        } catch {
          // ignore
        }
      }
      if (err || !bytecode || !isDxbc(bytecode)) {
        const reason = log || (err && "code" in err && err.code === "ENOENT"
          ? "dxc not found: install the Vulkan SDK (or the DirectX Shader Compiler) and set VULKAN_SDK or INSPECTOR_TOOLS_DIR"
          : err?.message ?? "dxc produced no output");
        resolve({ ok: false, log: reason, tool: "dxc" });
      } else {
        resolve({ ok: true, spirv: bytecode, log, tool: "dxc" });
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
