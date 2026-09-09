// Android targets: adb discovery, device and package listing, getting the layer onto the device
// and launching a package with it enabled.
//
// An Android app inherits no environment and no layer search path, so the desktop launch flow
// (VK_ADD_LAYER_PATH + VKINSP_* variables) does not apply. What the inspector does instead is
// what RenderDoc's Android support does (renderdoc/android/android.cpp):
//
//   * Android 10+: install the layer APK (tools/build_android.py packages the layer as an app
//     with no code) and name it in the `gpu_debug_layer_app` setting, so the loader finds the
//     library in that package. Android 9: push the .so into the target's data directory with
//     `run-as` (the loader searches it when the debug layer settings are on).
//   * `settings put global enable_gpu_debug_layers 1 / gpu_debug_app <pkg> / gpu_debug_layers
//     <layer>` enables the layer for that package. The target must be debuggable (a Unity
//     Development Build), or the device rooted; there is no way around that on Android.
//   * The layer's settings are `debug.vkinsp.*` system properties (`adb shell setprop`), the
//     Android counterpart of the VKINSP_* environment variables (see ConfigValue in layer.cpp).
//   * The layer listens on an abstract Unix socket named after the port and the package;
//     `adb forward` maps the host port to it, and the session's TCP client connects to
//     127.0.0.1 exactly as for a local process.
//   * The layer log is read from logcat (tag "vkinsp"); the process is watched with `pidof`.
//
// Unlike RenderDoc, no helper process runs on the device: the capture streams straight over the
// socket, so the only device-side component is the layer itself.
import { execFile, execFileSync, spawn, type ChildProcess } from "node:child_process";
import crypto from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import type { AndroidDevice } from "../shared/protocol.js";

export const LAYER_NAME = "VK_LAYER_INSPECTOR_capture";
export const LAYER_LIB = "libVkLayer_inspector_capture.so";
export const LAYER_APK = "gpu_inspector_layer.apk";
const DEVICE_TMP = "/data/local/tmp";
const ADB_TIMEOUT_MS = 20000;
const INSTALL_TIMEOUT_MS = 180000;
const START_TIMEOUT_MS = 60000;
const PID_RETRIES = 20;
const PID_RETRY_MS = 500;
const POLL_MS = 2000;
/** The first Android release with the GPU debug layer settings. */
const MIN_SDK = 28;
/** The first release that can load layers from another package (gpu_debug_layer_app). */
const LAYER_APP_SDK = 29;

// ------------------------------------------------------------------------------------------
// adb

/** The adb executable: INSPECTOR_ADB, the SDK named by ANDROID_HOME / ANDROID_SDK_ROOT, the default SDK location, or PATH. */
export function findAdb(): string | null {
  const exe = process.platform === "win32" ? "adb.exe" : "adb";
  const candidates: string[] = [];
  if (process.env.INSPECTOR_ADB) candidates.push(process.env.INSPECTOR_ADB);
  for (const v of ["ANDROID_HOME", "ANDROID_SDK_ROOT"]) {
    if (process.env[v]) candidates.push(path.join(process.env[v]!, "platform-tools", exe));
  }
  if (process.platform === "win32") {
    if (process.env.LOCALAPPDATA) candidates.push(path.join(process.env.LOCALAPPDATA, "Android", "Sdk", "platform-tools", exe));
  } else if (process.platform === "darwin") {
    candidates.push(path.join(os.homedir(), "Library", "Android", "sdk", "platform-tools", exe));
  } else {
    candidates.push(path.join(os.homedir(), "Android", "Sdk", "platform-tools", exe), "/opt/android-sdk/platform-tools/adb");
  }
  for (const c of candidates) if (fs.existsSync(c)) return c;
  for (const dir of (process.env.PATH ?? "").split(path.delimiter)) {
    if (dir && fs.existsSync(path.join(dir, exe))) return path.join(dir, exe);
  }
  return null;
}

function adbArgs(serial: string | null, args: string[]): string[] {
  return serial ? ["-s", serial, ...args] : args;
}

/** Runs adb and resolves with its stdout; rejects with adb's error output. */
function adb(adbPath: string, serial: string | null, args: string[], timeoutMs = ADB_TIMEOUT_MS): Promise<string> {
  return new Promise((resolve, reject) => {
    execFile(adbPath, adbArgs(serial, args), { timeout: timeoutMs, maxBuffer: 16 * 1024 * 1024, windowsHide: true }, (err, stdout, stderr) => {
      if (err) {
        const detail = `${stderr ?? ""}${stdout ?? ""}`.trim() || err.message;
        reject(new Error(`adb ${args[0] === "shell" ? "shell" : args.slice(0, 2).join(" ")}: ${detail}`));
      } else {
        resolve(stdout);
      }
    });
  });
}

/** `adb shell <command>`; the command is one string interpreted by the device's shell. */
function shell(adbPath: string, serial: string, command: string, timeoutMs = ADB_TIMEOUT_MS): Promise<string> {
  return adb(adbPath, serial, ["shell", command], timeoutMs);
}

// ------------------------------------------------------------------------------------------
// Devices and packages

/** Parses `adb devices -l`. Exported for tests. */
export function parseDevices(text: string): { serial: string; state: string; model: string }[] {
  const out: { serial: string; state: string; model: string }[] = [];
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || line.startsWith("List of devices") || line.startsWith("*")) continue;
    const parts = line.split(/\s+/);
    if (parts.length < 2) continue;
    const model = parts.find((p) => p.startsWith("model:"))?.substring(6).replace(/_/g, " ") ?? "";
    out.push({ serial: parts[0], state: parts[1], model });
  }
  return out;
}

/** Connected devices with their Android API level and ABI (queried per device; unauthorized and offline devices are listed as such). */
export async function listDevices(adbPath: string): Promise<AndroidDevice[]> {
  const listed = parseDevices(await adb(adbPath, null, ["devices", "-l"]));
  const devices: AndroidDevice[] = [];
  for (const d of listed) {
    const dev: AndroidDevice = { serial: d.serial, state: d.state, model: d.model, sdk: 0, abi: "" };
    if (d.state === "device") {
      try {
        const props = (await shell(adbPath, d.serial, "getprop ro.build.version.sdk; getprop ro.product.cpu.abi; getprop ro.product.manufacturer; getprop ro.product.model")).split(/\r?\n/).map((s) => s.trim());
        dev.sdk = Number(props[0]) || 0;
        dev.abi = props[1] ?? "";
        if (!dev.model) dev.model = [props[2], props[3]].filter(Boolean).join(" ");
      } catch {
        // Listed but not answering: shown with what `adb devices` said.
      }
    }
    devices.push(dev);
  }
  return devices;
}

/** Third-party packages installed on the device, sorted. */
export async function listPackages(adbPath: string, serial: string): Promise<string[]> {
  const text = await shell(adbPath, serial, "pm list packages -3");
  return text.split(/\r?\n/).map((l) => l.trim()).filter((l) => l.startsWith("package:")).map((l) => l.substring(8)).sort();
}

/** The launchable activity of a package (`cmd package resolve-activity`), or null when it cannot be determined. */
async function resolveActivity(adbPath: string, serial: string, pkg: string): Promise<string | null> {
  try {
    const text = await shell(adbPath, serial, `cmd package resolve-activity --brief ${pkg}`);
    const lines = text.split(/\r?\n/).map((l) => l.trim()).filter(Boolean);
    const component = lines.reverse().find((l) => l.startsWith(`${pkg}/`));
    return component ?? null;
  } catch {
    return null;
  }
}

// ------------------------------------------------------------------------------------------
// Layer files (what tools/build_android.py produces, staged by app/tools/stage_layer.mjs)

export interface AndroidLayerFiles {
  dir: string;
  /** ABI -> path of the layer library. */
  libs: Record<string, string>;
  apk: string | null;
  apkInfo: { package: string; versionName: string; abis: string[] } | null;
}

/** Looks for `lib/<abi>/libVkLayer_inspector_capture.so` and the layer APK in each candidate directory. */
export function findAndroidLayer(candidates: string[]): AndroidLayerFiles | null {
  for (const dir of candidates) {
    const libRoot = path.join(dir, "lib");
    if (!fs.existsSync(libRoot)) continue;
    const libs: Record<string, string> = {};
    for (const abi of fs.readdirSync(libRoot)) {
      const lib = path.join(libRoot, abi, LAYER_LIB);
      if (fs.existsSync(lib)) libs[abi] = lib;
    }
    if (!Object.keys(libs).length) continue;
    const apk = path.join(dir, LAYER_APK);
    let apkInfo: AndroidLayerFiles["apkInfo"] = null;
    if (fs.existsSync(apk)) {
      try {
        apkInfo = JSON.parse(fs.readFileSync(`${apk}.json`, "utf8")) as AndroidLayerFiles["apkInfo"];
      } catch {
        apkInfo = null;
      }
    }
    return { dir, libs, apk: apkInfo ? apk : null, apkInfo };
  }
  return null;
}

// ------------------------------------------------------------------------------------------
// Launching

export interface AndroidLaunchOptions {
  adb: string;
  serial: string;
  package: string;
  /** Activity to start (component suffix or full name); resolved from the package when empty. */
  activity: string;
  /** Port on both sides: the layer listens on it on the device, adb forwards the host's to it. */
  port: number;
  log: boolean;
  recordAlways: boolean;
  layer: AndroidLayerFiles;
  onLog: (line: string) => void;
  /** The application stopped on its own (not through stop()). */
  onExit: () => void;
}

/** One launched Android application: the layer installation, the port forward, logcat and the process watch. */
export class AndroidTarget {
  pid: number | null = null;
  private _logcat: ChildProcess | null = null;
  private _poll: NodeJS.Timeout | null = null;
  private _polling = false;
  private _stopped = false;

  constructor(private readonly opts: AndroidLaunchOptions) {}

  /** Installs and enables the layer, starts the application and the watches. Rejects with a readable message. */
  async start(): Promise<void> {
    const { adb: adbPath, serial, package: pkg, port } = this.opts;
    const log = this.opts.onLog;

    const props = (await shell(adbPath, serial, "getprop ro.build.version.sdk; getprop ro.product.cpu.abi; getprop ro.product.cpu.abilist; getprop ro.product.model")).split(/\r?\n/).map((s) => s.trim());
    const sdk = Number(props[0]) || 0;
    const abi = props[1] ?? "";
    const abilist = (props[2] ?? abi).split(",").map((s) => s.trim()).filter(Boolean);
    log(`device ${serial}: ${props[3] ?? ""}, Android API ${sdk}, ${abi}`);
    if (sdk < MIN_SDK) throw new Error(`Android 9 (API ${MIN_SDK}) or newer is required for Vulkan layers; the device runs API ${sdk}`);

    const how = await this._installLayer(sdk, abilist);
    log(`layer: ${how}`);

    // Enable the layer for the package (Android's GPU debug layer settings).
    await shell(adbPath, serial, "settings put global enable_gpu_debug_layers 1");
    await shell(adbPath, serial, `settings put global gpu_debug_app ${pkg}`);
    await shell(adbPath, serial, `settings put global gpu_debug_layers ${LAYER_NAME}`);
    // The layer's settings, read through ConfigValue() in the layer.
    await shell(adbPath, serial, `setprop debug.vkinsp.port ${port}`);
    await shell(adbPath, serial, `setprop debug.vkinsp.log ${this.opts.log ? 1 : 0}`);
    await shell(adbPath, serial, `setprop debug.vkinsp.record_always ${this.opts.recordAlways ? 1 : 0}`);

    await shell(adbPath, serial, `am force-stop ${pkg}`);
    // A previous instance that has not finished dying still holds the layer's socket, which the
    // new instance would then fail to bind (or, worse, the inspector would connect to the old
    // one): wait for it to be gone.
    for (let i = 0; i < 20 && !this._stopped; ++i) {
      const pid = await this._findPid();
      if (pid === null) break;
      if (i === 0) log(`waiting for the previous instance (pid ${pid}) to exit`);
      await new Promise((r) => setTimeout(r, 250));
    }
    // The layer listens on an abstract Unix socket (no INTERNET permission needed in the target).
    // Still held after the wait: something else answers on the name (a process of the package
    // pidof does not see). The layer keeps retrying its bind for 30 s, so say why it may stall.
    if (!this._stopped) {
      const unix = await shell(adbPath, serial, "cat /proc/net/unix").catch(() => "");
      if (unix.includes(`@${this.socketName}`)) log(`warning: @${this.socketName} is still held on the device by another process; the layer waits for it`);
    }
    await adb(adbPath, serial, ["forward", `tcp:${port}`, `localabstract:${this.socketName}`]);
    log(`forwarding localhost:${port} to the device's @${this.socketName}`);
    if (this._stopped) return;

    this._startLogcat();

    let activity = this.opts.activity.trim();
    if (!activity) activity = (await resolveActivity(adbPath, serial, pkg)) ?? "";
    if (activity && !activity.includes("/")) activity = `${pkg}/${activity}`;
    if (activity) {
      log(`starting ${activity}`);
      // Not -W: waiting for the launch to complete can outlast the timeout on a headset (the
      // shell's launch flow) and report a started application as an error; the pid poll below
      // waits for the process instead.
      const out = await shell(adbPath, serial, `am start -S -n ${activity}`, START_TIMEOUT_MS);
      const error = out.split(/\r?\n/).find((l) => /^Error/.test(l.trim()));
      if (error) throw new Error(`${error.trim()} (activity ${activity})`);
    } else {
      log(`starting ${pkg} (no launchable activity resolved; using the launcher intent)`);
      const out = await shell(adbPath, serial, `monkey -p ${pkg} -c android.intent.category.LAUNCHER 1`, START_TIMEOUT_MS);
      if (/No activities found|monkey aborted/i.test(out)) throw new Error(`no launchable activity in ${pkg}`);
    }

    for (let i = 0; i < PID_RETRIES && this.pid === null && !this._stopped; ++i) {
      this.pid = await this._findPid();
      if (this.pid === null) await new Promise((r) => setTimeout(r, PID_RETRY_MS));
      // Still no process after a couple of seconds: say what may be holding the launch up.
      if (this.pid === null && i === 4) await this._launchDiagnostics(true);
    }
    if (this._stopped) return;
    if (this.pid === null) throw new Error(`${pkg} did not start (no process found)`);
    await this._launchDiagnostics(false);
    this._poll = setInterval(() => void this._pollProcess(), POLL_MS);
  }

  /** The layer's abstract socket on the device: the port and the package (see transport.cpp). */
  get socketName(): string {
    return `vkinsp:${this.opts.port}:${this.opts.package}`;
  }

  /**
   * Re-establishes the port forward when it is gone. adb drops a device's forwards whenever the
   * device disconnects, and a headset's USB link blips when it changes power state, so a
   * connection attempt refused on the host side is checked against `adb forward --list`.
   * Resolves true when the forward had to be re-created.
   */
  async ensureForward(): Promise<boolean> {
    if (this._stopped) return false;
    const { adb: adbPath, serial, port } = this.opts;
    const target = `localabstract:${this.socketName}`;
    const list = await adb(adbPath, serial, ["forward", "--list"]);
    const present = list.split(/\r?\n/).some((l) => {
      const f = l.trim().split(/\s+/);
      return f[0] === serial && f[1] === `tcp:${port}` && f[2] === target;
    });
    if (present || this._stopped) return false;
    await adb(adbPath, serial, ["forward", `tcp:${port}`, target]);
    this.opts.onLog(`the port forward was gone (device reconnected?): forwarding localhost:${port} to @${this.socketName} again`);
    return true;
  }

  /** Terminates the application, the port forward and the watches. */
  async stop(): Promise<void> {
    this._stopWatching();
    const { adb: adbPath, serial, package: pkg, port } = this.opts;
    try {
      await shell(adbPath, serial, `am force-stop ${pkg}`);
    } catch {
      // device gone
    }
    try {
      await adb(adbPath, serial, ["forward", "--remove", `tcp:${port}`]);
    } catch {
      // never forwarded
    }
  }

  /** stop() for application exit, where nothing can be awaited. */
  stopSync(): void {
    this._stopWatching();
    const { adb: adbPath, serial, package: pkg, port } = this.opts;
    for (const args of [["shell", `am force-stop ${pkg}`], ["forward", "--remove", `tcp:${port}`]]) {
      try {
        execFileSync(adbPath, adbArgs(serial, args), { timeout: 3000, stdio: "ignore", windowsHide: true });
      } catch {
        // best effort
      }
    }
  }

  private _stopWatching(): void {
    this._stopped = true;
    if (this._poll) {
      clearInterval(this._poll);
      this._poll = null;
    }
    if (this._logcat) {
      try {
        this._logcat.kill();
      } catch {
        // already gone
      }
      this._logcat = null;
    }
  }

  /**
   * Gets the layer where the device's loader will find it. Android 10+ with the layer APK:
   * install it (when the installed version differs) and point gpu_debug_layer_app at it. Otherwise
   * copy the .so into the target's data directory with run-as, skipped when the copy there
   * already matches.
   */
  private async _installLayer(sdk: number, abilist: string[]): Promise<string> {
    const { adb: adbPath, serial, package: pkg, layer } = this.opts;
    const apkAbi = layer.apkInfo ? abilist.find((a) => layer.apkInfo!.abis.includes(a)) : undefined;
    if (sdk >= LAYER_APP_SDK && layer.apk && layer.apkInfo && apkAbi) {
      const info = layer.apkInfo;
      let installed = "";
      try {
        const dump = await shell(adbPath, serial, `dumpsys package ${info.package}`);
        installed = /versionName=(\S+)/.exec(dump)?.[1] ?? "";
      } catch {
        installed = "";
      }
      if (installed !== info.versionName) {
        this.opts.onLog(`installing the layer package ${info.package} (${installed ? `replacing ${installed}` : "not installed"})`);
        // --force-queryable: Android 11+ package visibility would otherwise hide the layer
        // package from the target, and the loader would not find the library.
        await adb(adbPath, serial, ["install", "-r", "-d", "--force-queryable", layer.apk], INSTALL_TIMEOUT_MS);
      }
      await shell(adbPath, serial, `settings put global gpu_debug_layer_app ${info.package}`);
      return `${info.package} ${info.versionName} (${apkAbi})`;
    }

    const abi = abilist.find((a) => layer.libs[a]);
    if (!abi) {
      throw new Error(`no Android layer built for ${abilist.join(", ")}: run tools/build_android.py --abi ${abilist[0] ?? "arm64-v8a"}`);
    }
    const lib = layer.libs[abi];
    const local = crypto.createHash("md5").update(fs.readFileSync(lib)).digest("hex");
    let remote = "";
    try {
      remote = (await shell(adbPath, serial, `run-as ${pkg} md5sum ${LAYER_LIB}`)).trim().split(/\s+/)[0] ?? "";
    } catch (e) {
      const message = e instanceof Error ? e.message : String(e);
      if (/not debuggable|is not debuggable|Could not set capabilities|run-as: Package/.test(message)) {
        throw new Error(`${pkg} is not debuggable: Android only loads layers into debuggable applications (a Unity Development Build) or on rooted devices`);
      }
      remote = "";
    }
    if (remote !== local) {
      this.opts.onLog(`copying ${LAYER_LIB} (${abi}) into ${pkg}'s data directory`);
      await adb(adbPath, serial, ["push", lib, `${DEVICE_TMP}/${LAYER_LIB}`], INSTALL_TIMEOUT_MS);
      try {
        await shell(adbPath, serial, `run-as ${pkg} cp ${DEVICE_TMP}/${LAYER_LIB} . && run-as ${pkg} chmod 700 ${LAYER_LIB}`);
      } catch (e) {
        const message = e instanceof Error ? e.message : String(e);
        throw new Error(`could not copy the layer into ${pkg}: ${message}. The application must be debuggable (a Unity Development Build).`);
      }
    }
    // A stale layer app setting would make the loader look there first.
    await shell(adbPath, serial, "settings delete global gpu_debug_layer_app");
    return `${LAYER_LIB} (${abi}) in ${pkg}'s data directory`;
  }

  private _startLogcat(): void {
    const { adb: adbPath, serial } = this.opts;
    // The layer's tag, plus native crash dumps and Java exceptions, starting from now (-T 1).
    const proc = spawn(adbPath, adbArgs(serial, ["logcat", "-v", "tag", "-T", "1", "vkinsp:*", "DEBUG:E", "AndroidRuntime:E", "*:S"]), { stdio: ["ignore", "pipe", "ignore"], windowsHide: true });
    this._logcat = proc;
    let rest = "";
    proc.stdout?.on("data", (d: Buffer) => {
      rest += d.toString("utf8");
      const lines = rest.split(/\r?\n/);
      rest = lines.pop() ?? "";
      for (const line of lines) {
        if (!line.length || line.startsWith("--------- beginning of")) continue;
        this.opts.onLog(line.startsWith("I/vkinsp") ? `[vkinsp] ${line.replace(/^I\/vkinsp\s*:\s?/, "")}` : line);
      }
    });
    proc.on("exit", () => {
      if (this._logcat === proc) this._logcat = null;
    });
    proc.on("error", () => {
      if (this._logcat === proc) this._logcat = null;
    });
  }

  /**
   * What can keep a launch from running, in the Log: a device that is asleep (an OpenXR session
   * stays idle until the headset is worn), and on a headset the shell's "controllers required"
   * dialog, which a launch attempted without controllers or tracked hands leaves behind and
   * which then blocks every later launch until the shell restarts.
   */
  private async _launchDiagnostics(noProcess: boolean): Promise<void> {
    const { adb: adbPath, serial } = this.opts;
    const log = this.opts.onLog;
    try {
      const power = await shell(adbPath, serial, "dumpsys power | grep -m1 mWakefulness=");
      const state = /mWakefulness=(\w+)/.exec(power)?.[1];
      if (state && state !== "Awake") log(`the device is ${state.toLowerCase()}: an OpenXR session stays idle (no frames) until the headset is worn or woken (adb shell input keyevent KEYCODE_WAKEUP)`);
    } catch {
      // not answering: the launch itself reports that
    }
    try {
      const windows = await shell(adbPath, serial, "dumpsys window windows | grep -c -i launchcheck");
      if (Number(windows.trim()) > 0) {
        log(`the headset shell is showing its launch check dialog ("controllers required"), which blocks ${noProcess ? "this launch" : "launches"}: put the headset on with controllers or tracked hands, or restart the shell (adb shell am force-stop com.oculus.vrshell)`);
      }
    } catch {
      // grep found nothing (exit 1) or no such service: nothing to report
    }
  }

  private async _findPid(): Promise<number | null> {
    try {
      const out = (await shell(this.opts.adb, this.opts.serial, `pidof ${this.opts.package}`)).trim();
      const pid = Number(out.split(/\s+/)[0]);
      return pid > 0 ? pid : null;
    } catch {
      return null;
    }
  }

  private async _pollProcess(): Promise<void> {
    if (this._polling || this._stopped) return;
    this._polling = true;
    try {
      const pid = await this._findPid();
      if (this._stopped) return;
      if (pid === null || (this.pid !== null && pid !== this.pid)) {
        // Gone, or restarted by something else: either way this launch is over.
        this._stopWatching();
        this.opts.onExit();
      }
    } finally {
      this._polling = false;
    }
  }
}

/** Turns the GPU debug layer settings off again, so the package no longer loads the layer when started from the device. */
export async function disableLayer(adbPath: string, serial: string): Promise<void> {
  for (const key of ["enable_gpu_debug_layers", "gpu_debug_app", "gpu_debug_layers", "gpu_debug_layer_app"]) {
    try {
      await shell(adbPath, serial, `settings delete global ${key}`);
    } catch {
      // device gone
    }
  }
}
