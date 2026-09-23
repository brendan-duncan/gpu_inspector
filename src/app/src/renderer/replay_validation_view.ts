// The Validate report (renderer/replay_validation.ts): the replay's messages grouped by id, worst
// first, each fired-on command a click away.
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import type { Widget } from "./widget/widget.js";
import { Button } from "./widget/button.js";
import type { CaptureData } from "./capture_data.js";
import { replayValidationCounts, type ReplayValidation, type ReplayValidationMessage } from "./replay_validation.js";

/** The message with the spec's explanation cut off, for a row; the tooltip carries the whole of it. */
function shortMessage(m: ReplayValidationMessage): string {
  let text = m.message;
  // The layer's text: "Validation Error: [ VUID ] Object 0: handle = ..., ... | MessageID = ... | <what happened>. The Vulkan spec states: ...".
  const bar = text.lastIndexOf("| ");
  if (bar >= 0) text = text.slice(bar + 2);
  const spec = text.indexOf(" The Vulkan spec states:");
  if (spec > 0) text = text.slice(0, spec);
  return text.trim();
}

export interface ReplayValidationViewOptions {
  /** Selects a command in the capture's list. */
  selectCommand?: (index: number) => void;
  /** Runs the validation again with synchronization validation on (or off). */
  rerun?: (sync: boolean) => void;
  sync?: boolean;
}

/** Renders the report into `container`. */
export function renderReplayValidationReport(container: Widget, data: CaptureData, v: ReplayValidation, options: ReplayValidationViewOptions = {}): void {
  container.html = "";
  const { errors, warnings, linked } = replayValidationCounts(v);
  const verdict = !v.layer
    ? "The Khronos validation layer is not installed on this machine (it comes with the Vulkan SDK), so the replay ran without it and reports nothing."
    : !v.messages.length
      ? `The frame replayed under the validation layer${options.sync ? " with synchronization validation" : ""} on ${v.device || "this GPU"} with no errors and no warnings: as far as the layer can see, every call in it is legal.`
      : `${errors} error${errors === 1 ? "" : "s"} and ${warnings} warning${warnings === 1 ? "" : "s"} from the validation layer${options.sync ? " with synchronization validation" : ""}, replaying the frame on ${v.device || "this GPU"}; ${linked} of them fired on a captured command, which the rows open. Errors are real bugs. A message the replay's own setup or read-backs caused is possible where a problem is listed below.`;
  new Div(container, { text: verdict, class: "frame-bound-verdict" });

  if (options.rerun) {
    const row = new Div(container, { class: "draw-state-row" });
    new Button(row, { label: options.sync ? "Validate again without synchronization validation" : "Validate again with synchronization validation", class: "btn btn-sm",
      tooltip: "Synchronization validation finds hazards between commands (a write with no barrier before the read); it is slower, and the replay's own read-back barriers can resolve a hazard the application has.",
      callback: () => options.rerun?.(!options.sync) });
  }

  const rowOf = (label: string, value: string, tooltip?: string): Div => {
    const r = new Div(container, { class: "draw-state-row" });
    new Span(r, { text: label, class: "draw-state-label device-info-label" });
    const val = new Span(r, { text: value, class: "device-info-value" });
    if (tooltip) val.tooltip = tooltip;
    return r;
  };

  // Grouped by id, worst and most frequent first, each with the commands it fired on.
  const groups = new Map<string, ReplayValidationMessage[]>();
  for (const m of v.messages) {
    const key = `${m.severity}:${m.id || m.message.slice(0, 80)}`;
    const list = groups.get(key) ?? [];
    list.push(m);
    groups.set(key, list);
  }
  const ordered = [...groups.values()].sort((a, b) => {
    const sev = (a[0].severity === "error" ? 0 : 1) - (b[0].severity === "error" ? 0 : 1);
    if (sev) return sev;
    return b.reduce((n, m) => n + m.count, 0) - a.reduce((n, m) => n + m.count, 0);
  });
  if (ordered.length) new Div(container, { text: "Messages", class: "frame-stats-heading" });
  for (const group of ordered) {
    const first = group[0];
    const total = group.reduce((n, m) => n + m.count, 0);
    const head = new Div(container, { class: "draw-state-row" });
    new Span(head, { text: first.severity === "error" ? "✖" : "▲", class: `validation-sev validation-sev-${first.severity}` });
    new Span(head, { text: `${first.id || "(no id)"}  ×${total}`, class: "draw-state-label device-info-label" });
    const text = new Span(head, { text: shortMessage(first), class: "device-info-value" });
    text.tooltip = first.message;
    for (const m of group) {
      const where = m.command >= 0
        ? `command ${m.command}${data.commands[m.command] ? ` ${data.commands[m.command].method}` : ""}`
        : m.phase === "setup" ? "while creating the capture's objects" : m.phase === "submit" ? "at submission"
          : "between commands, in the replay's own work (uploads, read-backs): likely the replay's rather than the application's";
      const r = rowOf(m.count > 1 ? `×${m.count}` : "", where, m.message);
      if (m.command >= 0 && options.selectCommand) {
        r.element.classList.add("timing-hitch");
        r.element.title = "Select the command";
        r.element.onclick = () => options.selectCommand?.(m.command);
      }
    }
  }

  if (v.problems.length) {
    new Div(container, { text: `What the replay could not do (${v.problems.length})`, class: "frame-stats-heading" });
    for (const p of v.problems.slice(0, 20)) new Div(container, { text: p, class: "text-muted font-sm" });
    if (v.problems.length > 20) new Div(container, { text: `${v.problems.length - 20} more.`, class: "text-muted font-sm" });
  }
}
