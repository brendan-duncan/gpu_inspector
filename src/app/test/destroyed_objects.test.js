// An object database keeps what a frame creates and releases within itself (object_database.ts,
// _recentlyDestroyed and pinCaptured): an engine makes per-frame buffers and command lists, and the
// capture of that frame arrives after they are gone. Without them its commands named nothing, in
// the UI and in the saved file, and a replay had no buffer to bind.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "destroyed-"));
const out = join(dir, "object_database.mjs");
buildSync({ entryPoints: [join(here, "..", "src", "renderer", "vulkan", "object_database.ts")], bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent" });
const { ObjectDatabase } = await import(pathToFileURL(out).href);

const add = (id) => ({ action: "AddObject", id, parent: 0, type: "ID3D12Resource", cmd: "CreateCommittedResource", index: 0, handle: `0x${id.toString(16)}`, label: null, args: { pDesc: { Width: 256 } } });

test("an object released before its capture arrives is still found, marked destroyed", () => {
  const db = new ObjectDatabase();
  db.handleMessage(add(7));
  db.handleMessage({ action: "DeleteObjects", ids: [7] });
  assert.equal(db.allObjects.has(7), false);
  const kept = db.getObject(7);
  assert.ok(kept);
  assert.equal(kept.isDeleted, true);
  assert.equal(kept.args.pDesc.Width, 256);
});

test("what a finished capture references outlives the ring of recently destroyed objects", () => {
  const db = new ObjectDatabase();
  db.handleMessage(add(1));
  db.handleMessage(add(2));
  db.handleMessage({ action: "DeleteObjects", ids: [1, 2] });
  db.pinCaptured([1, 99]);   // 99 was never there: nothing to pin, and no error
  // The ring holds a bounded number, oldest out first.
  const limit = ObjectDatabase.RECENTLY_DESTROYED_LIMIT;
  for (let id = 1000; id < 1000 + limit + 10; id++) {
    db.handleMessage(add(id));
    db.handleMessage({ action: "DeleteObjects", ids: [id] });
  }
  assert.ok(db.getObject(1), "the pinned object was dropped");
  assert.equal(db.getObject(2), null, "an object no capture references is kept for a while, not for good");
  assert.ok(db.getObject(1000 + limit + 9), "the most recent are still there");
});

test("a reset forgets them", () => {
  const db = new ObjectDatabase();
  db.handleMessage(add(3));
  db.handleMessage({ action: "DeleteObjects", ids: [3] });
  db.pinCaptured([3]);
  db.reset();
  assert.equal(db.getObject(3), null);
});
