// Launching a Windows application with the Direct3D 12 capture library in it, without Electron:
// shared by the app's sessions (main.ts) and the MCP server's live sessions (mcp/live_session.ts),
// the way metal.ts is for the Metal library.
//
// The Vulkan loader inserts a layer for us when the right variables are set. D3D12 has no loader
// layers (src/d3d12/README.md, "Getting in"): the library has to be inside the process before its
// first D3D12 call, and a device made in a static initializer runs before anything a hook could
// wait for. So dxinsp_launch.exe creates the target suspended, injects dxinsp_capture.dll, runs its
// initializer and only then resumes the main thread; it inherits its standard handles to the
// target, waits for it and exits with its exit code, so the session's process handling (log
// capture, taskkill /T, the exit status) sees one process tree.
//
// There is no "which API" field in the launch dialog: every Windows target is started with both
// the Vulkan layer environment and the D3D12 library, and whichever API the application uses
// connects to the session's port. A target the library cannot be injected into (32-bit, protected)
// is still started, with the reason on stderr, so a Vulkan application launched the same way keeps
// working; when only one of the two libraries is built, the launch says which and goes on with it.
import fs from "node:fs";
import path from "node:path";
import { vulkanLayerEnvironment, type VulkanLayerOptions } from "./launch_env.js";

export const CAPTURE_LIBRARY = "dxinsp_capture.dll";
export const LAUNCHER = "dxinsp_launch.exe";
export const SHADER_TOOL = "dxinsp_shader.exe";

export interface D3D12Tools {
  /** The directory the library and launcher were found in. */
  dir: string;
  library: string;
  launcher: string;
  /** dxinsp_shader.exe beside them, when it was built (shader_tools.ts runs it for DXBC/DXIL text). */
  shaderTool: string | null;
}

/**
 * Where the D3D12 tools may be: INSPECTOR_D3D12_DIR, then a build tree under each of `roots`
 * (Release, RelWithDebInfo, Debug, then build/bin itself), then `packaged`, the layer directories
 * of installed apps (resources/layer, where tools/stage_layer.mjs puts them).
 */
export function d3d12ToolDirs(roots: string[], packaged: string[] = []): string[] {
  const dirs: string[] = [];
  if (process.env.INSPECTOR_D3D12_DIR) dirs.push(process.env.INSPECTOR_D3D12_DIR);
  for (const root of roots) {
    const bin = path.join(root, "build", "bin");
    dirs.push(path.join(bin, "Release"), path.join(bin, "RelWithDebInfo"), path.join(bin, "Debug"), bin);
  }
  dirs.push(...packaged);
  return dirs;
}

/**
 * The capture library and the launcher, both required, from the first directory holding them,
 * and the shader tool when it is there too; null when they are not built.
 */
export function findD3D12Tools(roots: string[], packaged: string[] = []): D3D12Tools | null {
  for (const dir of d3d12ToolDirs(roots, packaged)) {
    const library = path.join(dir, CAPTURE_LIBRARY);
    const launcher = path.join(dir, LAUNCHER);
    if (!fs.existsSync(library) || !fs.existsSync(launcher)) continue;
    const shaderTool = path.join(dir, SHADER_TOOL);
    return { dir, library, launcher, shaderTool: fs.existsSync(shaderTool) ? shaderTool : null };
  }
  return null;
}

/** dxinsp_shader.exe from the same directories, on its own: the text of a shader needs neither the library nor the launcher. */
export function findD3D12ShaderTool(roots: string[], packaged: string[] = []): string | null {
  for (const dir of d3d12ToolDirs(roots, packaged)) {
    const tool = path.join(dir, SHADER_TOOL);
    if (fs.existsSync(tool)) return tool;
  }
  return null;
}

export interface D3D12EnvironmentOptions {
  port: number;
  log: boolean;
  recordAlways: boolean;
  stacktraces: boolean;
  /** "Validation layer": the D3D12 debug layer is enabled before the device is created. */
  validation: boolean;
  /** Also append the library's log to this file (a GUI application has no usable stderr). */
  logFile?: string;
}

/** The variables the capture library reads (src/d3d12/README.md, "Getting in"). */
export function d3d12Environment(o: D3D12EnvironmentOptions): NodeJS.ProcessEnv {
  return {
    DXINSP_PORT: String(o.port),
    DXINSP_LOG: o.log ? "1" : "0",
    ...(o.logFile ? { DXINSP_LOG_FILE: o.logFile } : {}),
    DXINSP_RECORD_ALWAYS: o.recordAlways ? "1" : "0",
    DXINSP_STACKTRACES: o.stacktraces ? "1" : "0",
    DXINSP_DEBUG_LAYER: o.validation ? "1" : "0",
  };
}

/**
 * The command line that starts `exe` through the launcher with the library injected. Quoting is
 * the launcher's business (it rebuilds the target's command line from its arguments); this is
 * only the argument list.
 */
export function wrapLaunch(tools: D3D12Tools, exe: string, args: string[], cwd?: string): { exe: string; args: string[] } {
  return { exe: tools.launcher, args: ["--dll", tools.library, ...(cwd ? ["--cwd", cwd] : []), "--", exe, ...args] };
}

export interface WindowsLaunchOptions {
  exe: string;
  args: string[];
  /** The target's working directory; also passed to the launcher. */
  cwd: string;
  /** The environment to start from (the inspector's own, plus the user's additions). */
  env: NodeJS.ProcessEnv;
  /** The Vulkan layer's options, or null when the layer was not found. */
  vulkan: VulkanLayerOptions | null;
  /** The D3D12 tools and the library's options, or null when they were not found. */
  d3d12: (D3D12EnvironmentOptions & { tools: D3D12Tools }) | null;
}

export interface WindowsLaunch {
  exe: string;
  args: string[];
  env: NodeJS.ProcessEnv;
  /** What is in effect, for the session log: one line per capture library, found or not. */
  notes: string[];
}

/**
 * A Windows target's launch: the Vulkan layer environment when the layer is there, the D3D12
 * environment and the launcher wrapping when the D3D12 tools are there. The caller has checked
 * that at least one of them is.
 */
export function windowsLaunch(o: WindowsLaunchOptions): WindowsLaunch {
  const env: NodeJS.ProcessEnv = { ...o.env };
  const notes: string[] = [];
  let exe = o.exe;
  let args = o.args;
  if (o.vulkan) {
    Object.assign(env, vulkanLayerEnvironment(o.vulkan));
    notes.push(`layer: ${o.vulkan.layerDir}`);
  } else {
    notes.push("Vulkan layer not found: build it (docs/BUILDING.md); only D3D12 will be captured");
  }
  if (o.d3d12) {
    const { tools, ...options } = o.d3d12;
    Object.assign(env, d3d12Environment(options));
    ({ exe, args } = wrapLaunch(tools, o.exe, o.args, o.cwd));
    notes.push(`D3D12 capture library: ${tools.library}${options.validation ? " (D3D12 debug layer on)" : ""}`);
  } else {
    notes.push("D3D12 capture library not found: build it (src/d3d12/README.md); only Vulkan will be captured");
  }
  return { exe, args, env, notes };
}
