// Stack traces from a connected capture library: the creation stacks of objects and the symbols of
// the addresses a capture's commands carry, requested once and cached in the object database. The
// UI (stacktrace_view.ts) adds host-side symbolization and renders them; saving a capture
// (capture_file.ts) stores them, in the app and in the MCP server's live sessions alike.
import type { ObjectDatabase } from "./vulkan/object_database.js";
import type { StackFrame, UiRequest } from "../shared/protocol.js";

/** A connection to a capture library, as far as requests go: its object database, whether it is up, and sending. */
export interface LayerSession {
  readonly database: ObjectDatabase;
  readonly connected: boolean;
  send(msg: UiRequest): Promise<boolean>;
}

const REQUEST_TIMEOUT_MS = 15000;

/** Creation stacks of objects, from the cache or the layer; null when the session cannot answer. */
export function requestStacks(session: LayerSession, ids: number[]): Promise<Map<number, StackFrame[]> | null> {
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
 * Symbolized frames for addresses (as "0x..." strings), from the cache or the layer.
 * `symbolizeOnHost` then resolves the frames the layer could only name by module and offset.
 */
export async function resolveSymbols(session: LayerSession, addresses: string[], symbolizeOnHost?: (frames: Map<string, StackFrame>) => Promise<void>): Promise<Map<string, StackFrame>> {
  const out = await resolveFromLayer(session, addresses);
  if (symbolizeOnHost) await symbolizeOnHost(out);
  return out;
}

function resolveFromLayer(session: LayerSession, addresses: string[]): Promise<Map<string, StackFrame>> {
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
