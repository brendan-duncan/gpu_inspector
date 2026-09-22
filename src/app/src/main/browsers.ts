// The browsers installed on this machine, and what it takes to open a page in one with the
// Direct3D 12 capture library in the process that renders it.
//
// A browser does not render in the process the user starts. Its WebGPU work — and its compositing —
// is in a GPU process the browser spawns itself, so on Windows the launch is an ordinary one with
// the launcher's follow mode on top, which puts the library into that child as it appears
// (src/d3d12/launcher/main.cpp, follow mode). What a capture then holds is the D3D12 underneath
// WebGPU, which is a lower question than the page-level WebGPU Inspector answers.
//
// Linux needs no injection at all: the Vulkan layer is enabled by environment variables, and a
// child process inherits its parent's environment, so the GPU process comes up with the layer in
// it already. What a capture holds there is the Vulkan underneath WebGPU — Dawn's backend on
// Linux, and wgpu's. The catch is which adapter the browser picks: left alone, Chrome on Linux
// answers requestAdapter with SwiftShader, its software renderer, which makes no Vulkan device
// through the loader and so cannot be captured. `--use-webgpu-adapter=vulkan` is what makes it
// use the real GPU, and is in the Linux arguments below for that reason.
//
// Two families, which agree on nothing: Chromium runs WebGPU through Dawn and names its child
// processes with --type=gpu-process, and takes its settings as switches; Firefox runs it through
// wgpu (whose Windows backend is D3D12 as well) and ends each child's command line with its type
// (" gpu"), and has no switch for the GPU sandbox at all — it is a preference, so it goes into the
// profile this launch makes. Firefox also starts its browser from a first process that exits
// straight away, which is why following tracks the target's whole tree.
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";

import type { BrowserInstall } from "../shared/protocol.js";

export type BrowserFamily = BrowserInstall["family"];

/**
 * Chromium: the GPU process, and not the second one Chrome starts to collect GPU information (it
 * makes a device of its own and exits again, and would take the session's port from the one that
 * renders; the library refuses a port another process already serves).
 */
export const CHROMIUM_FOLLOW = ["--type=gpu-process", "!--use-gl=disabled"];

/**
 * Firefox: its children end with "- <childID> <type>" (gpu, tab, rdd, socket, utility), so the
 * type is matched with its space. Bare "gpu" would match any child whose command line holds it
 * somewhere else — a profile path, for one — and inject into every tab.
 */
export const FIREFOX_FOLLOW = [" gpu"];

/** Where the browsers are, relative to each of the roots below. */
const KNOWN: { name: string; relative: string; family: BrowserFamily }[] = [
  { name: "Google Chrome", relative: "Google\\Chrome\\Application\\chrome.exe", family: "chromium" },
  { name: "Google Chrome Beta", relative: "Google\\Chrome Beta\\Application\\chrome.exe", family: "chromium" },
  { name: "Google Chrome Dev", relative: "Google\\Chrome Dev\\Application\\chrome.exe", family: "chromium" },
  // Canary installs per user, under its own "SxS" directory, so it sits beside a stable Chrome.
  { name: "Google Chrome Canary", relative: "Google\\Chrome SxS\\Application\\chrome.exe", family: "chromium" },
  { name: "Microsoft Edge", relative: "Microsoft\\Edge\\Application\\msedge.exe", family: "chromium" },
  { name: "Microsoft Edge Canary", relative: "Microsoft\\Edge SxS\\Application\\msedge.exe", family: "chromium" },
  { name: "Brave", relative: "BraveSoftware\\Brave-Browser\\Application\\brave.exe", family: "chromium" },
  { name: "Firefox", relative: "Mozilla Firefox\\firefox.exe", family: "firefox" },
  { name: "Firefox ESR", relative: "Mozilla Firefox ESR\\firefox.exe", family: "firefox" },
  { name: "Firefox Developer Edition", relative: "Firefox Developer Edition\\firefox.exe", family: "firefox" },
  { name: "Firefox Nightly", relative: "Firefox Nightly\\firefox.exe", family: "firefox" },
];

/**
 * Where the browsers are on Linux. Absolute paths rather than a PATH search: a distribution's
 * wrapper script in /usr/bin is what the user starts, and following it to the real binary would
 * lose the wrapper's own setup. The snap paths are listed because a snap Chromium or Firefox is
 * what Ubuntu installs by default.
 */
const KNOWN_LINUX: { name: string; paths: string[]; family: BrowserFamily }[] = [
  { name: "Google Chrome", paths: ["/usr/bin/google-chrome", "/usr/bin/google-chrome-stable", "/opt/google/chrome/google-chrome"], family: "chromium" },
  { name: "Google Chrome Beta", paths: ["/usr/bin/google-chrome-beta", "/opt/google/chrome-beta/google-chrome-beta"], family: "chromium" },
  { name: "Google Chrome Dev", paths: ["/usr/bin/google-chrome-unstable", "/opt/google/chrome-unstable/google-chrome-unstable"], family: "chromium" },
  { name: "Chromium", paths: ["/usr/bin/chromium", "/usr/bin/chromium-browser", "/snap/bin/chromium"], family: "chromium" },
  { name: "Microsoft Edge", paths: ["/usr/bin/microsoft-edge", "/usr/bin/microsoft-edge-stable"], family: "chromium" },
  { name: "Brave", paths: ["/usr/bin/brave-browser", "/usr/bin/brave"], family: "chromium" },
  { name: "Firefox", paths: ["/usr/bin/firefox", "/snap/bin/firefox", "/opt/firefox/firefox"], family: "firefox" },
  { name: "Firefox ESR", paths: ["/usr/bin/firefox-esr"], family: "firefox" },
  { name: "Firefox Nightly", paths: ["/usr/bin/firefox-nightly", "/opt/firefox-nightly/firefox"], family: "firefox" },
];

/**
 * The version on Linux, which unlike Windows is not in the installed layout: there is no
 * versioned directory beside the executable and no application.ini, so the browser is asked.
 * Bounded and failure-tolerant — an unreadable version only costs the label beside the name.
 */
function linuxVersion(exe: string): string {
  try {
    const out = execFileSync(exe, ["--version"], { encoding: "utf8", timeout: 4000, stdio: ["ignore", "pipe", "ignore"] });
    return /(\d+(?:\.\d+)+)/.exec(out)?.[1] ?? "";
  } catch {
    return "";
  }
}

function roots(): string[] {
  const dirs = [process.env["ProgramFiles"], process.env["ProgramFiles(x86)"], process.env["LOCALAPPDATA"]];
  return dirs.filter((d): d is string => !!d);
}

/** Which of the two a browser belongs to, by its executable, for one chosen by path as well. */
export function browserFamily(exe: string): BrowserFamily {
  // A browser's path may be a Windows path whatever this runs on (the unit tests run on every
  // platform), and only path.win32 takes one apart there; it takes a POSIX path apart too, since
  // it treats a forward slash as a separator as well.
  const base = path.win32.basename(exe).toLowerCase();
  // firefox.exe on Windows; on Linux the executable has no extension and the channel is in its
  // name (firefox-esr, firefox-nightly), all of them wgpu rather than Dawn.
  return base === "firefox.exe" || base === "firefox" || base.startsWith("firefox-") ? "firefox" : "chromium";
}

/**
 * The version to show beside the name, so a Chrome and a Canary (or a Firefox and a Nightly) are
 * told apart by more than that name; empty when there is none to read. Chromium keeps its build in
 * a versioned directory beside the executable ("Application\\153.0.8010.48"); Firefox writes its
 * version into application.ini ("154.0a1" for a Nightly).
 */
function versionOf(exe: string, family: BrowserFamily): string {
  try {
    if (family === "firefox") {
      const ini = fs.readFileSync(path.join(path.dirname(exe), "application.ini"), "utf8");
      return /^Version=(.+)$/m.exec(ini)?.[1].trim() ?? "";
    }
    const versions = fs.readdirSync(path.dirname(exe), { withFileTypes: true })
      .filter((e) => e.isDirectory() && /^\d+\.\d+\.\d+\.\d+$/.test(e.name))
      .map((e) => e.name);
    // Newest first, by each number rather than as text (153 sorts before 99 as text).
    versions.sort((a, b) => {
      const x = a.split(".").map(Number), y = b.split(".").map(Number);
      for (let i = 0; i < 4; i++) if (x[i] !== y[i]) return y[i] - x[i];
      return 0;
    });
    return versions[0] ?? "";
  } catch {
    return "";
  }
}

/**
 * The browsers found on this machine, in the order above. Windows and Linux; macOS has no capture
 * layer for a browser's Metal to go into.
 */
export function installedBrowsers(): BrowserInstall[] {
  if (process.platform === "linux") {
    const found: BrowserInstall[] = [];
    for (const { name, paths, family } of KNOWN_LINUX) {
      const exe = paths.find((f) => fs.existsSync(f));
      if (exe) found.push({ name, path: exe, version: linuxVersion(exe), family });
    }
    return found;
  }
  if (process.platform !== "win32") return [];
  const found: BrowserInstall[] = [];
  const seen = new Set<string>();
  for (const { name, relative, family } of KNOWN) {
    for (const root of roots()) {
      const exe = path.join(root, relative);
      const key = exe.toLowerCase();
      if (seen.has(key) || !fs.existsSync(exe)) continue;
      seen.add(key);
      found.push({ name, path: exe, version: versionOf(exe, family), family });
    }
  }
  return found;
}

/** The processes of this browser to inject into: the one that renders the page, and no other. */
export function browserFollow(exe: string): string[] {
  return browserFamily(exe) === "firefox" ? FIREFOX_FOLLOW : CHROMIUM_FOLLOW;
}

/**
 * The browser's command line for a page.
 *
 * The GPU sandbox has to be off either way, and not as a nicety: the capture library in a sandboxed
 * GPU process cannot open its port, so nothing ever connects. Chromium takes that as a switch (with
 * `--disable-gpu-watchdog`, so it does not kill the GPU process while a capture holds it); Firefox
 * has no switch for it and takes it as a preference, which `prepareProfile` writes.
 *
 * The profile is the inspector's own on both: the browser the user has open keeps its windows, its
 * session and its extensions, and a browser already running does not hand the page to that instance
 * and exit, leaving nothing to capture (`-no-remote` is what tells Firefox not to).
 */
export function browserArgs(exe: string, url: string, profileDir: string, platform: NodeJS.Platform = process.platform): string[] {
  const page = url.trim() ? [url.trim()] : [];
  if (browserFamily(exe) === "firefox") {
    return ["-no-remote", "-profile", profileDir, ...page];
  }
  return [
    "--disable-gpu-sandbox",
    "--disable-gpu-watchdog",
    "--no-first-run",
    "--no-default-browser-check",
    // Linux: without this Chrome answers requestAdapter with SwiftShader, which renders WebGPU on
    // the CPU and makes no Vulkan device for the layer to capture. Windows needs no counterpart —
    // its default WebGPU adapter is the D3D12 one the library is already in.
    ...(platform === "linux" ? ["--use-webgpu-adapter=vulkan", "--enable-features=Vulkan", "--enable-unsafe-webgpu"] : []),
    `--user-data-dir=${profileDir}`,
    ...page,
  ];
}

/**
 * Makes the profile directory, and for Firefox writes the preferences the capture needs into it:
 * the GPU process sandbox off (there is no command line switch for it), and WebGPU on for a build
 * that still has it behind the preference. user.js is read at every start and wins over whatever
 * the profile has saved, so this holds however the profile was left last time.
 */
export function prepareProfile(exe: string, profileDir: string): void {
  fs.mkdirSync(profileDir, { recursive: true });
  if (browserFamily(exe) !== "firefox") return;
  const prefs = [
    "// Written by GPU Inspector for capturing this browser's GPU process.",
    'user_pref("security.sandbox.gpu.level", 0);',
    'user_pref("dom.webgpu.enabled", true);',
    'user_pref("browser.shell.checkDefaultBrowser", false);',
    "",
  ].join("\n");
  fs.writeFileSync(path.join(profileDir, "user.js"), prefs);
}

/** A profile directory of its own per browser, under `parent` (the app's user data directory). */
export function browserProfileDir(parent: string, exe: string): string {
  // Which naming applies is the path's own shape, not the platform this runs on: the unit tests
  // take a Windows path apart on Linux, and a launch configuration saved on one machine can be
  // opened on another.
  const windows = exe.includes("\\") || /^[A-Za-z]:/.test(exe);
  const name = windows
    // "Chrome SxS" out of ...\Google\Chrome SxS\Application\chrome.exe, "Firefox Nightly" out of
    // ...\Firefox Nightly\firefox.exe: the directory that names the install either way.
    ? path.win32.basename(browserFamily(exe) === "firefox"
        ? path.win32.dirname(exe)
        : path.win32.dirname(path.win32.dirname(exe))) || "browser"
    // A Linux install is not a directory per channel — every browser is a file in /usr/bin — so
    // the executable's own name is what tells a google-chrome from a google-chrome-beta.
    : path.posix.basename(exe) || "browser";
  return path.join(parent, "browser-profiles", name.replace(/[^\w.-]+/g, "_"));
}
