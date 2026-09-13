// The replays kept alive for captures (src/main/replay.ts): requests answered by one process in turn,
// a process that cannot serve or dies falling back to a one-shot replay, and the renderer's keyed
// captures. A fake vkinsp_replay stands in for the real one: Node running a script that speaks the
// --serve protocol (Serve in replay/src/main.cpp), so the "capture file" handed to the pool is the
// script itself.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, readdirSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "replayserver-"));
const out = join(dir, "replay.mjs");
buildSync({ entryPoints: [join(here, "..", "src", "main", "replay.ts")], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
const { ReplayServerPool, replayKeyed, releaseReplayKey, replayServers } = await import(pathToFileURL(out).href);

// The fake tool. FAKE_MODE: "serve" answers requests; "crash" exits on its first request; anything
// else behaves like a tool from before --serve (prints usage). Without --serve it is a one-shot replay
// writing its data file. CommonJS, since the keyed test hands it over under a .gpucap name.
const fake = join(dir, "fake_replay.cjs");
writeFileSync(fake, `
const { writeFileSync } = require("node:fs");
const { createInterface } = require("node:readline");
const args = process.argv.slice(2);
const mode = process.env.FAKE_MODE;
if (!args.includes("--serve")) {
  const flag = args.findIndex((a) => a.endsWith("-data"));
  writeFileSync(args[flag + 1], JSON.stringify({ oneShot: true, flag: args[flag] }));
  process.exit(0);
}
if (mode !== "serve" && mode !== "crash") {
  console.error("usage: vkinsp_replay <capture.gpucap> ...");
  process.exit(2);
}
console.log("some driver chatter");
console.log("@replay " + JSON.stringify({ ready: true, device: "Fake GPU", problems: 0 }));
const rl = createInterface({ input: process.stdin });
rl.on("line", (line) => {
  const r = JSON.parse(line);
  if (r.kind === "quit") process.exit(0);
  if (mode === "crash") process.exit(3);
  // Answered a little later, so requests sent together are still waiting when the next arrives.
  setTimeout(() => {
    writeFileSync(r.out, JSON.stringify({ pid: process.pid, request: r }));
    console.log("@replay " + JSON.stringify({ id: r.id, ok: true, ms: 1 }));
  }, 5);
});
`);

const json = (run) => JSON.parse(new TextDecoder().decode(run.data));

test("analyses of one capture go to one process, each answered with its own data", async () => {
  process.env.FAKE_MODE = "serve";
  const pool = new ReplayServerPool();
  try {
    const first = json(await pool.run(process.execPath, fake, { kind: "overdraw" }));
    assert.equal(first.request.kind, "overdraw");
    const [pixel, mesh, overlay] = (await Promise.all([
      pool.run(process.execPath, fake, { kind: "pixel", image: 17, x: 3, y: 4 }),
      pool.run(process.execPath, fake, { kind: "mesh", commands: [9, 10] }),
      pool.run(process.execPath, fake, { kind: "overlay", commands: [11] }),
    ])).map(json);
    assert.deepEqual([pixel.request.kind, mesh.request.kind, overlay.request.kind], ["pixel", "mesh", "overlay"]);
    assert.deepEqual([pixel.request.image, pixel.request.x, pixel.request.y, pixel.request.mip, pixel.request.layer], [17, 3, 4, 0, 0]);
    assert.deepEqual(mesh.request.commands, [9, 10]);
    assert.ok([pixel, mesh, overlay].every((r) => r.pid === first.pid), "the same process answered every request");
  } finally {
    pool.disposeAll();
  }
});

test("a tool that cannot serve, or a process that dies, falls back to a one-shot replay", async () => {
  const pool = new ReplayServerPool();
  try {
    process.env.FAKE_MODE = "old";
    const old = await pool.run(process.execPath, fake, { kind: "draws" });
    assert.deepEqual(json(old), { oneShot: true, flag: "--draw-data" });

    process.env.FAKE_MODE = "crash";
    const crashed = await pool.run(process.execPath, fake, { kind: "mesh", commands: [1] });
    assert.deepEqual(json(crashed), { oneShot: true, flag: "--mesh-data" }, "the request that killed the process was answered by a one-shot replay");

    process.env.FAKE_MODE = "serve";
    const again = json(await pool.run(process.execPath, fake, { kind: "overdraw" }));
    assert.equal(again.request.kind, "overdraw", "the next request started a new process");
  } finally {
    pool.disposeAll();
  }
});

test("a renderer's capture is sent once under its key, and removed when released", async () => {
  process.env.FAKE_MODE = "serve";
  const key = `test:${Date.now()}`;
  try {
    const asked = await replayKeyed(process.execPath, key, undefined, { kind: "overdraw" }, "cap");
    assert.equal(asked.needData, true, "a new key asks for the capture's bytes");
    // The "capture" is the fake tool's script, since that is what the tool runs.
    const bytes = new TextEncoder().encode(readFileSync(fake, "utf8"));
    const served = await replayKeyed(process.execPath, key, bytes, { kind: "overdraw" }, "cap");
    assert.equal(json(served).request.kind, "overdraw");
    const later = await replayKeyed(process.execPath, key, undefined, { kind: "draws" }, "cap");
    assert.equal(later.needData, undefined, "the key's capture is kept");
    assert.equal(json(later).pid, json(served).pid, "and so is its replay");
    const files = () => readdirSync(tmpdir()).filter((f) => f.startsWith(`vkinsp_replay_${process.pid}_`) && f.endsWith("_cap.gpucap"));
    assert.equal(files().length, 1);
    releaseReplayKey(key);
    await new Promise((r) => setTimeout(r, 200));
    assert.equal(files().length, 0, "releasing the key removes its file");
    assert.equal((await replayKeyed(process.execPath, key, undefined, { kind: "draws" }, "cap")).needData, true);
  } finally {
    replayServers.disposeAll();
  }
});
