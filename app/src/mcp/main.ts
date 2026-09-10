// Entry point of GPU Inspector's MCP server, which the Claude Code plugin (claude-plugin/) starts:
// the tools of server.ts on stdin and stdout.
import process from "node:process";
import { createServer } from "./server.js";

// stdout is the protocol stream; anything the renderer modules log goes to stderr instead.
console.log = console.info = console.debug = (...parts: unknown[]): void => {
  process.stderr.write(`${parts.map(String).join(" ")}\n`);
};

createServer().serve(process.stdin, process.stdout).then(
  () => process.exit(0),
  (e: unknown) => {
    process.stderr.write(`gpu-inspector MCP server: ${(e as Error)?.stack ?? String(e)}\n`);
    process.exit(1);
  },
);
