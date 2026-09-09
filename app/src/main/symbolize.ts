// Host-side symbolization of stack frames the layer could only name by module and offset: on
// Android (and Linux) the layer has dladdr, which knows exported symbols only, so frames inside
// the application's own stripped libraries come back as "libfoo.so+0x1234". Given directories
// holding the unstripped libraries (the build tree), llvm-symbolizer from the NDK (or the one
// on PATH, or addr2line) turns the offsets into functions, files and lines.
import { execFile } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import type { StackFrame } from "../shared/protocol.js";

const SEARCH_DEPTH = 5;
const SYMBOLIZER_TIMEOUT_MS = 30000;

/** llvm-symbolizer from the NDK named by the environment or under the default SDK, else on PATH; addr2line as a fallback. */
export function findSymbolizer(): { exe: string; llvm: boolean } | null {
  const exe = process.platform === "win32" ? ".exe" : "";
  const roots: string[] = [];
  for (const v of ["ANDROID_NDK_HOME", "ANDROID_NDK_ROOT", "ANDROID_NDK"]) if (process.env[v]) roots.push(process.env[v]!);
  const sdks: string[] = [];
  for (const v of ["ANDROID_HOME", "ANDROID_SDK_ROOT"]) if (process.env[v]) sdks.push(process.env[v]!);
  if (process.platform === "win32" && process.env.LOCALAPPDATA) sdks.push(path.join(process.env.LOCALAPPDATA, "Android", "Sdk"));
  else if (process.platform === "darwin") sdks.push(path.join(os.homedir(), "Library", "Android", "sdk"));
  else sdks.push(path.join(os.homedir(), "Android", "Sdk"));
  for (const sdk of sdks) {
    const ndk = path.join(sdk, "ndk");
    if (fs.existsSync(ndk)) for (const v of fs.readdirSync(ndk).sort().reverse()) roots.push(path.join(ndk, v));
  }
  for (const root of roots) {
    const prebuilt = path.join(root, "toolchains", "llvm", "prebuilt");
    if (!fs.existsSync(prebuilt)) continue;
    for (const host of fs.readdirSync(prebuilt)) {
      const candidate = path.join(prebuilt, host, "bin", `llvm-symbolizer${exe}`);
      if (fs.existsSync(candidate)) return { exe: candidate, llvm: true };
    }
  }
  for (const dir of (process.env.PATH ?? "").split(path.delimiter)) {
    if (!dir) continue;
    for (const [name, llvm] of [["llvm-symbolizer", true], ["addr2line", false]] as const) {
      const candidate = path.join(dir, name + exe);
      if (fs.existsSync(candidate)) return { exe: candidate, llvm };
    }
  }
  return null;
}

/** The file named `module` under one of the directories (a few levels deep), preferring the largest (unstripped) copy. */
function findModule(module: string, dirs: string[]): string | null {
  let best: { file: string; size: number } | null = null;
  const visit = (dir: string, depth: number): void => {
    let entries: fs.Dirent[];
    try {
      entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
      return;
    }
    for (const e of entries) {
      const full = path.join(dir, e.name);
      if (e.isFile() && e.name === module) {
        const size = fs.statSync(full).size;
        if (!best || size > best.size) best = { file: full, size };
      } else if (e.isDirectory() && depth < SEARCH_DEPTH && !e.name.startsWith(".") && e.name !== "node_modules") {
        visit(full, depth + 1);
      }
    }
  };
  for (const d of dirs) if (d) visit(d, 0);
  return best ? (best as { file: string }).file : null;
}

function run(exe: string, args: string[]): Promise<string> {
  return new Promise((resolve) => {
    execFile(exe, args, { timeout: SYMBOLIZER_TIMEOUT_MS, maxBuffer: 16 * 1024 * 1024, windowsHide: true }, (err, stdout) => resolve(err ? "" : stdout));
  });
}

const moduleCache = new Map<string, string | null>();

/**
 * Frames with a module and offset but no source location, resolved through the unstripped
 * module found under `dirs`. Returns the frames that gained something; the rest are left out.
 */
export async function symbolizeFrames(frames: StackFrame[], dirs: string[]): Promise<StackFrame[]> {
  const wanted = frames.filter((f) => f.module && f.offset > 0 && !f.file && !f.internal);
  if (!wanted.length || !dirs.length) return [];
  const tool = findSymbolizer();
  if (!tool) return [];
  const byModule = new Map<string, StackFrame[]>();
  for (const f of wanted) {
    const list = byModule.get(f.module!);
    if (list) list.push(f); else byModule.set(f.module!, [f]);
  }
  const out: StackFrame[] = [];
  for (const [module, list] of byModule) {
    const key = `${dirs.join(";")}|${module}`;
    let file = moduleCache.get(key);
    if (file === undefined) {
      file = findModule(module, dirs);
      moduleCache.set(key, file);
    }
    if (!file) continue;
    const offsets = list.map((f) => `0x${f.offset.toString(16)}`);
    const text = tool.llvm
      ? await run(tool.exe, [`--obj=${file}`, "--functions=linkage", "--demangle", "--inlining=false", "--output-style=LLVM", ...offsets])
      : await run(tool.exe, ["-C", "-f", "-e", file, ...offsets]);
    // Both tools print two lines per address: the function, then "file:line[:column]".
    const lines = text.split(/\r?\n/).map((l) => l.trim()).filter((l) => l.length);
    for (let i = 0; i < list.length && 2 * i + 1 < lines.length; ++i) {
      const fn = lines[2 * i];
      const loc = /^(.*?):(\d+)(?::\d+)?$/.exec(lines[2 * i + 1]);
      const f = list[i];
      const resolved: StackFrame = { ...f };
      let gained = false;
      if (fn && fn !== "??") { resolved.function = fn; gained = true; }
      if (loc && loc[1] !== "??" && Number(loc[2]) > 0) { resolved.file = loc[1]; resolved.line = Number(loc[2]); gained = true; }
      if (gained) out.push(resolved);
    }
  }
  return out;
}
