// Call stacks sampled during a timing capture (src/renderer/timing_samples.ts): what each thread
// was doing over a stretch of frames. The judgement being tested is which threads are worth a line
// and what is said of them — a thread blocked for a whole hitch is as much the answer as one that
// ran through it, and a process has dozens that did neither.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const out = join(mkdtempSync(join(tmpdir(), "timingsamples-")), "timing_samples.mjs");
buildSync({ entryPoints: [join(here, "..", "src", "renderer", "timing_samples.ts")], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
const { appendTimingSamples, emptyTimingSamples, summarizeSamples, summaryAddresses, stackText } = await import(pathToFileURL(out).href);

const message = (samples, stacks = [], threads = [{ id: 100, name: "Render" }, { id: 200 }, { id: 300, name: "Loader" }]) =>
  ({ action: "TimingSamples", periodMs: 4, threads, stacks, samples });

test("batches append: a stack is sent once, and the thread list replaces itself", () => {
  const store = emptyTimingSamples();
  appendTimingSamples(store, message([[10, 0, 1, 1, 3]], [{ id: 1, addresses: ["0x10", "0x20"] }], [{ id: 100, name: "Render" }]));
  appendTimingSamples(store, message([[11, 0, 1, 1, 2], [11, 2, 2, 0, 5]], [{ id: 2, addresses: ["0x30"] }]));
  assert.equal(store.samples.length, 3);
  assert.deepEqual(store.stacks.get(1), ["0x10", "0x20"]);
  assert.equal(store.threads.length, 3);
  assert.equal(store.periodMs, 4);
});

test("a stretch is summed per thread, with samples turned into time", () => {
  const store = emptyTimingSamples();
  appendTimingSamples(store, message([
    [10, 0, 1, 1, 5], [10, 0, 2, 0, 1], [10, 2, 3, 0, 6],
    [11, 0, 1, 1, 50],   // another frame: not asked about
  ], [{ id: 1, addresses: ["0xa"] }, { id: 2, addresses: ["0xb"] }, { id: 3, addresses: ["0xc"] }]));
  const s = summarizeSamples(store, 10, 10);
  assert.equal(s.samples, 12);
  const render = s.threads.find((t) => t.thread.name === "Render");
  assert.deepEqual([render.running, render.samples, render.runningMs, render.waitingMs], [5, 6, 20, 4]);
  assert.deepEqual(render.ranIn.map((x) => x.addresses), [["0xa"]]);
  assert.deepEqual(render.waitedIn.map((x) => x.addresses), [["0xb"]]);
});

test("a thread blocked through the stretch is listed when it works elsewhere, and a parked one is only counted", () => {
  const store = emptyTimingSamples();
  appendTimingSamples(store, message([
    [4, 0, 5, 1, 2],                     // Render ran in the frame before
    [5, 0, 1, 0, 100],                   // and was blocked for the whole of this one
    [5, 2, 2, 1, 90], [5, 2, 3, 0, 10],  // Loader ran through it
    [4, 1, 4, 0, 2], [5, 1, 4, 0, 100],  // a pool thread that never runs at all
  ], [1, 2, 3, 4, 5].map((id) => ({ id, addresses: [`0x${id}`] }))));
  const s = summarizeSamples(store, 5, 5);
  // Who ran first, then who waited: the render thread did nothing here, and what it was blocked
  // in says whom it was waiting for.
  assert.deepEqual(s.threads.map((t) => t.thread.name), ["Loader", "Render"]);
  assert.deepEqual([s.threads[1].running, s.threads[1].waitingMs], [0, 400]);
  assert.deepEqual(s.threads[1].waitedIn.map((x) => x.addresses), [["0x1"]]);
  assert.equal(s.idle, 1);
  assert.deepEqual(summaryAddresses(s).sort(), ["0x1", "0x2", "0x3"]);
});

test("no samples in the stretch is no summary", () => {
  const store = emptyTimingSamples();
  appendTimingSamples(store, message([[5, 0, 1, 1, 3]], [{ id: 1, addresses: ["0x1"] }]));
  assert.equal(summarizeSamples(store, 50, 60), null);
});

test("a stack reads innermost first, without the inspector's frames or the thread's start", () => {
  const symbols = new Map(Object.entries({
    "0x1": { function: "NtWaitForSingleObject", module: "ntdll.dll" },
    "0x2": { function: "dxinsp::Hook_WaitForSingleObject", module: "dxinsp_capture.dll", internal: true },
    "0x3": { function: "Loader::ReadBlocking", file: "C:\\game\\src\\loader.cpp", line: 212 },
    "0x4": { function: "Game::Tick" },
    "0x5": { function: "BaseThreadInitThunk" },
    "0x6": { module: "nvwgf2umx.dll" },
  }));
  const text = stackText(["0x1", "0x2", "0x3", "0x4", "0x5"], (a) => symbols.get(a));
  assert.equal(text, "NtWaitForSingleObject ← Loader::ReadBlocking (loader.cpp:212) ← Game::Tick");
  // A driver with no symbols is its module, once, however many of its frames there are.
  assert.equal(stackText(["0x6", "0x6", "0x4"], (a) => symbols.get(a)), "nvwgf2umx.dll ← Game::Tick");
  // An address nobody could name is still something to search for.
  assert.equal(stackText(["0x99"], () => undefined), "0x99");
});
