// What the shader debugger needs of a shader, and of one invocation of it, whatever language the
// shader was written in.
//
// The debugger has two back ends. `../spirv/` interprets the SPIR-V of a Vulkan capture; `../msl/`
// interprets the Metal Shading Language of a Metal one. Everything above them — the stepping
// (../shader_debugger.ts), the tab (../shader_debugger_view.ts) and the MCP server's debug_shader
// — is written against these two interfaces and never imports either back end.
//
// A "type" and an "id" are numbers each back end reads in its own tables: SPIR-V a result type id
// and a result id, MSL an index into the program's types and declarations. Only the DebugProgram
// turns one into a name, a type name or a value's text.
import type { Value } from "./values.js";
import type { HighlightLanguage } from "../code_editor.js";

/** What a stop is keyed by: source lines ("f:file:line" keys) or instructions ("i:ordinal" keys). */
export type LineMode = "source" | "instruction";

export function sourceKey(file: number, line: number): string {
  return `f:${file}:${line}`;
}

export function instructionKey(ordinal: number): string {
  return `i:${ordinal}`;
}

/** A source file a program was built from, with its text when the capture (or the host) has it. */
export interface DebugSourceFile {
  name: string;
  /** Source text, or null when only the file name was embedded. */
  text: string | null;
  /** The text came from a file on this machine (the launch configuration's source roots), not the module. */
  fromHost?: boolean;
}

export interface DebugLocation {
  file: number;     // index into DebugProgram.files
  line: number;     // 1-based
  column: number;   // 1-based, 0 when unknown
}

/**
 * A place an invocation can be stopped at: a SPIR-V instruction, an MSL statement. `index` is its
 * ordinal in the program, which is what an instruction-mode key and a listing line are made from.
 */
export interface DebugStep {
  readonly index: number;
}

/** One result the stepped instructions produced: what the watch shows as "values on this line". */
export interface StepResult {
  inst: DebugStep;
  id: number;
  value: Value;
}

export interface VariableView {
  id: number;
  name: string;
  type: number;
  value: Value;
  storage: number;
  location?: number;
  builtin?: number;
  set?: number;
  binding?: number;
}

/** A call stack entry, innermost first. */
export interface DebugFrameView {
  name: string;
  /** Where the frame is stopped: the instruction running, or the call an outer frame is inside. */
  step: DebugStep | null;
}

export type InvocationStatus = "running" | "blocked" | "returned" | "discarded" | "error";

/** An element a composite value opens to in the variables table. */
export interface DebugChild {
  name: string;
  type: number;
  value: Value;
}

/**
 * A shader the debugger can step through: its source, where its instructions came from in it, and
 * how to name and print the values its invocations produce.
 */
export interface DebugProgram {
  /** "spirv" or "msl": what the tab says it is stepping, and which panes it offers. */
  readonly kind: "spirv" | "msl";
  /** Stepping modes this program supports; a program with only source offers just "source". */
  readonly modes: readonly LineMode[];
  readonly files: readonly DebugSourceFile[];
  /** Index of the main compilation unit in `files`, -1 when unknown. */
  readonly mainFile: number;
  /** Which highlighter the source pane uses, null for plain text. */
  readonly language: HighlightLanguage | null;
  /** The language as a reader knows it ("GLSL", "HLSL", "Metal Shading Language"), for prose. */
  readonly languageName: string;

  /** Whether the lines have text to show (embedded, or found under the host's source roots). */
  hasSourceText(): boolean;
  /** Where an instruction came from in the source, when the program knows. */
  locationOf(step: DebugStep | null): DebugLocation | null;
  /** Every place a stop can be, as the keys of `mode`: what the gutter marks and a breakpoint goes on. */
  stopKeys(mode: LineMode): Set<string>;

  nameOf(id: number): string;
  typeName(type: number): string;
  /** The type of the value an id holds (a variable's pointee, not the pointer), for a hover. */
  typeOfId(id: number): number;
  /** One line of text for a value: scalars and vectors in full, composites shortened past `limit` elements. */
  valueText(type: number, value: Value | undefined, limit?: number): string;
  /** The rows a composite value opens to in the variables table; null for a value that does not open. */
  children(type: number, value: Value): DebugChild[] | null;
  /** Ids a name refers to (a hover in the source pane), innermost meaning first. */
  idsNamed(name: string): number[];

  /** A step result's type, and whether its name is one the shader gave it or one the back end made up. */
  resultType(r: StepResult): number;
  resultTemporary(r: StepResult): boolean;
  /** Where a variable comes from, for the type column's tooltip ("location 3", "set 0 binding 1", "buffer(2)"). */
  variableWhere(v: VariableView): string;

  /**
   * Instruction mode's listing. `bytes` is what the host disassembles (SPIR-V words); `listing`
   * is the fallback when no disassembler is available, and `mapDisassembly` ties the
   * disassembler's lines to instruction ordinals. All three are absent for a program that only
   * has source, which is why `modes` does not offer instruction mode for one.
   */
  readonly disassembly?: {
    bytes(): Uint8Array;
    listing(): { text: string; number: string; key: string }[];
    mapDisassembly(lines: string[]): { keys: (string | null)[]; numbers: string[] };
  };
}

/** One invocation of a program, stepped by the debugger. */
export interface DebugInvocation {
  readonly program: DebugProgram;
  readonly status: InvocationStatus;
  readonly error: string;
  /** Things the interpreter could not do faithfully: uncaptured resources, unsupported operations. */
  readonly warnings: Set<string>;
  readonly steps: number;
  readonly finished: boolean;
  /** Where it is stopped. */
  readonly current: DebugStep | null;
  /** How deep the call stack is (1 in the entry point). */
  readonly depth: number;

  step(): InvocationStatus;
  /** Results of the instructions executed since the last call. */
  takeResults(): StepResult[];

  /** The call stack, innermost first. */
  callStack(): DebugFrameView[];
  /** The value an id has in a frame (0 is the innermost), or undefined where it has none. */
  valueOf(id: number, depth: number): Value | undefined;
  /** Whether a frame is the one that owns an id, so a hover prefers it. */
  frameOwns(depth: number, id: number): boolean;

  locals(depth: number): VariableView[];
  inputVariables(): VariableView[];
  outputs(): VariableView[];
  privateVariables(): VariableView[];
  resourceVariables(): VariableView[];
}

/** Something that steps an invocation: the invocation itself, or a pixel quad around it. */
export interface Stepper {
  readonly invocation: DebugInvocation;
  step(): InvocationStatus;
  run(): InvocationStatus;
}
