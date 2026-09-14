// The syntax tree the MSL parser builds, before types are resolved (lower.ts does that).
//
// Every node carries the line it started on, because that is what the debugger steps by: the
// lowering copies a statement's line onto each instruction it produces, and a breakpoint is a line.
import type { Attribute } from "./types.ts";

export interface Span {
  line: number;
  column: number;
}

/**
 * A type as it was written, before it is resolved: `constant Uniforms &`, `device float*`,
 * `texture2d<float, access::read>`, `array<float4, 8>`.
 */
export interface TypeRefNode {
  name: string;
  /** `<...>` arguments: a type for `texture2d<float>`, a number for `array<T, 8>`, a name for `access::read`. */
  templateArgs: (TypeRefNode | number | string)[];
  addressSpace?: string;
  /** How many `*` follow the name. */
  pointers: number;
  reference: boolean;
  const_: boolean;
  span: Span;
}

export type Expr =
  | { kind: "number"; span: Span; text: string }
  | { kind: "bool"; span: Span; value: boolean }
  | { kind: "name"; span: Span; name: string }
  | { kind: "member"; span: Span; object: Expr; name: string; arrow: boolean }
  | { kind: "index"; span: Span; object: Expr; index: Expr }
  | { kind: "call"; span: Span; callee: Expr; args: Expr[] }
  /** `float4(1, 2, 3, 4)`, `Uniforms{...}`: a type used as a constructor. */
  | { kind: "construct"; span: Span; type: TypeRefNode; args: Expr[] }
  | { kind: "unary"; span: Span; op: string; operand: Expr; prefix: boolean }
  | { kind: "binary"; span: Span; op: string; left: Expr; right: Expr }
  | { kind: "assign"; span: Span; op: string; target: Expr; value: Expr }
  | { kind: "conditional"; span: Span; cond: Expr; then: Expr; otherwise: Expr }
  | { kind: "cast"; span: Span; type: TypeRefNode; operand: Expr }
  /** `{ 1, 2, 3 }` as an initializer, whose type comes from what it initializes. */
  | { kind: "initializer"; span: Span; values: Expr[] }
  /** `as_type<float>(x)`, `sizeof(T)`: a call whose first argument is a type. */
  | { kind: "typeCall"; span: Span; name: string; type: TypeRefNode; args: Expr[] };

export interface Declarator {
  name: string;
  /** `[4][3]` after the name; an empty expression means an unsized `[]`. */
  arrayDims: (Expr | null)[];
  init?: Expr;
  span: Span;
}

export type Stmt =
  | { kind: "decl"; span: Span; type: TypeRefNode; declarators: Declarator[] }
  | { kind: "expr"; span: Span; expr: Expr }
  | { kind: "if"; span: Span; cond: Expr; then: Stmt; otherwise?: Stmt }
  | { kind: "for"; span: Span; init?: Stmt; cond?: Expr; step?: Expr; body: Stmt }
  | { kind: "while"; span: Span; cond: Expr; body: Stmt }
  | { kind: "do"; span: Span; body: Stmt; cond: Expr }
  | { kind: "switch"; span: Span; value: Expr; cases: SwitchCase[] }
  | { kind: "block"; span: Span; body: Stmt[] }
  | { kind: "return"; span: Span; value?: Expr }
  | { kind: "break"; span: Span }
  | { kind: "continue"; span: Span }
  | { kind: "discard"; span: Span }
  | { kind: "empty"; span: Span };

export interface SwitchCase {
  /** null for `default:`. */
  value: Expr | null;
  span: Span;
  body: Stmt[];
}

export interface ParamDecl {
  name: string;
  type: TypeRefNode;
  attributes: Attribute[];
  arrayDims: (Expr | null)[];
  init?: Expr;
  span: Span;
}

export interface FunctionDecl {
  name: string;
  /** "vertex", "fragment", "kernel" for an entry point; "" for an ordinary function. */
  qualifier: string;
  returnType: TypeRefNode;
  returnAttributes: Attribute[];
  params: ParamDecl[];
  body: Stmt[] | null;
  span: Span;
}

export interface StructMemberDecl {
  name: string;
  type: TypeRefNode;
  attributes: Attribute[];
  arrayDims: (Expr | null)[];
  span: Span;
}

export interface StructDecl {
  name: string;
  members: StructMemberDecl[];
  span: Span;
}

export interface GlobalDecl {
  name: string;
  type: TypeRefNode;
  attributes: Attribute[];
  arrayDims: (Expr | null)[];
  init?: Expr;
  span: Span;
}

export interface AliasDecl {
  name: string;
  type: TypeRefNode;
  span: Span;
}

export interface Unit {
  structs: StructDecl[];
  functions: FunctionDecl[];
  globals: GlobalDecl[];
  aliases: AliasDecl[];
}
