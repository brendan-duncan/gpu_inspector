// A saved capture validated after the fact: the frame replayed under the Khronos validation layer
// (vkinsp_replay --validate --validate-data, src/replay/src/main.cpp), with each message tied to
// the captured command the replay was re-issuing when it fired.
//
// A live session only has validation messages when the application was launched with the layer
// on. A capture file arrives from a tester, an agent or another machine, and the question about it
// is whether the frame in it is legal; replaying the file needs neither the application nor the
// machine. The replay knows which command it is re-issuing, so the link from a message to a command
// is exact rather than matched from the message's text as the live path has to.
//
// Data only (no widgets): the MCP server's get_validation reads this too; the report is replay_validation_view.ts.

export interface ReplayValidationMessage {
  severity: "error" | "warning";
  /** VUID-..., or the layer's own message id name; "" when the layer gave none. */
  id: string;
  message: string;
  /** The captured command being re-issued when it fired, or -1 (the capture's objects being created). */
  command: number;
  /** "setup" (creating the capture's objects), "frame" (recording its command buffers), "submit". */
  phase: string;
  count: number;
}

export interface ReplayValidation {
  device: string;
  /** False when the validation layer was not installed: the replay ran, and reported nothing. */
  layer: boolean;
  messages: ReplayValidationMessage[];
  /** What the replay could not do; a message fired near one of these may be the replay's own. */
  problems: string[];
  /** Messages by the captured command they fired on. */
  byCommand: Map<number, ReplayValidationMessage[]>;
}

/** Parses the tool's --validate-data file. */
export function parseReplayValidation(bytes: Uint8Array): ReplayValidation {
  const doc = JSON.parse(new TextDecoder().decode(bytes)) as {
    format?: string; device?: string; layer?: boolean; problems?: string[];
    messages?: { severity?: string; id?: string; message?: string; command?: number; phase?: string; count?: number }[];
  };
  if (doc.format !== "gpu-inspector-validation") throw new Error("the replay wrote no validation data");
  const messages: ReplayValidationMessage[] = (doc.messages ?? []).map((m) => ({
    severity: m.severity === "error" ? "error" : "warning", id: m.id ?? "", message: m.message ?? "",
    command: typeof m.command === "number" ? m.command : -1, phase: m.phase ?? "", count: Math.max(1, m.count ?? 1),
  }));
  const byCommand = new Map<number, ReplayValidationMessage[]>();
  for (const m of messages) {
    if (m.command < 0) continue;
    const list = byCommand.get(m.command) ?? [];
    list.push(m);
    byCommand.set(m.command, list);
  }
  return { device: doc.device ?? "", layer: doc.layer !== false, messages, problems: doc.problems ?? [], byCommand };
}

/** Counts by severity. */
export function replayValidationCounts(v: ReplayValidation): { errors: number; warnings: number; linked: number } {
  let errors = 0, warnings = 0, linked = 0;
  for (const m of v.messages) {
    if (m.severity === "error") errors += m.count; else warnings += m.count;
    if (m.command >= 0) linked += m.count;
  }
  return { errors, warnings, linked };
}
