// Ported from WebGPU Inspector (MIT): src/utils/base64.js
// Synchronous base64 helpers. Used to ferry binary buffer/texture chunks across the
// page → content-script → background → panel pipeline, which is JSON-only.
//
// Prefer the native Uint8Array.prototype.toBase64 / Uint8Array.fromBase64 when present
// (Chrome 137+, Firefox 132+). Fall back to btoa/atob with chunked String.fromCharCode
// for older runtimes.

// The TC39 base64 proposal methods are not in the TypeScript DOM/ES2022 libs yet, so
// describe the optional native surface locally and probe for it at runtime.
interface Uint8ArrayBase64Methods {
  toBase64?: (this: Uint8Array) => string;
}

interface Uint8ArrayBase64Statics {
  fromBase64?: (str: string) => Uint8Array;
}

const _uint8Proto = Uint8Array.prototype as Uint8Array & Uint8ArrayBase64Methods;
const _uint8Ctor = Uint8Array as Uint8ArrayConstructor & Uint8ArrayBase64Statics;

const _hasNativeToBase64 = typeof _uint8Proto.toBase64 === "function";
const _hasNativeFromBase64 = typeof _uint8Ctor.fromBase64 === "function";

// 0x8000 keeps String.fromCharCode.apply below typical engine argument limits.
const _fromCharCodeChunk = 0x8000;

export function encodeBase64(bytes: Uint8Array): string {
  if (_hasNativeToBase64) {
    return (bytes as Uint8Array & Required<Uint8ArrayBase64Methods>).toBase64();
  }
  let binary = "";
  const len = bytes.length;
  for (let i = 0; i < len; i += _fromCharCodeChunk) {
    const end = i + _fromCharCodeChunk < len ? i + _fromCharCodeChunk : len;
    binary += String.fromCharCode.apply(null, bytes.subarray(i, end) as unknown as number[]);
  }
  return btoa(binary);
}

export function decodeBase64(str: string): Uint8Array {
  if (_hasNativeFromBase64) {
    return (_uint8Ctor as UintArrayConstructorWithFromBase64).fromBase64(str);
  }
  const binary = atob(str);
  const len = binary.length;
  const out = new Uint8Array(len);
  for (let i = 0; i < len; i++) {
    out[i] = binary.charCodeAt(i);
  }
  return out;
}

type UintArrayConstructorWithFromBase64 = Uint8ArrayConstructor & Required<Uint8ArrayBase64Statics>;

// Legacy async helpers retained as thin wrappers so anything still importing them
// keeps working. New code should use the sync helpers above.
export async function encodeDataUrl(bytes: Uint8Array, type: string = "application/octet-stream"): Promise<string> {
  return `data:${type};base64,${encodeBase64(bytes)}`;
}

export async function decodeDataUrl(dataUrl: string): Promise<Uint8Array> {
  const comma = dataUrl.indexOf(",");
  return decodeBase64(comma >= 0 ? dataUrl.slice(comma + 1) : dataUrl);
}
