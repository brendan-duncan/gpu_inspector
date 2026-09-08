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
import { ShaderReflectionCache } from "./shader_cache.js";
import type { LoadedCapture } from "./capture_file.js";
import type { CapturedTexture } from "./capture_data.js";
import type { LayerMessage, SessionInfo, StatusMessage, UiRequest } from "../shared/protocol.js";

/** What the Inspect and Capture panels need from the session that owns them. */
export interface SessionContext {
  readonly database: ObjectDatabase;
  readonly connected: boolean;
  /** Display name of the session (the application, or the capture file). */
  readonly name: string;
  /** SPIR-V reflection of the session's shaders, fetched from the layer on first use. */
  readonly shaders: ShaderReflectionCache;
  send(msg: UiRequest): Promise<boolean>;
  /** Reveals an object in the Inspect tab. */
  showObject(objectId: number): void;
  /** Contents of an image read back by a capture (a sampled image or render target), when one has it. */
  capturedImage(imageId: number): CapturedTexture | null;
}

const MAX_LOG_LINES = 2000;

// Session bar icons (inline SVG in the button's text color).
const ICON_STOP = '<svg viewBox="0 0 16 16" aria-label="Stop"><rect x="3" y="3" width="10" height="10" rx="1.5" fill="currentColor"/></svg>';
const ICON_RELAUNCH = '<svg viewBox="0 0 16 16" aria-label="Relaunch"><path d="M13.2 9.2A5.3 5.3 0 1 1 12 4.3" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/><path d="M13.6 1.8v3.6h-3.6" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/><path d="M6.8 6.2v3.6l3-1.8z" fill="currentColor"/></svg>';

export class SessionPanel extends Div implements SessionContext {
  readonly sessionId: number;
  info: SessionInfo;
  readonly database = new ObjectDatabase();
  readonly shaders: ShaderReflectionCache;
  readonly inspectPanel: InspectPanel;
  readonly capturePanel: CapturePanel;

  private _tabs: TabWidget;
  private _log: Widget;
  private _logLines: string[] = [];
  private _nameLabel!: Span;
  private _statusLabel!: Span;
  private _frameLabel!: Span;
  private _validationLabel!: Span;
  private _stopButton!: Button;
  private _restartButton!: Button;
  private _recordAlwaysCheck!: Checkbox;
  /** The launch configuration's queued capture has been taken (or scheduled) for this run. */
  private _queuedCaptureDone = false;
  private _queuedCaptureTimer: ReturnType<typeof setTimeout> | null = null;

  constructor(info: SessionInfo) {
    super(null, { class: "session-panel" });
    this.sessionId = info.id;
    this.info = info;
    this.shaders = new ShaderReflectionCache(this.database, (msg) => this.send(msg));
    this._buildBar();

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
    // Shader edits are logged too, so their outcome is visible even when the editor is closed.
    this.database.onOtherMessage.addListener((msg) => {
      if (msg.action !== "ShaderReplaced") return;
      const pipeline = this.database.getObject(msg.pipeline);
      const name = pipeline ? pipeline.name : `Pipeline ${msg.pipeline}`;
      this.appendLog(`shader edit: ${name} ${msg.stage}: ${msg.ok ? (msg.replacement ? `applied as object ${msg.replacement}` : "restored") : `failed: ${msg.error ?? "unknown error"}`}${msg.note ? ` (${msg.note})` : ""}`);
    });

    for (const line of info.log) this._logLines.push(line);
    this._renderLog();
    this.setStatus(info);
  }

  // Moving the session between windows is in the session tab's context menu.
  private _buildBar(): void {
    const bar = new Div(this, { class: "control-bar session-bar" });
    const row = new Div(bar, { class: "launch-row" });
    this._nameLabel = new Span(row, { text: "", class: "session-name" });
    this._statusLabel = new Span(row, { text: "disconnected", class: "launch-status status-disconnected" });
    this._frameLabel = new Span(row, { text: "", class: "launch-frame" });
    // Validation counter: errors and warnings the layer's debug-utils messenger reported.
    this._validationLabel = new Span(row, { text: "", class: "launch-validation", tooltip: "Validation messages: click to list them in the Inspect tab" });
    this._validationLabel.style.display = "none";
    this._validationLabel.element.onclick = () => {
      this._tabs.activeTab = 0;
      this.inspectPanel.showValidation();
    };
    this.database.onValidationMessage.addListener((entry, isNew) => {
      this._updateValidationLabel();
      if (isNew) {
        const first = entry.message.split("\n")[0];
        this.appendLog(`validation ${entry.severity}${entry.idName ? ` ${entry.idName}` : ""}: ${first.length > 300 ? `${first.slice(0, 300)}...` : first}`);
      }
    });
    this.database.onReset.addListener(() => this._updateValidationLabel());
    this.database.onLeakReport.addListener((r) => {
      const owner = this.database.getObject(r.owner);
      const summary = Object.entries(r.byType).map(([t, n]) => `${n} ${t.replace(/^Vk/, "")}`).join(", ");
      this.appendLog(`leak report: ${owner ? owner.name : `${r.ownerClass} ${r.owner}`} destroyed with ${r.count} live objects: ${summary}`);
      this._updateValidationLabel();
    });
    const spacer = new Span(row, { class: "launch-spacer" });
    spacer.style.flexGrow = "1";
    this._recordAlwaysCheck = new Checkbox(row, { label: "Record all command buffers", checked: this.info.recordAlways,
      tooltip: "Record every command buffer as it is built, so buffers recorded once and reused every frame appear in captures. Costs CPU time in the target." });
    this._recordAlwaysCheck.input.onchange = () => {
      void this.send({ action: "Settings", recordAlways: this._recordAlwaysCheck.checked });
    };
    this._stopButton = new Button(row, { html: ICON_STOP, class: "btn btn-danger btn-icon", tooltip: "Stop: terminate the application", callback: () => {
      void window.inspector.kill(this.sessionId);
    }});
    this._restartButton = new Button(row, { html: ICON_RELAUNCH, class: "btn btn-icon", tooltip: "Relaunch: terminate the application and launch it again", callback: () => {
      void window.inspector.restart(this.sessionId);
    }});
  }

  /** Takes the launch configuration's queued capture once the application is connected. */
  private _scheduleQueuedCapture(): void {
    const capture = this.info.config?.capture;
    if (!capture || capture.mode === "none" || this._queuedCaptureDone) return;
    this._queuedCaptureDone = true;
    if (capture.mode === "frame") {
      this.showCaptureTab();
      this.capturePanel.capture(undefined, capture.value);
    } else {
      this._queuedCaptureTimer = setTimeout(() => {
        this._queuedCaptureTimer = null;
        if (!this.connected) return;
        this.showCaptureTab();
        this.capturePanel.capture();
      }, capture.value * 1000);
    }
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

  capturedImage(imageId: number): CapturedTexture | null {
    return this.capturePanel.capturedImage(imageId);
  }

  handleMessages(batch: LayerMessage[]): void {
    for (const msg of batch) this.database.handleMessage(msg);
  }

  setStatus(s: StatusMessage): void {
    const wasConnected = this.info.state === "connected";
    this.info = { ...this.info, state: s.state, detail: s.detail };
    const running = s.state === "launched" || s.state === "connecting" || s.state === "connected";
    if (s.state === "launched") {
      // A new run of the application: its queued capture applies again.
      this._queuedCaptureDone = false;
      if (this._queuedCaptureTimer) {
        clearTimeout(this._queuedCaptureTimer);
        this._queuedCaptureTimer = null;
      }
    }
    if (s.state === "connected" && !wasConnected) this._scheduleQueuedCapture();
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

  private _updateValidationLabel(): void {
    const [errors, warnings] = this.database.validationCounts;
    const leaks = this.database.leakCount;
    const parts: string[] = [];
    if (errors) parts.push(`${errors} error${errors === 1 ? "" : "s"}`);
    if (warnings) parts.push(`${warnings} warning${warnings === 1 ? "" : "s"}`);
    if (leaks) parts.push(`${leaks} leaked object${leaks === 1 ? "" : "s"}`);
    this._validationLabel.text = parts.length ? `⚠ ${parts.join(", ")}` : "";
    this._validationLabel.style.display = parts.length ? "" : "none";
    this._validationLabel.element.className = `launch-validation ${errors ? "status-error" : "status-warning"}`;
  }

  /** Bar of a session that shows a capture file: no application to stop, relaunch or configure. */
  protected setFileMode(path: string): void {
    this._nameLabel.text = this.info.name;
    this._nameLabel.tooltip = path;
    this._statusLabel.text = "capture file";
    this._statusLabel.element.className = "launch-status status-file";
    this._statusLabel.tooltip = path;
    this._frameLabel.text = "";
    this._recordAlwaysCheck.style.display = "none";
    this._stopButton.style.display = "none";
    this._restartButton.style.display = "none";
  }
}

/**
 * A session showing a capture loaded from a file (capture_file.ts): its object database comes
 * from the file, its Capture tab holds the loaded capture, and requests that only a running
 * application could answer (image read-back, descriptor contents, shader edits) are declined.
 * Shader payloads are answered from the file, so reflection, shader views and the source view
 * work as they do live.
 */
export class FileSessionPanel extends SessionPanel {
  readonly path: string;

  constructor(info: SessionInfo, capture: LoadedCapture, path: string) {
    super(info);
    this.path = path;
    this.setFileMode(path);
    const m = capture.manifest;
    this.database.loadObjects(capture.objects, capture.blobs, { frame: m.frame, frameTimeMs: m.frameTimeMs ?? 0, submitMs: m.submitMs ?? 0 });
    this.database.loadValidation(capture.validation);
    this.appendLog(`loaded ${path}: ${capture.commands.length} commands, ${capture.objects.length} objects, saved ${m.savedAt} from ${m.source?.name ?? "?"}`);
    this.capturePanel.setFileMode();
    this.capturePanel.openLoaded(capture);
    this.showCaptureTab();
  }

  override get connected(): boolean {
    return false;
  }

  override send(msg: UiRequest): Promise<boolean> {
    if (msg.action === "RequestBlob") {
      const data = this.database.blobData.get(`${msg.id}:${msg.index}`) ?? null;
      // Answered asynchronously, as the layer would, so callers finish registering first.
      setTimeout(() => this.database.handleMessage({ action: "ObjectBlob", id: msg.id, index: msg.index, size: data?.byteLength ?? 0, ...(data ? { __binary: data } : {}) }), 0);
      return Promise.resolve(true);
    }
    return Promise.resolve(false);
  }
}
