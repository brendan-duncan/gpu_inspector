// Stack traces: where an object was created (the launch dialog's "Stack traces" option) and
// where a captured command was recorded (the capture bar's). The layer keeps raw addresses and
// symbolizes on request; the results are cached in the object database and saved with captures.
import { Div } from "./widget/div.js";
import { Span } from "./widget/span.js";
import { Widget } from "./widget/widget.js";
import type { SessionContext } from "./session_panel.js";
import type { StackFrame } from "../shared/protocol.js";

const REQUEST_TIMEOUT_MS = 15000;

/** Creation stacks of objects, from the cache or the layer; null when the session cannot answer. */
export function requestStacks(session: SessionContext, ids: number[]): Promise<Map<number, StackFrame[]> | null> {
  const db = session.database;
  const out = new Map<number, StackFrame[]>();
  const missing: number[] = [];
  for (const id of ids) {
    const cached = db.stacks.get(id);
    if (cached) out.set(id, cached); else missing.push(id);
  }
  if (!missing.length || db.stacksAvailable === false) return Promise.resolve(out);
  return new Promise((resolve) => {
    let done = false;
    const finish = (ok: boolean): void => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      db.onStacktraces.disconnect(listener);
      if (!ok) { resolve(out.size ? out : null); return; }
      for (const id of missing) {
        const s = db.stacks.get(id);
        if (s) out.set(id, s);
      }
      resolve(out);
    };
    const listener = (): void => finish(true);
    const timer = setTimeout(() => finish(false), REQUEST_TIMEOUT_MS);
    db.onStacktraces.addListener(listener);
    void session.send({ action: "RequestStacktraces", ids: missing }).then((ok) => { if (!ok) finish(false); });
  });
}

/**
 * Frames the layer named by module and offset only (dladdr on Android and Linux knows exported
 * symbols), resolved on this machine from the unstripped libraries (see main/symbolize.ts). The
 * results replace the cached frames, so a capture file saves them.
 */
async function symbolizeOnHost(session: SessionContext, frames: Map<string, StackFrame>): Promise<void> {
  const wanted = [...frames.values()].filter((f) => f.module && f.offset > 0 && !f.file && !f.internal && !f.hostResolved);
  if (!wanted.length) return;
  for (const f of wanted) f.hostResolved = true;   // one attempt per frame
  let resolved: StackFrame[] = [];
  try {
    resolved = await window.inspector.symbolize(wanted, session.symbolDirs);
  } catch {
    return;
  }
  for (const f of resolved) {
    const merged = { ...frames.get(f.address), ...f, hostResolved: true };
    frames.set(f.address, merged);
    session.database.symbols.set(f.address, merged);
  }
}

/** Symbolized frames for addresses (as "0x..." strings), from the cache or the layer, then the host's symbolizer. */
export async function resolveSymbols(session: SessionContext, addresses: string[]): Promise<Map<string, StackFrame>> {
  const out = await resolveFromLayer(session, addresses);
  await symbolizeOnHost(session, out);
  return out;
}

function resolveFromLayer(session: SessionContext, addresses: string[]): Promise<Map<string, StackFrame>> {
  const db = session.database;
  const out = new Map<string, StackFrame>();
  const missing: string[] = [];
  for (const a of addresses) {
    const cached = db.symbols.get(a);
    if (cached) out.set(a, cached); else if (!missing.includes(a)) missing.push(a);
  }
  if (!missing.length) return Promise.resolve(out);
  return new Promise((resolve) => {
    let done = false;
    const finish = (): void => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      db.onSymbols.disconnect(listener);
      for (const a of missing) {
        const f = db.symbols.get(a);
        if (f) out.set(a, f);
      }
      resolve(out);
    };
    const listener = (): void => finish();
    const timer = setTimeout(finish, REQUEST_TIMEOUT_MS);
    db.onSymbols.addListener(listener);
    void session.send({ action: "RequestSymbols", addresses: missing }).then((ok) => { if (!ok) finish(); });
  });
}

function frameText(f: StackFrame): string {
  if (f.function) return f.function;
  if (f.module) return `${f.module}+0x${f.offset.toString(16)}`;
  return f.address;
}

/**
 * Renders frames innermost first. Frames inside the Vulkan loader and layers are hidden behind
 * a toggle: the interesting frame is the application's call into Vulkan and what led to it.
 */
export function renderStackFrames(container: Widget, frames: StackFrame[]): void {
  const internal = frames.filter((f) => f.internal).length;
  const list = new Div(container, { class: "stack-frames" });
  let showInternal = false;
  const draw = (): void => {
    list.html = "";
    let index = 0;
    for (const f of frames) {
      if (f.internal && !showInternal) continue;
      const row = new Div(list, { class: `stack-frame${f.internal ? " stack-frame-internal" : ""}` });
      new Span(row, { text: `${index++}`, class: "stack-frame-index" });
      const text = new Span(row, { text: frameText(f), class: "stack-frame-function" });
      text.tooltip = `${f.address}${f.module ? `  ${f.module}` : ""}${f.offset ? `+0x${f.offset.toString(16)}` : ""}`;
      if (f.file) new Span(row, { text: `${f.file}:${f.line}`, class: "stack-frame-location text-muted" });
      else if (f.function && f.module) new Span(row, { text: f.module, class: "stack-frame-location text-muted" });
    }
    if (!index) new Div(list, { text: "No frames.", class: "text-muted font-sm" });
  };
  draw();
  if (internal) {
    const toggle = new Div(container, { text: `Show ${internal} frame${internal === 1 ? "" : "s"} inside the Vulkan loader and layers`, class: "stack-frames-toggle dependency_link font-sm" });
    toggle.element.onclick = () => {
      showInternal = !showInternal;
      toggle.text = showInternal ? "Hide the frames inside the Vulkan loader and layers" : `Show ${internal} frame${internal === 1 ? "" : "s"} inside the Vulkan loader and layers`;
      draw();
    };
  }
}

/** Fills a container with an object's creation stack, requesting it from the layer or the file. */
export async function renderObjectStack(container: Widget, session: SessionContext, objectId: number): Promise<void> {
  container.html = "";
  const db = session.database;
  const note = (text: string): void => { new Div(container, { text, class: "text-muted font-sm" }); };
  if (db.stacksAvailable === false) {
    note(session.connected ? "Stack traces were not collected: enable \"Stack traces\" in the launch dialog." : "The capture file has no stack traces for its objects.");
    return;
  }
  note("loading...");
  const stacks = await requestStacks(session, [objectId]);
  container.html = "";
  const frames = stacks?.get(objectId);
  if (!stacks) { note(session.connected ? "No answer from the layer." : "Not available in a capture file."); return; }
  if ((db.stacksAvailable as boolean | null) === false) { note("Stack traces were not collected: enable \"Stack traces\" in the launch dialog."); return; }
  if (!frames || !frames.length) { note("No stack was recorded for this object."); return; }
  renderStackFrames(container, frames);
}

/** Fills a container with a captured command's recording stack from its raw addresses. */
export async function renderCommandStack(container: Widget, session: SessionContext, addresses: string[]): Promise<void> {
  container.html = "";
  new Div(container, { text: "resolving symbols...", class: "text-muted font-sm" });
  const symbols = await resolveSymbols(session, addresses);
  container.html = "";
  const frames: StackFrame[] = addresses.map((a) => symbols.get(a) ?? { address: a, offset: 0 });
  if (!symbols.size) new Div(container, { text: session.connected ? "The layer did not resolve the addresses." : "Symbols were not saved with the capture; the raw addresses follow.", class: "text-muted font-sm" });
  renderStackFrames(container, frames);
}
