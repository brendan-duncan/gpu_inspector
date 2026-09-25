// "Tile-Based GPUs": the Reports menu's view of tile_analysis.ts, what a captured frame would cost
// a mobile GPU in memory traffic, and what the frame does that a tiled GPU cannot keep on chip.
import { tileReport, type TileReport } from "./tile_analysis.js";
import type { FrameAnalysisDatabase, FrameFinding } from "./vulkan/frame_analysis.js";
import { formatBytes } from "./utils/format.js";
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import type { Widget } from "./widget/widget.js";
import type { CaptureData } from "./capture_data.js";
import type { RenderGraph } from "./render_graph.js";

/** The rules of the other analyses that are about tile memory, which the report lists beside its own. */
export const TILE_RULES = new Set([
  "color-load", "color-store", "depth-store", "depth-transient", "memoryless-candidate", "msaa-store", "msaa-sampled",
  "clear-outside-pass", "clear-then-discard", "barrier-in-render-pass", "oversized-attachment", "stereo-without-multiview",
  "mergeable-passes", "subpass-candidate", "transient-candidate", "unread-store", "overwritten-before-read",
]);

/** The frame rate the traffic is quoted at: what a mobile title aims for, whatever the capture ran at. */
const TARGET_FPS = 60;

/** How each API spells the fixes the report points at. */
const API_NOTES: Record<string, string[]> = {
  vulkan: [
    "Load and store ops are the whole of it: LOAD_OP_CLEAR or DONT_CARE for what the pass replaces, STORE_OP_DONT_CARE (or NONE) for what nothing reads after it.",
    "Passes that feed each other pixel for pixel belong in one VkRenderPass as subpasses, the later ones reading the earlier results as input attachments (subpassLoad); with dynamic rendering, VK_KHR_dynamic_rendering_local_read does the same.",
    "An attachment that lives only inside a render pass wants VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT and LAZILY_ALLOCATED memory: it then never has memory at all.",
    "Multisampled attachments should resolve in the pass (pResolveAttachments, STORE_OP_DONT_CARE on the multisampled one), never be stored.",
  ],
  d3d12: [
    "OMSetRenderTargets has no load or store actions, so a tiled GPU has to assume both. BeginRenderPass says them: DISCARD or CLEAR beginning access, DISCARD ending access for what nothing reads after.",
    "Direct3D 12 has no subpasses; a post-processing step that reads each pixel once is cheapest drawn in the same render pass where the design allows it.",
    "A render pass resolve (D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE) keeps a multisampled target on chip where ResolveSubresource needs it in memory.",
  ],
  metal: [
    "loadAction and storeAction are the whole of it: MTLLoadActionClear or DontCare, MTLStoreActionDontCare for what nothing reads after.",
    "An attachment used only inside one render pass can be MTLStorageModeMemoryless: no memory at all.",
    "A later draw can read an earlier result at its own pixel with programmable blending ([[color(n)]]), or a tile shader can run between draws in the same pass: neither leaves the tile.",
    "Multisampled attachments resolve in the pass with MTLStoreActionMultisampleResolve.",
  ],
};

const mb = (bytes: number): string => formatBytes(bytes);
const rate = (bytes: number): string => `${((bytes * TARGET_FPS) / 1e9).toFixed(2)} GB/s`;

export function renderTileReport(container: Widget, data: CaptureData, db: FrameAnalysisDatabase, graph: RenderGraph,
                                 findings: FrameFinding[], onJump: (commandIndex: number) => void): TileReport {
  const report = tileReport(data, db, graph);
  const root = new Div(container, { class: "frame-stats tile-report" });
  new Div(root, { text: "Tile-Based GPUs", class: "frame-stats-title" });
  new Div(root, {
    text: "What this frame would cost a tiled GPU (the mobile ones: Mali, Adreno, Apple, PowerVR) in memory traffic: each render pass runs in on-chip tile memory, and what it loads into the tile, stores out of it, or reads and writes in memory around it is what it pays for. Sizes are the attachments' own; a GPU that compresses its framebuffers moves less.",
    class: "text-muted tile-intro",
  });
  if (!report.renderPasses) {
    new Div(root, { text: "No render passes in this capture.", class: "text-muted" });
    return report;
  }

  // ---- The frame in one card.
  const summary = new Div(root, { class: "frame-stats-section" });
  const traffic = report.loadBytes + report.storeBytes;
  const share = traffic ? report.avoidableBytes / traffic : 0;
  const verdict = !traffic ? "The render passes load and store nothing the capture can size."
    : share > 0.5 ? `Most of the attachment traffic (${Math.round(share * 100)}%) is avoidable: the frame stores and reloads what it could keep in tile memory.`
    : share >= 0.15 ? `${Math.round(share * 100)}% of the attachment traffic is avoidable.`
    : "The attachment traffic is close to what the frame needs.";
  new Div(summary, { text: verdict, class: "bottleneck-verdict" });
  const facts = new Div(summary, { class: "frame-stats-list" });
  const fact = (label: string, value: string, tooltip?: string): void => {
    const line = new Div(facts, { class: "frame-stats-row" });
    const l = new Div(line, { text: label, class: "frame-stats-label" });
    if (tooltip) l.tooltip = tooltip;
    new Div(line, { text: value, class: "frame-stats-value" });
  };
  fact("Render passes", String(report.renderPasses));
  fact("Loaded into tile memory", `${mb(report.loadBytes)} per frame`, "Attachments whose contents a pass keeps (not cleared, not discarded), read from memory when it starts.");
  fact("Stored from tile memory", `${mb(report.storeBytes)} per frame`, "Attachments a pass does not throw away, written to memory when it ends.");
  fact("Avoidable", `${mb(report.avoidableBytes)} per frame`, "Stores replaced before anything reads them, depth nothing reads, and results stored for the next pass alone or loaded straight back by it.");
  if (report.unreadBytes) {
    fact("Stored, read by nothing captured", `${mb(report.unreadBytes)} per frame`,
      "Color results nothing later in the capture reads: the frame's output to something the capture does not see (an XR compositor, the next frame, the host), or stores that could be dropped. Not counted as avoidable.");
  }
  fact(`At ${TARGET_FPS} frames a second`, `${rate(traffic)} of attachment traffic, ${rate(report.avoidableBytes)} of it avoidable`,
    "A mobile GPU's whole memory bandwidth is some tens of GB/s, shared with the CPU and the display; attachment traffic is on top of texture and vertex reads.");
  if (data.api === "vulkan") {
    const withSubpasses = report.subpassPasses.length;
    const inputs = report.passes.reduce((s, p) => s + (p.subpasses?.inputAttachments ?? 0), 0);
    fact("Subpasses", withSubpasses ? `${withSubpasses} of ${report.renderPasses} render passes have more than one, with ${inputs} input attachment reference${inputs === 1 ? "" : "s"}` : "none: every render pass has a single subpass");
  }
  fact("Out of the tile", report.outOfTile.length ? `${report.outOfTile.length} place${report.outOfTile.length === 1 ? "" : "s"} (below)` : "nothing found");

  // ---- Render passes.
  const passes = new Div(root, { class: "frame-stats-section" });
  new Div(passes, { text: "Render passes", class: "frame-stats-heading" });
  const table = new Div(passes, { class: "tile-table" });
  for (const head of ["Pass", "Loads", "Stores", "Avoidable", "Attachments"]) new Div(table, { text: head, class: "tile-head" });
  for (const p of report.passes) {
    const name = new Div(table, { text: p.node.label, class: "tile-cell tile-link" });
    name.element.onclick = () => onJump(p.node.commandIndex);
    new Div(table, { text: p.loadBytes ? mb(p.loadBytes) : "—", class: "tile-cell" });
    new Div(table, { text: p.storeBytes ? mb(p.storeBytes) : "—", class: "tile-cell" });
    new Div(table, { text: p.avoidableBytes ? mb(p.avoidableBytes) : "—", class: `tile-cell${p.avoidableBytes ? " tile-bad" : ""}` });
    const cell = new Div(table, { class: "tile-cell tile-attachments" });
    for (const a of p.attachments) {
      const line = new Div(cell, { class: "tile-attachment" });
      const ops = a.loads && a.stores ? "loaded and stored" : a.loads ? "loaded" : a.stores ? "stored" : "kept in the tile";
      new Div(line, { text: `${a.label} · ${mb(a.bytes)} · ${ops}` });
      // Why, on a line of its own under the attachment.
      if (a.avoidable) new Div(line, { text: a.avoidable, class: "tile-bad tile-why" });
      else if (a.unreadBytes) new Div(line, { text: "stored, and nothing in the capture reads it (the frame's output?)", class: "text-muted tile-why" });
      else if (a.note) new Div(line, { text: a.note, class: "text-muted tile-why" });
    }
    if (p.subpasses && p.subpasses.subpasses > 1) new Div(cell, { text: `${p.subpasses.subpasses} subpasses, ${p.subpasses.inputAttachments} input attachment reference${p.subpasses.inputAttachments === 1 ? "" : "s"}`, class: "tile-good" });
  }

  // ---- Post-processing.
  if (report.post.length) {
    const section = new Div(root, { class: "frame-stats-section" });
    new Div(section, { text: "Post-processing", class: "frame-stats-heading" });
    new Div(section, {
      text: "Render passes that read, as a texture, what an earlier pass rendered at their own size. A step that reads each pixel at its own position could take it from tile memory — a subpass input, framebuffer fetch, or drawn in the same pass — where one that filters neighbouring pixels (a blur, bloom, anti-aliasing) has to read it from memory.",
      class: "text-muted",
    });
    for (const step of report.post) {
      const line = new Div(section, { class: "tile-line" });
      const link = new Span(line, { text: step.node.label, class: "tile-link" });
      link.element.onclick = () => onJump(step.node.commandIndex);
      const how = step.kind === "same-pixel"
        ? (step.adjacent ? "reads it once per pixel, right after it is rendered: it could stay in tile memory" : "reads it once per pixel, but other passes run in between")
        : step.kind === "filters" ? "filters it (several reads, or a loop): it has to come from memory"
        : "how its shader reads it is not known (only Vulkan shaders are analysed for this)";
      new Span(line, { text: ` reads ${step.input} from ${step.producer.label}: ${how}`, class: step.kind === "same-pixel" && step.adjacent ? "tile-bad" : "" });
    }
  }

  // ---- What leaves the tile.
  const out = new Div(root, { class: "frame-stats-section" });
  new Div(out, { text: "Out of the tile", class: "frame-stats-heading" });
  if (!report.outOfTile.length) {
    new Div(out, { text: "No draw writes memory through a shader, samples its own render target, or is split from the next pass by compute or a transfer.", class: "text-muted" });
  }
  for (const o of report.outOfTile) {
    const line = new Div(out, { class: "tile-line" });
    const link = new Span(line, { text: o.node.label, class: "tile-link" });
    link.element.onclick = () => onJump(o.node.commandIndex);
    new Span(line, { text: `: ${o.message}.` });
  }

  // ---- The other analyses' tile rules.
  const related = findings.filter((f) => TILE_RULES.has(f.rule));
  const rel = new Div(root, { class: "frame-stats-section" });
  new Div(rel, { text: "Frame issues about tile memory", class: "frame-stats-heading" });
  if (!related.length) new Div(rel, { text: "None of the frame's issues are about tile memory.", class: "text-muted" });
  for (const f of related) {
    const row = new Div(rel, { class: `perf-finding perf-row-${f.severity}` });
    const head = new Div(row, { class: "perf-finding-head" });
    new Span(head, { text: f.severity.toUpperCase(), class: `perf-badge perf-${f.severity}` });
    const rule = new Span(head, { text: f.rule, class: "perf-rule tile-link" });
    if (f.commandIndex !== undefined) rule.element.onclick = () => onJump(f.commandIndex!);
    if (f.count > 1) new Span(head, { text: `×${f.count}`, class: "perf-count text-muted" });
    new Div(row, { text: f.message, class: "perf-msg" });
  }

  // ---- How the API spells the fixes.
  const notes = API_NOTES[data.api];
  if (notes) {
    const section = new Div(root, { class: "frame-stats-section" });
    new Div(section, { text: "In this API", class: "frame-stats-heading" });
    for (const n of notes) new Div(section, { text: `• ${n}`, class: "tile-line" });
  }
  return report;
}
