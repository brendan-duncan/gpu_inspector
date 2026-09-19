// Finding the Chromium browsers on this machine and the command line that opens a page in one
// with its GPU process captured (src/main/browsers.ts). The installations are faked under a
// temporary root, since what a machine has installed is not something a test can rely on.
//
//     cd src/app && npm test
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "browsers-"));
const out = join(dir, "browsers.mjs");
buildSync({
  entryPoints: [join(here, "..", "src", "main", "browsers.ts")],
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const { BROWSER_FOLLOW, browserArgs, browserProfileDir, installedBrowsers } = await import(pathToFileURL(out).href);

/** An installation: <root>/<relative>, with `versions` as the versioned directories beside it. */
function install(root, relative, versions = []) {
  const exe = join(root, relative);
  mkdirSync(dirname(exe), { recursive: true });
  writeFileSync(exe, "");
  for (const v of versions) mkdirSync(join(dirname(exe), v), { recursive: true });
  return exe;
}

const windows = process.platform === "win32";

test("the installed browsers are found under the Windows roots, newest version first", { skip: !windows }, () => {
  const local = join(dir, "local");
  const programs = join(dir, "programs");
  const chrome = install(programs, "Google\\Chrome\\Application\\chrome.exe", ["99.0.1.2", "153.0.8010.48"]);
  const canary = install(local, "Google\\Chrome SxS\\Application\\chrome.exe", ["155.0.8100.0"]);
  const edge = install(programs, "Microsoft\\Edge\\Application\\msedge.exe");
  const saved = { p: process.env["ProgramFiles"], x: process.env["ProgramFiles(x86)"], l: process.env["LOCALAPPDATA"] };
  process.env["ProgramFiles"] = programs;
  process.env["ProgramFiles(x86)"] = programs;   // the same root twice: each browser is listed once
  process.env["LOCALAPPDATA"] = local;
  try {
    const found = installedBrowsers();
    assert.deepEqual(found.map((b) => b.path), [chrome, canary, edge]);
    assert.deepEqual(found.map((b) => b.name), ["Google Chrome", "Google Chrome Canary", "Microsoft Edge"]);
    // The version comes from the versioned directory Chromium keeps its build in; 153 is newer
    // than 99, which sorting as text would get backwards, and an install without one has none.
    assert.equal(found[0].version, "153.0.8010.48");
    assert.equal(found[1].version, "155.0.8100.0");
    assert.equal(found[2].version, "");
  } finally {
    process.env["ProgramFiles"] = saved.p;
    process.env["ProgramFiles(x86)"] = saved.x;
    process.env["LOCALAPPDATA"] = saved.l;
  }
});

test("the browser's command line turns the GPU sandbox off and keeps its own profile", () => {
  const args = browserArgs("https://example.com/page", "C:\\profiles\\Chrome");
  // Without this the capture library in the GPU process cannot open its port and nothing connects.
  assert.ok(args.includes("--disable-gpu-sandbox"));
  assert.ok(args.includes("--disable-gpu-watchdog"));
  // A profile of our own, or a browser already running takes the page and this process exits.
  assert.ok(args.includes("--user-data-dir=C:\\profiles\\Chrome"));
  // The page is last, as a browser expects, and an empty one leaves the browser to open its own.
  assert.equal(args[args.length - 1], "https://example.com/page");
  assert.ok(!browserArgs("  ", "C:\\profiles\\Chrome").some((a) => !a.startsWith("--")));
});

test("the GPU process is followed and the information-collection one left alone", () => {
  assert.deepEqual(BROWSER_FOLLOW, ["--type=gpu-process", "!--use-gl=disabled"]);
});

test("each browser gets a profile directory of its own", () => {
  const chrome = browserProfileDir("C:\\data", "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe");
  const canary = browserProfileDir("C:\\data", "C:\\Users\\me\\AppData\\Local\\Google\\Chrome SxS\\Application\\chrome.exe");
  assert.notEqual(chrome, canary);
  assert.ok(chrome.startsWith(join("C:\\data", "browser-profiles")));
  // No spaces or separators from the browser's own directory name.
  assert.ok(/^[\w.-]+$/.test(canary.slice(canary.lastIndexOf("\\") + 1) || canary));
});
