// The linear form the MSL interpreter runs.
//
// A tree-walking interpreter cannot stop between two halves of an expression without threading
// continuations through every node, and the debugger has to stop anywhere: so the syntax tree is
// lowered (lower.ts) to a flat list of instructions with explicit jumps, and the interpreter is a
// program counter over it — the same shape as the SPIR-V interpreter, which is why both step
// identically under DebugController.
//
// It is not SSA and there are no basic blocks: control flow is `jump` and `branch` to instruction
// ordinals, so nothing here needs the structurization a SPIR-V back end would. Registers are
// numbered across the whole program (a function owns a contiguous range) and double as the ids the
// debugger names values by.
import type { Value } from "../debug/values.js";
import type { Attribute } from "./types.js";

export type BinaryOp =
  | "+" | "-" | "*" | "/" | "%"
  | "==" | "!=" | "<" | ">" | "<=" | ">="
  | "&" | "|" | "^" | "<<" | ">>"
  /** Already short-circuited by the lowering: these are the bitwise-style forms on bools. */
  | "&&" | "||";

export type UnaryOp = "-" | "+" | "!" | "~";

interface Base {
  /** Ordinal in the program: what DebugStep.index is, and what jumps target. */
  index: number;
  line: number;
  column: number;
  /** The function it belongs to. */
  fn: number;
}

/** An instruction's own fields, without the ordinal and position the emitter adds. */
export type InstrBody =
  | { op: "const"; dst: number; value: Value; type: number }
  /** A pointer to a local variable, a parameter's cell, or a global. */
  | { op: "addr"; dst: number; variable: number; type: number }
  | { op: "load"; dst: number; ptr: number; type: number }
  | { op: "store"; ptr: number; value: number; type: number }
  /** Pointer arithmetic: into a struct member, an array or vector element, or past a `device T*`. */
  | { op: "member"; dst: number; ptr: number; member: number; type: number }
  | { op: "index"; dst: number; ptr: number; at: number; type: number }
  /** Reading a member, an element or a swizzle out of a value. */
  | { op: "extract"; dst: number; value: number; indices: number[]; type: number }
  /** Writing a swizzle or a member back into a value, giving a new value. */
  | { op: "insert"; dst: number; value: number; element: number; indices: number[]; type: number }
  /** A dynamic index into a value (a vector indexed by a variable). */
  | { op: "extractAt"; dst: number; value: number; at: number; type: number }
  | { op: "insertAt"; dst: number; value: number; element: number; at: number; type: number }
  | { op: "binary"; dst: number; kind: BinaryOp; a: number; b: number; type: number }
  | { op: "unary"; dst: number; kind: UnaryOp; a: number; type: number }
  /** A conversion between scalar types, applied to every component. */
  | { op: "convert"; dst: number; a: number; type: number }
  /** A vector, matrix, array or struct built from its arguments, with MSL's flattening rules. */
  | { op: "construct"; dst: number; args: number[]; type: number }
  /** The bits of one type read as another: `as_type<uint>(f)`. */
  | { op: "bitcast"; dst: number; a: number; type: number }
  | { op: "call"; dst: number; target: number; args: number[]; type: number }
  | { op: "builtin"; dst: number; name: string; args: number[]; type: number }
  | { op: "move"; dst: number; src: number; type: number }
  | { op: "jump"; target: number }
  | { op: "branch"; cond: number; then: number; otherwise: number }
  | { op: "return"; value: number; type: number }
  | { op: "discard" };

export type Instr = Base & InstrBody;

/** A register, local, parameter or global: what the debugger names a value by. */
export interface Symbol {
  id: number;
  name: string;
  type: number;
  /** A compiler temporary the shader did not name, which the values table greys out. */
  temporary: boolean;
  kind: "register" | "local" | "param" | "global";
  /** Globals and entry-point parameters: what bound them. */
  attribute?: Attribute;
  /** Entry-point parameters: how the invocation's inputs fill them in. */
  binding?: ParamBinding;
}

/** Where an entry point's parameter comes from. */
export type ParamBinding =
  | { kind: "buffer"; index: number }
  | { kind: "texture"; index: number }
  | { kind: "sampler"; index: number }
  | { kind: "stage_in" }
  /** A built-in like `[[vertex_id]]` or `[[thread_position_in_grid]]`, by its attribute name. */
  | { kind: "builtin"; name: string }
  /** `[[threadgroup(n)]]`: threadgroup memory, which a single invocation sees as zeroed. */
  | { kind: "threadgroup"; index: number };

export interface FunctionIr {
  name: string;
  /** "vertex", "fragment", "kernel", or "" for an ordinary function. */
  qualifier: string;
  returnType: number;
  /** The return value's attributes, for an entry point returning a bare `[[color(0)]]` value. */
  returnAttributes: Attribute[];
  params: { id: number; type: number; name: string }[];
  /** Locals, in declaration order, with the ids the frame keys their cells by. */
  locals: { id: number; type: number; name: string }[];
  /** First instruction of the body. */
  entry: number;
  /** Range of instruction ordinals belonging to it (end is exclusive). */
  start: number;
  end: number;
}

/** A struct member's or parameter's attribute by name, or undefined. */
export function attributeNamed(attributes: Attribute[] | undefined, name: string): Attribute | undefined {
  return attributes?.find((a) => a.name === name);
}
