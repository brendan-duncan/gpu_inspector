// Shader source files for modules that carry line information but no text (dxc with -Zi, a
// stripped build): the debug info names the file, the launch configuration's "Source roots"
// say where the sources live on this machine. A root is walked once (a few levels deep) into
// an index by file name; a name resolves to the indexed file whose path ends with the most of
// the name's own path, so "shaders/lit.frag" prefers .../shaders/lit.frag over another lit.frag.
import fs from "node:fs";
import path from "node:path";

const SEARCH_DEPTH = 6;
const MAX_SOURCE_BYTES = 4 * 1024 * 1024;
const SKIP_DIRS = new Set(["node_modules", ".git", ".svn", "__pycache__"]);

const indexCache = new Map<string, Map<string, string[]>>();

function indexRoot(root: string): Map<string, string[]> {
  const cached = indexCache.get(root);
  if (cached) return cached;
  const byName = new Map<string, string[]>();
  const visit = (dir: string, depth: number): void => {
    let entries: fs.Dirent[];
    try {
      entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
      return;
    }
    for (const e of entries) {
      const full = path.join(dir, e.name);
      if (e.isFile()) {
        const key = e.name.toLowerCase();
        const list = byName.get(key);
        if (list) list.push(full); else byName.set(key, [full]);
      } else if (e.isDirectory() && depth < SEARCH_DEPTH && !SKIP_DIRS.has(e.name) && !e.name.startsWith(".")) {
        visit(full, depth + 1);
      }
    }
  };
  visit(root, 0);
  indexCache.set(root, byName);
  return byName;
}

/** Forgets the indexes (a root's contents changed, or roots were edited). */
export function forgetSourceIndex(): void {
  indexCache.clear();
}

function parts(p: string): string[] {
  return p.replace(/\\/g, "/").split("/").filter((s) => s && s !== ".");
}

/** How many trailing path components of `name` the candidate path shares. */
function suffixMatch(name: string[], candidate: string): number {
  const c = parts(candidate).map((s) => s.toLowerCase());
  let n = 0;
  while (n < name.length && n < c.length && name[name.length - 1 - n].toLowerCase() === c[c.length - 1 - n]) n++;
  return n;
}

function readText(file: string): string | null {
  try {
    if (fs.statSync(file).size > MAX_SOURCE_BYTES) return null;
    return fs.readFileSync(file, "utf8");
  } catch {
    return null;
  }
}

/** The text of each named source file that can be found: as given (absolute paths), or under the roots. */
export function findShaderSources(names: string[], roots: string[]): Record<string, string> {
  const out: Record<string, string> = {};
  const cleanRoots = roots.map((r) => r.trim()).filter((r) => r && fs.existsSync(r));
  for (const name of names) {
    if (!name) continue;
    if (path.isAbsolute(name) && fs.existsSync(name)) {
      const text = readText(name);
      if (text !== null) { out[name] = text; continue; }
    }
    const nameParts = parts(name);
    const base = nameParts[nameParts.length - 1]?.toLowerCase();
    if (!base) continue;
    let best: { file: string; score: number } | null = null;
    for (const root of cleanRoots) {
      for (const file of indexRoot(root).get(base) ?? []) {
        const score = suffixMatch(nameParts, file);
        if (!best || score > best.score) best = { file, score };
      }
    }
    if (!best) continue;
    const text = readText(best.file);
    if (text !== null) out[name] = text;
  }
  return out;
}
