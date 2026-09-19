// The Chromium browsers installed on this machine, and the command line that opens a page in one
// with the Direct3D 12 capture library in the process that renders it.
//
// A browser does not render in the process the user starts. Its WebGPU work — and its compositing —
// is in the GPU process, which the browser spawns itself, so the launch is an ordinary one with the
// launcher's follow mode on top: `--follow --type=gpu-process` puts the library into that child as
// it appears (src/d3d12/launcher/main.cpp, follow mode). What a capture then holds is the D3D12
// underneath WebGPU, which is a lower question than the page-level WebGPU Inspector answers.
import fs from "node:fs";
import path from "node:path";

import type { BrowserInstall } from "../shared/protocol.js";

/**
 * Chrome's GPU process, and not the second one Chrome starts to collect GPU information (it makes
 * a device of its own and exits again, and would take the session's port from the one that
 * renders; the library refuses a port another process already serves).
 */
export const BROWSER_FOLLOW = ["--type=gpu-process", "!--use-gl=disabled"];

/** Where the browsers are, relative to each of the roots below. */
const KNOWN: { name: string; relative: string }[] = [
  { name: "Google Chrome", relative: "Google\\Chrome\\Application\\chrome.exe" },
  { name: "Google Chrome Beta", relative: "Google\\Chrome Beta\\Application\\chrome.exe" },
  { name: "Google Chrome Dev", relative: "Google\\Chrome Dev\\Application\\chrome.exe" },
  // Canary installs per user, under its own "SxS" directory, so it sits beside a stable Chrome.
  { name: "Google Chrome Canary", relative: "Google\\Chrome SxS\\Application\\chrome.exe" },
  { name: "Microsoft Edge", relative: "Microsoft\\Edge\\Application\\msedge.exe" },
  { name: "Microsoft Edge Canary", relative: "Microsoft\\Edge SxS\\Application\\msedge.exe" },
  { name: "Brave", relative: "BraveSoftware\\Brave-Browser\\Application\\brave.exe" },
];

function roots(): string[] {
  const dirs = [process.env["ProgramFiles"], process.env["ProgramFiles(x86)"], process.env["LOCALAPPDATA"]];
  return dirs.filter((d): d is string => !!d);
}

/**
 * The version, from the versioned directory Chromium keeps its build in beside the executable
 * ("Application\\153.0.8010.48"); empty when there is none to read. Shown in the dialog so a Chrome
 * and a Canary are told apart by more than their name.
 */
function versionOf(exe: string): string {
  try {
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
 * The Chromium browsers found on this machine, newest-looking first in the order above. Windows
 * only: follow mode is the D3D12 launcher's, and a browser on the other platforms is captured
 * through the Vulkan implicit layer or not at all.
 */
export function installedBrowsers(): BrowserInstall[] {
  if (process.platform !== "win32") return [];
  const found: BrowserInstall[] = [];
  const seen = new Set<string>();
  for (const { name, relative } of KNOWN) {
    for (const root of roots()) {
      const exe = path.join(root, relative);
      const key = exe.toLowerCase();
      if (seen.has(key) || !fs.existsSync(exe)) continue;
      seen.add(key);
      found.push({ name, path: exe, version: versionOf(exe) });
    }
  }
  return found;
}

/**
 * The browser's command line for a page.
 *
 * `--disable-gpu-sandbox` is required rather than advisable: the capture library in a sandboxed GPU
 * process cannot open its port, so nothing ever connects. `--disable-gpu-watchdog` keeps Chromium
 * from killing the GPU process while a capture holds it. The profile is the inspector's own, so the
 * browser the user has open keeps its windows, its session and its extensions, and so a browser
 * already running does not hand the page to that instance and exit (leaving nothing to capture).
 */
export function browserArgs(url: string, profileDir: string): string[] {
  return [
    "--disable-gpu-sandbox",
    "--disable-gpu-watchdog",
    "--no-first-run",
    "--no-default-browser-check",
    `--user-data-dir=${profileDir}`,
    ...(url.trim() ? [url.trim()] : []),
  ];
}

/** A profile directory of its own per browser, under `parent` (the app's user data directory). */
export function browserProfileDir(parent: string, exe: string): string {
  const name = path.basename(path.dirname(path.dirname(exe))) || "browser";
  return path.join(parent, "browser-profiles", name.replace(/[^\w.-]+/g, "_"));
}
