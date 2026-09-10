// Entry point of GPU Inspector's MCP server, which the Claude Code plugin (claude-plugin/) starts:
// the tools of server.ts on stdin and stdout. The applications it launched end with it.
import process from "node:process";
import { SessionManager, createServer } from "./server.js";

// stdout is the protocol stream; anything the renderer modules log goes to stderr instead.
console.log = console.info = console.debug = (...parts: unknown[]): void => {
  process.stderr.write(`${parts.map(String).join(" ")}\n`);
};

const sessions = new SessionManager();
const exit = (code: number): void => {
  void sessions.stopAll().finally(() => process.exit(code));
};
process.on("SIGINT", () => exit(0));
process.on("SIGTERM", () => exit(0));

createServer(undefined, sessions).serve(process.stdin, process.stdout).then(
  () => exit(0),
  (e: unknown) => {
    process.stderr.write(`gpu-inspector MCP server: ${(e as Error)?.stack ?? String(e)}\n`);
    exit(1);
  },
);
