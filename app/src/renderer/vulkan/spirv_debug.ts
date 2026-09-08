// Shader source maps: the debug information a compiler can embed in SPIR-V.
//
// Two forms exist. The core one is OpSource / OpSourceContinued (the source text, optionally
// with an OpString file name), OpLine / OpNoLine (which source line the following instructions
// came from). glslang and glslc emit it with `-g`. The other is the NonSemantic.Shader.DebugInfo.100
// extended instruction set: DebugSource / DebugSourceContinued carry the text, DebugLine the
// line ranges; dxc emits it with `-fspv-debug=vulkan-with-source`, glslang with `-gV`. Both are
// parsed into one model: the embedded files and, for every instruction of the module, the source
// line it belongs to. The instruction ordinal is what ties this to `spirv-dis` output, which
// prints exactly one instruction per (possibly multi-line) statement in module order.

export interface DebugSourceFile {
  name: string;
  /** Source text, or null when only the file name was embedded. */
  text: string | null;
}

export interface DebugLocation {
  file: number;     // index into SpirvDebugInfo.files
  line: number;     // 1-based
  column: number;   // 1-based, 0 when unknown
}

export type DebugInfoForm = "OpLine" | "NonSemantic.Shader.DebugInfo.100" | "none";

export interface SpirvDebugInfo {
  language: string;             // "GLSL", "HLSL", "ESSL", "Slang", ...
  languageVersion: number;
  generator: string;
  files: DebugSourceFile[];
  /** Index of the main compilation unit in `files`, -1 when unknown. */
  mainFile: number;
  form: DebugInfoForm;
  /** Source location per instruction ordinal (header excluded); null where nothing applies. */
  locations: (DebugLocation | null)[];
  /** Strings from OpModuleProcessed (tools that transformed the module). */
  processed: string[];
}

const SOURCE_LANGUAGES: Record<number, string> = {
  0: "Unknown", 1: "ESSL", 2: "GLSL", 3: "OpenCL C", 4: "OpenCL C++", 5: "HLSL", 6: "C++ for OpenCL", 7: "SYCL",
  8: "HERO C", 9: "NZSL", 10: "WGSL", 11: "Slang", 12: "Zig",
};

// Registered SPIR-V generator magic numbers (upper 16 bits of the header's generator word).
const GENERATORS: Record<number, string> = {
  0: "Khronos", 1: "LunarG", 2: "Valve", 3: "Codeplay", 4: "NVIDIA", 5: "ARM", 6: "Khronos LLVM/SPIR-V Translator",
  7: "Khronos SPIR-V Tools Assembler", 8: "Khronos Glslang", 9: "Qualcomm", 10: "AMD", 11: "Intel", 12: "Imagination",
  13: "Google Shaderc over Glslang", 14: "Google spiregg (DXC)", 15: "Google rspirv", 16: "X-LEGEND Mesa-IR/SPIR-V Translator",
  17: "Khronos SPIR-V Tools Linker", 18: "Wine VKD3D", 19: "Clay Shader Compiler", 20: "W3C WHLSL Translator", 21: "Google Clspv",
  22: "MLIR SPIR-V Serializer", 23: "Google Tint", 24: "Google ANGLE", 25: "Netease Messiah", 26: "Xenia", 27: "Embark Rust GPU",
  28: "gfx-rs Naga", 29: "Mikkosoft MSP", 30: "SpvGenTwo", 31: "Skia SkSL", 32: "TornadoVM", 33: "DragonJoker ShaderWriter",
  34: "Khronos SPIR-V Tools Optimizer", 35: "Rayan Hatoum", 36: "Khronos SPIR-V Tools Diff", 37: "Nintendo", 38: "Khronos Slang",
  39: "Zig", 40: "Rendong Liang", 41: "Mesa Rusticl", 42: "Adobe", 43: "Netease", 44: "NVIDIA nvidia-spirv", 45: "Roblox Studio",
};

const enum Op {
  SourceContinued = 2, Source = 3, String = 7, ExtInstImport = 11, ExtInst = 12, Line = 8, Constant = 43, Function = 54,
  NoLine = 317, ModuleProcessed = 330,
}

// NonSemantic.Shader.DebugInfo.100 instructions.
const enum Dbg { CompilationUnit = 1, Source = 35, SourceContinued = 102, Line = 103, NoLine = 104 }

function readString(words: Uint32Array, start: number, end: number): string {
  const bytes: number[] = [];
  for (let i = start; i < end; i++) {
    const w = words[i];
    for (let b = 0; b < 4; b++) {
      const c = (w >>> (b * 8)) & 0xff;
      if (c === 0) return new TextDecoder().decode(new Uint8Array(bytes));
      bytes.push(c);
    }
  }
  return new TextDecoder().decode(new Uint8Array(bytes));
}

/** Parses the debug information of a SPIR-V module; null when the data is not SPIR-V. */
export function parseSpirvDebugInfo(data: Uint8Array): SpirvDebugInfo | null {
  if (data.byteLength < 20 || data.byteLength % 4) return null;
  // A view into a capture file can start at any byte; word views need 4-byte alignment.
  const bytes = data.byteOffset % 4 ? data.slice() : data;
  const words = new Uint32Array(bytes.buffer, bytes.byteOffset, bytes.byteLength / 4);
  if (words[0] !== 0x07230203) return null;
  const genWord = words[2];
  const genId = genWord >>> 16;
  const generator = `${GENERATORS[genId] ?? `generator ${genId}`} ${genWord & 0xffff}`;

  const strings = new Map<number, string>();          // OpString id -> text
  const constants = new Map<number, number>();        // OpConstant id -> low word
  const files: DebugSourceFile[] = [];
  const fileByName = new Map<string, number>();
  const fileByStringId = new Map<number, number>();   // OpString (file name) id -> file index
  const fileByDebugSourceId = new Map<number, number>(); // DebugSource result id -> file index
  const locations: (DebugLocation | null)[] = [];
  const processed: string[] = [];
  let language = "Unknown";
  let languageVersion = 0;
  let mainFile = -1;
  let form: DebugInfoForm = "none";
  let debugSet = 0;          // id of the NonSemantic.Shader.DebugInfo.100 import
  let lastOpSourceFile = -1; // OpSourceContinued appends to this file
  let lastDebugSourceFile = -1;
  let current: DebugLocation | null = null;

  const fileIndex = (name: string): number => {
    let idx = fileByName.get(name);
    if (idx === undefined) {
      idx = files.length;
      files.push({ name, text: null });
      fileByName.set(name, idx);
    }
    return idx;
  };
  const append = (idx: number, text: string): void => {
    if (idx < 0) return;
    files[idx].text = (files[idx].text ?? "") + text;
  };

  let i = 5;
  while (i < words.length) {
    const w = words[i];
    const op = w & 0xffff;
    const len = w >>> 16;
    if (len === 0) break;
    const a = i + 1;
    const end = Math.min(words.length, i + len);
    switch (op) {
      case Op.String:
        strings.set(words[a], readString(words, a + 1, end));
        break;
      case Op.Constant:
        constants.set(words[a + 1], words[a + 2]);
        break;
      case Op.ModuleProcessed:
        processed.push(readString(words, a, end));
        break;
      case Op.Source: {
        language = SOURCE_LANGUAGES[words[a]] ?? `language ${words[a]}`;
        languageVersion = words[a + 1];
        lastOpSourceFile = -1;
        if (end > a + 2) {
          const name = strings.get(words[a + 2]) ?? `source ${files.length + 1}`;
          lastOpSourceFile = fileIndex(name);
          fileByStringId.set(words[a + 2], lastOpSourceFile);
          if (end > a + 3) {
            append(lastOpSourceFile, readString(words, a + 3, end));
            if (mainFile < 0) mainFile = lastOpSourceFile;
          }
        }
        break;
      }
      case Op.SourceContinued:
        append(lastOpSourceFile, readString(words, a, end));
        break;
      case Op.Line: {
        if (form === "none") form = "OpLine";
        let file = fileByStringId.get(words[a]);
        if (file === undefined) {
          file = fileIndex(strings.get(words[a]) ?? `file %${words[a]}`);
          fileByStringId.set(words[a], file);
        }
        current = { file, line: words[a + 1], column: words[a + 2] };
        break;
      }
      case Op.NoLine:
        current = null;
        break;
      case Op.ExtInstImport:
        if (readString(words, a + 1, end) === "NonSemantic.Shader.DebugInfo.100") debugSet = words[a];
        break;
      case Op.ExtInst: {
        if (!debugSet || words[a + 2] !== debugSet) break;
        const inst = words[a + 3];
        const o = a + 4;
        if (inst === Dbg.Source) {
          const name = strings.get(words[o]) ?? `source ${files.length + 1}`;
          lastDebugSourceFile = fileIndex(name);
          fileByDebugSourceId.set(words[a + 1], lastDebugSourceFile);
          if (end > o + 1) {
            const text = strings.get(words[o + 1]);
            if (text !== undefined) append(lastDebugSourceFile, text);
          }
        } else if (inst === Dbg.SourceContinued) {
          const text = strings.get(words[o]);
          if (text !== undefined) append(lastDebugSourceFile, text);
        } else if (inst === Dbg.CompilationUnit) {
          const src = fileByDebugSourceId.get(words[o + 2]);
          if (src !== undefined && mainFile < 0) mainFile = src;
          const lang = constants.get(words[o + 3]);
          if (lang !== undefined && language === "Unknown") language = SOURCE_LANGUAGES[lang] ?? language;
        } else if (inst === Dbg.Line) {
          form = "NonSemantic.Shader.DebugInfo.100";
          const file = fileByDebugSourceId.get(words[o]);
          const line = constants.get(words[o + 1]);
          if (file !== undefined && line !== undefined) current = { file, line, column: constants.get(words[o + 3]) ?? 0 };
        } else if (inst === Dbg.NoLine) {
          current = null;
        }
        break;
      }
      case Op.Function:
        // A location does not carry across function boundaries.
        current = null;
        break;
      default:
        break;
    }
    // OpLine / DebugLine map to their own line too, so the annotation lands on them.
    locations.push(current);
    i += len;
  }
  if (mainFile < 0 && files.length) mainFile = files.findIndex((f) => f.text !== null);
  return { language, languageVersion, generator, files, mainFile, form, locations, processed };
}

/**
 * The physical lines of an embedded file and the source line number each one has, honoring
 * `#line` directives: glslang prefixes the text it embeds with `// OpModuleProcessed` comments
 * and a `#line 1`, so OpLine numbers count from there. `physicalOf[n]` is the physical index of
 * source line n (the last one when a number occurs twice); `lineOf[i]` is the source line number
 * of physical line i, or 0 for lines no OpLine can refer to (directives, shadowed prefix lines).
 */
export interface SourceLineMap {
  lines: string[];
  lineOf: number[];
  physicalOf: Map<number, number>;
}

export function sourceLineMap(text: string): SourceLineMap {
  const lines = text.split("\n");
  if (lines.length > 1 && lines[lines.length - 1] === "") lines.pop();
  const lineOf: number[] = new Array(lines.length).fill(0);
  const physicalOf = new Map<number, number>();
  let logical = 1;
  for (let i = 0; i < lines.length; i++) {
    const m = /^\s*#\s*line\s+(\d+)/.exec(lines[i]);
    if (m) {
      logical = Number(m[1]);
      continue;
    }
    lineOf[i] = logical;
    physicalOf.set(logical, i);
    logical++;
  }
  // A line number claimed twice belongs to its last occurrence; the earlier one is unnumbered.
  for (let i = 0; i < lines.length; i++) if (lineOf[i] && physicalOf.get(lineOf[i]) !== i) lineOf[i] = 0;
  return { lines, lineOf, physicalOf };
}

/** True when at least one embedded file carries source text. */
export function hasEmbeddedSource(info: SpirvDebugInfo | null): boolean {
  return !!info && info.files.some((f) => f.text !== null);
}

/** The highlighter / compiler language of the embedded source, or null for languages without one. */
export function sourceLanguageOf(info: SpirvDebugInfo | null): "glsl" | "hlsl" | null {
  if (!info) return null;
  if (info.language === "GLSL" || info.language === "ESSL") return "glsl";
  if (info.language === "HLSL") return "hlsl";
  return null;
}

/** One-line description of the debug information for the shader header. */
export function describeDebugInfo(info: SpirvDebugInfo | null): string {
  if (!info) return "";
  const parts: string[] = [];
  const withText = info.files.filter((f) => f.text !== null);
  if (withText.length) {
    parts.push(`Embedded source: ${withText.map((f) => f.name).join(", ")}`);
  } else if (info.files.length) {
    parts.push(`Source file name only: ${info.files.map((f) => f.name).join(", ")}`);
  } else {
    parts.push("No embedded source");
  }
  const lang = info.language !== "Unknown" ? `${info.language}${info.languageVersion ? ` ${info.languageVersion}` : ""}` : "";
  const lines = info.form === "none" ? "no line mapping" : `line mapping via ${info.form}`;
  parts.push([lang, info.generator, lines].filter(Boolean).join(", "));
  if (info.processed.length) parts.push(`processed by ${info.processed.join("; ")}`);
  return parts.join(" | ");
}

/**
 * Splits `spirv-dis` output into instructions. Comment lines (`;`) and blank lines are skipped;
 * a string literal with embedded newlines (an OpSource with its text) keeps its statement on one
 * instruction. Returns, per instruction, the indices of the text lines it spans.
 */
export function disassemblyInstructions(lines: string[]): number[][] {
  const result: number[][] = [];
  let open: number[] | null = null;   // lines of an instruction whose string literal is still open
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    if (open) {
      open.push(i);
      if (quoteToggles(line)) {
        result.push(open);
        open = null;
      }
      continue;
    }
    const t = line.trim();
    if (!t || t.startsWith(";")) continue;
    if (quoteToggles(line)) open = [i];
    else result.push([i]);
  }
  if (open) result.push(open);
  return result;
}

/** True when the line has an odd number of unescaped quotes (a string literal opens or closes here). */
function quoteToggles(line: string): boolean {
  let n = 0;
  for (let i = 0; i < line.length; i++) {
    const c = line[i];
    if (c === "\\") i++;
    else if (c === '"') n++;
  }
  return (n & 1) === 1;
}
