// Modal dialog for launching an application with the layer enabled: an executable on this
// machine, or a package on an Android device reached through adb (see main/android.ts).
// Recently launched configurations can be picked from a dropdown to fill in the fields.
import { Dialog } from "./widget/dialog.js";
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import { Button } from "./widget/button.js";
import { Checkbox } from "./widget/checkbox.js";
import { Select } from "./widget/select.js";
import { TextArea } from "./widget/text_area.js";
import { TextInput } from "./widget/text_input.js";
import type { AndroidDevice, LaunchConfig, QueuedCapture } from "../shared/protocol.js";

const DEFAULT_PORT = 47531;

export function emptyLaunchConfig(): LaunchConfig {
  return {
    target: "native", exe: "", args: "", cwd: "", env: "", device: "", activity: "",
    port: DEFAULT_PORT, log: true, recordAlways: false, validation: false, syncValidation: false, stacktraces: true, capture: { mode: "none", value: 0 },
  };
}

const CAPTURE_MODES: [string, QueuedCapture["mode"]][] = [["No queued capture", "none"], ["Capture frame", "frame"], ["Capture after seconds", "time"]];
type Target = [string, LaunchConfig["target"]];
const TARGETS: Target[] = [["This computer", "native"], ["Android device (adb)", "android"], ["An application started elsewhere (implicit layer)", "implicit"]];

// Both local targets need the capture layer, which does not build on Apple, so a macOS build
// offers only Android. The main process reports the host in AppConfig.platform and the window
// passes it here before any dialog is opened.
let hostPlatform = "";
export function setHostPlatform(platform: string): void {
  hostPlatform = platform;
}
function hostTargets(): Target[] {
  return hostPlatform === "darwin" ? TARGETS.filter(([, t]) => t === "android") : TARGETS;
}

export function launchDisplayName(c: LaunchConfig): string {
  if (c.target === "android") return `${c.exe} (Android)`;
  if (c.target === "implicit") return `any application (port ${c.port})`;
  const base = c.exe.replace(/\\/g, "/").split("/").pop() || c.exe;
  return c.args ? `${base} ${c.args}` : base;
}

function deviceLabel(d: AndroidDevice): string {
  const name = d.model ? `${d.model} (${d.serial})` : d.serial;
  const detail = d.sdk ? `  API ${d.sdk}, ${d.abi}` : "";
  const state = d.state === "device" ? "" : `  [${d.state}]`;
  return `${name}${detail}${state}`;
}

function setOptions(select: Select, options: string[]): void {
  select.select.element.innerHTML = "";
  for (const o of options) select.addOption(o);
}

export class LaunchDialog extends Dialog {
  private _recents: LaunchConfig[];
  private _recentSelect: Select | null = null;
  private _targets: Target[] = hostTargets();
  private _target: Select;
  private _nativeRows: Div;
  private _androidRows: Div;
  private _implicitRows!: Div;
  private _implicitStatus!: Span;
  private _implicitButton!: Button;
  private _implicitRegistered = false;
  private _launchButton!: Button;
  private _exe: TextInput;
  private _cwd: TextInput;
  private _args: TextInput;
  private _env: TextArea;
  private _device: Select;
  private _deviceHint: Span;
  private _devices: AndroidDevice[] = [];
  /** Serial to select once the device list has loaded (from a recent configuration). */
  private _pendingDevice = "";
  private _loadingDevices = false;
  private _package: Select;
  private _packages: string[] = [];
  private _packageHint: Span;
  private _activity: TextInput;
  private _symbolDirs!: TextInput;
  private _sourceRoots!: TextInput;
  private _port: TextInput;
  private _log: Checkbox;
  private _recordAlways: Checkbox;
  private _validation!: Checkbox;
  private _syncValidation!: Checkbox;
  private _stacktraces!: Checkbox;
  private _captureMode: Select;
  private _captureValue: TextInput;
  private _captureValueLabel: Span;

  constructor(recents: LaunchConfig[], initial: LaunchConfig | null, onLaunch: (config: LaunchConfig) => void) {
    super({ title: "Launch Application", width: 720, windowClass: "dialog launch-dialog" });
    this._recents = recents;
    const body = this.body;
    body.classList.add("launch-dialog-body");

    if (recents.length) {
      const row = new Div(body, { class: "launch-dialog-row" });
      new Span(row, { text: "Recent", class: "launch-dialog-label" });
      this._recentSelect = new Select(row, {
        options: ["(choose a recent launch)", ...recents.map((r) => launchDisplayName(r))],
        class: "launch-dialog-select",
        onChange: (_value: string, index: number) => {
          if (index > 0) this.setConfig(recents[index - 1]);
        },
      });
    }

    const section = (parent: Div, title: string): void => {
      new Div(parent, { text: title, class: "launch-dialog-section" });
    };

    section(body, "Target");
    {
      const row = new Div(body, { class: "launch-dialog-row" });
      new Span(row, { text: "Run On", class: "launch-dialog-label" });
      this._target = new Select(row, { options: this._targets.map((t) => t[0]), class: "launch-dialog-select", onChange: () => this._updateTarget() });
    }
    if (hostPlatform === "darwin") {
      new Div(body, { class: "launch-dialog-hint",
        text: "The macOS build has no capture layer, so it cannot launch an application on this computer. "
          + "Inspect an Android device over adb, or open a saved .gpucap file." });
    }

    // Native target: the program to run.
    this._nativeRows = new Div(body);
    section(this._nativeRows, "Program");
    this._exe = this._pathRow(this._nativeRows, "Executable Path", "path to a Vulkan application", "Choose Vulkan application", false);
    this._cwd = this._pathRow(this._nativeRows, "Working Directory", "(executable's folder)", "Choose working directory", true);
    this._args = this._inputRow(this._nativeRows, "Command-line Arguments", "");
    {
      const row = new Div(this._nativeRows, { class: "launch-dialog-row launch-dialog-row-top" });
      new Span(row, { text: "Environment Variables", class: "launch-dialog-label" });
      this._env = new TextArea(row, { placeholder: "KEY=VALUE, one per line", class: "launch-dialog-textarea" });
    }

    // Android target: a device and a package. The device list comes from adb; the package can be
    // picked from the device's third-party packages or typed.
    this._androidRows = new Div(body);
    section(this._androidRows, "Android Application");
    {
      const row = new Div(this._androidRows, { class: "launch-dialog-row" });
      new Span(row, { text: "Device", class: "launch-dialog-label" });
      this._device = new Select(row, { options: [], class: "launch-dialog-select", onChange: () => void this._loadPackages() });
      new Button(row, { label: "Refresh", class: "btn", tooltip: "Look for devices again (adb devices)", callback: () => void this._loadDevices() });
    }
    this._deviceHint = new Span(this._androidRows, { text: "", class: "launch-dialog-hint" });
    {
      const row = new Div(this._androidRows, { class: "launch-dialog-row" });
      new Span(row, { text: "Package", class: "launch-dialog-label" });
      this._package = new Select(row, { options: [], editable: true, class: "launch-dialog-package",
        tooltip: "The application's package name; typing filters the list. It must be debuggable (a Unity Development Build) for Android to load the layer." });
      // Typing narrows the dropdown to the packages containing the text.
      const edit = this._package.selectEdit!;
      edit.placeholder = "type to filter the device's packages";
      edit.element.addEventListener("input", () => this._filterPackages());
      this._packageHint = new Span(row, { text: "", class: "launch-dialog-count" });
    }
    // Implicit layer: nothing to launch; the layer is registered for this user and loads into
    // any application started with VKINSP_ENABLE=1, which the session then waits for.
    this._implicitRows = new Div(body);
    section(this._implicitRows, "Implicit Layer");
    {
      const row = new Div(this._implicitRows, { class: "launch-dialog-row" });
      new Span(row, { text: "Registration", class: "launch-dialog-label" });
      this._implicitStatus = new Span(row, { text: "checking...", class: "launch-dialog-status" });
      this._implicitButton = new Button(row, { label: "Register", class: "btn", callback: () => void this._toggleImplicit() });
    }
    new Div(this._implicitRows, {
      text: "Start the application yourself with these environment variables, then press Wait: VKINSP_ENABLE=1 and VKINSP_PORT set to the port below (VKINSP_LOG_FILE=<path> writes the layer's log to a file, since the inspector cannot read the output of a process it did not start). For an editor started from a launcher, set them for your user account (setx on Windows) and restart the launcher. The registration is per user and stays until you unregister it.",
      class: "launch-dialog-hint",
    });
    this._activity = this._inputRow(this._androidRows, "Activity", "(the package's launcher activity)");
    this._activity.tooltip = "Activity to start, as com.example.Activity or .Activity; empty for the launcher activity";
    this._symbolDirs = this._inputRow(this._androidRows, "Symbol directories", "(directories with the unstripped .so files, separated by ;)");
    this._symbolDirs.tooltip = "Where to look for the application's unstripped libraries (the build tree): stack traces of objects and captured commands then show functions, files and lines, resolved with the NDK's llvm-symbolizer on this machine.";

    section(body, "Inspector Options");
    this._sourceRoots = this._inputRow(body, "Source roots", "(directories with the shader sources, separated by ;)");
    this._sourceRoots.tooltip = "Where the shader sources live on this machine: a shader compiled with line information but without embedded text (dxc -Zi, a stripped build) then gets its Source view, line costs and findings from the file its debug information names.";
    {
      const row = new Div(body, { class: "launch-dialog-row launch-dialog-options" });
      this._recordAlways = new Checkbox(row, { label: "Record all command buffers", checked: false,
        tooltip: "Record every command buffer as it is built, so buffers recorded once and reused every frame appear in captures. Costs CPU time in the target." });
      this._log = new Checkbox(row, { label: "Layer log", checked: true, tooltip: "Log the layer's activity (the target's stderr, or logcat on Android), shown in the Log tab" });
      this._validation = new Checkbox(row, { label: "Validation layer", checked: false,
        tooltip: "Also enable the Khronos validation layer (VK_LAYER_KHRONOS_validation from the Vulkan SDK). Its errors and warnings are listed in the Inspect tab and linked to the objects they name. Native targets only; slows the application down." });
      this._syncValidation = new Checkbox(row, { label: "Sync validation", checked: false,
        tooltip: "With the validation layer: synchronization validation, which reports hazards between commands (at record time) and between submissions (at vkQueueSubmit, linked to the command the message names). Slow." });
      this._stacktraces = new Checkbox(row, { label: "Stack traces", checked: true,
        tooltip: "Record the call stack of every object creation, shown in the object's details (symbols from the application's PDBs or exports). A few microseconds per created object." });
      new Span(row, { text: "Port", class: "launch-dialog-label launch-dialog-label-inline" });
      this._port = new TextInput(row, { value: String(DEFAULT_PORT), class: "launch-dialog-input launch-dialog-port" });
    }
    {
      // Queued capture: taken automatically once the application connects.
      const row = new Div(body, { class: "launch-dialog-row launch-dialog-options" });
      new Span(row, { text: "Queued Capture", class: "launch-dialog-label" });
      this._captureMode = new Select(row, { options: CAPTURE_MODES.map((m) => m[0]), class: "launch-dialog-capture-mode", onChange: () => this._updateCaptureFields() });
      this._captureValueLabel = new Span(row, { text: "Frame", class: "launch-dialog-label launch-dialog-label-inline" });
      this._captureValue = new TextInput(row, { value: "0", class: "launch-dialog-input launch-dialog-port",
        tooltip: "Frame number to capture (0 = the first frame the inspector sees), or seconds to wait after connecting" });
    }

    const footer = new Div(this, { class: "dialog-footer launch-dialog-footer" });
    new Button(footer, { label: "Cancel", class: "btn", callback: () => this.close() });
    this._launchButton = new Button(footer, { label: "Launch", class: "btn btn-success", callback: () => {
      const config = this.config;
      if (config.target === "android") {
        if (!config.device) {
          this._deviceHint.text = this._devices.length ? "Choose a device." : "No device: connect one with USB debugging enabled and press Refresh.";
          return;
        }
        if (!config.exe) {
          (this._package.selectEdit ?? this._package).element.focus();
          return;
        }
      } else if (!config.exe) {
        this._exe.element.focus();
        return;
      }
      this.close();
      onLaunch(config);
    }});

    this.setConfig(initial ?? recents[0] ?? emptyLaunchConfig());
    setTimeout(() => {
      if (this.target === "native") this._exe.element.focus();
    }, 0);
  }

  private _inputRow(parent: Div, label: string, placeholder: string): TextInput {
    const row = new Div(parent, { class: "launch-dialog-row" });
    new Span(row, { text: label, class: "launch-dialog-label" });
    return new TextInput(row, { placeholder, class: "launch-dialog-input" });
  }

  private _pathRow(parent: Div, label: string, placeholder: string, title: string, directory: boolean): TextInput {
    const row = new Div(parent, { class: "launch-dialog-row" });
    new Span(row, { text: label, class: "launch-dialog-label" });
    const input = new TextInput(row, { placeholder, class: "launch-dialog-input" });
    new Button(row, { label: "...", class: "btn", tooltip: "Browse", callback: () => {
      void window.inspector.chooseFile({ title, directory }).then((f) => { if (f) input.value = f; });
    }});
    return input;
  }

  private get target(): LaunchConfig["target"] {
    return this._targets[this._target.index]?.[1] ?? this._targets[0]?.[1] ?? "native";
  }

  private _updateTarget(): void {
    const android = this.target === "android";
    const implicit = this.target === "implicit";
    this._nativeRows.style.display = android || implicit ? "none" : "";
    this._androidRows.style.display = android ? "" : "none";
    this._implicitRows.style.display = implicit ? "" : "none";
    this._launchButton.text = implicit ? "Wait" : "Launch";
    if (android && !this._devices.length) void this._loadDevices();
    if (implicit) void this._refreshImplicit();
  }

  private async _refreshImplicit(): Promise<void> {
    const status = await window.inspector.implicitLayer();
    this._implicitRegistered = status.registered;
    this._implicitStatus.text = status.error ? status.error : status.registered ? `registered: ${status.manifest}` : "not registered";
    this._implicitButton.text = status.registered ? "Unregister" : "Register";
  }

  private async _toggleImplicit(): Promise<void> {
    this._implicitButton.disabled = true;
    const status = await window.inspector.setImplicitLayer(!this._implicitRegistered);
    this._implicitButton.disabled = false;
    this._implicitRegistered = status.registered;
    this._implicitStatus.text = status.error ? status.error : status.registered ? `registered: ${status.manifest}` : "not registered";
    this._implicitButton.text = status.registered ? "Unregister" : "Register";
  }

  private async _loadDevices(): Promise<void> {
    if (this._loadingDevices) return;
    this._loadingDevices = true;
    this._deviceHint.text = "Looking for devices...";
    try {
      const list = await window.inspector.androidDevices();
      this._devices = list.devices;
      setOptions(this._device, list.devices.map(deviceLabel));
      const hints: string[] = [];
      if (list.error) hints.push(list.error);
      else if (!list.devices.length) hints.push("No devices found. Connect a device with USB debugging enabled, or start an emulator, then press Refresh.");
      else if (!list.devices.some((d) => d.state === "device")) hints.push("No usable device: accept the USB debugging prompt on the device, then press Refresh.");
      if (!list.layer) hints.push("The Android layer is not built: run tools/build_android.py first.");
      this._deviceHint.text = hints.join("\n");
      // The remembered device when present, else the first usable one.
      let index = list.devices.findIndex((d) => d.serial === this._pendingDevice);
      if (index < 0) index = list.devices.findIndex((d) => d.state === "device");
      this._device.index = Math.max(0, index);
    } finally {
      this._loadingDevices = false;
    }
    await this._loadPackages();
  }

  private async _loadPackages(): Promise<void> {
    const device = this._devices[this._device.index];
    const typed = this._package.value;
    if (!device || device.state !== "device") {
      setOptions(this._package, []);
      return;
    }
    const packages = await window.inspector.androidPackages(device.serial);
    if (this._devices[this._device.index] !== device) return;  // changed meanwhile
    this._packages = packages;
    setOptions(this._package, packages);
    // Keep what the user typed or a recent configuration filled in; otherwise offer the first.
    this._package.value = typed || packages[0] || "";
    this._filterPackages();
  }

  /** The dropdown lists the device's packages containing the typed text (all of them when the
   *  text is a package of the list, so the neighbors stay reachable). */
  private _filterPackages(): void {
    const text = this._package.value.trim().toLowerCase();
    const shown = text && !this._packages.some((p) => p.toLowerCase() === text)
      ? this._packages.filter((p) => p.toLowerCase().includes(text)) : this._packages;
    setOptions(this._package, shown);
    const i = shown.findIndex((p) => p.toLowerCase() === text);
    this._package.index = i >= 0 ? i : shown.length ? 0 : -1;
    this._packageHint.text = !this._packages.length ? "" : shown.length === this._packages.length
      ? `${shown.length} packages` : `${shown.length} of ${this._packages.length} match`;
  }

  private _updateCaptureFields(): void {
    const mode = CAPTURE_MODES[this._captureMode.index]?.[1] ?? "none";
    const show = mode !== "none";
    this._captureValueLabel.text = mode === "time" ? "Seconds" : "Frame";
    this._captureValueLabel.style.display = show ? "" : "none";
    this._captureValue.style.display = show ? "" : "none";
  }

  get config(): LaunchConfig {
    const mode = CAPTURE_MODES[this._captureMode.index]?.[1] ?? "none";
    const target = this.target;
    const android = target === "android";
    return {
      target,
      exe: android ? this._package.value.trim() : target === "implicit" ? "" : this._exe.value.trim(),
      args: android ? "" : this._args.value,
      cwd: android ? "" : this._cwd.value.trim(),
      env: android ? "" : this._env.value,
      device: android ? (this._devices[this._device.index]?.serial ?? this._pendingDevice) : "",
      activity: android ? this._activity.value.trim() : "",
      port: Number(this._port.value) || DEFAULT_PORT,
      log: this._log.checked,
      recordAlways: this._recordAlways.checked,
      validation: !android && this._validation.checked,
      syncValidation: !android && this._validation.checked && this._syncValidation.checked,
      symbolDirs: android ? this._symbolDirs.value.trim() : "",
      sourceRoots: this._sourceRoots.value.trim(),
      stacktraces: this._stacktraces.checked,
      capture: { mode, value: Math.max(0, Number(this._captureValue.value) || 0) },
    };
  }

  setConfig(c: LaunchConfig): void {
    // A recent launch whose target this host does not offer (a native launch in a configuration
    // carried to a mac) falls back to the first target, so `android` follows the selection rather
    // than the configuration.
    const wanted = this._targets.findIndex(([, t]) => t === c.target);
    this._target.index = wanted >= 0 ? wanted : 0;
    const android = this.target === "android";
    if (android) {
      this._package.value = c.exe ?? "";
      this._activity.value = c.activity ?? "";
      this._pendingDevice = c.device ?? "";
      const index = this._devices.findIndex((d) => d.serial === this._pendingDevice);
      if (index >= 0) this._device.index = index;
    } else {
      this._exe.value = c.exe ?? "";
      this._args.value = c.args ?? "";
      this._cwd.value = c.cwd ?? "";
      this._env.value = c.env ?? "";
    }
    this._port.value = String(c.port || DEFAULT_PORT);
    this._log.checked = c.log ?? true;
    this._recordAlways.checked = c.recordAlways ?? false;
    this._validation.checked = c.validation ?? false;
    this._syncValidation.checked = c.syncValidation ?? false;
    this._symbolDirs.value = c.symbolDirs ?? "";
    this._sourceRoots.value = c.sourceRoots ?? "";
    this._stacktraces.checked = c.stacktraces ?? true;
    const mode = c.capture?.mode ?? "none";
    this._captureMode.index = Math.max(0, CAPTURE_MODES.findIndex((m) => m[1] === mode));
    this._captureValue.value = String(c.capture?.value ?? 0);
    this._updateCaptureFields();
    this._updateTarget();
  }
}
