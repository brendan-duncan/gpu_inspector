// Application window. The main window has the launch toolbar and one tab per inspected
// application (a SessionPanel with its own Inspect / Capture / Log tabs). A session window
// (opened with "Open in New Window") shows the sessions moved into it and has no launcher.
import { Window } from "./widget/window.js";
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import { Button } from "./widget/button.js";
import { TextInput } from "./widget/text_input.js";
import { TabWidget } from "./widget/tab_widget.js";
import { TabHandle } from "./widget/tab_handle.js";
import { Dialog } from "./widget/dialog.js";
import { Widget } from "./widget/widget.js";
import { showContextMenu, type ContextMenuItem } from "./widget/context_menu.js";
import { SessionPanel } from "./session_panel.js";
import { LaunchDialog, launchDisplayName } from "./launch_dialog.js";
import { applyTheme, currentTheme, themeLabel } from "./theme.js";
import { THEMES, type AppConfig, type LaunchConfig, type LaunchResult, type SessionInfo, type ThemeName, type UpdateStatus } from "../shared/protocol.js";

// Theme icons (inline SVG in the button's text color): a moon for dark, a sun for light.
const THEME_ICONS: Record<ThemeName, string> = {
  dark: '<svg viewBox="0 0 16 16" aria-label="Dark theme"><path d="M10.5 2.2a6 6 0 1 0 3.3 8.6A5 5 0 0 1 10.5 2.2z" fill="currentColor"/></svg>',
  light: '<svg viewBox="0 0 16 16" aria-label="Light theme"><circle cx="8" cy="8" r="3" fill="currentColor"/><path d="M8 1.2v2M8 12.8v2M1.2 8h2M12.8 8h2M3.2 3.2l1.4 1.4M11.4 11.4l1.4 1.4M3.2 12.8l1.4-1.4M11.4 4.6l1.4-1.4" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/></svg>',
};

// Toolbar icon (inline SVG in the button's text color): a clock face for the recent launches.
const ICON_RECENT = '<svg viewBox="0 0 16 16" aria-label="Recent"><circle cx="8" cy="8" r="6.2" fill="none" stroke="currentColor" stroke-width="1.5"/><path d="M8 4.5V8l2.6 1.8" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/><path d="M12.2 13.4l1.8 0.6-0.6-1.8" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"/></svg>';

export class InspectorWindow extends Window {
  private _mode: "main" | "session" = "main";
  private _tabs: TabWidget;
  private _sessions = new Map<number, SessionPanel>();
  private _handles = new Map<number, TabHandle>();
  private _placeholder: Div;
  /** Set while a tab is removed because the main process dropped the session. */
  private _removingSession = 0;

  private _recents: LaunchConfig[] = [];
  private _lastLaunch: LaunchConfig | null = null;
  private _recentMenu: Div | null = null;
  private _portInput: TextInput | null = null;
  private _themeButton: Button | null = null;
  private _themeMenu: Div | null = null;
  private _debug: AppConfig["debug"] | null = null;
  private _versionLabel: Button | null = null;
  private _updateBar: Div | null = null;
  private _updateText: Span | null = null;
  private _updateButtons: Div | null = null;
  private _updateTimer = 0;
  /** Set when the user asked for the check, so "up to date" and errors are shown; startup checks stay silent. */
  private _manualCheck = false;

  constructor() {
    super();
    this.classList.add("main-window");
    const params = new URLSearchParams(window.location.search);
    this._mode = params.has("session") ? "session" : "main";

    if (this._mode === "main") {
      this._buildToolbar();
      this._buildUpdateBar();
    }

    this._tabs = new TabWidget(this, { class: "main-tabs tabs-fill", displayCloseButton: true });
    new Widget("span", this._tabs.headerElement, { text: "GPU Inspector", class: "app-title" });
    this._tabs.onTabClosed.addListener((panel) => this._tabClosed(panel));

    this._placeholder = new Div(this, { class: "main-placeholder" });
    new Div(this._placeholder, { text: this._mode === "main"
      ? "No application is being inspected. Use Launch... to start one, Recent to relaunch a previous one, or Connect to attach to a running application with the layer enabled."
      : "No sessions in this window.", class: "text-muted" });

    window.inspector.onSessionAdded((info) => this._addSession(info));
    window.inspector.onSessionRemoved((id) => this._removeSession(id));
    window.inspector.onMessages((batch) => this._sessions.get(batch.sessionId)?.handleMessages(batch.messages));
    window.inspector.onStatus((s) => {
      const panel = this._sessions.get(s.sessionId);
      if (!panel) return;
      panel.setStatus(s);
      if (s.state === "connected" && this._debug?.capture) this._debugCapture(panel);
    });
    window.inspector.onLog((l) => this._sessions.get(l.sessionId)?.appendLog(l.line));
    window.inspector.onRecents((recents) => this._setRecents(recents));
    window.inspector.onTheme((theme) => this._setTheme(theme));
    window.inspector.onUpdate((status) => this._setUpdateStatus(status));

    void window.inspector.getConfig().then((cfg) => {
      this._debug = cfg.debug;
      this._setTheme(cfg.theme);
      this._setRecents(cfg.recents);
      this._setVersion(cfg.version, cfg.canUpdate);
      for (const s of cfg.sessions) this._addSession(s);
      if (this._mode === "main") {
        if (cfg.debug?.launchDialog) this.showLaunchDialog();
        if (!cfg.layerDir) this._showMessage("Layer not built", "The capture layer was not found. Build it first (see docs/ARCHITECTURE.md).");
      }
      this._updatePlaceholder();
    });
  }

  // ---------------------------------------------------------------------------------------
  // Sessions

  private _addSession(info: SessionInfo): void {
    if (this._sessions.has(info.id)) return;
    const panel = new SessionPanel(info);
    this._sessions.set(info.id, panel);
    const handle = this._tabs.addTab(info.name, panel);
    handle.tooltip = info.config ? `${info.config.exe}\n${info.config.args}\nport ${info.port}` : `port ${info.port}`;
    handle.element.oncontextmenu = (e: MouseEvent) => {
      e.preventDefault();
      this._tabs.setHandleActive(handle);
      showContextMenu(e.clientX, e.clientY, this._sessionMenu(panel));
    };
    this._handles.set(info.id, handle);
    this._tabs.setHandleActive(handle);
    this._updatePlaceholder();
    // A window that picks up a running session needs the layer's object list again.
    if (info.state === "connected") {
      void window.inspector.refresh(info.id);
      if (this._debug?.capture) this._debugCapture(panel);
    }
    if (this._debug?.select) this._debugSelect(panel, this._debug.select);
  }

  private _sessionMenu(panel: SessionPanel): ContextMenuItem[] {
    const id = panel.sessionId;
    const running = panel.info.state === "launched" || panel.info.state === "connecting" || panel.info.state === "connected";
    return [
      this._mode === "main"
        ? { label: "Open in New Window", callback: () => void window.inspector.openSessionWindow(id) }
        : { label: "Move to Main Window", callback: () => void window.inspector.moveSessionToMain(id) },
      { separator: true },
      { label: "Relaunch", disabled: !panel.info.config, callback: () => void window.inspector.restart(id) },
      { label: "Stop", disabled: !running, callback: () => void window.inspector.kill(id) },
      { separator: true },
      { label: "Close", callback: () => this._closeSession(id) },
    ];
  }

  /** The main process dropped the session or moved it to another window: remove its tab. */
  private _removeSession(id: number): void {
    const handle = this._handles.get(id);
    if (!handle) return;
    this._removingSession = id;
    this._tabs.closeTabHandle(handle);
    this._removingSession = 0;
    this._forgetSession(id);
  }

  private _forgetSession(id: number): void {
    this._sessions.delete(id);
    this._handles.delete(id);
    this._updatePlaceholder();
  }

  /** The user closed a session tab: end the session (terminating its application). */
  private _tabClosed(panel: Widget): void {
    if (!(panel instanceof SessionPanel)) return;
    if (this._removingSession === panel.sessionId) return;
    this._forgetSession(panel.sessionId);
    void window.inspector.closeSession(panel.sessionId);
  }

  private _closeSession(id: number): void {
    const handle = this._handles.get(id);
    if (handle) this._tabs.closeTabHandle(handle);
  }

  private _updatePlaceholder(): void {
    const empty = this._sessions.size === 0;
    this._placeholder.style.display = empty ? "" : "none";
    this._tabs.style.display = empty ? "none" : "";
  }

  // ---------------------------------------------------------------------------------------
  // Launch toolbar (main window)

  private _buildToolbar(): void {
    const bar = new Div(this, { class: "control-bar launch-bar" });
    const row = new Div(bar, { class: "launch-row" });
    new Button(row, { label: "Launch...", class: "btn btn-success", tooltip: "Launch an application with the inspector layer", callback: () => this.showLaunchDialog() });

    // Recents dropdown: one click relaunches a previous configuration.
    const menu = new Div(row, { class: "menu-container" });
    new Button(menu, { html: ICON_RECENT, class: "btn btn-icon", tooltip: "Recent: relaunch a previous configuration", callback: () => {
      this._recentMenu?.classList.toggle("open");
    }});
    this._recentMenu = new Div(menu, { class: "menu-dropdown recent-menu" });
    document.addEventListener("mousedown", (e) => {
      if (!menu.element.contains(e.target as Node)) this._recentMenu?.classList.remove("open");
    });

    new Span(row, { text: "Port", class: "launch-label" });
    this._portInput = new TextInput(row, { class: "launch-input launch-input-narrow", value: "47531" });
    new Button(row, { label: "Connect", class: "btn", tooltip: "Connect to an already running application that has the layer enabled", callback: () => {
      void window.inspector.connect(Number(this._portInput?.value));
    }});

    // Theme picker, right-aligned (margin-left: auto). The choice is saved and applied to every
    // window. The button shows the current theme's icon; the menu lists every theme.
    const themeMenu = new Div(row, { class: "menu-container theme-container" });
    this._themeButton = new Button(themeMenu, { html: THEME_ICONS[currentTheme()], class: "btn btn-icon theme-button",
      tooltip: `Theme: ${themeLabel(currentTheme())}`, callback: () => {
      this._themeMenu?.classList.toggle("open");
    }});
    this._themeMenu = new Div(themeMenu, { class: "menu-dropdown theme-menu" });
    for (const theme of THEMES) {
      const item = new Div(this._themeMenu, { class: "menu-item theme-menu-item" });
      new Span(item, { html: THEME_ICONS[theme], class: "theme-menu-icon" });
      new Span(item, { text: themeLabel(theme) });
      item.element.onclick = () => {
        this._themeMenu?.classList.remove("open");
        void window.inspector.setTheme(theme);
      };
    }
    document.addEventListener("mousedown", (e) => {
      if (!themeMenu.element.contains(e.target as Node)) this._themeMenu?.classList.remove("open");
    });

    // Version label; clicking it checks for updates (installed builds).
    this._versionLabel = new Button(row, { label: "", class: "btn version-label", callback: () => {
      this._manualCheck = true;
      void window.inspector.checkForUpdates();
    }});
    this._setRecents([]);
  }

  // ---------------------------------------------------------------------------------------
  // Self-update (main window). A bar below the toolbar reports the state and offers the next
  // step: download, then restart. The main process drives it (see main.ts, "Self-update").

  private _buildUpdateBar(): void {
    this._updateBar = new Div(this, { class: "update-bar" });
    this._updateText = new Span(this._updateBar, { class: "update-text" });
    this._updateButtons = new Div(this._updateBar, { class: "update-buttons" });
    this._updateBar.style.display = "none";
  }

  private _setVersion(version: string, canUpdate: boolean): void {
    if (!this._versionLabel) return;
    this._versionLabel.text = `v${version}`;
    this._versionLabel.tooltip = canUpdate ? `GPU Inspector ${version}. Click to check for updates.`
      : `GPU Inspector ${version} (development build; updates are only available in installed builds)`;
  }

  private _setUpdateStatus(status: UpdateStatus): void {
    const bar = this._updateBar;
    const text = this._updateText;
    const buttons = this._updateButtons;
    if (!bar || !text || !buttons) return;
    clearTimeout(this._updateTimer);
    buttons.html = "";
    bar.classList.toggle("update-error", status.state === "error");
    const show = (message: string, hideAfterMs = 0): void => {
      text.text = message;
      bar.style.display = "";
      if (hideAfterMs) this._updateTimer = window.setTimeout(() => this._hideUpdateBar(), hideAfterMs);
    };
    const later = (): Button => new Button(buttons, { label: "Later", class: "btn", callback: () => this._hideUpdateBar() });
    switch (status.state) {
      case "checking":
        if (this._manualCheck) show("Checking for updates...");
        break;
      case "available":
        show(`GPU Inspector ${status.version} is available.`);
        new Button(buttons, { label: "Download", class: "btn btn-success", callback: () => void window.inspector.downloadUpdate() });
        later();
        break;
      case "up-to-date":
        if (this._manualCheck) show("GPU Inspector is up to date.", 4000);
        else this._hideUpdateBar();
        break;
      case "downloading":
        show(`Downloading update... ${status.percent.toFixed(0)}%`);
        break;
      case "downloaded":
        show(`GPU Inspector ${status.version} is ready and will be installed when the app closes.`);
        new Button(buttons, { label: "Restart and Install", class: "btn btn-success",
          tooltip: "Stops the inspected applications, installs the update and restarts",
          callback: () => void window.inspector.installUpdate() });
        later();
        break;
      case "error":
        // Startup checks fail quietly (offline, no release yet); a user-requested check reports it.
        if (this._manualCheck) {
          show(status.message.split("\n")[0], 8000);
          later();
        } else this._hideUpdateBar();
        break;
    }
    if (status.state !== "checking") this._manualCheck = false;
  }

  private _hideUpdateBar(): void {
    clearTimeout(this._updateTimer);
    if (this._updateBar) this._updateBar.style.display = "none";
  }

  private _setTheme(theme: ThemeName): void {
    applyTheme(theme);
    if (this._themeButton) {
      this._themeButton.html = THEME_ICONS[theme];
      this._themeButton.tooltip = `Theme: ${themeLabel(theme)}`;
    }
    this._themeMenu?.children.forEach((item, i) => item.classList.toggle("active", THEMES[i] === theme));
  }

  private _setRecents(recents: LaunchConfig[]): void {
    this._recents = recents;
    const menu = this._recentMenu;
    if (!menu) return;
    menu.html = "";
    if (!recents.length) {
      new Div(menu, { text: "No recent launches", class: "menu-item disabled" });
      return;
    }
    recents.forEach((r, index) => {
      const item = new Div(menu, { class: "menu-item recent-item", tooltip: `${r.exe}\n${r.args}` });
      new Span(item, { text: launchDisplayName(r), class: "recent-item-name" });
      new Span(item, { text: r.exe, class: "recent-item-path" });
      const remove = new Span(item, { text: "×", class: "recent-item-remove", tooltip: "Remove from recents" });
      remove.element.onclick = (e: MouseEvent) => {
        e.stopPropagation();
        void window.inspector.removeRecent(index).then((list) => this._setRecents(list));
      };
      item.element.onclick = () => {
        menu.classList.remove("open");
        this.launch(r);
      };
    });
    new Div(menu, { class: "menu-separator" });
    const clear = new Div(menu, { text: "Clear recents", class: "menu-item" });
    clear.element.onclick = () => {
      menu.classList.remove("open");
      void window.inspector.clearRecents().then((list) => this._setRecents(list));
    };
  }

  showLaunchDialog(): void {
    new LaunchDialog(this._recents, this._lastLaunch, (config) => this.launch(config));
  }

  launch(config: LaunchConfig): void {
    this._lastLaunch = config;
    if (this._portInput) this._portInput.value = String(config.port);
    void window.inspector.launch(config).then((r: LaunchResult) => {
      if (!r.ok) this._showMessage("Launch failed", r.error ?? "unknown error");
    });
  }

  private _showMessage(title: string, text: string): void {
    const dlg = new Dialog({ title, width: 520 });
    new Div(dlg.body, { text, style: "padding: 12px 16px; white-space: pre-wrap;" });
    const footer = new Div(dlg, { class: "dialog-footer launch-dialog-footer" });
    new Button(footer, { label: "OK", class: "btn", callback: () => dlg.close() });
  }

  // ---------------------------------------------------------------------------------------
  // Testing aids (--debug-select=<VkType>, --debug-capture)

  // --debug-select=<VkType> or <VkType>:<name substring>
  private _debugSelect(panel: SessionPanel, spec: string): void {
    const [type, name] = spec.split(":");
    const tryIt = (): void => {
      if (!this._sessions.has(panel.sessionId)) return;
      const objs = panel.database.getObjectsOfType(type);
      const first = name ? [...(objs?.values() ?? [])].find((o) => o.name.includes(name)) : objs?.values().next().value;
      if (first) panel.inspectPanel.revealObject(first);
      else setTimeout(tryIt, 500);
    };
    setTimeout(tryIt, 1000);
  }

  private _debugCapture(panel: SessionPanel): void {
    setTimeout(() => {
      if (!this._sessions.has(panel.sessionId) || !panel.connected) return;
      panel.showCaptureTab();
      panel.capturePanel.capture(this._debug?.captureFrames);
    }, 1500);
  }
}

applyTheme(new URLSearchParams(window.location.search).get("theme"));
new InspectorWindow();
