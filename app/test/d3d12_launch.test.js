// Launching a Windows target with the D3D12 capture library (src/main/d3d12.ts): finding the
// tools in a build tree, the environment the library reads, the launcher's command line, and the
// DXBC path of shaderText (src/main/shader_tools.ts) saying the shader tool is missing when it is.
//
//     cd app && npm test
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "d3d12launch-"));
buildSync({
  entryPoints: [join(here, "..", "src", "main", "d3d12.ts"), join(here, "..", "src", "main", "shader_tools.ts")],
  bundle: true, format: "esm", platform: "node", outdir: join(dir, "build"), logLevel: "silent",
});
const { CAPTURE_LIBRARY, LAUNCHER, SHADER_TOOL, d3d12Environment, findD3D12Tools, findD3D12ShaderTool, windowsLaunch, wrapLaunch } =
  await import(pathToFileURL(join(dir, "build", "d3d12.js")).href);
const { isDxbc, shaderText } = await import(pathToFileURL(join(dir, "build", "shader_tools.js")).href);

/** A fake checkout with a build tree holding the named files under build/bin/<config>. */
function checkout(name, config, files) {
  const root = join(dir, name);
  const bin = join(root, "build", "bin", config);
  mkdirSync(bin, { recursive: true });
  for (const f of files) writeFileSync(join(bin, f), "");
  return { root, bin };
}

test("findD3D12Tools: the library and launcher are required, the shader tool optional, Release before Debug", () => {
  const savedDir = process.env.INSPECTOR_D3D12_DIR;
  delete process.env.INSPECTOR_D3D12_DIR;
  // Only the library: not enough.
  const partial = checkout("partial", "Release", [CAPTURE_LIBRARY]);
  assert.equal(findD3D12Tools([partial.root]), null);
  assert.equal(findD3D12ShaderTool([partial.root]), null);

  const debugOnly = checkout("debug", "Debug", [CAPTURE_LIBRARY, LAUNCHER]);
  const found = findD3D12Tools([debugOnly.root]);
  assert.deepEqual(found, { dir: debugOnly.bin, library: join(debugOnly.bin, CAPTURE_LIBRARY), launcher: join(debugOnly.bin, LAUNCHER), shaderTool: null });

  // Release wins over Debug in the same checkout, and the shader tool is found beside them.
  const both = checkout("both", "Debug", [CAPTURE_LIBRARY, LAUNCHER]);
  const release = checkout("both", "Release", [CAPTURE_LIBRARY, LAUNCHER, SHADER_TOOL]);
  const r = findD3D12Tools([both.root]);
  assert.equal(r.dir, release.bin);
  assert.equal(r.shaderTool, join(release.bin, SHADER_TOOL));
  assert.equal(findD3D12ShaderTool([both.root]), join(release.bin, SHADER_TOOL));

  // The packaged directory is looked at after the roots; INSPECTOR_D3D12_DIR before everything.
  const packaged = join(dir, "packaged", "layer");
  mkdirSync(packaged, { recursive: true });
  for (const f of [CAPTURE_LIBRARY, LAUNCHER]) writeFileSync(join(packaged, f), "");
  assert.equal(findD3D12Tools([partial.root], [packaged]).dir, packaged);
  assert.equal(findD3D12Tools([both.root], [packaged]).dir, release.bin);
  process.env.INSPECTOR_D3D12_DIR = packaged;
  assert.equal(findD3D12Tools([both.root]).dir, packaged);
  if (savedDir === undefined) delete process.env.INSPECTOR_D3D12_DIR; else process.env.INSPECTOR_D3D12_DIR = savedDir;
});

test("d3d12Environment: the variables the library reads", () => {
  assert.deepEqual(d3d12Environment({ port: 47540, log: true, recordAlways: false, stacktraces: true, validation: true }), {
    DXINSP_PORT: "47540", DXINSP_LOG: "1", DXINSP_RECORD_ALWAYS: "0", DXINSP_STACKTRACES: "1", DXINSP_DEBUG_LAYER: "1",
  });
  const off = d3d12Environment({ port: 47531, log: false, recordAlways: true, stacktraces: false, validation: false, logFile: "C:\\tmp\\d3d12.log" });
  assert.equal(off.DXINSP_LOG, "0");
  assert.equal(off.DXINSP_RECORD_ALWAYS, "1");
  assert.equal(off.DXINSP_STACKTRACES, "0");
  assert.equal(off.DXINSP_DEBUG_LAYER, "0");
  assert.equal(off.DXINSP_LOG_FILE, "C:\\tmp\\d3d12.log");
  assert.ok(!("DXINSP_LOG_FILE" in d3d12Environment({ port: 1, log: true, recordAlways: false, stacktraces: false, validation: false })));
});

test("wrapLaunch: the launcher's argument list (quoting is the launcher's job)", () => {
  const tools = { dir: "C:\\b", library: "C:\\b\\dxinsp_capture.dll", launcher: "C:\\b\\dxinsp_launch.exe", shaderTool: null };
  assert.deepEqual(wrapLaunch(tools, "C:\\Games\\app.exe", ["--frames", "3", "a b"], "C:\\Games"), {
    exe: "C:\\b\\dxinsp_launch.exe",
    args: ["--dll", "C:\\b\\dxinsp_capture.dll", "--cwd", "C:\\Games", "--", "C:\\Games\\app.exe", "--frames", "3", "a b"],
  });
  assert.deepEqual(wrapLaunch(tools, "app.exe", []).args, ["--dll", "C:\\b\\dxinsp_capture.dll", "--", "app.exe"]);
});

test("windowsLaunch: both libraries when found, and a note for each", () => {
  const tools = { dir: "C:\\b", library: "C:\\b\\dxinsp_capture.dll", launcher: "C:\\b\\dxinsp_launch.exe", shaderTool: null };
  const d3d12 = { tools, port: 47531, log: true, recordAlways: false, stacktraces: true, validation: false };
  const vulkan = { layerDir: "C:\\layer", validationDir: null, port: 47531, log: true, recordAlways: false, stacktraces: true, validation: false, syncValidation: false };
  const both = windowsLaunch({ exe: "C:\\app.exe", args: ["-x"], cwd: "C:\\", env: { USER_VAR: "1" }, vulkan, d3d12 });
  assert.equal(both.exe, tools.launcher);
  assert.deepEqual(both.args.slice(-2), ["C:\\app.exe", "-x"]);
  assert.equal(both.env.USER_VAR, "1");
  assert.equal(both.env.VKINSP_PORT, "47531");
  assert.equal(both.env.DXINSP_PORT, "47531");
  assert.ok(both.env.VK_LOADER_LAYERS_ENABLE.includes("VK_LAYER_INSPECTOR_capture"));
  assert.ok(both.notes.some((n) => n.startsWith("layer: C:\\layer")));
  assert.ok(both.notes.some((n) => n === `D3D12 capture library: ${tools.library}`));

  // Only Vulkan: no launcher, and the log says D3D12 is missing.
  const vulkanOnly = windowsLaunch({ exe: "C:\\app.exe", args: [], cwd: "C:\\", env: {}, vulkan, d3d12: null });
  assert.equal(vulkanOnly.exe, "C:\\app.exe");
  assert.ok(!("DXINSP_PORT" in vulkanOnly.env));
  assert.ok(vulkanOnly.notes.some((n) => n.startsWith("D3D12 capture library not found")));

  // Only D3D12: wrapped, no layer variables, and the log says Vulkan is missing.
  const d3d12Only = windowsLaunch({ exe: "C:\\app.exe", args: [], cwd: "C:\\", env: {}, vulkan: null, d3d12 });
  assert.equal(d3d12Only.exe, tools.launcher);
  assert.ok(!("VKINSP_PORT" in d3d12Only.env));
  assert.ok(d3d12Only.notes.some((n) => n.startsWith("Vulkan layer not found")));
});

test("shaderText on DXBC without dxinsp_shader.exe says so; other modes are not available for it", async () => {
  const dxbc = new Uint8Array([0x44, 0x58, 0x42, 0x43, 0, 0, 0, 0, 1, 2, 3, 4]);
  assert.ok(isDxbc(dxbc));
  assert.ok(!isDxbc(new Uint8Array([0x03, 0x02, 0x23, 0x07])));
  assert.ok(!isDxbc(new Uint8Array([0x44, 0x58])));

  // An empty tools directory, no checkout build and no PATH: nothing to find.
  const empty = join(dir, "empty-tools");
  mkdirSync(empty, { recursive: true });
  const saved = { tools: process.env.INSPECTOR_TOOLS_DIR, d3d12: process.env.INSPECTOR_D3D12_DIR, root: process.env.GPU_INSPECTOR_ROOT, path: process.env.PATH };
  process.env.INSPECTOR_TOOLS_DIR = empty;
  process.env.INSPECTOR_D3D12_DIR = empty;
  process.env.GPU_INSPECTOR_ROOT = empty;
  process.env.PATH = "";
  try {
    const dis = await shaderText(dxbc, "dis");
    assert.equal(dis.ok, false);
    assert.match(dis.text, /dxinsp_shader\.exe not found: build the D3D12 library \(d3d12\/README\.md\)/);
    const glsl = await shaderText(dxbc, "glsl");
    assert.equal(glsl.ok, false);
    assert.match(glsl.text, /not available for DXBC\/DXIL/);
  } finally {
    for (const [k, v] of [["INSPECTOR_TOOLS_DIR", saved.tools], ["INSPECTOR_D3D12_DIR", saved.d3d12], ["GPU_INSPECTOR_ROOT", saved.root], ["PATH", saved.path]]) {
      if (v === undefined) delete process.env[k]; else process.env[k] = v;
    }
  }
});
