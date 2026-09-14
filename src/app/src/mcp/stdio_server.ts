// A Model Context Protocol server over stdio, as much of it as a tools-only server needs:
// `initialize`, `ping`, `tools/list` and `tools/call`, one JSON-RPC 2.0 message per line in each
// direction. Written out rather than taken from the SDK, whose server brings a schema validator,
// an HTTP stack and a schema library with it for what, over stdio, is these four methods.
//
// stdout carries the protocol and nothing else; anything to log goes to stderr.
import { createInterface } from "node:readline";
import type { Readable, Writable } from "node:stream";

export type ToolContent = { type: "text"; text: string } | { type: "image"; data: string; mimeType: string };

export interface ToolResult {
  content: ToolContent[];
  isError?: boolean;
}

export type ToolArgs = Record<string, unknown>;

export interface ToolDefinition {
  name: string;
  description: string;
  inputSchema: Record<string, unknown>;
  /** The tool only reads: a client may run it without asking. */
  readOnly?: boolean;
  handler: (args: ToolArgs) => ToolResult | Promise<ToolResult>;
}

interface JsonRpcRequest {
  jsonrpc: "2.0";
  id?: string | number | null;
  method?: string;
  params?: Record<string, unknown>;
}

/** The protocol revisions this server speaks, newest first; the tools surface is the same in all. */
const PROTOCOL_VERSIONS = ["2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"];

const METHOD_NOT_FOUND = -32601;
const INVALID_PARAMS = -32602;
const PARSE_ERROR = -32700;

export class McpStdioServer {
  private readonly _tools = new Map<string, ToolDefinition>();

  constructor(private readonly _info: { name: string; version: string }, tools: ToolDefinition[], private readonly _instructions = "") {
    for (const t of tools) this._tools.set(t.name, t);
  }

  /** Serves `input` until it closes, writing responses to `output`. */
  serve(input: Readable, output: Writable): Promise<void> {
    return new Promise((resolve) => {
      const lines = createInterface({ input, crlfDelay: Infinity });
      lines.on("line", (line) => {
        if (!line.trim()) return;
        void this._receive(line).then((reply) => {
          if (reply) output.write(`${JSON.stringify(reply)}\n`);
        });
      });
      lines.on("close", () => resolve());
    });
  }

  private async _receive(line: string): Promise<unknown> {
    let message: unknown;
    try {
      message = JSON.parse(line);
    } catch {
      return { jsonrpc: "2.0", id: null, error: { code: PARSE_ERROR, message: "Parse error" } };
    }
    if (Array.isArray(message)) {
      const replies = (await Promise.all(message.map((m) => this.handle(m)))).filter((r) => r !== null);
      return replies.length ? replies : null;
    }
    return this.handle(message);
  }

  /** The response to one message, or null for a notification (which gets none). */
  async handle(message: unknown): Promise<Record<string, unknown> | null> {
    const req = message as JsonRpcRequest;
    if (!req || typeof req !== "object" || req.id === undefined || req.id === null) return null;
    const id = req.id;
    const params = req.params ?? {};
    switch (req.method) {
      case "initialize": {
        const asked = typeof params.protocolVersion === "string" ? params.protocolVersion : "";
        return {
          jsonrpc: "2.0", id, result: {
            protocolVersion: PROTOCOL_VERSIONS.includes(asked) ? asked : PROTOCOL_VERSIONS[0],
            capabilities: { tools: { listChanged: false } },
            serverInfo: this._info,
            ...(this._instructions ? { instructions: this._instructions } : {}),
          },
        };
      }
      case "ping":
        return { jsonrpc: "2.0", id, result: {} };
      case "tools/list":
        return {
          jsonrpc: "2.0", id, result: {
            tools: [...this._tools.values()].map((t) => ({
              name: t.name, description: t.description, inputSchema: t.inputSchema,
              ...(t.readOnly ? { annotations: { readOnlyHint: true } } : {}),
            })),
          },
        };
      case "tools/call": {
        const tool = this._tools.get(String(params.name));
        if (!tool) return { jsonrpc: "2.0", id, error: { code: INVALID_PARAMS, message: `Unknown tool: ${String(params.name)}` } };
        const args = params.arguments && typeof params.arguments === "object" ? params.arguments as ToolArgs : {};
        try {
          return { jsonrpc: "2.0", id, result: await tool.handler(args) };
        } catch (e) {
          // A failed call is a result the model reads, not a protocol error.
          return { jsonrpc: "2.0", id, result: { content: [{ type: "text", text: `Error: ${(e as Error)?.message ?? String(e)}` }], isError: true } };
        }
      }
      default:
        return { jsonrpc: "2.0", id, error: { code: METHOD_NOT_FOUND, message: `Method not found: ${String(req.method)}` } };
    }
  }
}
