// Attaching to an application that is already running, chosen from a list rather than by port.
//
// The capture libraries listen on a small range of ports and answer a probe with what they are
// (src/vulkan/src/target_probe.h), so this lists what is running and attaches to the one picked.
// Probing is safe for a session in progress: an inspector already attached to one of these keeps
// its connection, and the row says so, because attaching would take it from them.
//
// The port box below the list is for what the list cannot see: an Android device reached through
// an adb forward, a Metal application (whose library does not answer probes yet), or anything
// started with a port of its own outside the range. It is the old main-bar Connect, kept for the
// cases that need it rather than for the common one.
import { Dialog } from "./widget/dialog.js";
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import { Button } from "./widget/button.js";
import { TextInput } from "./widget/text_input.js";
import { targetDisplayName, type InspectableTarget } from "../shared/protocol.js";

const DEFAULT_PORT = 47531;

export class AttachDialog extends Dialog {
  private _list: Div;
  private _status: Span;
  private _refreshButton: Button;
  private _attachButton: Button;
  private _port: TextInput;
  private _targets: InspectableTarget[] = [];
  private _selected = -1;
  private _looking = false;
  private _onAttach: (port: number) => void;

  constructor(onAttach: (port: number) => void) {
    super({ title: "Attach to Running Application", width: 620, windowClass: "dialog attach-dialog" });
    this._onAttach = onAttach;
    const body = this.body;
    body.classList.add("attach-dialog-body");

    new Div(body, {
      class: "attach-dialog-hint",
      text: "Applications running with a capture library in them. An application the inspector did "
        + "not start shows up here once it has been started with the layer enabled (Launch..., "
        + "Implicit Layer says how); it does not have to have drawn anything yet.",
    });

    this._list = new Div(body, { class: "attach-dialog-list" });
    this._status = new Span(body, { class: "attach-dialog-status", text: "" });

    // The port box, for a target the probe cannot reach (see the note at the top of this file).
    const portRow = new Div(body, { class: "attach-dialog-port-row" });
    new Span(portRow, { text: "Or attach to port", class: "attach-dialog-port-label" });
    this._port = new TextInput(portRow, { class: "attach-dialog-port", value: String(DEFAULT_PORT) });
    this._port.element.addEventListener("keydown", (e: KeyboardEvent) => {
      if (e.key === "Enter") this._attachToPort(Number(this._port.value) || DEFAULT_PORT);
    });
    new Button(portRow, {
      label: "Attach", class: "btn",
      tooltip: "Attach to whatever is listening on that port: an Android device through an adb forward, "
        + "a Metal application, or one started with a port of its own",
      callback: () => this._attachToPort(Number(this._port.value) || DEFAULT_PORT),
    });

    const footer = new Div(this, { class: "dialog-footer attach-dialog-footer" });
    new Button(footer, { label: "Cancel", class: "btn", callback: () => this.close() });
    this._refreshButton = new Button(footer, {
      label: "Refresh", class: "btn", tooltip: "Look for running applications again",
      callback: () => void this.refresh(),
    });
    this._attachButton = new Button(footer, {
      label: "Attach", class: "btn btn-success",
      callback: () => {
        const target = this._targets[this._selected];
        if (target) this._attachToPort(target.port);
      },
    });

    this._updateButtons();
    void this.refresh();
  }

  /** Looks for running applications again and rebuilds the list. */
  async refresh(): Promise<void> {
    if (this._looking) return;
    this._looking = true;
    this._status.text = "looking for running applications...";
    this._updateButtons();
    let targets: InspectableTarget[] = [];
    try {
      targets = await window.inspector.listTargets();
    } catch (e) {
      this._status.text = `could not look for applications: ${e instanceof Error ? e.message : String(e)}`;
      this._looking = false;
      this._updateButtons();
      return;
    }
    this._looking = false;
    // Keep the selection on the same application across a refresh, so pressing Refresh and then
    // Attach cannot attach to something else that appeared in the meantime.
    const wasOn = this._targets[this._selected];
    this._targets = targets;
    this._selected = wasOn ? targets.findIndex((t) => t.port === wasOn.port && t.pid === wasOn.pid) : -1;
    if (this._selected < 0 && targets.length === 1) this._selected = 0;
    this._build();
  }

  private _build(): void {
    this._list.html = "";
    for (const [index, target] of this._targets.entries()) {
      const row = new Div(this._list, { class: "attach-dialog-item" });
      if (index === this._selected) row.classList.add("selected");
      const text = new Div(row, { class: "attach-dialog-item-text" });
      new Span(text, { text: targetDisplayName(target), class: "attach-dialog-item-name" });
      new Span(text, { text: this._describe(target), class: "attach-dialog-item-detail" });
      if (target.busy) new Span(row, { text: "in use", class: "attach-dialog-item-busy" });
      row.element.onclick = () => {
        this._selected = index;
        this._build();
      };
      row.element.ondblclick = () => this._attachToPort(target.port);
    }
    this._status.text = this._targets.length
      ? `${this._targets.length} running application${this._targets.length === 1 ? "" : "s"}`
      : "No application is running with a capture library in it. Start one with Launch..., or with the "
        + "layer enabled by hand, then press Refresh.";
    this._updateButtons();
  }

  /** The second line of a row: what it is, and what taking it would cost. */
  private _describe(t: InspectableTarget): string {
    // A capture library too old to answer the probe gave nothing away but its port.
    if (!t.api) return `port ${t.port} — a capture library too old to say what it is`;
    const parts = [t.api, `pid ${t.pid}`, `port ${t.port}`];
    if (t.busy) parts.push("an inspector is attached: attaching here takes the connection from it");
    return parts.join(" · ");
  }

  private _updateButtons(): void {
    this._refreshButton.disabled = this._looking;
    this._attachButton.disabled = this._looking || !this._targets[this._selected];
  }

  private _attachToPort(port: number): void {
    this.close();
    this._onAttach(port);
  }
}
