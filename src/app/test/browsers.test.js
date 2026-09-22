// Finding the browsers on this machine and what it takes to open a page in one with its GPU
// process captured (src/main/browsers.ts): the two families want different command lines, different
// settings and different child processes followed. The installations are faked under a temporary
// root, since what a machine has installed is not something a test can rely on.
//
//     cd src/app && npm test
import { test } from "node:test";
import assert from "node:assert/strict";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { buildSync } from "esbuild";

const here = dirname(fileURLToPath(import.meta.url));
const dir = mkdtempSync(join(tmpdir(), "browsers-"));
const out = join(dir, "browsers.mjs");
buildSync({
  entryPoints: [join(here, "..", "src", "main", "browsers.ts")],
  bundle: true, format: "esm", platform: "node", outfile: out, logLevel: "silent",
});
const { CHROMIUM_FOLLOW, FIREFOX_FOLLOW, browserArgs, browserFamily, browserFollow, browserProfileDir, installedBrowsers, prepareProfile } =
  await import(pathToFileURL(out).href);

const CHROME = "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe";
const CANARY = "C:\\Users\\me\\AppData\\Local\\Google\\Chrome SxS\\Application\\chrome.exe";
const FIREFOX = "C:\\Program Files\\Mozilla Firefox\\firefox.exe";
const NIGHTLY = "C:\\Program Files\\Firefox Nightly\\firefox.exe";
const LINUX_CHROME = "/usr/bin/google-chrome";

/** An installation: <root>/<relative>, with `versions` as the versioned directories beside it. */
function install(root, relative, versions = []) {
  const exe = join(root, relative);
  mkdirSync(dirname(exe), { recursive: true });
  writeFileSync(exe, "");
  for (const v of versions) mkdirSync(join(dirname(exe), v), { recursive: true });
  return exe;
}

const windows = process.platform === "win32";

test("the installed browsers are found under the Windows roots, with their versions", { skip: !windows }, () => {
  const local = join(dir, "local");
  const programs = join(dir, "programs");
  const chrome = install(programs, "Google\\Chrome\\Application\\chrome.exe", ["99.0.1.2", "153.0.8010.48"]);
  const canary = install(local, "Google\\Chrome SxS\\Application\\chrome.exe", ["155.0.8100.0"]);
  const edge = install(programs, "Microsoft\\Edge\\Application\\msedge.exe");
  // Firefox has no versioned directory: its version is in application.ini beside the executable.
  const firefox = install(programs, "Mozilla Firefox\\firefox.exe");
  writeFileSync(join(dirname(firefox), "application.ini"), "[App]\nName=Firefox\nVersion=156.0\n");
  const nightly = install(programs, "Firefox Nightly\\firefox.exe");
  writeFileSync(join(dirname(nightly), "application.ini"), "[App]\nName=Firefox\nVersion=154.0a1\n");
  const saved = { p: process.env["ProgramFiles"], x: process.env["ProgramFiles(x86)"], l: process.env["LOCALAPPDATA"] };
  process.env["ProgramFiles"] = programs;
  process.env["ProgramFiles(x86)"] = programs;   // the same root twice: each browser is listed once
  process.env["LOCALAPPDATA"] = local;
  try {
    const found = installedBrowsers();
    assert.deepEqual(found.map((b) => b.path), [chrome, canary, edge, firefox, nightly]);
    assert.deepEqual(found.map((b) => b.name),
      ["Google Chrome", "Google Chrome Canary", "Microsoft Edge", "Firefox", "Firefox Nightly"]);
    assert.deepEqual(found.map((b) => b.family), ["chromium", "chromium", "chromium", "firefox", "firefox"]);
    // Chromium's version is the versioned directory: 153 is newer than 99, which sorting as text
    // would get backwards, and an install without one has no version to show.
    assert.equal(found[0].version, "153.0.8010.48");
    assert.equal(found[1].version, "155.0.8100.0");
    assert.equal(found[2].version, "");
    assert.equal(found[3].version, "156.0");
    assert.equal(found[4].version, "154.0a1");
  } finally {
    process.env["ProgramFiles"] = saved.p;
    process.env["ProgramFiles(x86)"] = saved.x;
    process.env["LOCALAPPDATA"] = saved.l;
  }
});

test("a Chromium browser's command line turns the GPU sandbox off and keeps its own profile", () => {
  const args = browserArgs(CHROME, "https://example.com/page", "C:\\profiles\\Chrome", "win32");
  // Without this the capture library in the GPU process cannot open its port and nothing connects.
  assert.ok(args.includes("--disable-gpu-sandbox"));
  assert.ok(args.includes("--disable-gpu-watchdog"));
  // A profile of our own, or a browser already running takes the page and this process exits.
  assert.ok(args.includes("--user-data-dir=C:\\profiles\\Chrome"));
  // The page is last, as a browser expects, and an empty one leaves the browser to open its own.
  assert.equal(args[args.length - 1], "https://example.com/page");
  assert.ok(!browserArgs(CHROME, "  ", "C:\\profiles\\Chrome", "win32").some((a) => !a.startsWith("--")));
  // Windows needs no adapter switch: its default WebGPU adapter is the D3D12 one already captured.
  assert.ok(!args.some((a) => a.startsWith("--use-webgpu-adapter")));
});

test("on Linux a Chromium browser is pointed at the real GPU, not SwiftShader", () => {
  // Left alone, Chrome on Linux answers requestAdapter with SwiftShader, which renders WebGPU on
  // the CPU and makes no Vulkan device through the loader — so there is nothing for the layer to
  // capture and the session never connects.
  const args = browserArgs(LINUX_CHROME, "https://example.com/page", "/home/me/profiles/Chrome", "linux");
  assert.ok(args.includes("--use-webgpu-adapter=vulkan"));
  assert.ok(args.includes("--disable-gpu-sandbox"));
  assert.ok(args.includes("--user-data-dir=/home/me/profiles/Chrome"));
  assert.equal(args[args.length - 1], "https://example.com/page");
});

test("a Linux browser is recognized by its extensionless name, and profiles by that name", () => {
  assert.equal(browserFamily(LINUX_CHROME), "chromium");
  assert.equal(browserFamily("/usr/bin/firefox"), "firefox");
  assert.equal(browserFamily("/usr/bin/firefox-esr"), "firefox");
  // The install is one file per channel rather than a directory per channel, so the executable's
  // own name is what keeps two channels' profiles apart.
  const chrome = browserProfileDir("/home/me/data", LINUX_CHROME);
  const beta = browserProfileDir("/home/me/data", "/usr/bin/google-chrome-beta");
  assert.notEqual(chrome, beta);
  assert.ok(chrome.endsWith("google-chrome"));
  // A Windows path is still taken apart the Windows way, whatever platform this runs on.
  assert.ok(browserProfileDir("C:\\data", NIGHTLY).endsWith("Firefox_Nightly"));
});

test("Firefox is launched into its own profile, away from the one the user has open", () => {
  // -no-remote, or a running Firefox takes the page and this process exits with nothing to capture.
  assert.deepEqual(browserArgs(NIGHTLY, "https://example.com/page", "C:\\profiles\\FF"),
    ["-no-remote", "-profile", "C:\\profiles\\FF", "https://example.com/page"]);
  assert.equal(browserFamily(NIGHTLY), "firefox");
  assert.equal(browserFamily(CHROME), "chromium");
});

test("Firefox's profile carries the settings a capture needs, since it has no switch for them", () => {
  const profile = join(dir, "ffprofile");
  prepareProfile(FIREFOX, profile);
  const prefs = readFileSync(join(profile, "user.js"), "utf8");
  // The GPU process sandbox: without it the capture library cannot open its port.
  assert.match(prefs, /user_pref\("security\.sandbox\.gpu\.level", 0\);/);
  assert.match(prefs, /user_pref\("dom\.webgpu\.enabled", true\);/);
  // A Chromium profile takes its settings on the command line, so nothing is written into it.
  const chromeProfile = join(dir, "chromeprofile");
  prepareProfile(CHROME, chromeProfile);
  assert.ok(existsSync(chromeProfile));
  assert.ok(!existsSync(join(chromeProfile, "user.js")));
});

test("each family's own renderer process is followed", () => {
  // Chromium: the GPU process, but not the second one Chrome starts to collect GPU information.
  assert.deepEqual(CHROMIUM_FOLLOW, ["--type=gpu-process", "!--use-gl=disabled"]);
  assert.deepEqual(browserFollow(CHROME), CHROMIUM_FOLLOW);
  // Firefox: its children end with their type, and that leading space keeps a "gpu" elsewhere in
  // the command line (a profile path, say) from matching every tab.
  assert.deepEqual(FIREFOX_FOLLOW, [" gpu"]);
  assert.deepEqual(browserFollow(NIGHTLY), FIREFOX_FOLLOW);
});

test("each browser gets a profile directory of its own", () => {
  const chrome = browserProfileDir("C:\\data", CHROME);
  const canary = browserProfileDir("C:\\data", CANARY);
  const firefox = browserProfileDir("C:\\data", FIREFOX);
  const nightly = browserProfileDir("C:\\data", NIGHTLY);
  assert.notEqual(chrome, canary);
  // Firefox keeps its executable one directory up from Chromium's, so the install's own directory
  // is what tells a Firefox profile from a Nightly one.
  assert.notEqual(firefox, nightly);
  assert.ok(nightly.endsWith("Firefox_Nightly"));
  assert.ok(chrome.startsWith(join("C:\\data", "browser-profiles")));
  // No spaces or separators from the browser's own directory name.
  assert.ok(/^[\w.-]+$/.test(basename(canary)));
});
