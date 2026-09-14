// Where the MCP server looks on this machine for what captures only name: the shader source files
// of modules compiled with line information but no text (under the source roots), and the
// unstripped libraries that turn a stack frame's module and offset into a function and a line
// (under the symbol directories). set_search_paths sets them for the server's lifetime; otherwise
// GPU_INSPECTOR_SOURCE_ROOTS and GPU_INSPECTOR_SYMBOL_DIRS; otherwise the ones GPU Inspector's
// launch dialog used last, from its settings.
import fs from "node:fs";
import { findShaderSources, forgetSourceIndex } from "../main/shader_sources.js";
import { findSymbolizer, symbolizeFrames } from "../main/symbolize.js";
import type { ObjectDatabase } from "../renderer/vulkan/object_database.js";
import { parseSpirvDebugInfo, sourceLineMap, type SpirvDebugInfo } from "../renderer/vulkan/spirv_debug.js";
import type { StackFrame } from "../shared/protocol.js";
import { appSetting } from "./capture_store.js";

export type PathKind = "sourceRoots" | "symbolDirs";
/** Each source line's code by file name (and base name), then by line number. */
export type LineTexts = Map<string, Map<number, string>>;

const ENV: Record<PathKind, string> = { sourceRoots: "GPU_INSPECTOR_SOURCE_ROOTS", symbolDirs: "GPU_INSPECTOR_SYMBOL_DIRS" };
const overrides: Partial<Record<PathKind, string[]>> = {};

/** Directories from a list, or from a string separated by ";" as GPU Inspector's settings keep them. */
export function splitPaths(value: unknown): string[] {
  const items = Array.isArray(value) ? value.map(String) : typeof value === "string" ? value.split(";") : [];
  return items.map((s) => s.trim()).filter(Boolean);
}

/** The directories in effect, and where they came from. */
export function searchPaths(kind: PathKind): { dirs: string[]; from: string } {
  const set = overrides[kind];
  if (set) return { dirs: set, from: "set_search_paths" };
  const env = splitPaths(process.env[ENV[kind]]);
  if (env.length) return { dirs: env, from: ENV[kind] };
  const saved = splitPaths(appSetting(kind));
  if (saved.length) return { dirs: saved, from: "GPU Inspector's settings" };
  return { dirs: [], from: "none" };
}

/** Replaces the directories for the server's lifetime; an empty list goes back to the environment or GPU Inspector's settings. */
export function setSearchPaths(kind: PathKind, dirs: string[]): void {
  if (dirs.length) overrides[kind] = dirs;
  else delete overrides[kind];
  if (kind === "sourceRoots") forgetSourceIndex();
}

export function describeSearchPaths(): Record<string, unknown> {
  const describe = (kind: PathKind): Record<string, unknown> => {
    const { dirs, from } = searchPaths(kind);
    const missing = dirs.filter((d) => !fs.existsSync(d));
    return { dirs, from, missing: missing.length ? missing : undefined };
  };
  const symbolizer = findSymbolizer();
  return {
    sourceRoots: describe("sourceRoots"),
    symbolDirs: describe("symbolDirs"),
    symbolizer: symbolizer ? symbolizer.exe : "None found: llvm-symbolizer from the Android NDK (ANDROID_NDK_HOME), or llvm-symbolizer or addr2line on PATH, resolves frames under symbolDirs.",
  };
}

/**
 * A module's debug information with the text of the files it only names read from this machine:
 * at the path named, when it exists, or under the source roots. `found` names the files read.
 */
export function debugInfoWithSources(spirv: Uint8Array): { info: SpirvDebugInfo | null; found: string[] } {
  const info = parseSpirvDebugInfo(spirv);
  if (!info) return { info, found: [] };
  const missing = info.files.filter((f) => f.text === null && f.name);
  if (missing.length) {
    const texts = findShaderSources(missing.map((f) => f.name), searchPaths("sourceRoots").dirs);
    for (const f of missing) {
      const text = texts[f.name];
      if (typeof text !== "string") continue;
      f.text = text;
      f.fromHost = true;
    }
    if (info.mainFile < 0) info.mainFile = info.files.findIndex((f) => f.text !== null);
  }
  return { info, found: info.files.filter((f) => f.fromHost).map((f) => f.name) };
}

/** The code of every numbered source line of the files that have text (#line directives honored). */
export function sourceLineTexts(info: SpirvDebugInfo | null): LineTexts {
  const out: LineTexts = new Map();
  for (const f of info?.files ?? []) {
    if (f.text === null) continue;
    const map = sourceLineMap(f.text);
    const byLine = new Map<number, string>();
    map.lines.forEach((text, i) => {
      if (map.lineOf[i] > 0) byLine.set(map.lineOf[i], text.trim());
    });
    out.set(f.name, byLine);
    const base = f.name.split(/[\\/]/).pop();
    if (base && !out.has(base)) out.set(base, byLine);
  }
  return out;
}

/** A source line's code, cut to 200 characters; undefined when its file has no text. */
export function codeAt(texts: LineTexts, file: string | undefined, line: number | undefined): string | undefined {
  if (!line) return undefined;
  const base = file?.split(/[\\/]/).pop();
  const byLine = file ? texts.get(file) ?? (base ? texts.get(base) : undefined) : texts.size === 1 ? [...texts.values()][0] : undefined;
  const code = byLine?.get(line);
  if (!code) return undefined;
  return code.length > 200 ? `${code.slice(0, 200)}...` : code;
}

/** Frames the capture library could only name by module and offset, resolved from the unstripped libraries under the symbol directories. */
export async function symbolizeOnHost(frames: StackFrame[]): Promise<StackFrame[]> {
  const dirs = searchPaths("symbolDirs").dirs;
  if (!dirs.length || !frames.some((f) => f.module && f.offset > 0 && !f.file && !f.internal)) return frames;
  const resolved = await symbolizeFrames(frames, dirs);
  if (!resolved.length) return frames;
  const byKey = new Map(resolved.map((f) => [`${f.module}+${f.offset}`, f]));
  return frames.map((f) => byKey.get(`${f.module}+${f.offset}`) ?? f);
}

/** symbolizeOnHost for resolveSymbols (stack_requests.ts): resolved frames replace the cached ones, so a saved capture keeps them. */
export async function symbolizeSymbolMap(db: ObjectDatabase, frames: Map<string, StackFrame>): Promise<void> {
  const list = [...frames.values()];
  const resolved = await symbolizeOnHost(list);
  resolved.forEach((f, i) => {
    if (f === list[i]) return;
    frames.set(f.address, f);
    db.symbols.set(f.address, f);
  });
}
