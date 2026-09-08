// Short forms of a validation message for lists and markers (the Inspect tab's list, the
// objects a message names, the commands of a capture).
import type { ValidationEntry } from "./vulkan/object_database.js";
import type { ValidationSeverity } from "../shared/protocol.js";

/** One line: the VUID and the readable part of the validation layer's first line. */
export function validationItemText(entry: ValidationEntry): string {
  // The validation layer's first line is "Validation Error: [ VUID ] Object 0: handle = ...;
  // | MessageID = ... | <what went wrong>": the last "|" segment is the readable part.
  let first = entry.message.split("\n")[0].replace(/^Validation (Error|Warning|Performance Warning): \[[^\]]*\]\s*/, "");
  const segments = first.split(" | ");
  if (segments.length > 1) first = segments[segments.length - 1].trim();
  const head = entry.idName ? `${entry.idName}: ` : "";
  const text = `${head}${first}`;
  return text.length > 140 ? `${text.slice(0, 140)}...` : text;
}

/** The marker glyph of a severity. */
export function severityMark(severity: ValidationSeverity): string {
  return severity === "error" ? "\u2716" : severity === "warning" ? "\u26a0" : "\u2139";
}

/** The worst severity among messages: "error" before "warning" before the rest. */
export function worstSeverity(entries: ValidationEntry[]): ValidationSeverity {
  if (entries.some((e) => e.severity === "error")) return "error";
  if (entries.some((e) => e.severity === "warning")) return "warning";
  return entries[0]?.severity ?? "info";
}
