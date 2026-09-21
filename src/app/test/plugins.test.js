// Plugins (docs/PLUGINS.md): how the app finds them and puts their capture libraries into a launch
// (src/main/plugins.ts), the backend registry a plugin's API joins (src/renderer/backend.ts), and the
// OpenGL ES plugin's backend (src/plugins/gles/ui/backend.ts) reading a capture the way its library
// records one -- synthetic passes, and a state snapshot on every draw.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "plugins-"));
const out = join(dir, "entry.mjs");
// One bundle, so the registry the plugin joins is the one the rest of the app reads.
buildSync({
  stdin: {
    contents: `
      export { findPlugins, pluginLaunches, pluginInfo, isInside, applyPreloads } from "../main/plugins.ts";
      export { backendFor, backendForObjectType, registerBackend, shortTypeName, apiDisplayName, isKnownApi, EMPTY_SETS } from "./backend.ts";
      export { activatePlugin } from "./plugin_host.ts";
      export { setsFor, labelNameOf } from "./command_sets.ts";
      export { CaptureData } from "./capture_data.ts";
      export { frameRenderGraph } from "./frame_graph.ts";
      export { drawState } from "./draw_state.ts";
      export { meshInput } from "./mesh_input.ts";
      export { VulkanObject } from "./vulkan/vulkan_object.ts";
      export { exportsToCpp } from "./export_cpp.ts";
      export { measuresWhileCapturing } from "./overdraw.ts";
      export * as gles from "../../../plugins/gles/ui/backend.ts";
    `,
    resolveDir: join(here, "..", "src", "renderer"), loader: "ts",
  },
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const m = await import(pathToFileURL(out).href);

// ------------------------------------------------------------------------------------------
// Finding plugins

function writePlugin(root, name, manifest, files = {}) {
  const p = join(root, name);
  mkdirSync(p, { recursive: true });
  writeFileSync(join(p, "plugin.json"), typeof manifest === "string" ? manifest : JSON.stringify(manifest));
  for (const [f, text] of Object.entries(files)) {
    mkdirSync(dirname(join(p, f)), { recursive: true });
    writeFileSync(join(p, f), text);
  }
  return p;
}

test("plugins are found in a directory of plugins, the first of each id winning", () => {
  const user = join(dir, "user");
  const build = join(dir, "build");
  writePlugin(user, "mine", { id: "gles", name: "My GLES", version: "9.0.0", sdk: 1, backend: "b.js" }, { "b.js": "" });
  writePlugin(build, "gles", { id: "gles", name: "OpenGL ES", version: "0.1.0", sdk: 1, backend: "ui/backend.js" }, { "ui/backend.js": "" });
  writePlugin(build, "future", { id: "future", name: "Future", version: "1", sdk: 99 });
  writePlugin(build, "broken", "{ not json");
  writePlugin(build, "unbuilt", { id: "unbuilt", name: "Unbuilt", version: "1", sdk: 1, backend: "ui/backend.js" });
  writePlugin(build, "escape", { id: "escape", name: "Escape", version: "1", sdk: 1, backend: "../outside.js" });
  const found = m.findPlugins([user, build, join(dir, "missing")]);
  const byId = Object.fromEntries(found.map((p) => [p.manifest.id, p]));
  assert.equal(byId.gles.manifest.name, "My GLES", "the user's copy overrides the built one");
  assert.equal(byId.gles.error, null);
  assert.equal(byId.gles.manifest.api, "gles", "the api defaults to the id");
  assert.match(byId.future.error, /plugin SDK 99/);
  assert.match(byId.broken.error, /does not parse/);
  assert.match(byId.unbuilt.error, /missing/);
  assert.match(byId.escape.error, /outside/);
  const info = m.pluginInfo(byId.gles);
  assert.equal(info.backendUrl, "gpuinsp-plugin://gles/b.js");
});

test("a launch gets each plugin's libraries and its settings with the session's values filled in", () => {
  const root = join(dir, "launch");
  const p = writePlugin(root, "gles", {
    id: "gles", name: "OpenGL ES", version: "1", sdk: 1,
    capture: {
      win32: { inject: ["bin/capture.dll"], env: { GLESINSP_PORT: "${port}", GLESINSP_LOG: "${log}", HOME_OF: "${pluginDir}" } },
      linux: { preload: ["lib/libcapture.so"], env: { GLESINSP_PORT: "${port}" } },
    },
  }, { "bin/capture.dll": "" });
  const plugins = m.findPlugins([root]);
  const [win] = m.pluginLaunches(plugins, { port: 47600, log: true, recordAlways: false, stacktraces: false }, "win32");
  assert.deepEqual(win.inject, [join(p, "bin", "capture.dll")]);
  assert.deepEqual(win.missing, []);
  assert.deepEqual(win.env, { GLESINSP_PORT: "47600", GLESINSP_LOG: "1", HOME_OF: p });
  const [linux] = m.pluginLaunches(plugins, { port: 1, log: false, recordAlways: false, stacktraces: false }, "linux");
  assert.deepEqual(linux.inject, [], "only Windows injects");
  assert.deepEqual(linux.missing, [join(p, "lib", "libcapture.so")], "a library not built is reported, not launched with");
  assert.equal(m.pluginLaunches(plugins, { port: 1, log: false, recordAlways: false, stacktraces: false }, "darwin").length, 0);
  const skipped = { LD_PRELOAD: "/usr/lib/theirs.so" };
  assert.match(m.applyPreloads(skipped, [linux], "LD_PRELOAD")[0], /not found/);
  assert.deepEqual(skipped, { LD_PRELOAD: "/usr/lib/theirs.so" }, "an unbuilt plugin changes nothing");
});

test("a preloaded library goes in front of what the environment already preloads, with its settings", () => {
  const root = join(dir, "preload");
  const p = writePlugin(root, "gles", {
    id: "gles", name: "OpenGL ES", version: "1", sdk: 1,
    capture: { linux: { preload: ["lib/libcapture.so"], env: { GLESINSP_PORT: "${port}" } } },
  }, { "lib/libcapture.so": "" });
  const launches = m.pluginLaunches(m.findPlugins([root]), { port: 7, log: false, recordAlways: false, stacktraces: false }, "linux");
  const env = { LD_PRELOAD: "/usr/lib/theirs.so" };
  const notes = m.applyPreloads(env, launches, "LD_PRELOAD");
  assert.deepEqual(env, { LD_PRELOAD: `${join(p, "lib", "libcapture.so")}:/usr/lib/theirs.so`, GLESINSP_PORT: "7" });
  assert.match(notes[0], /OpenGL ES capture library/);
});

// ------------------------------------------------------------------------------------------
// The registry

test("an API nobody registered opens on a backend that classifies nothing", () => {
  const b = m.backendFor("nosuchapi");
  assert.equal(b.sets, m.EMPTY_SETS);
  assert.equal(b.displayName, "nosuchapi");
  assert.equal(m.isKnownApi("nosuchapi"), false);
  assert.equal(m.exportsToCpp("nosuchapi"), false);
  assert.equal(m.measuresWhileCapturing("nosuchapi"), false);
  assert.equal(m.backendFor(undefined).id, "vulkan", "captures older than the api field are Vulkan's");
  assert.equal(m.shortTypeName("VkImage"), "Image");
  assert.equal(m.shortTypeName("ID3D12Resource"), "Resource");
  assert.equal(m.shortTypeName("MTLTexture"), "MTLTexture");
});

test("a built-in backend cannot be replaced by a plugin", () => {
  assert.throws(() => m.registerBackend({ id: "vulkan", displayName: "Fake", objectTypePrefixes: [], sets: m.EMPTY_SETS }), /built in/);
});

const pluginInfo = { id: "gles", name: "OpenGL ES", version: "0.1.0", api: "gles", dir: "/plugins/gles", backendUrl: null, error: null };
await m.activatePlugin(m.gles, pluginInfo, "mcp");

test("the OpenGL ES plugin registers its backend under its api, with its object prefix", async () => {
  const b = m.backendFor("gles");
  assert.equal(b.displayName, "OpenGL ES");
  assert.equal(b.builtin, false);
  assert.equal(b.plugin.id, "gles");
  assert.equal(m.setsFor("gles"), b.sets);
  assert.equal(m.backendForObjectType("GLTexture"), b);
  assert.equal(m.shortTypeName("GLTexture"), "Texture");
  assert.equal(m.apiDisplayName("gles"), "OpenGL ES");
  assert.equal(b.replay.draws, false, "no replay: the flags a plugin leaves out are off");
  await assert.rejects(m.activatePlugin({ activate: () => ({ ...b, id: "other" }) }, pluginInfo, "mcp"), /no backend for "gles"/);
  await assert.rejects(m.activatePlugin({}, pluginInfo, "mcp"), /no activate/);
});

// ------------------------------------------------------------------------------------------
// A capture of the OpenGL ES library: an offscreen pass drawing an indexed cube with a uniform block
// and a texture, its depth invalidated, then the surface drawing the offscreen color.

const CTX = 2;
const ref = (id, cls) => ({ __id: id, __class: cls });
const ctx = ref(CTX, "GLContext");
const addObject = (id, type, cmd, args, label = null) => ({ action: "AddObject", id, parent: CTX, type, cmd, index: 0, handle: String(id), label, args });
const objectMessages = [
  { action: "AddObject", id: 1, parent: 0, type: "GLSurface", cmd: "eglCreateWindowSurface", index: 0, handle: "0x1", label: null, args: { kind: "window", width: 8, height: 8 } },
  { action: "AddObject", id: CTX, parent: 0, type: "GLContext", cmd: "eglCreateContext", index: 0, handle: "0x2", label: null, args: { clientVersion: 3 } },
  addObject(5, "GLProgram", "glCreateProgram", {
    linked: true, stages: [{ type: "GL_VERTEX_SHADER", source: "void main() {}" }],
    uniformBlocks: [{ name: "Transform", index: 0, dataSize: 80, members: [{ name: "mvp", type: "mat4", offset: 0, matrixStride: 16 }, { name: "tint", type: "vec4", offset: 64 }] }],
  }, "cube"),
  addObject(13, "GLBuffer", "glGenBuffers", { size: 96, usage: "GL_STATIC_DRAW" }, "vertices"),
  addObject(14, "GLBuffer", "glGenBuffers", { size: 12, usage: "GL_STATIC_DRAW" }, "indices"),
  addObject(15, "GLBuffer", "glGenBuffers", { size: 80, usage: "GL_DYNAMIC_DRAW" }, "transform"),
  addObject(18, "GLTexture", "glGenTextures", { target: "GL_TEXTURE_2D", format: "VK_FORMAT_R8G8B8A8_UNORM", width: 4, height: 4, levels: 1 }, "checker"),
  addObject(20, "GLTexture", "glGenTextures", { target: "GL_TEXTURE_2D", format: "VK_FORMAT_R8G8B8A8_UNORM", width: 8, height: 8, levels: 1 }, "offscreen color"),
  addObject(21, "GLRenderbuffer", "glGenRenderbuffers", { format: "VK_FORMAT_X8_D24_UNORM_PACK32", width: 8, height: 8 }),
  addObject(22, "GLFramebuffer", "glGenFramebuffers", {}, "offscreen"),
];
const objects = new Map(objectMessages.map((msg) => [msg.id, new m.VulkanObject(msg)]));
const db = { getObject: (id) => objects.get(id) ?? null };

const cubeState = {
  program: ref(5, "GLProgram"), framebuffer: ref(22, "GLFramebuffer"), passIndex: 0, mode: "GL_TRIANGLES",
  draw: { indexed: true, count: 6, first: 0, instances: 1, baseVertex: 0, indirect: false },
  vertexArray: ref(12, "GLVertexArray"), elementBuffer: ref(14, "GLBuffer"), indexType: "GL_UNSIGNED_SHORT", indexOffset: 0, indexData: 1,
  attributes: [
    { name: "position", location: 0, type: "GL_FLOAT_VEC3", enabled: true, buffer: ref(13, "GLBuffer"), size: 3, componentType: "GL_FLOAT", normalized: false, integer: false, stride: 20, offset: 0, divisor: 0, data: 2 },
    { name: "uv", location: 1, type: "GL_FLOAT_VEC2", enabled: true, buffer: ref(13, "GLBuffer"), size: 2, componentType: "GL_UNSIGNED_BYTE", normalized: true, integer: false, stride: 20, offset: 12, divisor: 0, data: 3 },
    { name: "color", location: 2, type: "GL_FLOAT_VEC4", enabled: false, value: [1, 0.5, 0, 1] },
  ],
  firstVertex: 0, lastVertex: 3,
  textures: [{ uniform: "checker", type: "sampler2D", unit: 0, target: "GL_TEXTURE_2D", texture: ref(18, "GLTexture"), sampler: null, capture: 1 }],
  uniformBlocks: [{ name: "Transform", index: 0, binding: 2, dataSize: 80, buffer: ref(15, "GLBuffer"), offset: 0, size: 0, data: 4 }],
  uniforms: [{ name: "checker", type: "sampler2D", location: 0, value: [0] }, { name: "scale", type: "float", location: 1, value: [0.5] }],
  raster: { viewport: [0, 0, 8, 8], scissorTest: false, cullFace: true, cullMode: "GL_BACK", frontFace: "GL_CCW" },
  depth: { test: true, func: "GL_LESS", write: true, range: [0, 1] },
  stencil: { test: false },
  blend: { enabled: false, colorMask: [true, true, true, false] },
};
const quadState = {
  program: ref(5, "GLProgram"), framebuffer: null, passIndex: 1, mode: "GL_TRIANGLE_STRIP",
  draw: { indexed: false, count: 4, first: 0, instances: 1, baseVertex: 0, indirect: false },
  attributes: [], firstVertex: 0, lastVertex: 3,
  textures: [{ uniform: "image", type: "sampler2D", unit: 0, target: "GL_TEXTURE_2D", texture: ref(20, "GLTexture"), sampler: null, capture: 2 }],
  uniformBlocks: [], uniforms: [],
};
const command = (method, args, extra = {}) => ({ frame: 0, method, object: ctx, args, ...extra });
const commands = [
  command("glPushDebugGroupKHR", { source: "GL_DEBUG_SOURCE_APPLICATION", id: 1, length: -1, message: "offscreen cube" }),
  command("glBindFramebuffer", { target: "GL_FRAMEBUFFER", framebuffer: ref(22, "GLFramebuffer") }),
  command("BeginRenderPass", {
    framebuffer: ref(22, "GLFramebuffer"), passIndex: 0, synthetic: true, cleared: ["color", "depth"], invalidated: ["GL_DEPTH_ATTACHMENT"],
    attachments: [
      { attachment: "GL_COLOR_ATTACHMENT0", object: ref(20, "GLTexture"), level: 0, format: "VK_FORMAT_R8G8B8A8_UNORM", width: 8, height: 8 },
      { attachment: "GL_DEPTH_ATTACHMENT", object: ref(21, "GLRenderbuffer"), level: 0, format: "VK_FORMAT_X8_D24_UNORM_PACK32", width: 8, height: 8 },
    ],
  }),
  command("glClear", { mask: "GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT" }),
  command("glUseProgram", { program: ref(5, "GLProgram") }),
  command("glDrawElements", { mode: "GL_TRIANGLES", count: 6, type: "GL_UNSIGNED_SHORT", indices: null }, { state: cubeState }),
  command("glInvalidateFramebuffer", { target: "GL_FRAMEBUFFER", numAttachments: 1, attachments: ["GL_DEPTH_ATTACHMENT"] }),
  command("EndRenderPass", { framebuffer: ref(22, "GLFramebuffer"), synthetic: true }),
  command("glPopDebugGroupKHR", {}),
  command("glBindFramebuffer", { target: "GL_FRAMEBUFFER", framebuffer: null }),
  command("BeginRenderPass", { framebuffer: null, surface: ref(1, "GLSurface"), width: 8, height: 8, passIndex: 1, synthetic: true, cleared: ["color"] }),
  command("glClear", { mask: "GL_COLOR_BUFFER_BIT" }),
  command("glDrawArrays", { mode: "GL_TRIANGLE_STRIP", first: 0, count: 4 }, { state: quadState }),
  command("EndRenderPass", { framebuffer: null, synthetic: true }),
  command("eglSwapBuffers", { dpy: "0x1", surface: ref(1, "GLSurface") }),
].map((c, index) => ({ index, ...c }));

const data = new m.CaptureData();
data.handleMessage({ action: "CaptureFrameResults", frame: 10, frames: 1, count: commands.length, batches: 1, api: "gles" });
data.handleMessage({ action: "CaptureFrameCommands", frame: 10, index: 0, commands });
// Index data: 0,1,2, 2,1,3. Vertices: four of 20 bytes, position floats then uv bytes.
const indexBytes = new Uint8Array(new Uint16Array([0, 1, 2, 2, 1, 3]).buffer);
const vertexBytes = new Uint8Array(80);
const view = new DataView(vertexBytes.buffer);
[[0, 0, 0], [1, 0, 0], [0, 1, 0], [1, 1, 0]].forEach((p, i) => p.forEach((v, k) => view.setFloat32(i * 20 + k * 4, v, true)));
data.handleMessage({
  action: "CaptureBuffers", count: 4, buffers: [
    { id: 1, buffer: 14, frame: 0, commandBuffer: CTX, offset: 0, size: 12 },
    { id: 2, buffer: 13, frame: 0, commandBuffer: CTX, offset: 0, size: 80 },
    { id: 3, buffer: 13, frame: 0, commandBuffer: CTX, offset: 12, size: 68 },
    { id: 4, buffer: 15, frame: 0, commandBuffer: CTX, offset: 0, size: 80 },
  ],
});
data.handleMessage({ action: "CaptureBufferData", id: 1, size: 12, __binary: indexBytes });
data.handleMessage({ action: "CaptureBufferData", id: 2, size: 80, __binary: vertexBytes });

test("the plugin's command sets classify the library's commands and name its passes and groups", () => {
  const sets = data.sets;
  assert.ok(sets.DRAW.has("glDrawElements") && sets.DRAW.has("glDrawArrays"));
  assert.ok(sets.PASS_BEGIN.has("BeginRenderPass") && sets.PASS_END.has("EndRenderPass"));
  assert.ok(sets.SUBMIT.has("eglSwapBuffers"), "a swap bounds a context's pass numbering, as the library's does");
  assert.equal(m.labelNameOf(commands[0], sets), "offscreen cube");
  const names = new Map([[22, "offscreen"], [5, "cube"]]);
  const nameOf = (v) => (v && typeof v.__id === "number" ? names.get(v.__id) ?? "" : "");
  assert.equal(sets.passLabel(commands[2], 0, nameOf), "Render Pass 0: offscreen + depth");
  assert.equal(sets.passLabel(commands[10], 1, nameOf), "Render Pass 1: default framebuffer");
  assert.equal(sets.summarize(commands[5], nameOf), "6 idx");
  assert.equal(sets.summarize(commands[12], nameOf), "4 verts");
  assert.equal(sets.summarize(commands[4], nameOf), "cube");
  assert.equal(sets.summarize(commands[3], nameOf), "depth | color");
  assert.deepEqual(sets.drawArgsOf(commands[5]), { indexed: true, indexCount: 6, firstIndex: 0, vertexOffset: 0, instanceCount: 1 });
});

test("a draw's state is the snapshot on it, in the shape the mesh view reads", () => {
  const state = m.drawState(data, db, commands[5]);
  assert.equal(state.pipeline?.id, 5, "the program stands where a pipeline would");
  assert.equal(state.dynamic.topology, "VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST");
  assert.deepEqual([...state.vertexBuffers.keys()], [0, 1], "a disabled attribute reads no buffer");
  assert.equal(state.vertexBuffers.get(1).dataId, 3);
  assert.equal(state.indexBuffer.indexType, "VK_INDEX_TYPE_UINT16");
  const formats = state.vertexInput.pVertexAttributeDescriptions.map((a) => a.format);
  assert.deepEqual(formats, ["VK_FORMAT_R32G32B32_SFLOAT", "VK_FORMAT_R8G8_UNORM"]);
  const mesh = m.meshInput(data, db, commands[5], new Map([[0, "position"]]));
  assert.deepEqual(mesh.indices, [0, 1, 2, 2, 1, 3]);
  assert.deepEqual(mesh.values(5, 0), [1, 1, 0], "the sixth index is vertex 3, read at 3 x stride in the attribute's range");
});

test("the plugin's command details describe a draw section by section", () => {
  const b = m.backendFor("gles");
  const sections = b.commandDetails(commands[5], { data, db, nameOf: (id) => db.getObject(id)?.name ?? "" });
  const titles = sections.map((s) => s.title);
  assert.deepEqual(titles, ["Program", "Vertex Shader", "Vertex Input", "Index Buffer", "Textures", "Uniform Blocks", "Uniforms", "Rasterizer", "Depth and Stencil", "Blend"]);
  const input = sections.find((s) => s.title === "Vertex Input");
  assert.deepEqual(input.table.rows[0][7], { buffer: 2 });
  assert.equal(input.table.rows[2][2], "disabled");
  const blocks = sections.find((s) => s.title === "Uniform Blocks");
  const contents = blocks.table.rows[0][5];
  assert.equal(contents.buffer, 4);
  assert.deepEqual(contents.members[0], { name: "mvp", type: "mat4", offset: 0, matrixStride: 16 }, "the block is typed by the offsets the driver reported");
  const uniforms = sections.find((s) => s.title === "Uniforms");
  assert.deepEqual(uniforms.table.rows, [["scale", "float", 1, "0.5"]], "samplers are the Textures section's");
  assert.deepEqual(sections.find((s) => s.title === "Textures").table.rows[0][5], { texture: 1 });
  const pass = b.commandDetails(commands[2], { data, db, nameOf: () => "" });
  assert.equal(pass[0].title, "Render Pass");
  assert.equal(pass[0].table.rows.length, 2);
});

test("the render graph sees the passes, what they sample, and what they clear and throw away", () => {
  const graph = m.frameRenderGraph(data, db);
  assert.equal(graph.nodes.length, 2);
  const [offscreen, surface] = graph.nodes;
  assert.equal(offscreen.label, "offscreen");
  assert.equal(surface.label, "default framebuffer");
  const depth = offscreen.writes.find((u) => u.resource.objectId === 21);
  assert.equal(depth.dropped, true, "the invalidated depth is thrown away");
  assert.equal(depth.discards, true, "and cleared before the pass drew");
  const color = offscreen.writes.find((u) => u.resource.objectId === 20);
  assert.equal(color.discards, true);
  assert.equal(color.dropped, false);
  assert.ok(surface.reads.some((u) => u.resource.objectId === 20 && u.usage.includes("sampled")));
  assert.ok(surface.writes.some((u) => u.resource.objectId === 1 && u.resource.presented), "the surface is what the frame is for");
  assert.ok(graph.edges.some((e) => e.from === offscreen && e.to === surface), "the offscreen pass feeds the surface's");
  assert.equal(graph.unreadNodes.length, 0);
});

test("GL objects are summarized by the plugin and their types read without the prefix", () => {
  assert.equal(objects.get(20).shortType, "Texture");
  assert.equal(objects.get(20).summary(db), "R8G8B8A8_UNORM 8x8");
  assert.equal(objects.get(15).summary(db), "80 B DYNAMIC_DRAW");
  assert.equal(objects.get(5).summary(db), "vertex");
  assert.equal(objects.get(CTX).summary(db), "OpenGL ES 3.0");
});
