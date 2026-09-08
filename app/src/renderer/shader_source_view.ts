// The embedded source of a SPIR-V module (OpSource / NonSemantic DebugInfo), rendered with line
// numbers and syntax highlighting. Shared by the Inspect tab's Source view (which adds the jump
// to the disassembly) and the captured draw's shader sections (which add the analysis findings).
import { Button } from "./widget/button.js";
import { Div } from "./widget/div.js";
import { Widget } from "./widget/widget.js";
import { escapeHtml, highlightLines } from "./code_editor.js";
import { describeDebugInfo, hasEmbeddedSource, parseSpirvDebugInfo, sourceLanguageOf, sourceLineMap, type SpirvDebugInfo } from "./vulkan/spirv_debug.js";

export interface SourceLinesOptions {
  /** The line to highlight and scroll to. */
  activeLine?: number;
  /** Marks the lines instructions map to as clickable, and gets their clicks. */
  onMappedLine?: (file: number, line: number) => void;
}

/** Fills `pre` with one file of the embedded source: line numbers, highlighting, mapped lines. */
export function renderSourceLines(pre: Widget, info: SpirvDebugInfo, fileIndex: number, opts: SourceLinesOptions = {}): void {
  const file = info.files[fileIndex];
  if (!file || file.text === null) {
    pre.text = "No embedded source.";
    return;
  }
  const language = sourceLanguageOf(info);
  const map = sourceLineMap(file.text);
  const lines = language ? highlightLines(file.text, language) : file.text.split("\n").map(escapeHtml);
  while (lines.length > map.lines.length) lines.pop();
  const mapped = new Set<number>();
  if (opts.onMappedLine) for (const loc of info.locations) if (loc && loc.file === fileIndex) mapped.add(loc.line);
  const width = String(Math.max(...map.lineOf, 1)).length;
  let html = "";
  for (let i = 0; i < lines.length; i++) {
    const n = map.lineOf[i];
    const isMapped = n > 0 && mapped.has(n);
    const cls = `code-line${isMapped ? " code-line-mapped" : ""}${n > 0 && n === opts.activeLine ? " code-line-active" : ""}${n ? "" : " code-line-unnumbered"}`;
    const title = isMapped ? ' title="Show the SPIR-V instructions of this line"' : "";
    html += `<span class="${cls}" data-line="${n}"${title}><span class="code-lineno">${(n ? String(n) : "").padStart(width)}</span>${lines[i]}</span>\n`;
  }
  pre.html = html;
  pre.element.onclick = opts.onMappedLine
    ? (e) => {
      const el = (e.target as HTMLElement).closest(".code-line-mapped") as HTMLElement | null;
      if (el) opts.onMappedLine!(fileIndex, Number(el.dataset.line));
    }
    : null;
  if (opts.activeLine) pre.element.querySelector(".code-line-active")?.scrollIntoView({ block: "center" });
}

/** A source view over a whole module: the summary line, a file bar for includes, and the text. */
export interface EmbeddedSourceView {
  info: SpirvDebugInfo | null;
  /** Shows a file and highlights a line (no-op without embedded source). */
  show(file: number, line?: number): void;
}

export function renderEmbeddedSource(container: Widget, data: Uint8Array): EmbeddedSourceView {
  const info = parseSpirvDebugInfo(data);
  const summary = new Div(container, { class: "shader-debug-summary text-muted font-sm" });
  if (!info || !hasEmbeddedSource(info)) {
    summary.text = `${info ? describeDebugInfo(info) : "No debug information"}. To embed the source, compile with -g (glslc, glslangValidator), -gVS (glslangValidator, NonSemantic form) or -fspv-debug=vulkan-with-source (dxc).`;
    return { info, show: () => undefined };
  }
  summary.text = describeDebugInfo(info);
  let current = info.mainFile >= 0 && info.files[info.mainFile]?.text !== null ? info.mainFile : info.files.findIndex((f) => f.text !== null);
  const withText = info.files.filter((f) => f.text !== null);
  let fileBar: Div | null = null;
  const pre = new Widget("pre", null, { class: "shader-text" });
  const show = (file: number, line = 0): void => {
    if (info.files[file]?.text === null) return;
    current = file;
    if (fileBar) for (const b of Array.from(fileBar.element.querySelectorAll("button"))) b.classList.toggle("active", b.dataset.file === String(file));
    renderSourceLines(pre, info, file, { activeLine: line });
  };
  if (withText.length > 1) {
    fileBar = new Div(container, { class: "shader-toolbar shader-file-bar" });
    info.files.forEach((f, i) => {
      if (f.text === null) return;
      const b = new Button(fileBar, { label: f.name, class: "btn btn-sm", tooltip: `${f.text.split("\n").length} lines`, callback: () => show(i) });
      b.element.dataset.file = String(i);
    });
  }
  container.appendChild(pre);
  show(current);
  return { info, show };
}
