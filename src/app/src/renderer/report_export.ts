// Exporting a report (Frame Stats, the flame graph, the render graph...) as a standalone HTML
// file: the report's DOM exactly as it is on screen, with the application's stylesheets inlined,
// so it can be filed with a bug, attached to a review or read on a machine with no GPU Inspector.
// The file is a snapshot and says so: nothing in it can be clicked, measured or reopened.
import { currentTheme } from "./theme.js";

/** The application's stylesheets, read from the main process once per window. */
let appStyles: Promise<string> | null = null;

export interface ReportExport {
  /** The report's name ("Frame Stats"), which becomes the page title and its heading. */
  title: string;
  /** What it is a report of: the capture's tab label, and the session or file it came from. */
  subtitle?: string;
  /** The report's root element. It is cloned, so the live report is left untouched. */
  element: HTMLElement;
  /** File name offered in the save dialog, without the extension. */
  fileName?: string;
  /** Skips the dialog and writes here instead (testing aid, as saveFile's `path` is). */
  path?: string;
}

/**
 * Writes a report to an HTML file the user picks. Returns the path written, or null if the dialog
 * was cancelled or the file could not be written.
 */
export async function exportReportHtml(report: ReportExport): Promise<string | null> {
  const html = await reportHtml(report);
  const name = `${slug(report.fileName ?? report.title)}.html`;
  return window.inspector.saveFile({
    title: `Export ${report.title}`, defaultPath: name,
    filters: [{ name: "HTML", extensions: ["html"] }, { name: "All files", extensions: ["*"] }],
    ...(report.path ? { path: report.path } : {}),
  }, new TextEncoder().encode(html));
}

/** The whole page a report is exported as; exported separately so tests can read it without a dialog. */
export async function reportHtml(report: ReportExport): Promise<string> {
  if (!appStyles) appStyles = window.inspector.appStyles().catch(() => "");
  const css = await appStyles;
  const body = exportBody(report.element);
  const when = new Date().toLocaleString();
  const subtitle = report.subtitle ? `${escapeHtml(report.subtitle)} · ` : "";
  return `<!DOCTYPE html>
<html lang="en" data-theme="${currentTheme()}">
<head>
<meta charset="utf-8">
<title>${escapeHtml(report.title)}</title>
<style>
${css}
</style>
<style>
${EXPORT_CSS}
</style>
</head>
<body class="report-export-page">
<h1 class="report-export-title">${escapeHtml(report.title)}</h1>
<div class="report-export-note">${subtitle}exported from GPU Inspector on ${escapeHtml(when)}</div>
<div class="report-export-body">
${body.outerHTML}
</div>
</body>
</html>
`;
}

/**
 * The report's DOM prepared to stand on its own: canvases become the images they were showing,
 * collapsed sections are written out open (nothing in the file can open them), and the controls
 * are disabled so the page does not pretend to still measure anything.
 */
function exportBody(element: HTMLElement): HTMLElement {
  const clone = element.cloneNode(true) as HTMLElement;

  // A canvas clones as an empty one: its pixels are not part of the DOM.
  const canvases = element.querySelectorAll("canvas");
  const clonedCanvases = clone.querySelectorAll("canvas");
  for (let i = 0; i < clonedCanvases.length && i < canvases.length; i++) {
    const source = canvases[i];
    const copy = clonedCanvases[i];
    const img = document.createElement("img");
    try {
      img.src = source.toDataURL("image/png");
    } catch {
      continue;   // a canvas drawn from a file:// image is tainted and cannot be read back
    }
    img.className = copy.className;
    img.style.cssText = copy.style.cssText;
    img.width = source.width;
    img.height = source.height;
    copy.replaceWith(img);
  }

  // Form state lives in properties, which do not clone into the markup.
  const inputs = element.querySelectorAll("input, select, textarea");
  const clonedInputs = clone.querySelectorAll("input, select, textarea");
  for (let i = 0; i < clonedInputs.length && i < inputs.length; i++) {
    const source = inputs[i] as HTMLInputElement;
    const copy = clonedInputs[i] as HTMLInputElement;
    if (source.tagName === "SELECT") {
      // A select's choice is the selected option, not a value attribute.
      const options = (copy as unknown as HTMLSelectElement).options;
      for (let o = 0; o < options.length; o++) {
        if (o === (source as unknown as HTMLSelectElement).selectedIndex) options[o].setAttribute("selected", "");
        else options[o].removeAttribute("selected");
      }
    } else if (source.type === "checkbox" || source.type === "radio") {
      if (source.checked) copy.setAttribute("checked", "");
      else copy.removeAttribute("checked");
    } else if (source.value !== undefined) copy.setAttribute("value", source.value);
  }
  for (const control of clone.querySelectorAll("input, select, textarea, button")) control.setAttribute("disabled", "");

  // Everything the report has is written out: a section left collapsed here could never be opened.
  for (const body of clone.querySelectorAll(".collapsible_body.collapsed")) body.classList.remove("collapsed");
  for (const button of clone.querySelectorAll(".collapse-button-closed")) {
    button.classList.remove("collapse-button-closed");
    button.classList.add("collapse-button-open");
    button.innerHTML = "&#9660;";
  }
  return clone;
}

/** The page is a document rather than a pane: it grows, and nothing inside it scrolls on its own. */
const EXPORT_CSS = `html, body { height: auto; overflow: visible; }
body.report-export-page { padding: 16px 22px 48px; box-sizing: border-box; }
.report-export-title { font-size: 15pt; font-weight: 600; margin: 0 0 3px 0; }
.report-export-note { color: var(--fg-secondary); font-size: 9pt; margin-bottom: 14px; }
.report-export-body > * { height: auto; max-height: none; overflow: visible; }
.report-export-body .btn, .report-export-body input, .report-export-body select { pointer-events: none; opacity: 0.75; }`;

function escapeHtml(text: string): string {
  return text.replace(/[&<>"]/g, (c) => (c === "&" ? "&amp;" : c === "<" ? "&lt;" : c === ">" ? "&gt;" : "&quot;"));
}

/** A file name from a report's name: "Shader Flame Graph: Frame 42" -> "shader_flame_graph_frame_42". */
function slug(text: string): string {
  return text.toLowerCase().replace(/[^a-z0-9]+/g, "_").replace(/^_+|_+$/g, "") || "report";
}
