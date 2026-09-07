// Builds the Electron app with esbuild: main process, preload, and the renderer bundle.
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

const targets = [
  {
    ...common,
    entryPoints: ["src/main/main.ts"],
    outfile: path.join(outdir, "main/main.js"),
    platform: "node",
    format: "esm",
    external: ["electron"],
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
];

function copyStatic() {
  fs.mkdirSync(path.join(outdir, "renderer/css"), { recursive: true });
  fs.copyFileSync("src/renderer/index.html", path.join(outdir, "renderer/index.html"));
  for (const f of fs.readdirSync("src/renderer/css")) {
    fs.copyFileSync(path.join("src/renderer/css", f), path.join(outdir, "renderer/css", f));
  }
}

copyStatic();
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
