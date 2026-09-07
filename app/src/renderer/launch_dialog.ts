// Modal dialog for launching an application with the layer enabled. Recently launched
// configurations can be picked from a dropdown to fill in the fields.
import { Dialog } from "./widget/dialog.js";
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import { Button } from "./widget/button.js";
import { Checkbox } from "./widget/checkbox.js";
import { Select } from "./widget/select.js";
import { TextArea } from "./widget/text_area.js";
import { TextInput } from "./widget/text_input.js";
import type { LaunchConfig } from "../shared/protocol.js";

const DEFAULT_PORT = 47531;

export function emptyLaunchConfig(): LaunchConfig {
  return { exe: "", args: "", cwd: "", env: "", port: DEFAULT_PORT, log: true, recordAlways: false };
}

export function launchDisplayName(c: LaunchConfig): string {
  const base = c.exe.replace(/\\/g, "/").split("/").pop() || c.exe;
  return c.args ? `${base} ${c.args}` : base;
}

export class LaunchDialog extends Dialog {
  private _recents: LaunchConfig[];
  private _recentSelect: Select | null = null;
  private _exe: TextInput;
  private _cwd: TextInput;
  private _args: TextInput;
  private _env: TextArea;
  private _port: TextInput;
  private _log: Checkbox;
  private _recordAlways: Checkbox;

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

    const section = (title: string): Div => {
      new Div(body, { text: title, class: "launch-dialog-section" });
      return body;
    };

    section("Program");
    this._exe = this._pathRow(body, "Executable Path", "path to a Vulkan application", "Choose Vulkan application", false);
    this._cwd = this._pathRow(body, "Working Directory", "(executable's folder)", "Choose working directory", true);
    this._args = this._inputRow(body, "Command-line Arguments", "");
    {
      const row = new Div(body, { class: "launch-dialog-row launch-dialog-row-top" });
      new Span(row, { text: "Environment Variables", class: "launch-dialog-label" });
      this._env = new TextArea(row, { placeholder: "KEY=VALUE, one per line", class: "launch-dialog-textarea" });
    }

    section("Inspector Options");
    {
      const row = new Div(body, { class: "launch-dialog-row launch-dialog-options" });
      this._recordAlways = new Checkbox(row, { label: "Record all command buffers", checked: false,
        tooltip: "Record every command buffer as it is built, so buffers recorded once and reused every frame appear in captures. Costs CPU time in the target." });
      this._log = new Checkbox(row, { label: "Layer log", checked: true, tooltip: "Log the layer's activity to the target's stderr (shown in the Log tab)" });
      new Span(row, { text: "Port", class: "launch-dialog-label launch-dialog-label-inline" });
      this._port = new TextInput(row, { value: String(DEFAULT_PORT), class: "launch-dialog-input launch-dialog-port" });
    }

    const footer = new Div(this, { class: "dialog-footer launch-dialog-footer" });
    new Button(footer, { label: "Cancel", class: "btn", callback: () => this.close() });
    new Button(footer, { label: "Launch", class: "btn btn-success", callback: () => {
      const config = this.config;
      if (!config.exe) {
        this._exe.element.focus();
        return;
      }
      this.close();
      onLaunch(config);
    }});

    this.setConfig(initial ?? recents[0] ?? emptyLaunchConfig());
    setTimeout(() => this._exe.element.focus(), 0);
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

  get config(): LaunchConfig {
    return {
      exe: this._exe.value.trim(),
      args: this._args.value,
      cwd: this._cwd.value.trim(),
      env: this._env.value,
      port: Number(this._port.value) || DEFAULT_PORT,
      log: this._log.checked,
      recordAlways: this._recordAlways.checked,
    };
  }

  setConfig(c: LaunchConfig): void {
    this._exe.value = c.exe ?? "";
    this._args.value = c.args ?? "";
    this._cwd.value = c.cwd ?? "";
    this._env.value = c.env ?? "";
    this._port.value = String(c.port || DEFAULT_PORT);
    this._log.checked = c.log ?? true;
    this._recordAlways.checked = c.recordAlways ?? false;
  }
}
