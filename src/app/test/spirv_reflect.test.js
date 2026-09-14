// Buffer layouts named from Vulkan debug information (src/renderer/vulkan/spirv_reflect.ts).
//
// A module can be stripped of OpName and OpMemberName and still carry
// NonSemantic.Shader.DebugInfo.100, which is not a debug instruction in SPIR-V's sense and so
// survives spirv-opt --strip-debug. Its DebugGlobalVariable / DebugTypeComposite / DebugTypeMember
// name the block and its fields, which is what a buffer's layout shows instead of member0, member1.
//
// vectors/debug_names.spv is such a module: a fragment shader with
//
//   layout(std140, binding = 0) uniform Params { vec4 tint; float scale; } params;
//
// compiled with `glslangValidator -V -gV`, disassembled, its OpName and OpMemberName lines removed,
// and assembled again.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const out = join(mkdtempSync(join(tmpdir(), "spirv-")), "spirv_reflect.mjs");
buildSync({ entryPoints: [join(here, "..", "src", "renderer", "vulkan", "spirv_reflect.ts")], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
const { reflectSpirv } = await import(pathToFileURL(out).href);

test("a buffer's fields are named from the debug information when OpMemberName is gone", () => {
  const spirv = new Uint8Array(readFileSync(join(here, "vectors", "debug_names.spv")));
  const r = reflectSpirv(spirv);
  assert.ok(r, "the module did not reflect");
  const blocks = r.resources.filter((x) => x.kind === "uniform");
  assert.equal(blocks.length, 1);
  const [block] = blocks;
  assert.equal(block.name, "params");
  assert.equal(block.typeName, "Params");
  assert.equal(block.type.kind, "struct");
  assert.deepEqual(block.type.members.map((m) => [m.name, m.offset]), [["tint", 0], ["scale", 16]]);

  // The names are a fallback: a module that has OpName / OpMemberName keeps using them, so nothing
  // changes for the usual build. Stripping the debug set leaves the old member0, member1.
  const withoutDebugInfo = reflectSpirv(stripDebugInfoSet(spirv)).resources.find((x) => x.kind === "uniform");
  assert.deepEqual(withoutDebugInfo.type.members.map((m) => m.name), ["member0", "member1"]);
  assert.equal(withoutDebugInfo.name, "");
});

/** The module with its NonSemantic.Shader.DebugInfo.100 import renamed, so nothing matches the set. */
function stripDebugInfoSet(spirv) {
  const copy = new Uint8Array(spirv);
  const text = new TextDecoder().decode(copy);
  const at = text.indexOf("NonSemantic.Shader.DebugInfo.100");
  assert.ok(at > 0, "the fixture no longer imports the debug info set");
  copy[at] = "x".charCodeAt(0);
  return copy;
}
