// Syntax highlighting for the shader views and a small highlighting editor (a transparent
// textarea over the highlighted text), with a color scheme in the spirit of WebGPU Inspector's
// shader editor. Languages: GLSL, HLSL and SPIR-V assembly.
import { Div } from "./widget/div.js";
import { Widget } from "./widget/widget.js";

export type HighlightLanguage = "glsl" | "hlsl" | "spirv-asm";

interface Rule {
  re: RegExp;                                   // sticky
  cls: string | ((text: string) => string);
}

const GLSL_KEYWORDS = new Set([
  "if", "else", "for", "while", "do", "return", "break", "continue", "discard", "switch", "case", "default", "struct",
  "layout", "uniform", "in", "out", "inout", "const", "buffer", "shared", "precision", "highp", "mediump", "lowp", "true",
  "false", "void", "invariant", "flat", "smooth", "noperspective", "centroid", "sample", "patch", "coherent", "volatile",
  "restrict", "readonly", "writeonly", "subroutine", "precise", "attribute", "varying",
]);
const GLSL_TYPE = /^(?:[biud]?vec[234]|[df]?mat[234](?:x[234])?|f16vec[234]|f16mat[234](?:x[234])?|float|int|uint|bool|double|half|float16_t|float32_t|float64_t|int8_t|int16_t|int64_t|uint8_t|uint16_t|uint64_t|i8vec[234]|i16vec[234]|i64vec[234]|u8vec[234]|u16vec[234]|u64vec[234]|atomic_uint|[iu]?sampler\w*|[iu]?texture\w*|[iu]?image\w*|[iu]?subpassInput\w*|accelerationStructureEXT|rayQueryEXT)$/;
const GLSL_BUILTINS = new Set([
  "texture", "textureLod", "textureGrad", "textureProj", "textureSize", "texelFetch", "textureGather", "textureQueryLod",
  "imageLoad", "imageStore", "imageSize", "normalize", "dot", "cross", "length", "distance", "reflect", "refract", "mix", "clamp",
  "min", "max", "abs", "sign", "floor", "ceil", "round", "fract", "mod", "sqrt", "inversesqrt", "pow", "exp", "exp2", "log",
  "log2", "sin", "cos", "tan", "asin", "acos", "atan", "step", "smoothstep", "fma", "transpose", "inverse", "determinant",
  "dFdx", "dFdy", "fwidth", "any", "all", "not", "equal", "notEqual", "lessThan", "greaterThan", "packHalf2x16",
  "unpackHalf2x16", "floatBitsToUint", "floatBitsToInt", "uintBitsToFloat", "intBitsToFloat", "barrier", "memoryBarrier",
  "atomicAdd", "atomicMin", "atomicMax", "atomicExchange", "atomicCompSwap", "subgroupAdd", "subgroupMin", "subgroupMax",
  "EmitVertex", "EndPrimitive", "main",
]);

const HLSL_KEYWORDS = new Set([
  "if", "else", "for", "while", "do", "return", "break", "continue", "discard", "switch", "case", "default", "struct",
  "cbuffer", "tbuffer", "register", "packoffset", "static", "const", "in", "out", "inout", "uniform", "true", "false", "void",
  "row_major", "column_major", "precise", "groupshared", "shared", "extern", "volatile", "nointerpolation", "linear",
  "centroid", "noperspective", "sample", "unroll", "loop", "branch", "flatten", "numthreads", "typedef", "namespace",
  "template", "class", "interface", "export", "globallycoherent", "snorm", "unorm",
]);
const HLSL_TYPE = /^(?:(?:float|half|double|int|uint|bool|min16float|min10float|min16int|min12int|min16uint|float16_t|int16_t|uint16_t|int64_t|uint64_t)(?:[1-4](?:x[1-4])?)?|matrix|vector|string|Texture1D|Texture1DArray|Texture2D|Texture2DArray|Texture2DMS|Texture2DMSArray|Texture3D|TextureCube|TextureCubeArray|RWTexture1D|RWTexture1DArray|RWTexture2D|RWTexture2DArray|RWTexture3D|Buffer|RWBuffer|StructuredBuffer|RWStructuredBuffer|AppendStructuredBuffer|ConsumeStructuredBuffer|ByteAddressBuffer|RWByteAddressBuffer|ConstantBuffer|SamplerState|SamplerComparisonState|RaytracingAccelerationStructure|RayDesc|RayQuery|SubpassInput|SubpassInputMS)$/;
const HLSL_BUILTINS = new Set([
  "mul", "saturate", "lerp", "dot", "cross", "normalize", "length", "distance", "reflect", "refract", "clamp", "min", "max",
  "abs", "sign", "floor", "ceil", "round", "frac", "fmod", "sqrt", "rsqrt", "pow", "exp", "exp2", "log", "log2", "sin", "cos",
  "tan", "asin", "acos", "atan", "atan2", "step", "smoothstep", "mad", "fma", "transpose", "determinant", "ddx", "ddy",
  "fwidth", "any", "all", "asfloat", "asuint", "asint", "f16tof32", "f32tof16", "Sample", "SampleLevel", "SampleGrad",
  "SampleBias", "SampleCmp", "SampleCmpLevelZero", "Load", "Store", "GetDimensions", "Gather", "GatherRed", "CalculateLevelOfDetail",
  "InterlockedAdd", "InterlockedMin", "InterlockedMax", "InterlockedExchange", "InterlockedCompareExchange",
  "GroupMemoryBarrierWithGroupSync", "AllMemoryBarrierWithGroupSync", "WaveActiveSum", "WaveReadLaneFirst", "WaveGetLaneIndex",
  "TraceRay", "main", "tex2D", "tex2Dlod", "texCUBE", "clip",
]);

function cLikeRules(keywords: Set<string>, typeRe: RegExp, builtins: Set<string>, semanticPrefix: RegExp | null): Rule[] {
  const word = (text: string): string => {
    if (keywords.has(text)) return "tok-keyword";
    if (typeRe.test(text)) return "tok-type";
    if (builtins.has(text)) return "tok-builtin";
    if (text.startsWith("gl_") || (semanticPrefix && semanticPrefix.test(text))) return "tok-builtin";
    if (/^[A-Z][A-Z0-9_]+$/.test(text) && text.length > 1) return "tok-constant";
    return "tok-ident";
  };
  return [
    { re: /\/\/[^\n]*|\/\*[\s\S]*?\*\//y, cls: "tok-comment" },
    { re: /#[^\n]*/y, cls: "tok-preprocessor" },
    { re: /"(?:[^"\\\n]|\\.)*"/y, cls: "tok-string" },
    { re: /(?:0[xX][0-9a-fA-F]+|\d+\.\d*(?:[eE][+-]?\d+)?|\.\d+(?:[eE][+-]?\d+)?|\d+(?:[eE][+-]?\d+)?)[uUfFhHlL]*/y, cls: "tok-number" },
    { re: /[A-Za-z_]\w*/y, cls: word },
    { re: /[+\-*/%=<>!&|^~?:]+/y, cls: "tok-operator" },
    { re: /[{}()[\];,.]/y, cls: "tok-punct" },
    { re: /\s+/y, cls: "" },
    { re: /[\s\S]/y, cls: "" },
  ];
}

const SPIRV_RULES: Rule[] = [
  { re: /;[^\n]*/y, cls: "tok-comment" },
  { re: /"(?:[^"\\]|\\.)*"/y, cls: "tok-string" },
  { re: /%[\w.]+/y, cls: "tok-id" },
  { re: /Op[A-Za-z0-9]+\b/y, cls: "tok-keyword" },
  { re: /\d+(?:\.\d+)?(?:[eE][+-]?\d+)?\b/y, cls: "tok-number" },
  { re: /[A-Za-z_][\w.]*/y, cls: (t) => (/^[A-Z]/.test(t) ? "tok-type" : "tok-ident") },
  { re: /=/y, cls: "tok-operator" },
  { re: /\s+/y, cls: "" },
  { re: /[\s\S]/y, cls: "" },
];

const RULES: Record<HighlightLanguage, Rule[]> = {
  glsl: cLikeRules(GLSL_KEYWORDS, GLSL_TYPE, GLSL_BUILTINS, null),
  hlsl: cLikeRules(HLSL_KEYWORDS, HLSL_TYPE, HLSL_BUILTINS, /^SV_\w+$/),
  "spirv-asm": SPIRV_RULES,
};

export function escapeHtml(s: string): string {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

/** HTML for `text` with tokens wrapped in <span class="tok-*">. */
export function highlight(text: string, language: HighlightLanguage): string {
  const rules = RULES[language];
  let out = "";
  let pos = 0;
  // The preprocessor rule only applies at the start of a line.
  while (pos < text.length) {
    let matched = false;
    for (const rule of rules) {
      if (rule.cls === "tok-preprocessor" && pos > 0 && text[pos - 1] !== "\n") continue;
      rule.re.lastIndex = pos;
      const m = rule.re.exec(text);
      if (!m || m.index !== pos || m[0].length === 0) continue;
      const cls = typeof rule.cls === "function" ? rule.cls(m[0]) : rule.cls;
      out += cls ? `<span class="${cls}">${escapeHtml(m[0])}</span>` : escapeHtml(m[0]);
      pos += m[0].length;
      matched = true;
      break;
    }
    if (!matched) {
      out += escapeHtml(text[pos]);
      pos++;
    }
  }
  return out;
}

/**
 * Highlighted HTML per text line. Tokens that span lines (block comments) are closed at the
 * newline and reopened on the next line, so each entry is self-contained.
 */
export function highlightLines(text: string, language: HighlightLanguage): string[] {
  const html = highlight(text, language);
  const out: string[] = [];
  let line = "";
  let open: string | null = null;
  const parts = html.split(/(<span class="[^"]*">|<\/span>)/);
  for (const part of parts) {
    if (!part) continue;
    if (part.startsWith("<span")) {
      open = part;
      line += part;
    } else if (part === "</span>") {
      open = null;
      line += part;
    } else {
      const segs = part.split("\n");
      for (let i = 0; i < segs.length; i++) {
        if (i > 0) {
          if (open) line += "</span>";
          out.push(line);
          line = open ?? "";
        }
        line += segs[i];
      }
    }
  }
  if (open) line += "</span>";
  out.push(line);
  return out;
}

export interface CodeEditorOptions {
  language: HighlightLanguage;
  value: string;
  class?: string;
}

/**
 * A code editor: a textarea (transparent text, visible caret) laid over a highlighted copy of
 * its contents, kept in sync on input and scroll, with a line-number gutter, error marks on
 * lines (compile errors), a find bar (Ctrl+F, Enter / Shift+Enter, Escape) and goToLine().
 * Tab inserts four spaces.
 */
export class CodeEditor extends Div {
  readonly textarea: HTMLTextAreaElement;
  private readonly _code: HTMLElement;
  private readonly _pre: HTMLPreElement;
  private readonly _gutter: HTMLPreElement;
  private readonly _findBar: HTMLDivElement;
  private readonly _findInput: HTMLInputElement;
  private readonly _findCount: HTMLSpanElement;
  private _language: HighlightLanguage;
  private _pending: ReturnType<typeof setTimeout> | null = null;
  private _errors = new Map<number, string>();
  private _lineCount = 0;

  constructor(parent: Widget | null, options: CodeEditorOptions) {
    super(parent, { class: `code-editor${options.class ? ` ${options.class}` : ""}` });
    this._language = options.language;
    this._gutter = document.createElement("pre");
    this._gutter.className = "code-editor-gutter";
    this._gutter.setAttribute("aria-hidden", "true");
    this.element.appendChild(this._gutter);
    this._pre = document.createElement("pre");
    this._pre.className = "code-editor-highlight";
    this._pre.setAttribute("aria-hidden", "true");
    this._code = document.createElement("code");
    this._pre.appendChild(this._code);
    this.element.appendChild(this._pre);

    this.textarea = document.createElement("textarea");
    this.textarea.className = "code-editor-input";
    this.textarea.spellcheck = false;
    this.textarea.wrap = "off";
    this.textarea.setAttribute("autocapitalize", "off");
    this.textarea.setAttribute("autocomplete", "off");
    this.textarea.value = options.value;
    this.element.appendChild(this.textarea);

    // Find bar: hidden until Ctrl+F.
    this._findBar = document.createElement("div");
    this._findBar.className = "code-editor-find";
    this._findBar.style.display = "none";
    this._findInput = document.createElement("input");
    this._findInput.type = "text";
    this._findInput.placeholder = "Find (Enter: next, Shift+Enter: previous, Esc: close)";
    this._findInput.className = "code-editor-find-input";
    this._findBar.appendChild(this._findInput);
    this._findCount = document.createElement("span");
    this._findCount.className = "code-editor-find-count";
    this._findBar.appendChild(this._findCount);
    const close = document.createElement("span");
    close.className = "code-editor-find-close";
    close.textContent = "✕";
    close.title = "Close (Esc)";
    close.onclick = () => this.hideFind();
    this._findBar.appendChild(close);
    this.element.appendChild(this._findBar);
    this._findInput.addEventListener("input", () => this._updateFindCount());
    this._findInput.addEventListener("keydown", (e: KeyboardEvent) => {
      if (e.key === "Enter") {
        e.preventDefault();
        this.findNext(!e.shiftKey);
      } else if (e.key === "Escape") {
        e.preventDefault();
        this.hideFind();
      }
    });

    this.textarea.addEventListener("input", () => {
      if (this._errors.size) {
        this._errors.clear();   // the text changed: the marks no longer point at the right lines
      }
      this._scheduleHighlight();
    });
    this.textarea.addEventListener("scroll", () => this._syncScroll());
    this.textarea.addEventListener("keydown", (e: KeyboardEvent) => {
      if ((e.ctrlKey || e.metaKey) && (e.key === "f" || e.key === "F")) {
        e.preventDefault();
        this.showFind();
        return;
      }
      if (e.key === "Escape" && this._findBar.style.display !== "none") {
        this.hideFind();
        return;
      }
      if (e.key !== "Tab") return;
      e.preventDefault();
      const t = this.textarea;
      const start = t.selectionStart;
      const end = t.selectionEnd;
      t.value = `${t.value.substring(0, start)}    ${t.value.substring(end)}`;
      t.selectionStart = t.selectionEnd = start + 4;
      this._scheduleHighlight();
    });
    this._highlight();
  }

  get value(): string {
    return this.textarea.value;
  }

  set value(v: string) {
    this.textarea.value = v;
    this._errors.clear();
    this._highlight();
  }

  get language(): HighlightLanguage {
    return this._language;
  }

  set language(l: HighlightLanguage) {
    this._language = l;
    this._highlight();
  }

  /** Marks lines (1-based) with an error message each; an empty map clears the marks. */
  setErrors(errors: Map<number, string>): void {
    this._errors = new Map(errors);
    this._highlight();
  }

  /** Places the caret on a line (1-based), selects it and scrolls it into view. */
  goToLine(line: number): void {
    const lines = this.textarea.value.split("\n");
    const index = Math.min(Math.max(1, line), lines.length) - 1;
    let start = 0;
    for (let i = 0; i < index; i++) start += lines[i].length + 1;
    const end = start + lines[index].length;
    this.textarea.focus();
    this.textarea.setSelectionRange(start, end);
    this._scrollToLine(index);
  }

  showFind(): void {
    this._findBar.style.display = "";
    const selected = this.textarea.value.substring(this.textarea.selectionStart, this.textarea.selectionEnd);
    if (selected && !selected.includes("\n")) this._findInput.value = selected;
    this._findInput.focus();
    this._findInput.select();
    this._updateFindCount();
  }

  hideFind(): void {
    this._findBar.style.display = "none";
    this.textarea.focus();
  }

  /** Selects the next (or previous) occurrence of the find text, wrapping around. */
  findNext(forward = true): void {
    const needle = this._findInput.value;
    if (!needle) return;
    const text = this.textarea.value;
    const lower = text.toLowerCase();
    const n = needle.toLowerCase();
    let at: number;
    if (forward) {
      at = lower.indexOf(n, this.textarea.selectionEnd);
      if (at < 0) at = lower.indexOf(n);
    } else {
      at = lower.lastIndexOf(n, Math.max(0, this.textarea.selectionStart - 1));
      if (at < 0) at = lower.lastIndexOf(n);
    }
    if (at < 0) return;
    this.textarea.setSelectionRange(at, at + needle.length);
    this._scrollToLine(text.substring(0, at).split("\n").length - 1);
    this._updateFindCount();
  }

  private _updateFindCount(): void {
    const needle = this._findInput.value.toLowerCase();
    if (!needle) {
      this._findCount.textContent = "";
      return;
    }
    let count = 0;
    let at = -1;
    const lower = this.textarea.value.toLowerCase();
    while ((at = lower.indexOf(needle, at + 1)) >= 0) count++;
    this._findCount.textContent = count ? `${count} match${count === 1 ? "" : "es"}` : "no matches";
  }

  private _scrollToLine(index: number): void {
    const lineHeight = this.textarea.scrollHeight / Math.max(1, this._lineCount + 1);
    const target = index * lineHeight - this.textarea.clientHeight / 2;
    this.textarea.scrollTop = Math.max(0, target);
    this._syncScroll();
  }

  private _scheduleHighlight(): void {
    if (this._pending) return;
    this._pending = setTimeout(() => {
      this._pending = null;
      this._highlight();
    }, 40);
  }

  private _highlight(): void {
    // One span per line, so error lines can be marked; a trailing newline keeps the block as
    // tall as the textarea's last empty line.
    const lines = highlightLines(`${this.textarea.value}\n`, this._language);
    this._lineCount = Math.max(1, this.textarea.value.split("\n").length);
    let html = "";
    let gutter = "";
    const width = String(this._lineCount).length;
    for (let i = 0; i < lines.length; i++) {
      const n = i + 1;
      const error = this._errors.get(n);
      html += error ? `<span class="code-editor-line code-editor-line-error" title="${escapeHtml(error)}">${lines[i]}</span>\n` : `${lines[i]}\n`;
      if (n <= this._lineCount) gutter += error ? `<span class="code-editor-lineno-error" title="${escapeHtml(error)}">${String(n).padStart(width)}</span>\n` : `${String(n).padStart(width)}\n`;
    }
    this._code.innerHTML = html;
    this._gutter.innerHTML = gutter;
    this.element.style.setProperty("--gutter-width", `${width + 2}ch`);
    this._syncScroll();
  }

  private _syncScroll(): void {
    this._pre.scrollTop = this.textarea.scrollTop;
    this._pre.scrollLeft = this.textarea.scrollLeft;
    this._gutter.scrollTop = this.textarea.scrollTop;
  }
}

/**
 * Lines and messages from a compiler log: glslangValidator ("ERROR: file:12: message"), dxc
 * ("file:12:34: error: message") and spirv-as ("error: 12: 34: message"). The first message of
 * a line wins; lines outside the source are ignored by the editor.
 */
export function parseCompileErrors(log: string): Map<number, string> {
  const errors = new Map<number, string>();
  const add = (line: number, message: string): void => {
    if (line > 0 && !errors.has(line)) errors.set(line, message.trim());
  };
  for (const raw of log.split("\n")) {
    const l = raw.trim();
    let m = /^ERROR: .*?:(\d+): (.*)$/.exec(l);          // glslang
    if (m) { add(Number(m[1]), m[2]); continue; }
    m = /^.*?:(\d+):\d+: (?:error|warning): (.*)$/.exec(l);  // dxc / clang style
    if (m) { add(Number(m[1]), m[2]); continue; }
    m = /^error: (\d+): \d+: (.*)$/.exec(l);           // spirv-as
    if (m) add(Number(m[1]), m[2]);
  }
  return errors;
}
