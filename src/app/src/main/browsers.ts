// The browsers installed on this machine, and what it takes to open a page in one with the
// Direct3D 12 capture library in the process that renders it.
//
// A browser does not render in the process the user starts. Its WebGPU work — and its compositing —
// is in a GPU process the browser spawns itself, so the launch is an ordinary one with the
// launcher's follow mode on top, which puts the library into that child as it appears
// (src/d3d12/launcher/main.cpp, follow mode). What a capture then holds is the D3D12 underneath
// WebGPU, which is a lower question than the page-level WebGPU Inspector answers.
//
// Two families, which agree on nothing: Chromium runs WebGPU through Dawn and names its child
// processes with --type=gpu-process, and takes its settings as switches; Firefox runs it through
// wgpu (whose Windows backend is D3D12 as well) and ends each child's command line with its type
// (" gpu"), and has no switch for the GPU sandbox at all — it is a preference, so it goes into the
// profile this launch makes. Firefox also starts its browser from a first process that exits
// straight away, which is why following tracks the target's whole tree.
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

function roots(): string[] {
  const dirs = [process.env["ProgramFiles"], process.env["ProgramFiles(x86)"], process.env["LOCALAPPDATA"]];
  return dirs.filter((d): d is string => !!d);
}

/** Which of the two a browser belongs to, by its executable, for one chosen by path as well. */
export function browserFamily(exe: string): BrowserFamily {
  // A browser's path is a Windows path whatever this runs on (the unit tests run on every platform),
  // and only path.win32 takes one apart there.
  return path.win32.basename(exe).toLowerCase() === "firefox.exe" ? "firefox" : "chromium";
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
 * The browsers found on this machine, in the order above. Windows only: follow mode is the D3D12
 * launcher's, and a browser on the other platforms is captured through the Vulkan implicit layer
 * or not at all.
 */
export function installedBrowsers(): BrowserInstall[] {
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
export function browserArgs(exe: string, url: string, profileDir: string): string[] {
  const page = url.trim() ? [url.trim()] : [];
  if (browserFamily(exe) === "firefox") {
    return ["-no-remote", "-profile", profileDir, ...page];
  }
  return [
    "--disable-gpu-sandbox",
    "--disable-gpu-watchdog",
    "--no-first-run",
    "--no-default-browser-check",
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
  // "Chrome SxS" out of ...\Google\Chrome SxS\Application\chrome.exe, "Firefox Nightly" out of
  // ...\Firefox Nightly\firefox.exe: the directory that names the install either way.
  const up = browserFamily(exe) === "firefox" ? path.win32.dirname(exe) : path.win32.dirname(path.win32.dirname(exe));
  const name = path.win32.basename(up) || "browser";
  return path.join(parent, "browser-profiles", name.replace(/[^\w.-]+/g, "_"));
}
