// The MCP server's live sessions (src/mcp/live_session.ts, live_tools.ts) against a fake capture
// library: a TCP server speaking the layer's framing (layer/src/transport.h) that sends a snapshot
// and frame reports, streams a capture when asked, and answers blob, stack, image, descriptor set
// and shader requests. Attaching, frame statistics, capturing into a saved and reopened .gpucap
// (with and without the CaptureComplete marker), live reads, shader restore, the log and stopping.
//
//     cd app && npm test
import { test, after } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import net from "node:net";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = fs.mkdtempSync(join(tmpdir(), "mcp-live-"));
process.env.GPU_INSPECTOR_CAPTURES_DIR = join(dir, "captures");
process.env.GPU_INSPECTOR_CAPTURE_QUIET_MS = "300";
const out = join(dir, "server.mjs");
buildSync({ entryPoints: [join(here, "..", "src", "mcp", "server.ts")], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
const { createServer, CaptureStore, SessionManager } = await import(pathToFileURL(out).href);

const sessions = new SessionManager();
const server = createServer(new CaptureStore(), sessions);
const layers = [];
after(async () => {
  await sessions.stopAll();
  for (const layer of layers) layer.close();
});

let nextId = 1;
async function call(name, args = {}) {
  const reply = await server.handle({ jsonrpc: "2.0", id: nextId++, method: "tools/call", params: { name, arguments: args } });
  const result = reply.result;
  const text = result.content.find((c) => c.type === "text")?.text ?? "";
  return { result, text, json: result.isError ? null : JSON.parse(text) };
}

// ------------------------------------------------------------------------------------------
// The fake capture library

const frame = (msg) => {
  const payload = Buffer.from(JSON.stringify(msg));
  const header = Buffer.alloc(5);
  header.writeUInt32LE(payload.length, 0);
  return Buffer.concat([header, payload]);
};
const binary = (msg, bytes) => {
  const json = Buffer.from(JSON.stringify(msg));
  const header = Buffer.alloc(9);
  header.writeUInt32LE(4 + json.length + bytes.byteLength, 0);
  header.writeUInt8(1, 4);
  header.writeUInt32LE(json.length, 5);
  return Buffer.concat([header, json, Buffer.from(bytes)]);
};

const spirv = new Uint8Array(new Uint32Array([0x07230203, 0x00010300, 0, 1, 0, 0x00020011, 1]).buffer);
const CB = { __id: 6, __class: "VkCommandBuffer" };

function fakeLayer({ marker, dropFirst = 0 }) {
  const add = (id, type, cmd, args, extra = {}) => ({ action: "AddObject", id, parent: 0, type, cmd, index: 0, handle: `0x${id}`, label: null, args, ...extra });
  const objects = [
    add(1, "VkInstance", "vkCreateInstance", { pCreateInfo: { pApplicationInfo: { pApplicationName: "Fake App" } } }),
    add(3, "VkImage", "vkCreateImage", { pCreateInfo: { format: "VK_FORMAT_R8G8B8A8_UNORM", extent: { width: 2, height: 2, depth: 1 }, mipLevels: 1, arrayLayers: 1 } }),
    add(5, "VkPipeline", "vkCreateGraphicsPipelines", { pCreateInfos: [{ pStages: [{ stage: "VK_SHADER_STAGE_VERTEX_BIT", pName: "main" }] }] },
      { blobs: [{ name: "vertex:main", size: spirv.byteLength }] }),
    add(6, "VkCommandBuffer", "vkAllocateCommandBuffers", { pAllocateInfo: {} }),
    add(7, "VkImageView", "vkCreateImageView", { pCreateInfo: { image: { __id: 3, __class: "VkImage" }, subresourceRange: { baseMipLevel: 0, levelCount: 1, baseArrayLayer: 0, layerCount: 1 } } }),
    add(8, "VkDescriptorSet", "vkAllocateDescriptorSets", { pAllocateInfo: {} }),
    add(9, "VkBuffer", "vkCreateBuffer", { pCreateInfo: { size: 64 } }),
    add(10, "VkRenderPass", "vkCreateRenderPass", { pCreateInfo: { pAttachments: [{ format: "VK_FORMAT_R8G8B8A8_UNORM", storeOp: "VK_ATTACHMENT_STORE_OP_DONT_CARE" }] } }),
    add(11, "VkFramebuffer", "vkCreateFramebuffer", { pCreateInfo: { renderPass: { __id: 10, __class: "VkRenderPass" }, pAttachments: [{ __id: 7, __class: "VkImageView" }] } }),
  ];
  const requests = [];
  const sockets = new Set();
  const respond = (sock, msg) => {
    requests.push(msg);
    const cmd = (index, method, args) => ({ index, frame: 0, method, object: CB, args, slot: index });
    switch (msg.action) {
      case "Capture":
        setTimeout(() => {
          sock.write(frame({ action: "CaptureFrameResults", frame: 200, frames: 1, count: 4, batches: 1 }));
          sock.write(frame({ action: "CaptureFrameCommands", frame: 200, index: 0, commands: [
            cmd(0, "vkCmdBeginRenderPass", { pRenderPassBegin: { renderArea: { offset: { x: 0, y: 0 }, extent: { width: 2, height: 2 } } } }),
            cmd(1, "vkCmdBindPipeline", { pipelineBindPoint: "VK_PIPELINE_BIND_POINT_GRAPHICS", pipeline: { __id: 5, __class: "VkPipeline" } }),
            cmd(2, "vkCmdDraw", { vertexCount: 3, instanceCount: 1, firstVertex: 0, firstInstance: 0 }),
            cmd(3, "vkCmdEndRenderPass", {}),
          ] }));
          sock.write(frame({ action: "CaptureTextureFrames", count: 1, textures: [
            { id: 3, frame: 0, commandBuffer: 6, passIndex: 0, attachment: 0, format: "VK_FORMAT_R8G8B8A8_UNORM", aspect: "color", width: 2, height: 2, depth: 1, layers: 1, mip: 0, size: 16 },
          ] }));
          sock.write(binary({ action: "CaptureTextureData", id: 3, frame: 0, commandBuffer: 6, passIndex: 0, attachment: 0, size: 16 }, new Uint8Array(16).fill(255)));
          sock.write(frame({ action: "CapturePassTimings", timestampPeriodNs: 1, count: 1, passes: [{ frame: 0, commandBuffer: 6, passIndex: 0, startMs: 0, durationMs: 1.5 }] }));
          if (marker) sock.write(frame({ action: "CaptureComplete", frame: 200, frames: 1 }));
        }, 30);
        break;
      case "RequestBlob":
        sock.write(msg.id === 5 ? binary({ action: "ObjectBlob", id: 5, index: msg.index, size: spirv.byteLength }, spirv) : frame({ action: "ObjectBlob", id: msg.id, index: msg.index, size: 0 }));
        break;
      case "RequestStacktraces":
        sock.write(frame({ action: "Stacktraces", available: false, stacks: [] }));
        break;
      case "RequestImage": {
        const head = { action: "ImageData", id: msg.id, mip: msg.mip, layer: msg.layer, depth: 1, layers: 1, format: "VK_FORMAT_R8G8B8A8_UNORM", aspect: "color", width: 2, height: 2 };
        if (msg.mip > 0) sock.write(frame({ ...head, size: 0, error: "The image's layout is not readable." }));
        else sock.write(binary({ ...head, size: 16 }, new Uint8Array([255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255])));
        break;
      }
      case "RequestDescriptorSet":
        sock.write(frame({ action: "ObjectUpdate", id: msg.id, tracked: true, layout: null, bindings: [
          { binding: 0, type: "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER", stages: "VK_SHADER_STAGE_VERTEX_BIT", descriptors: [{ buffer: { __id: 9, __class: "VkBuffer" }, offset: 0, range: 64 }] },
          { binding: 1, type: "VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER", descriptors: [{ imageView: { __id: 7, __class: "VkImageView" }, imageLayout: "VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL" }, null] },
        ] }));
        break;
      case "RestoreShader":
        sock.write(frame({ action: "ShaderReplaced", pipeline: msg.pipeline, stage: msg.stage ?? "", ok: true }));
        break;
      default:
        break;
    }
  };
  // The first connections are accepted and closed at once, as adb does before the layer listens on the device.
  let dropped = 0;
  const tcp = net.createServer((sock) => {
    if (dropped < dropFirst) {
      dropped++;
      sock.destroy();
      return;
    }
    sockets.add(sock);
    sock.on("error", () => {});
    sock.write(frame({ action: "Snapshot", count: objects.length }));
    for (const o of objects) sock.write(frame(o));
    let n = 0;
    const tick = setInterval(() => sock.write(frame({ action: "FrameStats", frame: 100 + n++, frameTimeMs: 16.6, frames: 6, submitMs: 1.2, refreshMs: 16.667, refreshSource: "estimate", presentMode: "FIFO" })), 50);
    sock.on("close", () => {
      clearInterval(tick);
      sockets.delete(sock);
    });
    let buffered = Buffer.alloc(0);
    sock.on("data", (chunk) => {
      buffered = Buffer.concat([buffered, chunk]);
      while (buffered.length >= 5) {
        const len = buffered.readUInt32LE(0);
        if (buffered.length < 5 + len) break;
        respond(sock, JSON.parse(buffered.subarray(5, 5 + len).toString()));
        buffered = buffered.subarray(5 + len);
      }
    });
  });
  let closed = false;
  const close = () => {
    if (closed) return;
    closed = true;
    for (const s of sockets) s.destroy();
    tcp.close();
  };
  // Closed after the tests too, so a failed assertion does not leave the process running.
  layers.push({ close });
  return new Promise((resolve) => tcp.listen(0, "127.0.0.1", () => resolve({ port: tcp.address().port, requests, close, dropped: () => dropped })));
}

// ------------------------------------------------------------------------------------------

test("an attached application is watched, captured, saved and reopened", async () => {
  const layer = await fakeLayer({ marker: true });
  const status = (await call("attach_app", { port: layer.port })).json;
  assert.equal(status.state, "connected");
  assert.equal(status.name, "Fake App", "named after the instance's application name");
  assert.equal(status.api, "vulkan");
  assert.equal(status.objects.live, 9, "the snapshot arrived before attach_app returned");

  const stats = (await call("get_live_frame_stats", { seconds: 0.4 })).json;
  assert.ok(stats.frames >= 6);
  assert.equal(stats.frameMs, 16.6);
  assert.match(stats.verdict, /Meeting the display refresh/);

  const captured = (await call("capture_frames", {})).json;
  assert.ok(fs.existsSync(captured.file), "saved as a file");
  assert.ok(captured.file.startsWith(process.env.GPU_INSPECTOR_CAPTURES_DIR));
  assert.equal(captured.captureNotes, undefined, "completed by the marker");
  assert.equal(captured.counts.draws, 1);
  assert.equal(captured.timing.profiled, true);
  assert.equal(captured.application, "Fake App");
  const request = layer.requests.find((r) => r.action === "Capture");
  assert.equal(request.profilePasses, true);
  assert.equal(request.maxBufferSize, 128 * 1024);

  // The pipeline's SPIR-V was fetched for the file, so the capture tools read the shader.
  assert.ok(layer.requests.some((r) => r.action === "RequestBlob" && r.id === 5));
  assert.equal((await call("get_shader", { capture: captured.capture, object: 5 })).json.stages[0].spirvVersion, "1.3");
  assert.equal((await call("read_texture", { capture: captured.capture, texture: 0 })).result.content[0].type, "image");

  // Reads between captures: an image through its view, a descriptor set, the live objects.
  const image = await call("read_live_image", { object: 7, texels: [[1, 1]] });
  assert.equal(image.result.content[0].type, "image", image.text);
  assert.equal(image.json.image, "VkImage#3");
  assert.deepEqual(image.json.texels[0].value, [1, 0, 0, 1]);
  assert.match(image.json.note, /discards it/, "the only render pass drawing into it does not store it");
  assert.equal(layer.requests.find((r) => r.action === "RequestImage").id, 3, "a view reads its image");
  const unreadable = await call("read_live_image", { object: 3, mip: 1 });
  assert.equal(unreadable.result.isError, true);
  assert.match(unreadable.text, /layout is not readable/);
  const set = (await call("get_live_descriptor_set", { set: 8 })).json;
  assert.equal(set.bindings[0].descriptors[0].buffer, "VkBuffer#9");
  assert.equal(set.bindings[1].descriptors[0].image, "VkImage#3");
  assert.equal(set.bindings[1].descriptors[1], "not written");
  assert.deepEqual((await call("list_live_objects", { type: "image" })).json.objects.map((o) => o.id), [3]);
  const view = (await call("get_live_object", { id: 7 })).json;
  assert.equal(view.createdBy, "vkCreateImageView");
  assert.match(view.contents, /read_live_image/);
  assert.match(view.stackNote, /records no stacks/);

  const restored = (await call("restore_shader", { pipeline: 5, stage: "vertex" })).json;
  assert.equal(restored.ok, true);
  assert.equal(layer.requests.find((r) => r.action === "RestoreShader").stage, "VK_SHADER_STAGE_VERTEX_BIT");
  assert.ok((await call("get_session_log", { match: "connected" })).json.lines.length > 0);

  const stopped = (await call("stop_app", {})).json;
  assert.equal(stopped.state, "disconnected");
  const refused = await call("capture_frames", {});
  assert.equal(refused.result.isError, true);
  assert.match(refused.text, /not connected/);
  layer.close();
});

test("a capture library without the end marker completes once its stream goes quiet", async () => {
  const layer = await fakeLayer({ marker: false });
  await call("attach_app", { port: layer.port });
  const captured = (await call("capture_frames", {})).json;
  assert.equal(captured.counts.draws, 1);
  assert.match(captured.captureNotes[0], /stream went quiet/);
  await call("stop_app", {});
  layer.close();
});

test("a connection closed before the capture library speaks is tried again, as over an adb forward", async () => {
  const layer = await fakeLayer({ marker: true, dropFirst: 2 });
  const status = (await call("attach_app", { port: layer.port })).json;
  assert.equal(status.state, "connected");
  assert.equal(status.objects.live, 9);
  assert.equal(layer.dropped(), 2);
  const lines = (await call("get_session_log", {})).json.lines;
  assert.equal(lines.filter((l) => l.startsWith("[disconnected]")).length, 0, "the dropped attempts are not reported as disconnections");
  await call("stop_app", {});
  layer.close();
});

test("launching and attaching fail with what to do", async () => {
  const missing = await call("launch_app", { exe: join(dir, "no-such-app.exe") });
  assert.equal(missing.result.isError, true);
  assert.match(missing.text, /No executable/);
  const nothing = await call("attach_app", { port: 1, waitSeconds: 1 });
  assert.equal(nothing.result.isError, true);
  assert.match(nothing.text, /Nothing answered on port 1/);
  const unknown = await call("get_session_status", { session: "app-99" });
  assert.match(unknown.text, /No live session "app-99"/);
});
