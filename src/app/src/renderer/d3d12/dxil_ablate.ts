// Shader ablation for Direct3D 12: variants of a DXIL module with one part of a shader stage taken
// out, so that timing a draw with each variant measures what the part costs (dxinsp_replay's
// ablation, src/d3d12/replay/src/dx_measure.cpp). The counterpart of vulkan/spirv_ablate.ts, with
// the same parts and the same rules, for a different kind of module.
//
// There is no editor for DXIL, which is LLVM 3.7 bitcode in a signed container. There are both
// halves of one, though: dxcompiler disassembles a module to LLVM IR as text, and assembles that
// text back into a container and signs it (`dxinsp_shader --assemble`, AssembleDxil in
// src/d3d12/src/shader_reflect.cpp). So a variant is made by editing the text, and the text of a
// DXIL module is an easy thing to edit: dxc has inlined every function into the entry point and
// scalarized every vector, so a shader is one function of scalar SSA values.
//
// That shapes the two things that differ from SPIR-V:
//
//   * A value is not replaced where it is defined but where it is *used*. Unnamed values in LLVM
//     text are numbered in order (%1, %2, ...), so removing an instruction would mean renumbering
//     everything after it. Instead the uses of a part's values are given a stand-in, the part's
//     own instructions are left in place with nothing reading them, and the driver's compiler
//     drops them — which it would have had to do for the replaced definition's operands anyway.
//   * Functions no longer exist, so a function part is every instruction whose debug location was
//     inlined from it (the `inlinedAt` chain of its !DILocation). A module with no debug
//     information has no functions or lines to measure, only its textures and the stage whole.
//
// The stand-in has to be something the compiler cannot fold: a constant would let it compute
// everything downstream at compile time and charge the saving to the part. It is a value the
// invocation already reads — an input the pixel shader loads, the thread id a compute shader asks
// for — loaded once more at the top of the function under a name, so nothing is renumbered and the
// signature's use of the input is unchanged. Where the shader reads nothing of the kind, a constant
// stands in: 0.5, 1 for integers, never zero.
//
// Values that decide control flow are never replaced, nor anything they are computed from: branch
// conditions, switch selectors, discards, and a pixel shader's depth and coverage keep their
// values, so an ablation does not skip other work by sending the shader down another path.
import type { AblationPart } from "../vulkan/spirv_ablate.js";

/** A variant of the module: the part it takes out and the module's text without it. */
export interface DxilAblationVariant extends AblationPart {
  /** The edited module, as LLVM IR text for `dxinsp_shader --assemble`. */
  text: string;
  /** Uses rewritten. */
  edits: number;
  /** Indices into the plan's variants of the other parts whose values reach this one (for lines). */
  upstream: number[];
}

export interface DxilAblationPlan {
  variants: DxilAblationVariant[];
  /** Parts considered that have no variant, and why. */
  skipped: (AblationPart & { reason: string })[];
}

export interface DxilAblationLimits {
  functions?: number;
  lines?: number;
  textures?: number;
}

// ---------------------------------------------------------------------------------------------
// The module, as far as ablation needs to read it

/** A value or a label: %name, %12, %"quoted name". */
const NAME = /%(?:"(?:[^"\\]|\\.)*"|[-a-zA-Z$._0-9]+)/g;

const SCALAR = new Set(["float", "half", "double", "i1", "i8", "i16", "i32", "i64"]);
const BINARY = new Set(["add", "fadd", "sub", "fsub", "mul", "fmul", "udiv", "sdiv", "fdiv", "urem", "srem", "frem", "shl", "lshr", "ashr", "and", "or", "xor"]);
const CAST = new Set(["trunc", "zext", "sext", "fptrunc", "fpext", "fptoui", "fptosi", "uitofp", "sitofp", "bitcast", "ptrtoint", "inttoptr", "addrspacecast"]);
const FLAGS = new Set(["fast", "nnan", "ninf", "nsz", "arcp", "nuw", "nsw", "exact"]);

/** dx.op functions that read a texture: the srv handle is their first operand after the opcode. */
const TEXTURE_READS = new Set([
  "sample", "sampleBias", "sampleLevel", "sampleGrad", "sampleCmp", "sampleCmpLevelZero", "sampleCmpLevel", "sampleCmpBias", "sampleCmpGrad",
  "textureLoad", "textureGather", "textureGatherCmp", "textureGatherRaw",
]);

/** dx.op functions that write a resource, with where their value operands start and how many there are. */
const STORES: Record<string, { first: number; count: number }> = {
  bufferStore: { first: 4, count: 4 },
  textureStore: { first: 5, count: 4 },
  rawBufferStore: { first: 4, count: 4 },
  textureStoreSample: { first: 5, count: 4 },
};

/** Output semantics a pixel shader keeps: they change which fragments are shaded at all. */
const KEPT_OUTPUTS = new Set(["DEPTH", "DEPTHGE", "DEPTHLE", "COVERAGE", "STENCILREF"]);

interface Instruction {
  /** Index into the module's lines. */
  at: number;
  /** The instruction without its trailing comment. */
  code: string;
  /** The value it defines, with its %, or null. */
  result: string | null;
  /** The type of that value when it is a scalar the planner can stand in for. */
  type: string | null;
  opcode: string;
  /** The values it reads (not labels, not its own result). */
  operands: string[];
  /** For a call: the function's name without @, and its arguments' text. */
  callee: string | null;
  args: string[];
  /** Its !dbg location id, or null. */
  dbg: number | null;
  /** The block it is in, by order. */
  block: number;
  /** llvm.dbg.* calls read values without using them. */
  debugOnly: boolean;
}

interface Location {
  line: number;
  scope: number;
  inlinedAt: number | null;
}

interface Scope {
  /** "DISubprogram", "DILexicalBlock", ... */
  kind: string;
  name: string;
  file: number | null;
  parent: number | null;
}

/** Splits at top-level commas: not inside (), [], {} or <>, nor inside a quoted name. */
function splitTopLevel(text: string): string[] {
  const out: string[] = [];
  let depth = 0;
  let quoted = false;
  let start = 0;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (quoted) {
      if (c === "\\") i++;
      else if (c === '"') quoted = false;
      continue;
    }
    if (c === '"') quoted = true;
    else if (c === "(" || c === "[" || c === "{" || c === "<") depth++;
    else if (c === ")" || c === "]" || c === "}" || c === ">") depth--;
    else if (c === "," && depth === 0) {
      out.push(text.slice(start, i).trim());
      start = i + 1;
    }
  }
  const last = text.slice(start).trim();
  if (last) out.push(last);
  return out;
}

/** The line without the comment the disassembler puts after it; a ; inside a quoted name is kept. */
function stripComment(line: string): string {
  let quoted = false;
  for (let i = 0; i < line.length; i++) {
    const c = line[i];
    if (quoted) {
      if (c === "\\") i++;
      else if (c === '"') quoted = false;
    } else if (c === '"') quoted = true;
    else if (c === ";") return line.slice(0, i).trimEnd();
  }
  return line.trimEnd();
}

/** The index of the ) that closes the ( at `open`. */
function closing(text: string, open: number): number {
  let depth = 0;
  let quoted = false;
  for (let i = open; i < text.length; i++) {
    const c = text[i];
    if (quoted) {
      if (c === "\\") i++;
      else if (c === '"') quoted = false;
      continue;
    }
    if (c === '"') quoted = true;
    else if (c === "(") depth++;
    else if (c === ")" && --depth === 0) return i;
  }
  return -1;
}

class Module {
  readonly lines: string[];
  readonly instructions: Instruction[] = [];
  readonly byResult = new Map<string, Instruction>();
  readonly structs = new Map<string, string[]>();
  readonly locations = new Map<number, Location>();
  readonly scopes = new Map<number, Scope>();
  readonly files = new Map<number, string>();
  /** Metadata nodes that are plain tuples, as their elements' text. */
  readonly tuples = new Map<number, string[]>();
  /** Where the entry function's first instruction is, for the stand-in to go before. */
  entryStart = -1;
  entryName = "";
  functions = 0;
  /** Blocks by label, in order; the entry block is 0 and has no label. */
  readonly blockOf = new Map<string, number>();
  /** Output signature elements by id: their system value name ("TARGET", "DEPTH", ...). */
  readonly outputs: string[] = [];

  constructor(text: string) {
    this.lines = text.split(/\r?\n/);
    this._readHeader();
    this._readBody();
    this._readMetadata();
  }

  /**
   * The signatures the disassembler prints as comments at the top. The order of the rows is the
   * order of the element ids storeOutput names.
   */
  private _readHeader(): void {
    let inOutput = false;
    let read = false;
    for (const line of this.lines) {
      if (!line.startsWith(";")) {
        if (line.trim()) break;
        continue;
      }
      // The first table only: the runtime information further down prints the signatures again, in other columns.
      if (/^; Output signature:/.test(line)) { inOutput = !read; read = true; continue; }
      if (/^; (Input signature|shader (debug name|hash)|Pipeline Runtime|Buffer Definitions|Resource Bindings)/.test(line)) { inOutput = false; continue; }
      if (!inOutput) continue;
      // ; SV_Target                0   xyzw        0   TARGET   float   xyzw
      const row = /^;\s+(\S+)\s+(\d+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)/.exec(line);
      if (row && row[1] !== "Name" && !/^-+$/.test(row[1])) this.outputs.push(row[5].toUpperCase());
    }
  }

  private _readBody(): void {
    let inFunction = false;
    let block = 0;
    this.lines.forEach((raw, at) => {
      const struct = /^(%(?:"(?:[^"\\]|\\.)*"|[-a-zA-Z$._0-9]+)) = type \{(.*)\}\s*$/.exec(raw);
      if (struct) {
        this.structs.set(struct[1], splitTopLevel(struct[2]));
        return;
      }
      if (raw.startsWith("define ")) {
        this.functions++;
        inFunction = true;
        block = 0;
        const name = /@(?:"((?:[^"\\]|\\.)*)"|([-a-zA-Z$._0-9]+))\s*\(/.exec(raw);
        this.entryName = name ? name[1] ?? name[2] : "";
        this.entryStart = -1;
        return;
      }
      if (!inFunction) return;
      if (raw.startsWith("}")) {
        inFunction = false;
        return;
      }
      // A block's label: `name:` at the start of the line, or for a numbered block only the
      // comment the printer writes where the label would be, `; <label>:9`.
      const numbered = /^; <label>:(\d+)/.exec(raw);
      if (numbered) {
        block++;
        this.blockOf.set(`%${numbered[1]}`, block);
        return;
      }
      const code = stripComment(raw);
      if (!code.trim()) return;
      const label = /^(?:"((?:[^"\\]|\\.)*)"|([-a-zA-Z$._0-9]+)):/.exec(code);
      if (label) {
        block++;
        this.blockOf.set(`%${label[1] !== undefined ? `"${label[1]}"` : label[2]}`, block);
        return;
      }
      if (!/^\s/.test(raw)) return;   // not an instruction
      if (this.entryStart < 0) this.entryStart = at;
      const ins = this._instruction(code.trim(), at, block);
      this.instructions.push(ins);
      if (ins.result) this.byResult.set(ins.result, ins);
    });
  }

  private _instruction(code: string, at: number, block: number): Instruction {
    let body = code;
    let result: string | null = null;
    const assign = /^(%(?:"(?:[^"\\]|\\.)*"|[-a-zA-Z$._0-9]+)) = /.exec(code);
    if (assign) {
      result = assign[1];
      body = code.slice(assign[0].length);
    }
    const dbgMatch = /, !dbg !(\d+)\s*$/.exec(body);
    const dbg = dbgMatch ? Number(dbgMatch[1]) : null;
    const words = body.split(/\s+/);
    let opcode = words[0];
    if (opcode === "tail" || opcode === "musttail" || opcode === "notail") opcode = words[1];

    let callee: string | null = null;
    let args: string[] = [];
    if (opcode === "call") {
      const fn = /@(?:"((?:[^"\\]|\\.)*)"|([-a-zA-Z$._0-9]+))\s*\(/.exec(body);
      if (fn) {
        callee = fn[1] ?? fn[2];
        const open = fn.index + fn[0].length - 1;
        const close = closing(body, open);
        if (close > open) args = splitTopLevel(body.slice(open + 1, close));
      }
    }
    const debugOnly = callee !== null && callee.startsWith("llvm.dbg.");

    // What it reads: every value name in it that is not its own result. Labels are taken out by
    // the caller, which is the only one that knows which names are labels.
    const operands: string[] = [];
    for (const m of body.matchAll(NAME)) operands.push(m[0]);

    return { at, code, result, type: result ? this._resultType(opcode, body) : null, opcode, operands, callee, args, dbg, block, debugOnly };
  }

  /** The type of what an instruction defines, when it is a scalar; null for anything else. */
  private _resultType(opcode: string, body: string): string | null {
    const rest = body.slice(body.indexOf(opcode) + opcode.length).trim();
    const words = rest.split(/\s+/);
    let type: string | null = null;
    if (BINARY.has(opcode)) {
      type = words.find((w) => !FLAGS.has(w)) ?? null;
    } else if (opcode === "icmp" || opcode === "fcmp") {
      type = "i1";
    } else if (opcode === "select") {
      const parts = splitTopLevel(rest);
      type = parts[1]?.split(/\s+/)[0] ?? null;
    } else if (CAST.has(opcode)) {
      const to = / to (\S+?)(?:,|$)/.exec(rest);
      type = to ? to[1] : null;
    } else if (opcode === "phi" || opcode === "load") {
      type = words[0]?.replace(/,$/, "") ?? null;
    } else if (opcode === "call") {
      const ret = /^(.*?)\s+@/.exec(rest);
      type = ret ? ret[1].trim() : null;
    } else if (opcode === "extractvalue") {
      const parts = splitTopLevel(rest);
      const aggregate = parts[0]?.split(/\s+/)[0];
      const index = Number(parts[1]);
      const fields = aggregate ? this.structs.get(aggregate) : undefined;
      type = fields && Number.isInteger(index) ? fields[index] ?? null : null;
    }
    return type && SCALAR.has(type) ? type : null;
  }

  private _readMetadata(): void {
    for (const raw of this.lines) {
      const node = /^!(\d+) = (distinct )?(.*)$/.exec(raw);
      if (!node) continue;
      const id = Number(node[1]);
      const body = node[3];
      const field = (name: string): string | null => {
        const m = new RegExp(`\\b${name}: ([^,)]+)`).exec(body);
        return m ? m[1].trim() : null;
      };
      const ref = (name: string): number | null => {
        const v = field(name);
        return v && /^!\d+$/.test(v) ? Number(v.slice(1)) : null;
      };
      if (body.startsWith("!DILocation(")) {
        this.locations.set(id, { line: Number(field("line") ?? 0), scope: ref("scope") ?? -1, inlinedAt: ref("inlinedAt") });
      } else if (body.startsWith("!DIFile(")) {
        const name = /filename: "((?:[^"\\]|\\.)*)"/.exec(body);
        this.files.set(id, name ? name[1].replace(/\\5C/gi, "\\").replace(/\\\\/g, "\\") : "");
      } else if (/^!DI(Subprogram|LexicalBlock|LexicalBlockFile|Namespace)\(/.test(body)) {
        const name = /\bname: "((?:[^"\\]|\\.)*)"/.exec(body);
        this.scopes.set(id, { kind: body.slice(1, body.indexOf("(")), name: name ? name[1] : "", file: ref("file"), parent: ref("scope") });
      } else if (body.startsWith("!{")) {
        this.tuples.set(id, splitTopLevel(body.slice(2, body.lastIndexOf("}"))));
      }
    }
  }

  /** The function a scope belongs to, through its lexical blocks. */
  subprogramOf(scope: number): { id: number; scope: Scope } | null {
    for (let id: number | null = scope, guard = 0; id !== null && guard < 64; guard++) {
      const s = this.scopes.get(id);
      if (!s) return null;
      if (s.kind === "DISubprogram") return { id, scope: s };
      id = s.parent;
    }
    return null;
  }

  /** The file a scope is in: its own, else its function's. */
  fileOf(scope: number): string {
    for (let id: number | null = scope, guard = 0; id !== null && guard < 64; guard++) {
      const s = this.scopes.get(id);
      if (!s) return "";
      if (s.file !== null) return this.files.get(s.file) ?? "";
      id = s.parent;
    }
    return "";
  }

  /**
   * The functions an instruction's location runs through, innermost first: the one its line is in,
   * then the ones that one was inlined into.
   */
  functionChain(dbg: number | null): { id: number; name: string }[] {
    const out: { id: number; name: string }[] = [];
    for (let id = dbg, guard = 0; id !== null && guard < 64; guard++) {
      const loc = this.locations.get(id);
      if (!loc) break;
      const fn = this.subprogramOf(loc.scope);
      if (fn) out.push({ id: fn.id, name: fn.scope.name });
      id = loc.inlinedAt;
    }
    return out;
  }
}

/** The value an operand names: the last name in it, since its type may be a named one (%dx.types.Handle %tex). */
function valueIn(operand: string | undefined): string | null {
  const names = operand?.match(NAME);
  return names?.length ? names[names.length - 1] : null;
}

function baseName(file: string): string {
  return file.replace(/^.*[\\/]/, "");
}

// ---------------------------------------------------------------------------------------------
// What may not be replaced

/**
 * The values control flow depends on, and everything they are computed from: through operands,
 * phis, and what is stored to the local arrays they are loaded from.
 */
function controlSlice(m: Module, labels: Set<string>, stage: string): Set<string> {
  const slice = new Set<string>();
  const work: string[] = [];
  const add = (v: string): void => {
    if (labels.has(v) || slice.has(v)) return;
    slice.add(v);
    work.push(v);
  };
  // What is stored where, by the pointer's base (an alloca, or a global): a load from the same base
  // reads it.
  const baseOf = (pointer: string): string => {
    for (let v = pointer, guard = 0; guard < 16; guard++) {
      const def = m.byResult.get(v);
      if (!def || (def.opcode !== "getelementptr" && def.opcode !== "bitcast")) return v;
      v = def.operands[0] ?? v;
    }
    return pointer;
  };
  const stored = new Map<string, string[]>();
  for (const ins of m.instructions) {
    if (ins.opcode !== "store") continue;
    // store T %value, T* %pointer
    const parts = splitTopLevel(ins.code.slice(ins.code.indexOf("store") + 5));
    const value = valueIn(parts[0]);
    const pointer = valueIn(parts[1]);
    if (!value || !pointer) continue;
    const base = baseOf(pointer);
    let list = stored.get(base);
    if (!list) stored.set(base, (list = []));
    list.push(value);
  }

  for (const ins of m.instructions) {
    if (ins.opcode === "br" || ins.opcode === "switch" || ins.opcode === "indirectbr") {
      for (const o of ins.operands) add(o);
    } else if (ins.callee?.startsWith("dx.op.discard")) {
      for (const o of ins.operands) add(o);
    } else if (stage === "fragment" && ins.callee?.startsWith("dx.op.storeOutput")) {
      // A pixel shader's depth and coverage decide which fragments are shaded at all.
      const id = Number(ins.args[1]?.split(/\s+/).pop());
      if (KEPT_OUTPUTS.has(m.outputs[id] ?? "")) for (const o of ins.operands) add(o);
    }
  }
  while (work.length) {
    const v = work.pop()!;
    const def = m.byResult.get(v);
    if (!def) continue;
    for (const o of def.operands) add(o);
    if (def.opcode === "load") {
      const pointer = def.operands[0];
      if (pointer) for (const value of stored.get(baseOf(pointer)) ?? []) add(value);
    }
  }
  return slice;
}

// ---------------------------------------------------------------------------------------------
// The stand-in

/** Calls whose result an invocation already has, by what they return. */
const SOURCES: { match: RegExp; type: string }[] = [
  { match: /^dx\.op\.loadInput\.f32$/, type: "float" },
  { match: /^dx\.op\.(threadId|flattenedThreadIdInGroup|threadIdInGroup|groupId)\.i32$/, type: "i32" },
  { match: /^dx\.op\.loadInput\.i32$/, type: "i32" },
  { match: /^dx\.op\.loadInput\.f16$/, type: "half" },
];

const CONSTANTS: Record<string, string> = {
  float: "5.000000e-01", half: "0xH3800", double: "5.000000e-01", i1: "true", i8: "1", i16: "1", i32: "1", i64: "1",
};

/**
 * The instructions that define a stand-in of each type the variant needs, to go at the top of the
 * function, and the operand text that names each. Without a source the constants stand in.
 */
function standIns(m: Module, types: Set<string>): { lines: string[]; operand: Map<string, string> } {
  const operand = new Map<string, string>();
  const lines: string[] = [];
  if (!types.size) return { lines, operand };   // nothing is replaced (the stage variant): nothing to stand in
  let source: Instruction | null = null;
  let sourceType = "";
  for (const candidate of SOURCES) {
    // One whose arguments are all constants: it goes to the top of the function, above whatever a value among them would be.
    source = m.instructions.find((ins) => ins.callee !== null && candidate.match.test(ins.callee) && ins.result !== null && !ins.operands.length) ?? null;
    if (source) {
      sourceType = candidate.type;
      break;
    }
  }
  if (!source) {
    for (const t of types) operand.set(t, CONSTANTS[t] ?? "undef");
    return { lines, operand };
  }
  // The same call once more, under a name: named values take no number, so nothing after it is
  // renumbered, and the call is one the function already makes, so nothing about the signature's
  // use changes. It keeps the original's debug location, which a call in a function with debug
  // information has to have.
  const dbg = source.dbg !== null ? `, !dbg !${source.dbg}` : "";
  const call = source.code.slice(source.code.indexOf("=") + 1).trim().replace(/, !dbg !\d+\s*$/, "");
  const base = "%ablate.src";
  lines.push(`  ${base} = ${call}${dbg}`);
  const isFloat = (t: string): boolean => t === "float" || t === "half" || t === "double";
  const bits = (t: string): number => (t === "half" ? 16 : t === "float" ? 32 : t === "double" ? 64 : Number(t.slice(1)));
  for (const t of types) {
    if (t === sourceType) {
      operand.set(t, base);
      continue;
    }
    const name = `%ablate.${t}`;
    let cast: string;
    if (t === "i1") {
      cast = isFloat(sourceType) ? `fcmp ogt ${sourceType} ${base}, 5.000000e-01` : `icmp ne ${sourceType} ${base}, 0`;
    } else if (isFloat(sourceType) && isFloat(t)) {
      cast = `${bits(t) < bits(sourceType) ? "fptrunc" : "fpext"} ${sourceType} ${base} to ${t}`;
    } else if (isFloat(sourceType)) {
      cast = `fptosi ${sourceType} ${base} to ${t}`;
    } else if (isFloat(t)) {
      cast = `uitofp ${sourceType} ${base} to ${t}`;
    } else {
      cast = `${bits(t) < bits(sourceType) ? "trunc" : "zext"} ${sourceType} ${base} to ${t}`;
    }
    lines.push(`  ${name} = ${cast}${dbg}`);
    operand.set(t, name);
  }
  return { lines, operand };
}

// ---------------------------------------------------------------------------------------------
// Writing a variant

/** A name as it appears in the text, made safe to build a RegExp from. */
function escape(text: string): string {
  return text.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

/**
 * The module with every use of `replace` outside the instructions that define them given a
 * stand-in of the value's type, and the operands at `constants` (instruction -> argument indices)
 * made constants. Returns the text and how many uses it rewrote.
 */
function rewrite(m: Module, replace: Set<string>, constants: Map<Instruction, number[]>): { text: string; edits: number } {
  const types = new Set<string>();
  for (const v of replace) {
    const t = m.byResult.get(v)?.type;
    if (t) types.add(t);
  }
  const { lines: prologue, operand } = standIns(m, types);
  const out = m.lines.slice();
  let edits = 0;

  for (const ins of m.instructions) {
    if (ins.debugOnly) continue;
    // An instruction that defines a replaced value is left as it is: nothing reads it any more.
    if (ins.result && replace.has(ins.result)) continue;
    // A phi is where a stand-in cannot always go (its operands have to be defined in the block they
    // come from, which the top of the function is), so it can; but a phi whose every input is the
    // same stand-in is a constant to the compiler, which is what is wanted.
    const used = ins.operands.filter((o) => replace.has(o));
    const positions = constants.get(ins);
    if (!used.length && !positions) continue;

    const raw = out[ins.at];
    const commentAt = stripComment(raw).length;
    let code = raw.slice(0, commentAt);
    const comment = raw.slice(commentAt);

    if (positions && ins.callee) {
      const fn = code.indexOf(`@${ins.callee}`) >= 0 ? code.indexOf(`@${ins.callee}`) : code.indexOf(`@"${ins.callee}"`);
      const open = code.indexOf("(", fn);
      const close = closing(code, open);
      if (open >= 0 && close > open) {
        const args = splitTopLevel(code.slice(open + 1, close));
        for (const p of positions) {
          const arg = args[p];
          if (!arg) continue;
          const space = arg.indexOf(" ");
          const type = space > 0 ? arg.slice(0, space) : "";
          const value = arg.slice(space + 1).trim();
          // Already a constant, or not written at all.
          if (!type || value === "undef" || !value.startsWith("%") || !CONSTANTS[type]) continue;
          args[p] = `${type} ${CONSTANTS[type]}`;
          edits++;
        }
        code = `${code.slice(0, open + 1)}${args.join(", ")}${code.slice(close)}`;
      }
    }
    for (const v of new Set(used)) {
      const t = m.byResult.get(v)?.type;
      const stand = t ? operand.get(t) : undefined;
      if (!stand) continue;
      // The whole name: %1 is not the start of %12.
      const pattern = new RegExp(`${escape(v)}(?![-a-zA-Z$._0-9])`, "g");
      const head = ins.result ? code.indexOf("=") + 1 : 0;
      const before = code.slice(0, head);
      const after = code.slice(head).replace(pattern, () => {
        edits++;
        return stand;
      });
      code = before + after;
    }
    out[ins.at] = code + comment;
  }
  if (prologue.length && m.entryStart >= 0) out.splice(m.entryStart, 0, ...prologue);
  return { text: out.join("\n"), edits };
}

// ---------------------------------------------------------------------------------------------
// Textures

interface TextureBinding {
  name: string;
  space: number;
  register: number;
}

/** The module's SRVs by their range id, from !dx.resources. */
function shaderResourceViews(m: Module): Map<number, TextureBinding> {
  const out = new Map<number, TextureBinding>();
  const named = m.lines.find((l) => l.startsWith("!dx.resources = "));
  const root = named ? /!(\d+)/.exec(named.slice(named.indexOf("{"))) : null;
  const classes = root ? m.tuples.get(Number(root[1])) : undefined;
  const srvs = classes && /^!\d+$/.test(classes[0] ?? "") ? m.tuples.get(Number(classes[0].slice(1))) : undefined;
  for (const entry of srvs ?? []) {
    const fields = /^!\d+$/.test(entry) ? m.tuples.get(Number(entry.slice(1))) : undefined;
    if (!fields || fields.length < 6) continue;
    // !{i32 id, T* undef, !"name", i32 space, i32 lowerBound, i32 rangeSize, ...}
    const number = (f: string): number => Number(f.trim().split(/\s+/).pop());
    const name = /^!"((?:[^"\\]|\\.)*)"$/.exec(fields[2].trim());
    out.set(number(fields[0]), { name: name ? name[1] : "", space: number(fields[3]), register: number(fields[4]) });
  }
  return out;
}

/** The SRV a handle is of, through annotateHandle, by range id or by its binding. */
function textureOf(m: Module, handle: string, srvs: Map<number, TextureBinding>): TextureBinding | null {
  for (let v = handle, guard = 0; guard < 8; guard++) {
    const def = m.byResult.get(v);
    if (!def?.callee) return null;
    const last = (arg: string | undefined): string => arg?.trim().split(/\s+/).pop() ?? "";
    if (def.callee.startsWith("dx.op.annotateHandle")) {
      v = valueIn(def.args[1]) ?? "";
      continue;
    }
    if (def.callee.startsWith("dx.op.createHandleFromBinding")) {
      // %dx.types.ResBind { i32 lower, i32 upper, i32 space, i8 class }
      const bind = /\{\s*i32 (\d+), i32 (\d+), i32 (\d+), i8 (\d+)\s*\}/.exec(def.args[1] ?? "");
      if (!bind || bind[4] !== "0") return null;
      for (const t of srvs.values()) if (t.space === Number(bind[3]) && t.register === Number(bind[1])) return t;
      return { name: "", space: Number(bind[3]), register: Number(bind[1]) };
    }
    if (def.callee.startsWith("dx.op.createHandle")) {
      if (last(def.args[1]) !== "0") return null;   // not an SRV
      return srvs.get(Number(last(def.args[2]))) ?? null;
    }
    return null;
  }
  return null;
}

// ---------------------------------------------------------------------------------------------

/** What an instruction costs, roughly, to rank the lines worth a variant each. */
function weight(ins: Instruction): number {
  if (ins.debugOnly || !ins.result) return 0;
  if (ins.callee) {
    const op = /^dx\.op\.([A-Za-z]+)/.exec(ins.callee)?.[1] ?? "";
    if (TEXTURE_READS.has(op)) return 16;
    if (op === "unary" || op === "binary" || op === "tertiary" || op === "dot2" || op === "dot3" || op === "dot4") return 4;
    return 1;
  }
  if (ins.opcode === "fdiv" || ins.opcode === "sdiv" || ins.opcode === "udiv" || ins.opcode === "frem") return 3;
  if (ins.opcode === "extractvalue" || ins.opcode === "phi" || CAST.has(ins.opcode)) return 0.25;
  return 1;
}

/**
 * Variants of a DXIL module, given as its disassembly, for one stage. `stage` is the capture's name
 * for it: only "fragment" and "compute" stages are measured, as on Vulkan, since another stage's
 * outputs decide what is rasterized.
 */
export function planDxilAblation(disassembly: string, stage: string, entryPoint: string, limits: DxilAblationLimits = {}): DxilAblationPlan {
  const plan: DxilAblationPlan = { variants: [], skipped: [] };
  const stagePart: AblationPart = { kind: "stage", name: `${stage}: ${entryPoint}` };
  let m: Module;
  try {
    m = new Module(disassembly);
  } catch {
    plan.skipped.push({ ...stagePart, reason: "the module's disassembly could not be read" });
    return plan;
  }
  if (m.functions !== 1 || m.entryStart < 0) {
    plan.skipped.push({ ...stagePart, reason: m.functions ? "the module defines more than one function (a library), which is not measured" : "the disassembly holds no function" });
    return plan;
  }
  if (stage !== "fragment" && stage !== "compute") {
    plan.skipped.push({ ...stagePart, reason: "only pixel and compute stages are measured: another stage's outputs decide what is rasterized" });
    return plan;
  }

  // Labels are named like values; an operand that is one is not a value read.
  const labels = new Set(m.blockOf.keys());
  for (const ins of m.instructions) ins.operands = ins.operands.filter((o) => o !== ins.result && !labels.has(o) && m.byResult.has(o));
  const slice = controlSlice(m, labels, stage);
  // What only reads is not work: an input, a constant buffer's row, the thread id, and the fields
  // taken out of one. They cost nothing to take out, and a line made of them (the entry point's own
  // signature is one) would otherwise be measured as whatever is computed from the inputs — all of it.
  const READS = /^dx\.op\.(loadInput|cbufferLoad|cbufferLoadLegacy|threadId|groupId|threadIdInGroup|flattenedThreadIdInGroup|viewID|primitiveID|sampleIndex|coverage|innerCoverage|isFrontFace|createHandle|createHandleFromBinding|createHandleFromHeap|annotateHandle)\b/;
  const isRead = (ins: Instruction): boolean => {
    if (ins.callee) return READS.test(ins.callee);
    if (ins.opcode !== "extractvalue") return false;
    const from = m.byResult.get(ins.operands[0] ?? "");
    return !!from?.callee && READS.test(from.callee);
  };
  const replaceable = (ins: Instruction): boolean => !!ins.result && !!ins.type && !slice.has(ins.result) && !ins.debugOnly && !isRead(ins);

  interface Candidate { part: AblationPart; replace: Set<string>; instructions: Instruction[] }
  const candidates: Candidate[] = [];

  // The stage: what it writes made constant, so the driver has nothing to compute them from. A
  // pixel shader keeps the depth and coverage it writes.
  {
    const constants = new Map<Instruction, number[]>();
    for (const ins of m.instructions) {
      if (!ins.callee) continue;
      const op = /^dx\.op\.([A-Za-z]+)/.exec(ins.callee)?.[1] ?? "";
      if (stage === "fragment" && op === "storeOutput") {
        const id = Number(ins.args[1]?.split(/\s+/).pop());
        if (!KEPT_OUTPUTS.has(m.outputs[id] ?? "")) constants.set(ins, [ins.args.length - 1]);
      } else if (stage === "compute" && STORES[op]) {
        constants.set(ins, Array.from({ length: STORES[op].count }, (_, k) => STORES[op].first + k));
      }
    }
    const written = constants.size ? rewrite(m, new Set(), constants) : null;
    if (!written || !written.edits) plan.skipped.push({ ...stagePart, reason: "the stage writes no outputs that can be left out" });
    else plan.variants.push({ ...stagePart, text: written.text, edits: written.edits, upstream: [] });
  }

  // Functions and lines, from where each instruction's debug location says it came from.
  const hasLocations = m.instructions.some((ins) => ins.dbg !== null && m.locations.has(ins.dbg));
  if (hasLocations) {
    const entry = m.instructions.map((ins) => m.functionChain(ins.dbg)).find((c) => c.length)?.slice(-1)[0] ?? null;
    const functions = new Map<number, { name: string; instructions: Instruction[]; cost: number }>();
    const lines = new Map<string, { functionName: string; file: string; line: number; instructions: Instruction[]; cost: number }>();
    for (const ins of m.instructions) {
      if (ins.debugOnly) continue;
      const chain = m.functionChain(ins.dbg);
      if (!chain.length) continue;
      const cost = weight(ins);
      // Every function the instruction was inlined from, but for the entry point itself: what a
      // function costs includes what it calls.
      for (const fn of new Map(chain.map((f) => [f.id, f])).values()) {
        if (entry && fn.id === entry.id) continue;
        let f = functions.get(fn.id);
        if (!f) functions.set(fn.id, (f = { name: fn.name, instructions: [], cost: 0 }));
        f.instructions.push(ins);
        f.cost += cost;
      }
      const loc = m.locations.get(ins.dbg!)!;
      if (!loc.line) continue;
      const file = baseName(m.fileOf(loc.scope));
      const key = `${chain[0].id}|${file}|${loc.line}`;
      let l = lines.get(key);
      if (!l) lines.set(key, (l = { functionName: chain[0].name, file, line: loc.line, instructions: [], cost: 0 }));
      l.instructions.push(ins);
      l.cost += cost;
    }

    for (const f of [...functions.values()].sort((a, b) => b.cost - a.cost).slice(0, limits.functions ?? 16)) {
      const part: AblationPart = { kind: "function", name: f.name, functionName: f.name };
      // Only what the function computes from its own values. dxc's optimizer moves arithmetic
      // across the boundaries inlining removed: `Blurred(uv) * (0.5 + n)` becomes a multiply of n
      // by the 1/16 that ended Blurred, carrying Blurred's line and none of Blurred's values. Giving
      // that a stand-in would leave nothing reading n, and the function would be charged with all
      // of Fbm. So a value with work from outside the function among its operands keeps its value:
      // the function's own values are replaced where it reads them, and the outside work lives on.
      const own = new Set(f.instructions);
      const mixesOutsideWork = (ins: Instruction): boolean => ins.operands.some((o) => {
        const def = m.byResult.get(o);
        return !!def && !own.has(def) && !isRead(def);
      });
      const replace = new Set(f.instructions.filter((ins) => replaceable(ins) && !mixesOutsideWork(ins)).map((ins) => ins.result!));
      const controlled = f.instructions.some((ins) => ins.result && slice.has(ins.result));
      if (!replace.size) plan.skipped.push({ ...part, reason: controlled ? "control flow depends on what it returns" : "it computes nothing that can be replaced" });
      else candidates.push({ part, replace, instructions: f.instructions });
    }

    // A loop recurrence: a line that updates, every iteration, a value other lines of the loop
    // read (p = p * 2.0). Replacing it makes every iteration the same, which a compiler hoists out
    // of the loop, so its saving would be the loop's. In SSA that is a value reaching a phi along
    // a back edge, where something of another line reads the phi.
    const users = new Map<string, Instruction[]>();
    for (const ins of m.instructions) {
      if (ins.debugOnly) continue;
      for (const o of ins.operands) {
        let list = users.get(o);
        if (!list) users.set(o, (list = []));
        list.push(ins);
      }
    }
    const lineKey = (ins: Instruction): string => {
      const loc = ins.dbg !== null ? m.locations.get(ins.dbg) : undefined;
      const chain = m.functionChain(ins.dbg);
      return loc && chain.length ? `${chain[0].id}|${loc.line}` : "";
    };
    const recurrence = (instructions: Instruction[]): boolean => instructions.some((ins) => {
      if (!ins.result) return false;
      const here = lineKey(ins);
      return (users.get(ins.result) ?? []).some((phi) => {
        if (phi.opcode !== "phi") return false;
        // [ %value, %block ] pairs: a back edge comes from a block at or after the phi's own.
        const pairs = [...phi.code.matchAll(/\[\s*([^,\]]+),\s*([^\]]+?)\s*\]/g)];
        const back = pairs.some((p) => p[1].trim() === ins.result && (m.blockOf.get(p[2].trim()) ?? -1) >= phi.block);
        return back && (users.get(phi.result ?? "") ?? []).some((u) => lineKey(u) !== here && lineKey(u) !== "");
      });
    });

    for (const l of [...lines.values()].sort((a, b) => b.cost - a.cost).slice(0, limits.lines ?? 32)) {
      const part: AblationPart = { kind: "line", name: `${l.file ? `${l.file}:` : "line "}${l.line}`, functionName: l.functionName, file: l.file, line: l.line };
      const replace = new Set(l.instructions.filter(replaceable).map((ins) => ins.result!));
      const controlled = l.instructions.some((ins) => ins.result && slice.has(ins.result));
      if (!replace.size) {
        plan.skipped.push({ ...part, reason: controlled ? "control flow depends on what the line computes" : "the line computes nothing that can be replaced" });
      } else if (recurrence(l.instructions)) {
        plan.skipped.push({ ...part, reason: "it updates a value that other lines of its loop read every iteration: taking it out would let the compiler hoist the loop's work, and charge that to the line" });
      } else {
        candidates.push({ part, replace, instructions: l.instructions });
      }
    }
  } else {
    plan.skipped.push({ kind: "line", name: "source lines", reason: "the module has no line information (it was not compiled with -Zi or -Zs)" });
  }

  // Textures: every sample, load and gather of one bound texture replaced. These need no debug
  // information, so they are what a shipped shader can be measured by.
  const srvs = shaderResourceViews(m);
  const textures = new Map<string, { binding: TextureBinding; reads: Instruction[] }>();
  for (const ins of m.instructions) {
    if (!ins.callee) continue;
    const op = /^dx\.op\.([A-Za-z]+)/.exec(ins.callee)?.[1] ?? "";
    if (!TEXTURE_READS.has(op)) continue;
    const handle = valueIn(ins.args[1]);
    const binding = handle ? textureOf(m, handle, srvs) : null;
    if (!binding) continue;
    const key = `${binding.space}|${binding.register}`;
    let t = textures.get(key);
    if (!t) textures.set(key, (t = { binding, reads: [] }));
    t.reads.push(ins);
  }
  const extracts = (read: Instruction): Instruction[] => m.instructions.filter((ins) => ins.opcode === "extractvalue" && ins.operands[0] === read.result);
  for (const t of [...textures.values()].sort((a, b) => b.reads.length - a.reads.length).slice(0, limits.textures ?? 16)) {
    const name = t.binding.name || `t${t.binding.register}${t.binding.space ? `, space${t.binding.space}` : ""}`;
    const part: AblationPart = { kind: "texture", name, set: t.binding.space, binding: t.binding.register };
    // What a read returns is a struct, which is taken apart before anything uses it: those are the values.
    const values = t.reads.flatMap(extracts);
    const replace = new Set(values.filter(replaceable).map((ins) => ins.result!));
    if (!replace.size) plan.skipped.push({ ...part, reason: values.length ? "control flow depends on what is read from it" : "nothing reads what it returns" });
    else candidates.push({ part, replace, instructions: [...t.reads, ...values] });
  }

  // Which other parts' values reach each part: through operands, to a fixed point.
  const taint = new Map<string, bigint>();
  candidates.forEach((c, i) => { for (const v of c.replace) taint.set(v, (taint.get(v) ?? 0n) | (1n << BigInt(i))); });
  for (let pass = 0; pass < 16; pass++) {
    let changed = false;
    for (const ins of m.instructions) {
      if (!ins.result || ins.debugOnly) continue;
      let bits = taint.get(ins.result) ?? 0n;
      const before = bits;
      for (const o of ins.operands) bits |= taint.get(o) ?? 0n;
      if (bits !== before) {
        taint.set(ins.result, bits);
        changed = true;
      }
    }
    if (!changed) break;
  }
  // A candidate nothing outside reads has no variant, so where each ends up among the variants is
  // known only once all are written.
  const written = candidates.map((c) => rewrite(m, c.replace, new Map()));
  const indexOf = new Map<number, number>();
  written.forEach((w, i) => { if (w.edits) indexOf.set(i, plan.variants.length + indexOf.size); });
  candidates.forEach((c, i) => {
    if (!written[i].edits) {
      plan.skipped.push({ ...c.part, reason: "nothing outside it reads what it computes" });
      return;
    }
    let bits = 0n;
    for (const ins of c.instructions) for (const o of ins.operands) bits |= taint.get(o) ?? 0n;
    bits &= ~(1n << BigInt(i));
    const upstream: number[] = [];
    for (let k = 0; k < candidates.length; k++) if ((bits & (1n << BigInt(k))) && indexOf.has(k)) upstream.push(indexOf.get(k)!);
    plan.variants.push({ ...c.part, text: written[i].text, edits: written[i].edits, upstream });
  });
  return plan;
}
