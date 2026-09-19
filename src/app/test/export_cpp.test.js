// Export to C++ (src/renderer/export_cpp.ts): what `vkinsp_replay --export-data` writes
// (WriteExportData in src/replay/src/main.cpp), how the status line says it, and the folder a
// capture's project goes into.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "exportcpp-"));
const out = join(dir, "export_cpp.mjs");
buildSync({ entryPoints: [join(here, "..", "src", "renderer", "export_cpp.ts")], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
const { parseExportSummary, exportSummaryText, exportFolderName, exportsToCpp } = await import(pathToFileURL(out).href);

const bytes = (value) => new TextEncoder().encode(JSON.stringify(value));
const summary = {
  format: "gpu-inspector-export-cpp", version: 1, device: "NVIDIA GeForce RTX 4080", directory: "D:/bugs/frame_cpp", ok: true, error: "",
  objects: 113, commands: 64, submissions: 1, targets: 8, leftOut: 8, dataBytes: 7864320,
  files: ["main.cpp", "frame_objects.cpp", "frame_data.bin"], notes: [], problems: ["command 78: the pass was left out"],
};

test("the summary the replay tool writes is read field by field", () => {
  const e = parseExportSummary(bytes(summary));
  assert.equal(e.ok, true);
  assert.equal(e.directory, "D:/bugs/frame_cpp");
  assert.deepEqual([e.objects, e.commands, e.submissions, e.targets, e.leftOut], [113, 64, 1, 8, 8]);
  assert.deepEqual(e.files, ["main.cpp", "frame_objects.cpp", "frame_data.bin"]);
  assert.deepEqual(e.problems, ["command 78: the pass was left out"]);
});

test("a file that is not an export summary is refused, and missing fields read as empty", () => {
  assert.throws(() => parseExportSummary(bytes({ format: "gpu-inspector-draw-stats" })), /not an export summary/);
  const bare = parseExportSummary(bytes({ format: "gpu-inspector-export-cpp" }));
  assert.equal(bare.ok, false);
  assert.deepEqual([bare.objects, bare.files, bare.error], [0, [], ""]);
});

test("the status line says where the project is, how big, and what is not in it", () => {
  const text = exportSummaryText(parseExportSummary(bytes(summary)));
  assert.match(text, /exported C\+\+ project to D:\/bugs\/frame_cpp/);
  assert.match(text, /113 objects, 64 commands in 1 submission, 7\.5 MB of data/);
  assert.match(text, /8 commands left out; 1 replay problem, listed in its README/);
  const clean = exportSummaryText(parseExportSummary(bytes({ ...summary, leftOut: 0, problems: [], submissions: 2 })));
  assert.match(clean, /in 2 submissions/);
  assert.doesNotMatch(clean, /left out|problem/);
});

test("a failed export says why", () => {
  assert.equal(exportSummaryText(parseExportSummary(bytes({ ...summary, ok: false, error: "could not create Q:/nowhere" }))),
    "export to C++ failed: could not create Q:/nowhere");
});

test("a capture's project folder is named after the capture, safely", () => {
  assert.equal(exportFolderName("vkinsp_triangle_frame_108.gpucap"), "vkinsp_triangle_frame_108_cpp");
  assert.equal(exportFolderName("My Game (dev) frame 12.GPUCAP"), "My_Game_dev_frame_12_cpp");
  assert.equal(exportFolderName("///"), "frame_cpp");
});

test("the captures with a replay export: Vulkan, Direct3D 12 and Metal", () => {
  assert.deepEqual(["vulkan", "d3d12", "metal", "webgpu", undefined].map(exportsToCpp), [true, true, true, false, false]);
});
