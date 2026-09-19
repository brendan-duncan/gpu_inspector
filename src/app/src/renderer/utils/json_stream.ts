// Reading and writing a large JSON object without ever holding it as one string.
//
// V8 caps a string at about 512 MB (0x1fffffe8 characters), so a capture manifest past that size
// can neither be written (JSON.stringify throws RangeError) nor read (TextDecoder.decode throws)
// however much memory the machine has. That is a ceiling on the size of a capture the app can open
// at all, not a slowdown.
//
// The way past it is to scan the bytes structurally and hand JSON.parse / JSON.stringify one batch
// of array elements at a time. Batches are bounded in bytes rather than elements-per-call: those
// functions carry a fixed cost per call, so one call per command would add seconds to a
// million-command capture, while one call per batch makes that cost disappear.
//
// A structural scan is not free either — it is a byte loop against V8's native parser, and measures
// 15-38% of JSON.parse's own time — so both entry points keep today's single-call path for anything
// that comfortably fits, and only reach for batching when the alternative is failing outright.
//
// This is the format's own concern, with no DOM behind it, so the MCP server reads what the UI
// writes (src/app/build.mjs checks that the server bundles no UI module).

const TAB = 0x09, LF = 0x0a, CR = 0x0d, SPACE = 0x20;
const QUOTE = 0x22, COMMA = 0x2c, COLON = 0x3a, BACKSLASH = 0x5c;
const LBRACE = 0x7b, RBRACE = 0x7d, LBRACKET = 0x5b, RBRACKET = 0x5d;

/** Manifests smaller than this are parsed in one call, as they always have been. */
export const STREAM_THRESHOLD_BYTES = 256 * 1024 * 1024;
/** Source bytes per JSON.parse once a manifest is batched. */
export const BATCH_BYTES = 32 * 1024 * 1024;
/** Array elements per JSON.stringify once a manifest is batched. */
export const BATCH_ELEMENTS = 8192;
/** An array this long skips the single-call attempt: it would very likely throw, and batching it is faster anyway. */
export const MAX_DIRECT_ELEMENTS = 500000;
/** Small pieces are gathered up to this many characters before being encoded, to keep the chunk list short. */
const FLUSH_CHARS = 1 << 20;

const decoder = new TextDecoder();
const encoder = new TextEncoder();

export interface ParseOptions {
  /** Byte length at or above which the batched path is used (tests set 0 to force it). */
  threshold?: number;
  /** Source bytes per JSON.parse on the batched path. */
  batchBytes?: number;
}

/** Transforms one element of a member on its way out. */
export type ElementHook = (value: unknown) => unknown;

export interface StringifyOptions {
  /** Array length above which the single-call attempt is skipped (tests set 0 to force batching). */
  maxDirectElements?: number;
  /** Array elements per JSON.stringify on the batched path. */
  batchElements?: number;
  /**
   * Transforms each element of the named member on its way out, so a caller can drop a field
   * without first copying the whole array. On the batched path only one batch of transformed
   * elements is alive at a time.
   */
  element?: Record<string, ElementHook | undefined>;
}

function isWhitespace(c: number): boolean {
  return c === SPACE || c === TAB || c === LF || c === CR;
}

function skipWhitespace(b: Uint8Array, i: number, end: number): number {
  while (i < end && isWhitespace(b[i])) i++;
  return i;
}

/** `i` is at a string's opening quote; the index just past its closing quote. */
function skipString(b: Uint8Array, i: number, end: number): number {
  i++;
  while (i < end) {
    const c = b[i];
    if (c === BACKSLASH) { i += 2; continue; }   // whatever follows cannot end the string
    if (c === QUOTE) return i + 1;
    i++;
  }
  throw new Error("unterminated string");
}

/** `i` is at a value's first byte; the index just past the value. The value itself is not interpreted. */
function skipValue(b: Uint8Array, i: number, end: number): number {
  const c = b[i];
  if (c === QUOTE) return skipString(b, i, end);
  if (c === LBRACE || c === LBRACKET) {
    let depth = 0;
    while (i < end) {
      const d = b[i];
      if (d === QUOTE) { i = skipString(b, i, end); continue; }   // braces inside a string are not structure
      if (d === LBRACE || d === LBRACKET) { depth++; i++; continue; }
      if (d === RBRACE || d === RBRACKET) {
        depth--;
        i++;
        if (depth === 0) return i;
        continue;
      }
      i++;
    }
    throw new Error("unterminated object or array");
  }
  while (i < end) {                                              // a number, true, false or null
    const d = b[i];
    if (d === COMMA || d === RBRACE || d === RBRACKET || isWhitespace(d)) break;
    i++;
  }
  return i;
}

interface Span { start: number; end: number }
interface Member { key: Span; value: Span }

/** Each member of the object starting at `start`, as byte spans. */
function* members(b: Uint8Array, start: number, end: number): Generator<Member> {
  let i = skipWhitespace(b, start + 1, end);
  if (i >= end || b[i] === RBRACE) return;
  for (;;) {
    i = skipWhitespace(b, i, end);
    const keyStart = i;
    const keyEnd = skipString(b, i, end);
    i = skipWhitespace(b, keyEnd, end);
    if (b[i] !== COLON) throw new Error("expected ':' after an object key");
    i = skipWhitespace(b, i + 1, end);
    const valueStart = i;
    const valueEnd = skipValue(b, i, end);
    yield { key: { start: keyStart, end: keyEnd }, value: { start: valueStart, end: valueEnd } };
    i = skipWhitespace(b, valueEnd, end);
    if (i < end && b[i] === COMMA) { i++; continue; }
    return;
  }
}

/** Each element of the array starting at `start`, as byte spans. */
function* elements(b: Uint8Array, start: number, end: number): Generator<Span> {
  let i = skipWhitespace(b, start + 1, end);
  if (i >= end || b[i] === RBRACKET) return;
  for (;;) {
    i = skipWhitespace(b, i, end);
    const valueStart = i;
    const valueEnd = skipValue(b, i, end);
    yield { start: valueStart, end: valueEnd };
    i = skipWhitespace(b, valueEnd, end);
    if (i < end && b[i] === COMMA) { i++; continue; }
    return;
  }
}

/**
 * The array at `start`, parsed a batch at a time. Elements are contiguous in the source, so a
 * batch's own bytes are already "el,el,el" and only need brackets around them.
 */
function parseArrayBatched(b: Uint8Array, start: number, end: number, batchBytes: number): unknown[] {
  const out: unknown[] = [];
  let batchStart = -1;
  let batchEnd = -1;
  const flush = (): void => {
    if (batchStart < 0) return;
    for (const v of JSON.parse(`[${decoder.decode(b.subarray(batchStart, batchEnd))}]`) as unknown[]) out.push(v);
    batchStart = -1;
  };
  for (const e of elements(b, start, end)) {
    if (batchStart < 0) { batchStart = e.start; batchEnd = e.end; continue; }
    if (e.end - batchStart > batchBytes) { flush(); batchStart = e.start; }
    batchEnd = e.end;
  }
  flush();
  return out;
}

/** Parses the JSON object in `bytes[start, end)`, in batches when it is too large to decode whole. */
export function parseJsonObject<T>(bytes: Uint8Array, start: number, end: number, options: ParseOptions = {}): T {
  if (end - start < (options.threshold ?? STREAM_THRESHOLD_BYTES)) {
    return JSON.parse(decoder.decode(bytes.subarray(start, end))) as T;
  }
  const batchBytes = options.batchBytes ?? BATCH_BYTES;
  const out: Record<string, unknown> = {};
  for (const m of members(bytes, start, end)) {
    const key = JSON.parse(decoder.decode(bytes.subarray(m.key.start, m.key.end))) as string;
    out[key] = m.value.end - m.value.start > batchBytes && bytes[m.value.start] === LBRACKET
      ? parseArrayBatched(bytes, m.value.start, m.value.end, batchBytes)
      : JSON.parse(decoder.decode(bytes.subarray(m.value.start, m.value.end)));
  }
  return out as T;
}

/** `obj` with each `element` hook applied, or `obj` itself when no hook names a member it has. */
function applyElementHooks(obj: Record<string, unknown>, element: Record<string, ElementHook | undefined>): Record<string, unknown> {
  const keys = Object.keys(element).filter((k) => element[k] && Array.isArray(obj[k]));
  if (!keys.length) return obj;
  const out = { ...obj };
  for (const k of keys) out[k] = (obj[k] as unknown[]).map(element[k] as ElementHook);
  return out;
}

function stringifyBatched(obj: Record<string, unknown>, batch: number, element: Record<string, ElementHook | undefined>): Uint8Array[] {
  const batchElements = Math.max(1, Math.floor(batch));   // a batch of none would never finish an array
  const chunks: Uint8Array[] = [];
  let parts: string[] = [];
  let pending = 0;
  const flush = (): void => {
    if (!parts.length) return;
    chunks.push(encoder.encode(parts.join("")));
    parts = [];
    pending = 0;
  };
  const emit = (s: string): void => {
    parts.push(s);
    pending += s.length;
    if (pending >= FLUSH_CHARS) flush();
  };

  emit("{");
  let first = true;
  for (const key of Object.keys(obj)) {
    const value = obj[key];
    if (value === undefined || typeof value === "function" || typeof value === "symbol") continue;   // as JSON.stringify drops them
    if (!first) emit(",");
    first = false;
    emit(`${JSON.stringify(key)}:`);
    const hook = element[key];
    if (Array.isArray(value) && value.length > batchElements) {
      emit("[");
      for (let i = 0; i < value.length; i += batchElements) {
        const batch = (value as unknown[]).slice(i, i + batchElements);
        // A batch stringified whole is "[el,el,el]"; its brackets belong to the array around it.
        emit((i ? "," : "") + JSON.stringify(hook ? batch.map(hook) : batch).slice(1, -1));
        flush();
      }
      emit("]");
    } else {
      emit(JSON.stringify(hook && Array.isArray(value) ? (value as unknown[]).map(hook) : value));
    }
  }
  emit("}");
  flush();
  return chunks;
}

/**
 * The JSON of `obj` as byte chunks, identical to what JSON.stringify would produce. One chunk for
 * anything that fits in a string, several once it does not.
 */
export function stringifyJsonObject(obj: Record<string, unknown>, options: StringifyOptions = {}): Uint8Array[] {
  const element = options.element ?? {};
  const maxDirect = options.maxDirectElements ?? MAX_DIRECT_ELEMENTS;
  const huge = Object.keys(obj).some((k) => Array.isArray(obj[k]) && (obj[k] as unknown[]).length > maxDirect);
  if (!huge) {
    try {
      return [encoder.encode(JSON.stringify(applyElementHooks(obj, element)))];
    } catch (e) {
      if (!(e instanceof RangeError)) throw e;   // only "Invalid string length" is ours to handle
    }
  }
  return stringifyBatched(obj, options.batchElements ?? BATCH_ELEMENTS, element);
}
