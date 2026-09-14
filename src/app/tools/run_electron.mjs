// Runs Electron with the arguments given, from npm scripts.
//
// Terminals that embed Electron (VS Code's integrated terminal, for one) export
// ELECTRON_RUN_AS_NODE=1, which makes the electron binary start as plain Node: the app then dies
// with "Cannot read properties of undefined (reading 'handle')" when it touches ipcMain. The
// variable is stripped here so `npm start` works from any terminal.
//
// Usage: node tools/run_electron.mjs . [app arguments]
import { spawn } from "node:child_process";
import { createRequire } from "node:module";

const electron = createRequire(import.meta.url)("electron");
const env = { ...process.env };
delete env.ELECTRON_RUN_AS_NODE;
delete env.ELECTRON_NO_ATTACH_CONSOLE;

const child = spawn(electron, process.argv.slice(2), { stdio: "inherit", env });
child.on("exit", (code, signal) => process.exit(signal ? 1 : (code ?? 0)));
child.on("error", (e) => {
  console.error(`failed to start electron: ${e.message}`);
  process.exit(1);
});
