// How a frame would fare on a tile-based GPU (src/renderer/tile_analysis.ts), over render graphs
// built from hand-made passes, one rule at a time.
//
// The numbers the report quotes are attachment bytes loaded into and stored out of tile memory,
// and the share of those the frame could avoid. What is avoidable has to be certain -- a store the
// next pass loads straight back, a store replaced before anything reads it, depth nothing reads,
// a result the next pass reads once per pixel -- because a report that calls a filter's input or
// the frame's own output "avoidable" sends someone to break a working frame.
//
//     cd src/app && npm test
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "tile-"));
const entry = join(dir, "entry.ts");
const src = join(here, "..", "src", "renderer").replace(/\\/g, "/");
writeFileSync(entry, `export { buildRenderGraph } from "${src}/render_graph.ts";\nexport { analyzeTiling } from "${src}/tile_analysis.ts";\n`);
const out = join(dir, "bundle.mjs");
buildSync({ entryPoints: [entry], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
const { buildRenderGraph, analyzeTiling } = await import(pathToFileURL(out).href);

const MB = 1 << 20;
const image = (id, label, size = "640x480", bytes = MB, presented = false) =>
  ({ key: `image:${id}`, objectId: id, type: "image", label, detail: `${size} R8G8B8A8_UNORM`, bytes, presented });
const buffer = (id, label) => ({ key: `buffer:${id}`, objectId: id, type: "buffer", label, detail: "64 KB", bytes: 65536, presented: false });

let command = 0;
/** A pass; attachments as [resource, "load/store" op pair, extra fields], reads as [resource, usage]. */
function pass(kind, label, { targets = [], reads = [], writes = [] } = {}) {
  const accesses = [];
  for (const [resource, ops, extra = {}] of targets) {
    const [load, store] = ops.split("/");
    accesses.push({ resource, mode: "write", usage: `color attachment (${load}/${store})`, discards: load !== "load" && load !== "local", dropped: store !== "store" && store !== "local", ...extra });
  }
  for (const [resource, usage] of reads) accesses.push({ resource, mode: "read", usage });
  for (const [resource, usage] of writes) accesses.push({ resource, mode: "write", usage });
  return { kind, label, commandIndex: command++, passKey: null, frame: 0, draws: 1, accesses };
}

test("traffic: what a pass loads and stores, at the attachments' sizes", () => {
  const scene = image(1, "Scene", "640x480", 2 * MB);
  const back = image(2, "Back buffer", "640x480", MB, true);
  const g = buildRenderGraph([
    pass("render", "Scene", { targets: [[scene, "clear/store"]] }),
    pass("render", "Composite", { targets: [[back, "load/store"]], reads: [[scene, "sampled"]] }),
  ]);
  const r = analyzeTiling(g);
  assert.equal(r.renderPasses, 2);
  assert.equal(r.loadBytes, MB, "the back buffer is loaded; the cleared scene is not");
  assert.equal(r.storeBytes, 3 * MB);
});

test("avoidable: a target stored and loaded straight back by the next pass to the same target", () => {
  const color = image(1, "Color");
  const g = buildRenderGraph([
    pass("render", "Opaque", { targets: [[color, "clear/store"]] }),
    pass("render", "Transparent", { targets: [[color, "load/store"]] }),
    pass("render", "Present", { targets: [[image(9, "Back", "640x480", MB, true), "clear/store"]], reads: [[color, "sampled"]] }),
  ]);
  const r = analyzeTiling(g);
  const [opaque, transparent] = r.passes;
  assert.match(opaque.attachments[0].avoidable, /loaded straight back/);
  assert.match(transparent.attachments[0].avoidable, /loaded from the pass before/);
  assert.equal(r.avoidableBytes, 2 * MB, "the store and the load of the one round trip");
});

test("a result the next pass reads once per pixel is avoidable; one it filters, or one not known, is not", () => {
  const lit = image(1, "Lit");
  const make = () => buildRenderGraph([
    pass("render", "Lighting", { targets: [[lit, "clear/store"]] }),
    pass("render", "Tonemap", { targets: [[image(2, "Out", "640x480", MB, true), "clear/store"]], reads: [[lit, "sampled"]] }),
  ]);
  const same = analyzeTiling(make(), { filtersInput: () => false });
  assert.match(same.passes[0].attachments[0].avoidable, /once per pixel/);
  assert.equal(same.avoidableBytes, MB);
  assert.equal(same.post[0].kind, "same-pixel");
  assert.equal(same.post[0].adjacent, true);

  const filtered = analyzeTiling(make(), { filtersInput: () => true });
  assert.equal(filtered.avoidableBytes, 0, "a blur's input has to be in memory");
  assert.match(filtered.passes[0].attachments[0].note, /filters it/);
  assert.equal(filtered.post[0].kind, "filters");

  const unknown = analyzeTiling(make());
  assert.equal(unknown.avoidableBytes, 0);
  assert.match(unknown.passes[0].attachments[0].note, /if that reads each pixel once/);
  assert.equal(unknown.post[0].kind, "unknown");
});

test("an unread color result is reported apart, not avoidable; unread depth and a replaced store are", () => {
  const eye = image(1, "Eye buffer");
  const depth = { ...image(2, "Eye depth"), key: "image:2" };
  const scratch = image(3, "Scratch");
  const g = buildRenderGraph([
    pass("render", "Scratch", { targets: [[scratch, "clear/store"]] }),
    pass("render", "Scratch again", { targets: [[scratch, "clear/store"]] }),
    pass("render", "Eye", { targets: [[eye, "clear/store"]], reads: [[scratch, "sampled"]] }),
  ]);
  // Depth goes in as a depth attachment of the eye pass.
  const withDepth = buildRenderGraph([
    ...[pass("render", "Scratch", { targets: [[scratch, "clear/store"]] }), pass("render", "Scratch again", { targets: [[scratch, "clear/store"]] })],
    { ...pass("render", "Eye", { targets: [[eye, "clear/store"]], reads: [[scratch, "sampled"]] }),
      accesses: [
        { resource: eye, mode: "write", usage: "color attachment (clear/store)", discards: true, dropped: false },
        { resource: depth, mode: "write", usage: "depth attachment (clear/store)", discards: true, dropped: false },
        { resource: scratch, mode: "read", usage: "sampled" },
      ] },
  ]);
  const r = analyzeTiling(g);
  assert.match(r.passes[0].attachments[0].avoidable, /replaced before anything reads it/);
  assert.equal(r.unreadBytes, MB, "the eye buffer: the frame's output to something the capture does not see");
  const d = analyzeTiling(withDepth);
  const eyePass = d.passes[2];
  assert.match(eyePass.attachments.find((a) => a.label === "Eye depth").avoidable, /depth stored/);
  assert.equal(eyePass.attachments.find((a) => a.label === "Eye buffer").avoidable, null);
});

test("a split pass's parts carry their attachments on chip: no traffic between them", () => {
  const gbuffer = image(1, "GBuffer0");
  const g = buildRenderGraph([
    pass("render", "GBuffer part 1", { targets: [[gbuffer, "clear/local"]] }),
    pass("render", "GBuffer part 2", { targets: [[gbuffer, "local/store"]] }),
    pass("render", "Lighting", { targets: [[image(2, "Out", "640x480", MB, true), "clear/store"]], reads: [[gbuffer, "sampled"]] }),
  ]);
  const r = analyzeTiling(g);
  assert.equal(r.passes[0].storeBytes, 0);
  assert.equal(r.passes[1].loadBytes, 0);
  assert.equal(r.passes[1].storeBytes, MB, "the last part's store reaches memory");
});

test("out of the tile: shader writes, sampling one's own target, compute between passes, transfers", () => {
  const color = image(1, "Color");
  const ssbo = buffer(2, "Particles");
  const g = buildRenderGraph([
    pass("render", "Draw", { targets: [[color, "clear/store"]], writes: [[ssbo, "storage"]] }),
    pass("compute", "Histogram", { reads: [[color, "sampled"]], writes: [[buffer(3, "Bins"), "storage"]] }),
    pass("render", "Feedback", { targets: [[color, "load/store"]], reads: [[color, "sampled"]] }),
    pass("transfer", "Copy", { reads: [[color, "copy src"]], writes: [[image(4, "Copy"), "copy dst"]] }),
    pass("render", "Present", { targets: [[image(5, "Back", "640x480", MB, true), "clear/store"]] }),
  ]);
  const kinds = analyzeTiling(g).outOfTile.map((o) => o.kind).sort();
  assert.deepEqual(kinds, ["compute-split", "feedback", "shader-write", "transfer"]);
});

test("subpasses: what the API says a render pass holds", () => {
  const g = buildRenderGraph([pass("render", "Deferred", { targets: [[image(1, "Out", "640x480", MB, true), "clear/store"]] })]);
  const r = analyzeTiling(g, { subpasses: () => ({ subpasses: 2, inputAttachments: 3 }) });
  assert.equal(r.subpassPasses.length, 1);
  assert.equal(r.passes[0].subpasses.inputAttachments, 3);
});
