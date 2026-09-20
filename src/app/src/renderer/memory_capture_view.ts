// A memory capture's report (renderer/memory_capture.ts): how much was held over the run, what
// made during it is still held, what was made and thrown away again, and which frames allocated.
//
// The graph is the step line of bytes held, because an allocation is a step and drawing a slope
// between two of them would invent memory that was never held. The lists under it are the part a
// series cannot give: each survivor is an object, and the row opens it.
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import type { Widget } from "./widget/widget.js";
import { formatBytes } from "./utils/format.js";
import { summarizeMemoryCapture, TRANSIENT_FRAMES, type MemoryCapture, type MemoryCaptureSummary } from "./memory_capture.js";

/** What the report needs to know about an object to name it. */
export interface MemoryCaptureObject {
  id: number;
  name: string;
  type: string;
  isDeleted?: boolean;
  /** Objects whose arguments refer to this one: for an allocation, what is bound to it. */
  dependents?: Iterable<{ name: string; type: string }>;
}

export interface MemoryCaptureViewOptions {
  /** The inspector's object for an id, when it still knows it. */
  getObject?: (id: number) => MemoryCaptureObject | null;
  /** Opens an object in the Inspect panel. */
  onInspect?: (id: number) => void;
  /** Names for the heaps, by index; "Heap N" when absent. */
  heapNames?: string[];
}

/** How many of a list to show before saying how many more there are. */
const LIST_ROWS = 20;

function drawGraph(canvas: HTMLCanvasElement, s: MemoryCaptureSummary): void {
  const ctx = canvas.getContext("2d");
  if (!ctx || !s.series.length) return;
  const w = canvas.width, h = canvas.height;
  ctx.clearRect(0, 0, w, h);
  let low = Math.min(s.startBytes, ...s.series.map((p) => p.bytes));
  let high = Math.max(s.startBytes, ...s.series.map((p) => p.bytes));
  // A flat run would divide by zero; and a run that moves by a few kilobytes on top of gigabytes
  // should still read as flat, so the range is never less than a hundredth of what is held.
  const floor = Math.max(1, high * 0.01);
  if (high - low < floor) {
    const mid = (high + low) / 2;
    low = mid - floor / 2;
    high = mid + floor / 2;
  }
  const span = Math.max(1, s.lastFrame - s.firstFrame);
  const x = (frame: number): number => ((frame - s.firstFrame) / span) * (w - 1);
  const y = (bytes: number): number => h - 2 - ((bytes - low) / (high - low)) * (h - 4);

  ctx.strokeStyle = "#5f8ad0";
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  ctx.moveTo(0, y(s.startBytes));
  let last = s.startBytes;
  for (const p of s.series) {
    // A step: held until the frame that changed it, then the new amount.
    ctx.lineTo(x(p.frame), y(last));
    ctx.lineTo(x(p.frame), y(p.bytes));
    last = p.bytes;
  }
  ctx.lineTo(w - 1, y(last));
  ctx.stroke();

  // Where it began, to read the rest against.
  ctx.strokeStyle = "#808080";
  ctx.lineWidth = 1;
  ctx.setLineDash([4, 4]);
  ctx.beginPath();
  ctx.moveTo(0, y(s.startBytes) - 0.5);
  ctx.lineTo(w, y(s.startBytes) - 0.5);
  ctx.stroke();
  ctx.setLineDash([]);
}

/** Renders the report into `container`, replacing what was there. */
export function renderMemoryCaptureReport(container: Widget, capture: MemoryCapture, running: boolean,
                                          options: MemoryCaptureViewOptions = {}): void {
  container.html = "";
  const s = summarizeMemoryCapture(capture);
  if (!s) {
    new Div(container, {
      text: running
        ? "Nothing allocated or freed yet. An application that allocates everything up front and then runs has nothing to show here, which is the best answer there is."
        : "Nothing was allocated or freed while this ran.",
      class: "text-muted",
    });
    return;
  }
  new Div(container, { text: s.verdict, class: "frame-bound-verdict" });

  const canvas = document.createElement("canvas");
  canvas.className = "timing-graph memory-capture-graph";
  canvas.width = 900;
  canvas.height = 90;
  canvas.style.width = "100%";
  canvas.style.height = "90px";
  canvas.title = "Bytes held by the application, frame by frame; the dashed line is what it held when the capture began.";
  container.element.appendChild(canvas);
  drawGraph(canvas, s);

  const row = (label: string, value: string): Div => {
    const r = new Div(container, { class: "draw-state-row" });
    new Span(r, { text: label, class: "draw-state-label device-info-label" });
    new Span(r, { text: value, class: "device-info-value" });
    return r;
  };
  const signed = (n: number): string => `${n < 0 ? "−" : "+"}${formatBytes(Math.abs(n))}`;
  const heapName = (index: number): string => options.heapNames?.[index] ?? `Heap ${index}`;

  row("Frames", `${s.firstFrame}–${s.lastFrame} (${s.frames}), ${(s.durationMs / 1000).toFixed(1)} s`);
  row("Held", `${formatBytes(s.startBytes)} at the start, ${formatBytes(s.endBytes)} at the end (${signed(s.netBytes)}), ${formatBytes(s.peakBytes)} at the most`);
  row("Allocated", `${formatBytes(s.allocatedBytes)} in ${s.allocations} allocation${s.allocations === 1 ? "" : "s"}, ${(s.allocations / s.frames).toFixed(2)} a frame`);
  row("Freed", `${formatBytes(s.freedBytes)} in ${s.frees} free${s.frees === 1 ? "" : "s"}${s.freedOlder.count ? `, ${s.freedOlder.count} of them of allocations older than the capture (${formatBytes(s.freedOlder.bytes)})` : ""}`);
  if (s.transient.count) {
    row("Transient", `${s.transient.count} allocation${s.transient.count === 1 ? "" : "s"} (${formatBytes(s.transient.bytes)}) freed within ${TRANSIENT_FRAMES} frames of being made`);
  }
  for (const h of s.heaps) {
    if (s.heaps.length < 2) break;
    row(heapName(h.heap), `${signed(h.netBytes)}: ${formatBytes(h.allocatedBytes)} allocated, ${formatBytes(h.freedBytes)} freed`);
  }
  if (capture.dropped) {
    row("Not recorded", `${capture.dropped} events past the capture's limit: the totals above stop where the recording did`);
  }
  if (s.unnamed) {
    row("Unnamed", `${s.unnamed} allocation${s.unnamed === 1 ? "" : "s"} the capture library could not tie to an object, counted above and left out of the lists below`);
  }

  if (s.survivors.length) {
    new Div(container, { text: `Made here and still held (${s.survivors.length}, ${formatBytes(s.survivorBytes)})`, class: "frame-stats-heading" });
    for (const v of s.survivors.slice(0, LIST_ROWS)) {
      const o = options.getObject?.(v.id) ?? null;
      const bound = o?.dependents ? [...o.dependents].slice(0, 2).map((d) => d.name).join(", ") : "";
      const name = o ? `${o.name}${bound ? ` (${bound})` : ""}` : `object ${v.id}`;
      const r = row(formatBytes(v.bytes), `${name} — frame ${v.frame}, ${heapName(v.heap).toLowerCase()}`);
      if (options.onInspect) {
        r.element.classList.add("timing-hitch");
        r.element.title = "Open in Inspect";
        r.element.onclick = () => options.onInspect?.(v.id);
      }
    }
    if (s.survivors.length > LIST_ROWS) {
      new Div(container, { text: `${s.survivors.length - LIST_ROWS} more, largest first.`, class: "text-muted font-sm" });
    }
  }

  if (s.busiest.length) {
    new Div(container, { text: "Frames that allocated most", class: "frame-stats-heading" });
    for (const f of s.busiest.slice(0, 5)) {
      row(`Frame ${f.frame}`, `${formatBytes(f.bytes)} in ${f.allocations} allocation${f.allocations === 1 ? "" : "s"}`);
    }
  }
}

/** The start/stop button's label for the current state. */
export function memoryButtonLabel(running: boolean): string {
  return running ? "Stop Memory" : "Memory Capture";
}
