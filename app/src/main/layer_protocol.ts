// The wire format between a capture library and its client (layer/src/transport.h): every frame is
// a little-endian u32 payload length, a u8 kind and the payload. Kind 0 is a JSON message; kind 1
// is a u32 header length, a JSON header, then raw bytes, which the message carries as `__binary`.
// The app's sessions (main.ts) and the MCP server's live sessions both speak it.
import type { LayerMessage, UiRequest } from "../shared/protocol.js";

/** A request as one frame. */
export function encodeRequest(msg: UiRequest): Buffer {
  const payload = Buffer.from(JSON.stringify(msg), "utf8");
  const header = Buffer.alloc(5);
  header.writeUInt32LE(payload.length, 0);
  header.writeUInt8(0, 4);
  return Buffer.concat([header, payload]);
}

/** A frame that did not parse: its JSON message, or the JSON header of its binary message. */
export interface FrameError {
  kind: "json" | "header";
  error: string;
  payload: Buffer;
}

/** Cuts the byte stream from the capture library into messages as its chunks arrive. */
export class FrameReader {
  private _buffered: Buffer = Buffer.alloc(0);

  /** The messages `chunk` completes, in order; a frame that does not parse goes to `onError` and is skipped. */
  push(chunk: Buffer, onError?: (e: FrameError) => void): LayerMessage[] {
    this._buffered = this._buffered.length ? Buffer.concat([this._buffered, chunk]) : chunk;
    const out: LayerMessage[] = [];
    while (this._buffered.length >= 5) {
      const len = this._buffered.readUInt32LE(0);
      const kind = this._buffered.readUInt8(4);
      if (this._buffered.length < 5 + len) break;
      const payload = this._buffered.subarray(5, 5 + len);
      this._buffered = this._buffered.subarray(5 + len);
      if (kind === 0) {
        try {
          out.push(JSON.parse(payload.toString("utf8")) as LayerMessage);
        } catch (e) {
          onError?.({ kind: "json", error: String(e), payload });
        }
      } else if (kind === 1) {
        const hl = payload.readUInt32LE(0);
        let header: Record<string, unknown>;
        try {
          header = JSON.parse(payload.subarray(4, 4 + hl).toString("utf8")) as Record<string, unknown>;
        } catch (e) {
          onError?.({ kind: "header", error: String(e), payload });
          continue;
        }
        // A copy: a standalone Uint8Array rather than a Buffer view into the stream (the renderer
        // gets its own ArrayBuffer, and Buffer.slice would not copy for the SPIR-V parsers).
        out.push({ ...header, __binary: new Uint8Array(payload.subarray(4 + hl)) } as unknown as LayerMessage);
      }
    }
    return out;
  }
}
