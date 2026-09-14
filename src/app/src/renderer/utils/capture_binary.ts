// Ported from WebGPU Inspector (MIT): src/utils/capture_binary.js
// Binary capture container ("WGPUCAP"), the default save format for captures.
//
// Layout (offsets are absolute file offsets):
//
//   "WGPUCAP <containerVersion> <jsonByteLength>\n"   ASCII header line
//   <jsonByteLength bytes>                            metadata JSON, UTF-8
//   "\n" * k                                          padding so the binary
//                                                     section starts 8-aligned
//   <binary section>                                  raw payload bytes
//
// The header and metadata are plain text, so the front of the file is readable
// in a text editor; only the trailing payload bytes are binary. The metadata
// object is the same one the 1.x NDJSON format carried (schemaVersion,
// objects, commands, ...) plus one extra field:
//
//   payloadTable: Array<[byteOffset, byteLength] | null>
//
// indexed by payloadId, with byteOffset relative to the start of the binary
// section. Each payload is zero-padded to an 8-byte boundary so loaders can
// create TypedArray views (up to Float64/BigInt64) directly over the file
// bytes without copying. `{__payloadId}` references inside the metadata are
// unchanged from the NDJSON format.

/** One payloadTable entry: byte offset (relative to the binary section) and byte length. */
export type PayloadTableEntry = [byteOffset: number, byteLength: number];

/** Indexed by payloadId; `null` marks an id with no payload. */
export type PayloadTable = Array<PayloadTableEntry | null>;

/**
 * Capture metadata as carried by the NDJSON 1.x format (schemaVersion, objects,
 * commands, ...). Only the fields this codec touches are named; the rest are
 * passed through untouched.
 */
export interface CaptureMetadata {
  schemaVersion?: number;
  payloadTable?: PayloadTable;
  [key: string]: unknown;
}

/** A binary payload produced by buildCaptureStream. */
export interface CapturePayload {
  id: number;
  typedArray?: ArrayBufferView | null;
  bytes?: Uint8Array | null;
}

/** Input to encodeCaptureBinaryParts: the `{ metadata, payloads }` from buildCaptureStream. */
export interface CaptureStream {
  metadata: CaptureMetadata;
  payloads?: CapturePayload[] | null;
}

/** Output of encodeCaptureBinaryParts: ordered byte chunks plus their total length. */
export interface CaptureBinaryParts {
  parts: Uint8Array[];
  byteLength: number;
}

/** Output of decodeCaptureBinary: the metadata and zero-copy payload views keyed by payloadId. */
export interface DecodedCaptureBinary {
  metadata: CaptureMetadata;
  payloads: Map<number, Uint8Array>;
}

const MAGIC = "WGPUCAP";
const CONTAINER_VERSION = 1;
// Longest legal header line; a "file" whose first newline is beyond this is
// not a capture.
const MAX_HEADER_LENGTH = 64;

const _textEncoder = new TextEncoder();
const _textDecoder = new TextDecoder();

// Arithmetic (not bitwise) so offsets past 2^31 don't truncate.
function _align8(n: number): number {
  return Math.ceil(n / 8) * 8;
}

/**
 * True if `bytes` (a Uint8Array of at least the first 8 bytes of a file)
 * starts with the binary capture magic.
 */
export function isCaptureBinary(bytes: Uint8Array | null | undefined): bytes is Uint8Array {
  if (!bytes || bytes.length < MAGIC.length + 1) {
    return false;
  }
  for (let i = 0; i < MAGIC.length; ++i) {
    if (bytes[i] !== MAGIC.charCodeAt(i)) {
      return false;
    }
  }
  return bytes[MAGIC.length] === 0x20; // "WGPUCAP "
}

/**
 * Encode a capture stream (`{ metadata, payloads }` from buildCaptureStream,
 * where payloads is `[{ id, typedArray, bytes: Uint8Array }]`) into an ordered
 * list of byte chunks. Payload bytes are referenced, never copied, so this is
 * cheap even for multi-GB captures; hand the parts to `new Blob(parts)` or
 * write them sequentially to a stream.
 */
export function encodeCaptureBinaryParts(stream: CaptureStream): CaptureBinaryParts {
  const payloads = stream.payloads || [];
  const table: PayloadTable = [];
  let offset = 0;
  for (const p of payloads) {
    const len = p.bytes ? p.bytes.length : 0;
    while (table.length < p.id) {
      table.push(null);
    }
    table[p.id] = [offset, len];
    offset = _align8(offset + len);
  }

  const metadata: CaptureMetadata = { ...stream.metadata, payloadTable: table };
  const jsonBytes = _textEncoder.encode(JSON.stringify(metadata));
  const header = _textEncoder.encode(`${MAGIC} ${CONTAINER_VERSION} ${jsonBytes.length}\n`);

  const parts: Uint8Array[] = [header, jsonBytes];
  const preludeLength = header.length + jsonBytes.length;
  let byteLength = _align8(preludeLength);
  const preludePad = byteLength - preludeLength;
  if (preludePad) {
    parts.push(new Uint8Array(preludePad).fill(0x0a));
  }

  for (const p of payloads) {
    const bytes = p.bytes || new Uint8Array(0);
    if (bytes.length) {
      parts.push(bytes);
      byteLength += bytes.length;
    }
    const pad = _align8(bytes.length) - bytes.length;
    if (pad) {
      parts.push(new Uint8Array(pad));
      byteLength += pad;
    }
  }

  return { parts, byteLength };
}

/**
 * Decode a binary capture file. `bytes` is a Uint8Array over the whole file.
 * Returns `{ metadata, payloads }` where payloads is a
 * Map<payloadId, Uint8Array> of zero-copy views into `bytes`. A payload whose
 * table entry runs past the end of the file (truncated download) is simply
 * absent from the map — loaders already treat missing payloads as omitted.
 */
export function decodeCaptureBinary(bytes: Uint8Array): DecodedCaptureBinary {
  if (!isCaptureBinary(bytes)) {
    throw new Error("Not a WebGPU Inspector binary capture (missing WGPUCAP header).");
  }
  const headerLimit = Math.min(bytes.length, MAX_HEADER_LENGTH);
  let newline = -1;
  for (let i = 0; i < headerLimit; ++i) {
    if (bytes[i] === 0x0a) {
      newline = i;
      break;
    }
  }
  if (newline < 0) {
    throw new Error("Malformed binary capture: header line not terminated.");
  }
  const header = _textDecoder.decode(bytes.subarray(0, newline)).split(" ");
  const version = parseInt(header[1], 10);
  const jsonLength = parseInt(header[2], 10);
  if (!Number.isFinite(version) || !Number.isFinite(jsonLength) || jsonLength < 0) {
    throw new Error("Malformed binary capture: bad header line.");
  }
  if (version > CONTAINER_VERSION) {
    throw new Error(`Binary capture container version ${version} is newer than this ` +
      `loader (max ${CONTAINER_VERSION}). Update WebGPU Inspector to load this file.`);
  }
  const jsonStart = newline + 1;
  const jsonEnd = jsonStart + jsonLength;
  if (jsonEnd > bytes.length) {
    throw new Error("Malformed binary capture: file shorter than its metadata.");
  }
  const metadata = JSON.parse(_textDecoder.decode(bytes.subarray(jsonStart, jsonEnd))) as CaptureMetadata;

  const binaryStart = _align8(jsonEnd);
  const payloads = new Map<number, Uint8Array>();
  // The table comes from untrusted file contents; entries are validated below.
  const table: ReadonlyArray<PayloadTableEntry | null | undefined> =
    Array.isArray(metadata.payloadTable) ? metadata.payloadTable : [];
  for (let id = 0; id < table.length; ++id) {
    const entry = table[id];
    if (!entry) {
      continue;
    }
    const start = binaryStart + entry[0];
    const len = entry[1];
    if (!(len >= 0) || start + len > bytes.length) {
      continue; // Truncated file -- treat this payload as omitted.
    }
    payloads.set(id, bytes.subarray(start, start + len));
  }
  return { metadata, payloads };
}
