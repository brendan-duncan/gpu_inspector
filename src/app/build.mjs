// Builds the Electron app with esbuild: main process, preload, and the renderer bundle; and the MCP
// server of the Claude Code plugin (../../claude-plugin).
// Usage: node build.mjs [--watch]
import { build, context } from "esbuild";
import fs from "node:fs";
import path from "node:path";

const watch = process.argv.includes("--watch");
const outdir = "dist";

const common = {
  bundle: true,
  sourcemap: true,
  logLevel: "info",
  target: "es2022",
};

// Renderer modules that build widgets or call the Electron preload API. The MCP server runs in
// plain Node, so pulling one of them in is a mistake the build reports rather than a crash at
// the first tool call.
const UI_MODULE = /src[\\/]renderer[\\/](widget[\\/]|[^\\/]*_(view|panel)\.ts$|capture_command_info\.ts$|inspector_window\.ts$|code_editor\.ts$|launch_dialog\.ts$|args_view\.ts$|theme\.ts$|frame_flamegraph\.ts$|stacktrace_view\.ts$)/;
const noUiModules = {
  name: "no-ui-modules",
  setup(b) {
    b.onEnd((result) => {
      const ui = Object.keys(result.metafile?.inputs ?? {}).filter((f) => UI_MODULE.test(f));
      if (ui.length) throw new Error(`the MCP server bundles UI modules: ${ui.join(", ")}`);
    });
  },
};

const targets = [
  {
    ...common,
    entryPoints: ["src/main/main.ts"],
    outfile: path.join(outdir, "main/main.js"),
    platform: "node",
    format: "esm",
    // electron-updater is CommonJS with its own dependency tree: leave it in node_modules, which
    // electron-builder packages (it is a runtime dependency in package.json).
    external: ["electron", "electron-updater"],
  },
  {
    ...common,
    entryPoints: ["src/main/preload.ts"],
    outfile: path.join(outdir, "main/preload.cjs"),
    platform: "node",
    format: "cjs",
    external: ["electron"],
  },
  {
    ...common,
    entryPoints: ["src/renderer/inspector_window.ts"],
    outfile: path.join(outdir, "renderer/inspector_window.js"),
    platform: "browser",
    format: "esm",
  },
  {
    // One file with no dependencies, committed: a Claude Code plugin installs from the repository
    // as it is, with no build step. No source map, so the committed file stays one file.
    ...common,
    sourcemap: false,
    entryPoints: ["src/mcp/main.ts"],
    outfile: "../../claude-plugin/server/gpu-inspector-mcp.mjs",
    platform: "node",
    format: "esm",
    banner: { js: "// GPU Inspector's MCP server, built from src/app/src/mcp by src/app/build.mjs: edit the sources, not this file." },
    metafile: true,
    plugins: [noUiModules],
  },
];

// Plugins (docs/PLUGINS.md): each src/plugins/<id> with a ui/backend.ts has it bundled into
// build/plugins/<id>/ui/backend.js beside its plugin.json, which is where the app and the MCP server
// look for a checkout's plugins (main/plugins.ts); CMake puts the plugin's native library there too.
// A backend module stands alone: the types it imports from src/sdk/ts are gone once bundled.
const pluginsDir = path.join("..", "plugins");
const pluginsOut = path.join("..", "..", "build", "plugins");
for (const id of fs.existsSync(pluginsDir) ? fs.readdirSync(pluginsDir) : []) {
  const entry = path.join(pluginsDir, id, "ui", "backend.ts");
  if (!fs.existsSync(entry)) continue;
  targets.push({
    ...common,
    entryPoints: [entry],
    outfile: path.join(pluginsOut, id, "ui", "backend.js"),
    platform: "neutral",
    format: "esm",
  });
}

function copyPlugins() {
  for (const id of fs.existsSync(pluginsDir) ? fs.readdirSync(pluginsDir) : []) {
    const manifest = path.join(pluginsDir, id, "plugin.json");
    if (!fs.existsSync(manifest)) continue;
    fs.mkdirSync(path.join(pluginsOut, id), { recursive: true });
    fs.copyFileSync(manifest, path.join(pluginsOut, id, "plugin.json"));
  }
}

function copyStatic() {
  fs.mkdirSync(path.join(outdir, "renderer/css"), { recursive: true });
  fs.copyFileSync("src/renderer/index.html", path.join(outdir, "renderer/index.html"));
  for (const f of fs.readdirSync("src/renderer/css")) {
    fs.copyFileSync(path.join("src/renderer/css", f), path.join(outdir, "renderer/css", f));
  }
}

copyStatic();
copyPlugins();
if (watch) {
  for (const t of targets) {
    const ctx = await context(t);
    await ctx.watch();
  }
  fs.watch("src/renderer", { recursive: true }, (_e, f) => {
    if (f && (f.endsWith(".html") || f.endsWith(".css"))) copyStatic();
  });
  console.log("watching...");
} else {
  await Promise.all(targets.map((t) => build(t)));
}
