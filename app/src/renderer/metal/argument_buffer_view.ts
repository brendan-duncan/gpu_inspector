// An argument buffer's resolved members in a captured draw's stage buffer section
// (argument_buffer.ts reads them).
import { Div } from "../widget/div.js";
import { Span } from "../widget/span.js";
import { Widget } from "../widget/widget.js";
import { objectLink } from "../args_view.js";
import type { VulkanObject } from "../vulkan/vulkan_object.js";
import type { ArgumentEntry } from "./argument_buffer.js";

/** The resolved members as a list: name, kind, and the object (with its offset for a pointer) or the raw value. */
export function renderArgumentBuffer(container: Widget, entries: ArgumentEntry[], onLink: (o: VulkanObject) => void): void {
  const box = new Div(container, { class: "argument-buffer" });
  new Div(box, { text: `Argument buffer: ${entries.length} resource${entries.length === 1 ? "" : "s"}`, class: "text-muted font-sm" });
  const ul = new Widget("ul", box, { class: "dependency-list" });
  for (const e of entries) {
    const li = new Widget("li", ul);
    new Span(li, { text: `${e.path}  `, class: "buffer-member" });
    new Span(li, { text: `${e.typeName}  `, class: "args-enum" });
    if (e.value === null) {
      new Span(li, { text: "(past the captured range)", class: "text-muted" });
    } else if (e.object) {
      objectLink(li, e.object, onLink);
      if (e.objectOffset) new Span(li, { text: `  +${e.objectOffset}`, class: "text-muted" });
    } else if (e.value === "0x0") {
      new Span(li, { text: "(null)", class: "text-muted" });
    } else {
      new Span(li, { text: `${e.value}  (no tracked ${e.kind === "pointer" ? "buffer holds this address" : "object has this id"})`, class: "text-muted" });
    }
  }
}
