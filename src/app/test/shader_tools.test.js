// The shader compilers behind the editor and replace_shader (src/main/shader_tools.ts), against the
// real tools: a GLSL source with an #include resolved from the source roots. Skipped where the
// Vulkan SDK's glslangValidator is not on this machine, which is how CI runs.
import { test } from "node:test";
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdirSync, mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "shadertools-"));
const out = join(dir, "shader_tools.mjs");
buildSync({ entryPoints: [join(here, "..", "src", "main", "shader_tools.ts")], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
const { compileShader, findTool } = await import(pathToFileURL(out).href);

function haveGlslang() {
  try {
    execFileSync(findTool("glslangValidator"), ["--version"], { stdio: "ignore" });
    return true;
  } catch {
    return false;
  }
}

test("GLSL #include resolves against the source roots, and errors keep the source's line numbers", { skip: haveGlslang() ? false : "glslangValidator not found (install the Vulkan SDK)" }, async () => {
  // The include lives under a root, named the way an engine writes it; the edited source itself is
  // compiled from a temporary file, so only the roots can resolve it.
  const root = join(dir, "shaders");
  mkdirSync(join(root, "common"), { recursive: true });
  writeFileSync(join(root, "common", "tint.glsl"), "#define TINT vec4(0.25, 0.5, 0.75, 1.0)\n");
  const source = [
    "#version 450",
    '#include "common/tint.glsl"',
    "layout(location = 0) out vec4 color;",
    "void main() {",
    "  color = TINT;",
    "}",
    "",
  ].join("\n");

  const ok = await compileShader(source, "glsl", "fragment", "main", "1.5", { includeDirs: [root] });
  assert.ok(ok.ok, `compile failed: ${ok.log}`);
  assert.ok(ok.spirv.byteLength > 20, "no SPIR-V produced");

  // Without the root the include cannot be found, and the message says which file.
  const missing = await compileShader(source, "glsl", "fragment", "main", "1.5", {});
  assert.equal(missing.ok, false);
  assert.match(missing.log, /tint\.glsl/);

  // An error after the include is still reported on its own line (5), not shifted by the include
  // or by the preamble that enables the directive.
  const bad = source.replace("  color = TINT;", "  color = no_such_symbol;");
  const failed = await compileShader(bad, "glsl", "fragment", "main", "1.5", { includeDirs: [root] });
  assert.equal(failed.ok, false);
  assert.match(failed.log, /:5:/);
});
