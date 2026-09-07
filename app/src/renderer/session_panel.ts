// One inspected application: its object database, the Inspect / Capture / Log tabs, and a bar
// with the session's state and controls. One SessionPanel is shown per session tab.
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import { Button } from "./widget/button.js";
import { Checkbox } from "./widget/checkbox.js";
import { TabWidget } from "./widget/tab_widget.js";
import { Widget } from "./widget/widget.js";
import { ObjectDatabase } from "./vulkan/object_database.js";
import { InspectPanel } from "./inspect_panel.js";
import { CapturePanel } from "./capture_panel.js";
import type { LayerMessage, SessionInfo, StatusMessage, UiRequest } from "../shared/protocol.js";

/** What the Inspect and Capture panels need from the session that owns them. */
export interface SessionContext {
  readonly database: ObjectDatabase;
  readonly connected: boolean;
  send(msg: UiRequest): Promise<boolean>;
  /** Reveals an object in the Inspect tab. */
  showObject(objectId: number): void;
}

const MAX_LOG_LINES = 2000;

export class SessionPanel extends Div implements SessionContext {
  readonly sessionId: number;
  info: SessionInfo;
  readonly database = new ObjectDatabase();
  readonly inspectPanel: InspectPanel;
  readonly capturePanel: CapturePanel;

  private _tabs: TabWidget;
  private _log: Widget;
  private _logLines: string[] = [];
  private _nameLabel!: Span;
  private _statusLabel!: Span;
  private _frameLabel!: Span;
  private _stopButton!: Button;
  private _restartButton!: Button;
  private _recordAlwaysCheck!: Checkbox;

  constructor(info: SessionInfo, options: { detachLabel: string; onDetach: () => void }) {
    super(null, { class: "session-panel" });
    this.sessionId = info.id;
    this.info = info;
    this._buildBar(options);

    this._tabs = new TabWidget(this, { class: "session-tabs tabs-fill" });
    const inspectorPanel = new Div(null, { class: "inspector_panel" });
    this._tabs.addTab("\u{1F50D} Inspect", inspectorPanel);
    this.inspectPanel = new InspectPanel(this, inspectorPanel);

    const capturePanel = new Div(null, { class: "capture_panel" });
    this._tabs.addTab("\u{1F5BC}️ Capture", capturePanel);
    this.capturePanel = new CapturePanel(this, capturePanel);

    const logPanel = new Div(null, { class: "log-panel" });
    this._tabs.addTab("\u{1F4DC} Log", logPanel);
    this._log = new Widget("pre", logPanel, { class: "log-text" });

    this.database.onFrameStats.addListener((m) => {
      this._frameLabel.text = `frame ${m.frame}  ${m.frameTimeMs.toFixed(2)} ms  (${(1000 / m.frameTimeMs).toFixed(0)} fps)`;
    });
    this.database.onSnapshotBegin.addListener((count) => this.appendLog(`snapshot: ${count} live objects`));

    for (const line of info.log) this._logLines.push(line);
    this._renderLog();
    this.setStatus(info);
  }

  private _buildBar(options: { detachLabel: string; onDetach: () => void }): void {
    const bar = new Div(this, { class: "control-bar session-bar" });
    const row = new Div(bar, { class: "launch-row" });
    this._nameLabel = new Span(row, { text: "", class: "session-name" });
    this._statusLabel = new Span(row, { text: "disconnected", class: "launch-status status-disconnected" });
    this._frameLabel = new Span(row, { text: "", class: "launch-frame" });
    const spacer = new Span(row, { class: "launch-spacer" });
    spacer.style.flexGrow = "1";
    this._recordAlwaysCheck = new Checkbox(row, { label: "Record all command buffers", checked: this.info.recordAlways,
      tooltip: "Record every command buffer as it is built, so buffers recorded once and reused every frame appear in captures. Costs CPU time in the target." });
    this._recordAlwaysCheck.input.onchange = () => {
      void this.send({ action: "Settings", recordAlways: this._recordAlwaysCheck.checked });
    };
    this._stopButton = new Button(row, { label: "Stop", class: "btn btn-danger", tooltip: "Terminate the application", callback: () => {
      void window.inspector.kill(this.sessionId);
    }});
    this._restartButton = new Button(row, { label: "Relaunch", class: "btn", tooltip: "Terminate the application and launch it again", callback: () => {
      void window.inspector.restart(this.sessionId);
    }});
    new Button(row, { label: options.detachLabel, class: "btn", callback: options.onDetach });
  }

  get name(): string {
    return this.info.name;
  }

  get connected(): boolean {
    return this.info.state === "connected";
  }

  send(msg: UiRequest): Promise<boolean> {
    return window.inspector.send(this.sessionId, msg);
  }

  showObject(objectId: number): void {
    const o = this.database.getObject(objectId);
    if (!o) return;
    this._tabs.activeTab = 0;
    this.inspectPanel.revealObject(o);
  }

  showCaptureTab(): void {
    this._tabs.activeTab = 1;
  }

  handleMessages(batch: LayerMessage[]): void {
    for (const msg of batch) this.database.handleMessage(msg);
  }

  setStatus(s: StatusMessage): void {
    this.info = { ...this.info, state: s.state, detail: s.detail };
    const running = s.state === "launched" || s.state === "connecting" || s.state === "connected";
    this._statusLabel.text = s.detail ? `${s.state}: ${s.detail}` : s.state;
    this._statusLabel.element.className = `launch-status status-${s.state}`;
    if (s.state !== "connected") this._frameLabel.text = "";
    const pid = /^pid (\d+)/.exec(s.detail);
    if (pid) this.info = { ...this.info, pid: Number(pid[1]) };
    const port = /^port (\d+)/.exec(s.detail);
    if (port) this.info = { ...this.info, port: Number(port[1]) };
    this._nameLabel.text = `${this.info.name}   port ${this.info.port}${this.info.pid && running ? `   pid ${this.info.pid}` : ""}`;
    this._stopButton.disabled = !running;
    this._restartButton.disabled = !this.info.config;
  }

  appendLog(line: string): void {
    this._logLines.push(line);
    if (this._logLines.length > MAX_LOG_LINES) this._logLines.splice(0, this._logLines.length - MAX_LOG_LINES);
    this._renderLog();
  }

  private _renderLog(): void {
    this._log.text = this._logLines.join("\n");
    this._log.element.scrollTop = this._log.element.scrollHeight;
  }
}
