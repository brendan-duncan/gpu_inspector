// A backend's DetailSections drawn as collapsible groups: the command details of a plugin's API
// (capture_command_info.ts) and the Inspect pane's object details (inspect_panel.ts). How a value is
// drawn is the caller's, since a captured texture or buffer only means something with a capture open.
import type { DetailSection, DetailValue } from "./backend.js";
import { highlight } from "./code_editor.js";
import { collapsible } from "./widget/collapsible.js";
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import { Widget } from "./widget/widget.js";

/**
 * Draws `sections` into `container`. `renderValue` draws one row value or table cell into `parent`;
 * `contents` makes a box below the section's rows or table, for a viewer a value opens.
 */
export function renderDetailSections(container: Widget, sections: DetailSection[],
  renderValue: (parent: Widget, value: DetailValue, contents: () => Div) => void): void {
  for (const section of sections) {
    const grp = new collapsible(container, { label: section.title, collapsed: !!section.collapsed });
    const body = grp.body;
    // Buffer contents and image viewers open below the section's rows or table, not inside a cell.
    const contents = (): Div => new Div(body, { class: "plugin-detail-contents" });
    if (section.note) new Div(body, { text: section.note, class: "text-muted capture-note" });
    if (section.rows?.length) {
      const rows = new Div(body, { class: "draw-state" });
      for (const [label, value] of section.rows) {
        const row = new Div(rows, { class: "draw-state-row" });
        new Span(row, { text: label, class: "draw-state-label" });
        renderValue(row, value, contents);
      }
    }
    if (section.table) {
      const wrap = new Div(body, { class: "plugin-detail-table-wrap" });
      const table = new Widget("table", wrap, { class: "plugin-detail-table" });
      const head = new Widget("tr", new Widget("thead", table));
      for (const c of section.table.columns) new Widget("th", head, { text: c });
      const tbody = new Widget("tbody", table);
      for (const r of section.table.rows) {
        const tr = new Widget("tr", tbody);
        for (const cell of r) renderValue(new Widget("td", tr), cell, contents);
      }
    }
    if (section.code) {
      const pre = new Widget("pre", body, { class: "plugin-detail-code" });
      const language = section.code.language;
      if (language === "glsl" || language === "hlsl" || language === "msl") pre.element.innerHTML = highlight(section.code.text, language);
      else pre.element.textContent = section.code.text;
    }
  }
}
