// Renders serialized Vulkan arguments as an expandable tree. {__id} references become links.
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import { Widget } from "./widget/widget.js";
import { fmt } from "./vulkan/vulkan_object.js";
import type { VulkanObject } from "./vulkan/vulkan_object.js";
import type { ObjectDatabase } from "./vulkan/object_database.js";
import type { ArgValue } from "../shared/protocol.js";

export type LinkHandler = (object: VulkanObject) => void;

export function objectLink(parent: Widget, obj: VulkanObject, onLink: LinkHandler, withSummary = false): Span {
  const label = (withSummary ? `${obj.type} ${obj.id}: ${obj.name}` : obj.name) + (obj.isDeleted ? " (destroyed)" : "");
  const link = new Span(parent, { text: label, class: obj.isDeleted ? "dependency_link dependency_ghost" : "dependency_link" });
  link.element.onclick = (e: MouseEvent) => {
    e.stopPropagation();
    onLink(obj);
  };
  return link;
}

function scalarText(v: ArgValue): string {
  return typeof v === "string" && !v.startsWith("VK_") ? JSON.stringify(v) : String(v);
}

export function renderArgs(parent: Widget, value: ArgValue | undefined, db: ObjectDatabase, onLink: LinkHandler, depth = 0, key: string | null = null): void {
  const row = new Div(parent, { class: "args-row" });
  if (key !== null) new Span(row, { text: `${key}: `, class: "args-key" });

  if (value === null || value === undefined) {
    new Span(row, { text: "null", class: "args-null" });
    return;
  }
  if (typeof value !== "object") {
    const cls = typeof value === "string" ? (value.startsWith("VK_") ? "args-enum" : "args-string") : "args-number";
    new Span(row, { text: scalarText(value), class: cls });
    return;
  }
  if (!Array.isArray(value)) {
    const rec = value as Record<string, ArgValue>;
    if (typeof rec.__id === "number") {
      const obj = db.getObject(rec.__id);
      if (obj) objectLink(row, obj, onLink);
      else new Span(row, { text: `${String(rec.__class)} ${rec.__id} (destroyed)`, class: "args-null" });
      return;
    }
    if (typeof rec.__handle === "string") {
      // Output parameters are serialized before the object is registered; resolve by handle.
      const obj = db.getObjectByHandle(String(rec.__class), rec.__handle);
      if (obj) objectLink(row, obj, onLink);
      else new Span(row, { text: `${String(rec.__class)} ${rec.__handle} (untracked)`, class: "args-null" });
      return;
    }
    if (typeof rec.__bytes === "number") {
      new Span(row, { text: `<${rec.__bytes} bytes>`, class: "args-null" });
      return;
    }
    if (rec.__truncated) {
      new Span(row, { text: `<array of ${String(rec.__count)}>`, class: "args-null" });
      return;
    }
  }

  const isArray = Array.isArray(value);
  const entries: [string, ArgValue][] = isArray
    ? value.map((v, i) => [String(i), v] as [string, ArgValue])
    : Object.entries(value);
  if (entries.length === 0) {
    new Span(row, { text: isArray ? "[]" : "{}", class: "args-null" });
    return;
  }
  // Short arrays of scalars are shown inline.
  if (isArray && entries.length <= 16 && entries.every(([, v]) => v === null || typeof v !== "object")) {
    new Span(row, { text: `[${value.map((v) => scalarText(v)).join(", ")}]`, class: "args-number" });
    return;
  }

  const details = document.createElement("details");
  details.open = depth < 2 || (isArray && entries.length <= 4);
  const summaryEl = document.createElement("summary");
  summaryEl.className = "args-summary";
  const sType = !isArray ? (value as Record<string, ArgValue>).sType : undefined;
  const typeName = typeof sType === "string" ? fmt(sType) : "";
  summaryEl.textContent = isArray ? `[${entries.length}]` : (typeName ? `{${entries.length}} ${typeName}` : `{${entries.length}}`);
  details.appendChild(summaryEl);
  const body = document.createElement("div");
  body.className = "args-children";
  details.appendChild(body);
  row.element.appendChild(details);
  const bodyWidget = new Widget(body);
  for (const [k, v] of entries) {
    if (k === "sType" && !isArray) continue;
    renderArgs(bodyWidget, v, db, onLink, depth + 1, k);
  }
}
