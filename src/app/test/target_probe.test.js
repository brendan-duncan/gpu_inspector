// Probing the ports for applications that can be attached to (src/main/target_probe.ts): what a
// capture library's answer turns into, what a port with nothing on it does, and the two cases a
// probe has to survive without listing a target wrongly -- a library too old to know the
// handshake, which takes the probe for a client and starts sending a snapshot, and one that
// accepts the connection and then says nothing at all.
//
//     cd src/app && npm test
import { test } from "node:test";
import assert from "node:assert/strict";
import net from "node:net";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "targetprobe-"));
buildSync({
  entryPoints: [join(here, "..", "src", "main", "target_probe.ts")],
  bundle: true, format: "esm", platform: "node", outdir: join(dir, "build"), logLevel: "silent",
});
const { probePort, listTargets, targetDisplayName, FIRST_PORT, PORT_COUNT } =
  await import(pathToFileURL(join(dir, "build", "target_probe.js")).href);

/** A frame in the capture libraries' format: u32 payload length, u8 kind, payload. */
function frame(object) {
  const payload = Buffer.from(JSON.stringify(object), "utf8");
  const header = Buffer.alloc(5);
  header.writeUInt32LE(payload.length, 0);
  header.writeUInt8(0, 4);
  return Buffer.concat([header, payload]);
}

/**
 * A stand-in for a capture library on a port of its own: `answer` is given the request it was
 * sent and the socket, and does whatever that kind of library would do.
 */
async function listener(answer) {
  const server = net.createServer((sock) => {
    sock.once("data", (chunk) => {
      const request = JSON.parse(chunk.subarray(5).toString("utf8"));
      answer(request, sock);
    });
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  return { port: server.address().port, close: () => new Promise((r) => server.close(r)) };
}

test("a library's answer becomes a target", async () => {
  let asked = null;
  const server = await listener((request, sock) => {
    asked = request;
    sock.write(frame({ action: "Target", api: "Vulkan", name: "Shipping Game", exe: "game.exe", pid: 4242, busy: true }));
    sock.end();
  });
  const target = await probePort(server.port);
  await server.close();

  assert.deepEqual(asked, { action: "Probe" }, "the probe says what it is before anything else");
  assert.deepEqual(target, {
    port: server.port, api: "Vulkan", name: "Shipping Game", exe: "game.exe", pid: 4242, busy: true,
  });
  // The port the target is reached on is the one that was probed, not one it names itself.
  assert.equal(target.port, server.port);
});

test("a port with nothing on it is not a target", async () => {
  // Port 1 on loopback: nothing of ours is ever there, and the connection is refused at once.
  assert.equal(await probePort(1, 250), null);
});

test("a library too old for the handshake is reported, not listed wrongly", async () => {
  // It takes the probe for a client and starts sending the snapshot it sends every client.
  const server = await listener((_request, sock) => {
    sock.write(frame({ action: "SnapshotBegin", count: 2 }));
    sock.write(frame({ action: "AddObject", id: 1 }));
  });
  const target = await probePort(server.port);
  await server.close();

  assert.ok(target, "something is there, and the list has to say so");
  assert.equal(target.api, "", "but not what it is");
  assert.equal(target.port, server.port);
  assert.equal(targetDisplayName(target), `port ${server.port}`, "with nothing better to call it");
});

test("a listener that says nothing is not a target", async () => {
  const server = await listener(() => {});
  const target = await probePort(server.port, 250);
  await server.close();
  assert.equal(target, null);
});

test("the range walked is the one the capture libraries pick from", async () => {
  assert.equal(FIRST_PORT, 47531);
  assert.equal(PORT_COUNT, 8);
  // Nothing of ours is running under the test, so the range comes back empty rather than hanging.
  assert.deepEqual(await listTargets(250), []);
});

test("a target is named by the application, falling back to the executable", () => {
  const base = { port: 47531, api: "Vulkan", pid: 1, busy: false };
  assert.equal(targetDisplayName({ ...base, name: "Shipping Game", exe: "game.exe" }), "Shipping Game (game.exe)");
  assert.equal(targetDisplayName({ ...base, name: "", exe: "game.exe" }), "game.exe", "Direct3D 12 has no name of its own");
  assert.equal(targetDisplayName({ ...base, name: "game.exe", exe: "game.exe" }), "game.exe", "not said twice");
  assert.equal(targetDisplayName({ ...base, name: "", exe: "" }), "port 47531");
});
