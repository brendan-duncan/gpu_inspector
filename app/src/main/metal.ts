// Launching a Metal application on macOS with the capture library injected.
//
// The Vulkan side asks the loader to insert a layer, which is a supported mechanism with its own
// environment variables. Metal has nothing of the kind, so the library is loaded with
// DYLD_INSERT_LIBRARIES and takes the API's entry points itself (see metal/README.md). That makes
// launching a little more involved than setting variables:
//
//   * an application is usually a bundle, and dyld wants the executable inside it;
//   * dyld drops DYLD_* for a process with the hardened runtime, unless it carries entitlements
//     that say otherwise — silently, so the failure looks like "the application never connected".
//
// Both are handled here, before the process is spawned, so the launch dialog can say what is
// wrong instead of leaving a session waiting for a connection that cannot happen.
import { execFileSync, spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

// The bundle is ESM, so there is no __dirname; main.ts derives its own the same way, and esbuild
// scopes each module's separately.
const moduleDir = path.dirname(fileURLToPath(import.meta.url));

export const CAPTURE_LIBRARY = "libmtlinsp_capture.dylib";

/**
 * The capture library, from a development build under one of `roots` or from one of `packaged`,
 * the layer directories of installed apps. The defaults are the app's own: the checkout it was
 * built in, and its resources.
 */
export function findCaptureLibrary(roots: string[] = [path.resolve(moduleDir, "..", "..", "..")],
                                   packaged: string[] = [path.join(process.resourcesPath ?? "", "layer")]): string | null {
  const candidates: string[] = [];
  if (process.env.INSPECTOR_METAL_LIB) candidates.push(process.env.INSPECTOR_METAL_LIB);
  for (const root of roots) {
    for (const dir of ["build/bin", "build/bin/Release", "build/bin/Debug"]) {
      candidates.push(path.join(root, dir, CAPTURE_LIBRARY));
    }
  }
  for (const dir of packaged) candidates.push(path.join(dir, CAPTURE_LIBRARY));
  return candidates.find((p) => fs.existsSync(p)) ?? null;
}

/**
 * The binary dyld will actually run for what the user picked.
 *
 * A `.app` is a directory, so spawning it directly fails; the executable is named by
 * CFBundleExecutable in its Info.plist, and falls back to the bundle's own name, which is what
 * Unity produces. Anything that is not a bundle is returned unchanged.
 */
export function resolveExecutable(exe: string): string {
  if (!exe.endsWith(".app")) return exe;
  const macOS = path.join(exe, "Contents", "MacOS");
  const plist = path.join(exe, "Contents", "Info.plist");
  if (fs.existsSync(plist)) {
    try {
      const name = execFileSync("/usr/libexec/PlistBuddy",
                                ["-c", "Print :CFBundleExecutable", plist],
                                { encoding: "utf8" }).trim();
      const candidate = path.join(macOS, name);
      if (name && fs.existsSync(candidate)) return candidate;
    } catch {
      // No such key, or an unreadable plist: fall through to the bundle name.
    }
  }
  const byBundleName = path.join(macOS, path.basename(exe, ".app"));
  if (fs.existsSync(byBundleName)) return byBundleName;
  // One executable in there is unambiguous even when it is named neither way.
  try {
    const entries = fs.readdirSync(macOS);
    if (entries.length === 1) return path.join(macOS, entries[0]);
  } catch {
    // Not a bundle after all.
  }
  return exe;
}

/**
 * Why the library could not be injected into this target, or null when it can be.
 *
 * A hardened-runtime binary needs `com.apple.security.cs.allow-dyld-environment-variables` for
 * dyld to keep the variable at all, and `com.apple.security.cs.disable-library-validation` to
 * load a library signed by someone else. Locally built applications — Unity player builds among
 * them — are normally ad-hoc signed without the hardened runtime and need none of this.
 */
export function injectionBlockedReason(exe: string): string | null {
  // spawnSync rather than execFileSync: codesign writes its report to stderr and exits 0, and
  // execFileSync hands back only stdout unless the command fails.
  const r = spawnSync("codesign", ["-d", "-v", "--entitlements", "-", "--xml", exe],
                      { encoding: "utf8" });
  const output = `${r.stderr ?? ""}${r.stdout ?? ""}`;
  if (!/flags=[^\s]*runtime/.test(output)) return null;  // no hardened runtime: nothing in the way
  const hasDyld = output.includes("com.apple.security.cs.allow-dyld-environment-variables");
  const hasLibrary = output.includes("com.apple.security.cs.disable-library-validation");
  if (hasDyld && hasLibrary) return null;

  return `${path.basename(exe)} is signed with the hardened runtime, so macOS drops `
    + `DYLD_INSERT_LIBRARIES and the capture library can never load. Re-sign it for injection:\n\n`
    + `  /usr/bin/codesign --force --deep --sign - --options runtime \\\n`
    + `    --entitlements <(echo '<?xml version="1.0" encoding="UTF-8"?>`
    + `<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">`
    + `<plist version="1.0"><dict>`
    + `<key>com.apple.security.cs.allow-dyld-environment-variables</key><true/>`
    + `<key>com.apple.security.cs.disable-library-validation</key><true/>`
    + `</dict></plist>') \\\n    "<the .app>"\n\n`
    + `This invalidates the application's signature and notarization, so do it to a development `
    + `build rather than to a shipping copy.`;
}

/**
 * The environment the capture library reads (metal/src/transport.mm, swizzle.mm).
 *
 * "Validation layer" turns on Metal's own API validation and shader validation, in the mode
 * that logs a failure rather than aborting on it: the library interposes NSLog and forwards
 * those lines as validation messages (metal/src/validation.mm). A variable the user already set
 * wins, so a launch can pick another mode.
 */
export function captureEnvironment(library: string, port: number, log: boolean, validation = false, stacktraces = false): NodeJS.ProcessEnv {
  const env: NodeJS.ProcessEnv = {
    // A stack at every object creation (metal/src/stacktrace.mm), the launch dialog's option.
    MTLINSP_STACKTRACES: stacktraces ? "1" : "0",
    // Appended rather than replacing: another inserted library is the caller's business.
    DYLD_INSERT_LIBRARIES: [library, ...(process.env.DYLD_INSERT_LIBRARIES ? [process.env.DYLD_INSERT_LIBRARIES] : [])].join(":"),
    MTLINSP_PORT: String(port),
    MTLINSP_LOG: log ? "1" : "0",
    // Lets the library write an Xcode GPU trace of a frame on request (metal/src/gpu_trace.mm);
    // without it MTLCaptureManager refuses the document destination.
    ...(process.env.METAL_CAPTURE_ENABLED ? {} : { METAL_CAPTURE_ENABLED: "1" }),
  };
  if (validation) {
    const defaults: Record<string, string> = {
      MTL_DEBUG_LAYER: "1",
      MTL_DEBUG_LAYER_ERROR_MODE: "nslog",
      MTL_DEBUG_LAYER_WARNING_MODE: "nslog",
      MTL_SHADER_VALIDATION: "1",
      MTL_SHADER_VALIDATION_REPORT_TO_STDERR: "1",
    };
    for (const [key, value] of Object.entries(defaults)) {
      if (!process.env[key]) env[key] = value;
    }
  }
  return env;
}
