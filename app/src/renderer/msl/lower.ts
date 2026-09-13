// Lowering: the MSL syntax tree (ast.ts) to the instructions the interpreter runs (ir.ts).
//
// This is where types are resolved, so it is also where MSL's arithmetic rules are applied once
// rather than in the interpreter: a scalar mixed with a vector is broadcast here, operands of
// different scalar types are converted here, and a matrix product becomes a named operation here.
// What reaches the interpreter is therefore always component-wise on operands of one shape, which
// is what keeps the interpreter small.
//
// Every instruction carries the line of the statement or expression it came from, which is what
// the debugger steps by and puts breakpoints on.
import type {
  Declarator, Expr, FunctionDecl, GlobalDecl, ParamDecl, Span, Stmt, StructDecl, TypeRefNode, Unit,
} from "./ast.js";
import type { BinaryOp, FunctionIr, Instr, InstrBody, ParamBinding, Symbol, UnaryOp } from "./ir.js";
import { isTextureName, TypeTable, type ScalarBase, type TextureAccess } from "./types.js";
import type { Value } from "../debug/values.js";

export interface LowerDiagnostic {
  line: number;
  message: string;
}

export interface LoweredProgram {
  types: TypeTable;
  instructions: Instr[];
  symbols: Symbol[];
  functions: FunctionIr[];
  /** Globals with the value they were initialized to. */
  globals: { symbol: Symbol; value: Value }[];
  entryPoints: FunctionIr[];
  diagnostics: LowerDiagnostic[];
}

/** MSL attributes that name a built-in input or output rather than a resource. */
const BUILTIN_ATTRIBUTES = new Set([
  "vertex_id", "instance_id", "base_vertex", "base_instance", "vertex_amplification_id", "vertex_amplification_count",
  "position", "point_size", "clip_distance", "point_coord", "front_facing", "primitive_id", "sample_id", "sample_mask",
  "render_target_array_index", "viewport_array_index", "barycentric_coord", "layer",
  "thread_position_in_grid", "thread_position_in_threadgroup", "threadgroup_position_in_grid",
  "thread_index_in_threadgroup", "threads_per_threadgroup", "threadgroups_per_grid", "threads_per_grid",
  "thread_index_in_simdgroup", "simdgroup_index_in_threadgroup", "simdgroups_per_threadgroup",
  "threads_per_simdgroup", "quad_index_in_threadgroup", "quad_index_in_simdgroup",
  "depth", "color", "raster_order_group", "amplification_count", "amplification_id",
]);

/** How strongly a scalar type pulls a mixed expression towards it (C's usual arithmetic conversions). */
const RANK: Record<ScalarBase, number> = {
  bool: 0, char: 1, uchar: 1, short: 2, ushort: 2, int: 3, uint: 4, long: 5, ulong: 6, half: 7, float: 8,
};

const COMPONENT_INDEX: Record<string, number> = {
  x: 0, y: 1, z: 2, w: 3, r: 0, g: 1, b: 2, a: 3, s: 0, t: 1, p: 2, q: 3,
};

interface Scope {
  names: Map<string, number>;
}

interface LoopTargets {
  /** Jump patches to fill in with the instruction after the loop, and with its continue point. */
  breaks: number[];
  continues: number[];
}

class Lowering {
  readonly types = new TypeTable();
  readonly instructions: Instr[] = [];
  readonly symbols: Symbol[] = [];
  readonly functions: FunctionIr[] = [];
  readonly globals: { symbol: Symbol; value: Value }[] = [];
  readonly diagnostics: LowerDiagnostic[] = [];

  private _unit: Unit;
  private _aliases = new Map<string, TypeRefNode>();
  private _globalScope: Scope = { names: new Map() };
  private _scopes: Scope[] = [];
  private _functionsByName = new Map<string, number[]>();
  private _fn = -1;
  private _loops: LoopTargets[] = [];
  private _currentLocals: { id: number; type: number; name: string }[] = [];
  private _returnType = 0;

  constructor(unit: Unit) {
    this._unit = unit;
  }

  run(): LoweredProgram {
    for (const alias of this._unit.aliases) this._aliases.set(alias.name, alias.type);
    // Structs first, and twice: a member may name a struct declared later in the file.
    for (const s of this._unit.structs) this.types.declareStruct(s.name);
    for (const s of this._unit.structs) this._fillStruct(s);
    for (const g of this._unit.globals) this._global(g);
    // Every function is registered before any body is lowered, so calls resolve in any order.
    this._unit.functions.forEach((f, i) => {
      if (!f.body) return;
      const list = this._functionsByName.get(f.name) ?? [];
      list.push(i);
      this._functionsByName.set(f.name, list);
    });
    const indexOfDecl = new Map<number, number>();
    this._unit.functions.forEach((f, i) => {
      if (!f.body) return;
      indexOfDecl.set(i, this.functions.length);
      this.functions.push(this._declareFunction(f));
    });
    this._unit.functions.forEach((f, i) => {
      const at = indexOfDecl.get(i);
      if (at !== undefined) this._lowerFunction(f, at);
    });
    // Calls were emitted with the declaration index; rewrite them to the function index.
    for (const inst of this.instructions) {
      if (inst.op === "call") inst.target = indexOfDecl.get(inst.target) ?? inst.target;
    }
    return {
      types: this.types, instructions: this.instructions, symbols: this.symbols, functions: this.functions,
      globals: this.globals, diagnostics: this.diagnostics,
      entryPoints: this.functions.filter((f) => f.qualifier),
    };
  }

  // -------------------------------------------------------------------------------------------
  // Diagnostics and symbols

  private _warn(span: Span, message: string): void {
    if (this.diagnostics.length > 200) return;
    this.diagnostics.push({ line: span.line, message });
  }

  private _symbol(name: string, type: number, kind: Symbol["kind"], temporary: boolean, extra?: Partial<Symbol>): Symbol {
    const symbol: Symbol = { id: this.symbols.length, name, type, temporary, kind, ...extra };
    this.symbols.push(symbol);
    return symbol;
  }

  private _temp(type: number): number {
    return this._symbol("", type, "register", true).id;
  }

  private _emit(body: InstrBody, span: Span): Instr {
    const full = { ...body, index: this.instructions.length, fn: this._fn, line: span.line, column: span.column } as Instr;
    this.instructions.push(full);
    return full;
  }

  // -------------------------------------------------------------------------------------------
  // Types

  /** A written type resolved against the table; `void` for one that cannot be. */
  private _type(node: TypeRefNode, seen = new Set<string>()): number {
    let base = this._baseType(node, seen);
    for (let i = 0; i < node.pointers; i++) base = this.types.pointer(base, (node.addressSpace as never) ?? "thread");
    if (node.reference) base = this.types.pointer(base, (node.addressSpace as never) ?? "thread", true);
    return base;
  }

  private _baseType(node: TypeRefNode, seen: Set<string>): number {
    const name = node.name;
    const builtin = this.types.builtin(name);
    if (builtin !== undefined) return builtin;
    if (isTextureName(name)) {
      const first = node.templateArgs[0];
      const sampled = typeof first === "object" ? this._type(first, seen) : this.types.float;
      const access = node.templateArgs.map((a) => (typeof a === "string" ? a : "")).find((a) => a) as TextureAccess | undefined;
      return this.types.texture(name, sampled, access ?? "sample") ?? this.types.void_;
    }
    if (name === "array") {
      const element = node.templateArgs[0];
      const length = node.templateArgs[1];
      return this.types.array(typeof element === "object" ? this._type(element, seen) : this.types.float,
        typeof length === "number" ? length : -1);
    }
    if (name === "vec") {
      const element = node.templateArgs[0];
      const count = node.templateArgs[1];
      return this.types.vector(typeof element === "object" ? this._type(element, seen) : this.types.float,
        typeof count === "number" ? count : 4);
    }
    if (name === "matrix") {
      const element = node.templateArgs[0];
      const columns = node.templateArgs[1];
      const rows = node.templateArgs[2];
      const scalar = typeof element === "object" ? this._type(element, seen) : this.types.float;
      return this.types.matrix(this.types.vector(scalar, typeof rows === "number" ? rows : 4), typeof columns === "number" ? columns : 4);
    }
    if (name.startsWith("atomic")) {
      const inner = node.templateArgs[0];
      const element = typeof inner === "object" ? this._type(inner, seen)
        : name === "atomic_uint" ? this.types.uint : name === "atomic_float" ? this.types.float
        : name === "atomic_bool" ? this.types.bool : this.types.int;
      return this.types.intern({ kind: "atomic", element });
    }
    const struct = this.types.structNamed(name);
    if (struct !== undefined) return struct;
    const alias = this._aliases.get(name);
    if (alias && !seen.has(name)) {
      seen.add(name);
      return this._type(alias, seen);
    }
    this._warn(node.span, `unknown type ${name}, which the interpreter treats as void`);
    return this.types.intern({ kind: "opaque", name });
  }

  /** A declarator's `[4][3]` wrapped around its base type, outermost dimension first. */
  private _withArrayDims(base: number, dims: (Expr | null)[]): number {
    let type = base;
    for (let i = dims.length - 1; i >= 0; i--) {
      const dim = dims[i];
      type = this.types.array(type, dim ? this._constantInt(dim) ?? -1 : -1);
    }
    return type;
  }

  private _fillStruct(decl: StructDecl): void {
    const ref = this.types.declareStruct(decl.name);
    const type = this.types.get(ref);
    if (type?.kind !== "struct" || type.members.length) return;
    for (const m of decl.members) {
      type.members.push({
        name: m.name,
        type: this._withArrayDims(this._type(m.type), m.arrayDims),
        attributes: m.attributes,
      });
    }
  }

  // -------------------------------------------------------------------------------------------
  // Globals

  private _global(decl: GlobalDecl): void {
    const type = this._withArrayDims(this._type(decl.type), decl.arrayDims);
    const symbol = this._symbol(decl.name, type, "global", false, { attribute: decl.attributes[0] });
    const value = decl.init ? this._constantValue(decl.init, type) : this.types.zero(type);
    if (decl.init && value === undefined) {
      this._warn(decl.span, `the initializer of ${decl.name} is not a constant the interpreter can evaluate: it reads as zero`);
    }
    this.globals.push({ symbol, value: value ?? this.types.zero(type) });
    this._globalScope.names.set(decl.name, symbol.id);
  }

  /** A constant expression's value, for a global initializer or an array size. Undefined when it is not one. */
  private _constantValue(expr: Expr, type: number): Value | undefined {
    const t = this.types.get(type);
    switch (expr.kind) {
      case "number": return numberOf(expr.text);
      case "bool": return expr.value;
      case "unary": {
        const inner = this._constantValue(expr.operand, type);
        if (inner === undefined) return undefined;
        if (expr.op === "-") return typeof inner === "number" ? -inner : typeof inner === "bigint" ? -inner : undefined;
        if (expr.op === "+") return inner;
        if (expr.op === "!") return !inner;
        if (expr.op === "~") return typeof inner === "number" ? ~inner : undefined;
        return undefined;
      }
      case "name": {
        const id = this._globalScope.names.get(expr.name);
        const global = id === undefined ? undefined : this.globals.find((g) => g.symbol.id === id);
        return global?.value;
      }
      case "initializer":
      case "construct": {
        const args = expr.kind === "construct" ? expr.args : expr.values;
        if (t?.kind === "struct") {
          return t.members.map((m, i) => (args[i] === undefined ? this.types.zero(m.type) : this._constantValue(args[i], m.type) ?? this.types.zero(m.type)));
        }
        if (t?.kind === "array") {
          const length = t.length < 0 ? args.length : t.length;
          return Array.from({ length }, (_, i) => (args[i] === undefined ? this.types.zero(t.element) : this._constantValue(args[i], t.element) ?? this.types.zero(t.element)));
        }
        if (t?.kind === "vector") {
          const flat: Value[] = [];
          for (const a of args) {
            const v = this._constantValue(a, t.element);
            if (v === undefined) return undefined;
            if (Array.isArray(v)) flat.push(...v);
            else flat.push(v);
          }
          if (flat.length === 1) return Array.from({ length: t.count }, () => flat[0]);
          return Array.from({ length: t.count }, (_, i) => flat[i] ?? 0);
        }
        // `float(2)`, `int(x)`: a scalar constructor.
        return args.length === 1 ? this._constantValue(args[0], type) : undefined;
      }
      case "binary": {
        const a = this._constantValue(expr.left, type);
        const b = this._constantValue(expr.right, type);
        if (typeof a !== "number" || typeof b !== "number") return undefined;
        switch (expr.op) {
          case "+": return a + b;
          case "-": return a - b;
          case "*": return a * b;
          case "/": return b === 0 ? undefined : a / b;
          case "<<": return a << b;
          case ">>": return a >> b;
          case "|": return a | b;
          case "&": return a & b;
          case "^": return a ^ b;
          default: return undefined;
        }
      }
      default:
        return undefined;
    }
  }

  private _constantInt(expr: Expr): number | undefined {
    const v = this._constantValue(expr, this.types.int);
    return typeof v === "number" ? Math.trunc(v) : typeof v === "bigint" ? Number(v) : undefined;
  }

  // -------------------------------------------------------------------------------------------
  // Functions

  private _declareFunction(decl: FunctionDecl): FunctionIr {
    const returnType = this._type(decl.returnType);
    const params = decl.params
      .filter((p) => this.types.get(this._type(p.type))?.kind !== "void" || p.name)
      .map((p) => {
        const type = this._withArrayDims(this._type(p.type), p.arrayDims);
        const symbol = this._symbol(p.name, type, "param", false, {
          attribute: p.attributes[0],
          binding: decl.qualifier ? bindingOf(p, type, this.types) : undefined,
        });
        return { id: symbol.id, type, name: p.name };
      });
    return {
      name: decl.name, qualifier: decl.qualifier, returnType, returnAttributes: decl.returnAttributes,
      params, locals: [], entry: 0, start: 0, end: 0,
    };
  }

  private _lowerFunction(decl: FunctionDecl, at: number): void {
    const fn = this.functions[at];
    if (!decl.body) return;
    this._fn = at;
    this._returnType = fn.returnType;
    this._currentLocals = [];
    this._scopes = [{ names: new Map() }];
    fn.params.forEach((p) => this._scopes[0].names.set(p.name, p.id));
    fn.start = this.instructions.length;
    fn.entry = this.instructions.length;
    for (const stmt of decl.body) this._statement(stmt);
    // Every function ends in a return, so the interpreter never runs off the end.
    const span = { line: decl.span.line, column: decl.span.column };
    this._emit({ op: "return", value: -1, type: fn.returnType }, span);
    fn.end = this.instructions.length;
    fn.locals = this._currentLocals;
    this._fn = -1;
  }

  // -------------------------------------------------------------------------------------------
  // Statements

  private _push(): void {
    this._scopes.push({ names: new Map() });
  }

  private _pop(): void {
    this._scopes.pop();
  }

  private _lookup(name: string): number | undefined {
    for (let i = this._scopes.length - 1; i >= 0; i--) {
      const id = this._scopes[i].names.get(name);
      if (id !== undefined) return id;
    }
    return this._globalScope.names.get(name);
  }

  private _statement(stmt: Stmt): void {
    switch (stmt.kind) {
      case "empty":
        return;
      case "block":
        this._push();
        for (const s of stmt.body) this._statement(s);
        this._pop();
        return;
      case "decl":
        for (const d of stmt.declarators) this._declare(stmt.type, d);
        return;
      case "expr":
        this._value(stmt.expr);
        return;
      case "return": {
        const value = stmt.value ? this._convert(this._value(stmt.value), this._returnType, stmt.span) : -1;
        this._emit({ op: "return", value, type: this._returnType }, stmt.span);
        return;
      }
      case "discard":
        this._emit({ op: "discard" }, stmt.span);
        return;
      case "break": {
        const loop = this._loops[this._loops.length - 1];
        const jump = this._emit({ op: "jump", target: -1 }, stmt.span);
        if (loop) loop.breaks.push(jump.index);
        else this._warn(stmt.span, "break outside a loop or switch");
        return;
      }
      case "continue": {
        const loop = this._loops[this._loops.length - 1];
        const jump = this._emit({ op: "jump", target: -1 }, stmt.span);
        if (loop) loop.continues.push(jump.index);
        else this._warn(stmt.span, "continue outside a loop");
        return;
      }
      case "if": {
        const cond = this._condition(stmt.cond);
        const branch = this._emit({ op: "branch", cond, then: -1, otherwise: -1 }, stmt.span);
        if (branch.op === "branch") branch.then = this.instructions.length;
        this._push();
        this._statement(stmt.then);
        this._pop();
        if (!stmt.otherwise) {
          if (branch.op === "branch") branch.otherwise = this.instructions.length;
          return;
        }
        const skip = this._emit({ op: "jump", target: -1 }, stmt.span);
        if (branch.op === "branch") branch.otherwise = this.instructions.length;
        this._push();
        this._statement(stmt.otherwise);
        this._pop();
        if (skip.op === "jump") skip.target = this.instructions.length;
        return;
      }
      case "while": {
        const top = this.instructions.length;
        const cond = this._condition(stmt.cond);
        const branch = this._emit({ op: "branch", cond, then: -1, otherwise: -1 }, stmt.span);
        if (branch.op === "branch") branch.then = this.instructions.length;
        this._loops.push({ breaks: [], continues: [] });
        this._push();
        this._statement(stmt.body);
        this._pop();
        this._emit({ op: "jump", target: top }, stmt.span);
        const after = this.instructions.length;
        if (branch.op === "branch") branch.otherwise = after;
        this._closeLoop(after, top);
        return;
      }
      case "do": {
        const top = this.instructions.length;
        this._loops.push({ breaks: [], continues: [] });
        this._push();
        this._statement(stmt.body);
        this._pop();
        const test = this.instructions.length;
        const cond = this._condition(stmt.cond);
        const branch = this._emit({ op: "branch", cond, then: top, otherwise: -1 }, stmt.span);
        const after = this.instructions.length;
        if (branch.op === "branch") branch.otherwise = after;
        this._closeLoop(after, test);
        return;
      }
      case "for": {
        this._push();
        if (stmt.init) this._statement(stmt.init);
        const top = this.instructions.length;
        let branch: Instr | null = null;
        if (stmt.cond) {
          const cond = this._condition(stmt.cond);
          branch = this._emit({ op: "branch", cond, then: -1, otherwise: -1 }, stmt.span);
          if (branch.op === "branch") branch.then = this.instructions.length;
        }
        this._loops.push({ breaks: [], continues: [] });
        this._push();
        this._statement(stmt.body);
        this._pop();
        const step = this.instructions.length;
        if (stmt.step) this._value(stmt.step);
        this._emit({ op: "jump", target: top }, stmt.span);
        const after = this.instructions.length;
        if (branch?.op === "branch") branch.otherwise = after;
        this._closeLoop(after, step);
        this._pop();
        return;
      }
      case "switch": {
        // Each case's label is tested in turn; the bodies fall through to each other, as C has it.
        const value = this._value(stmt.value);
        this._loops.push({ breaks: [], continues: [] });
        const tests: { branch: Instr; case: number }[] = [];
        let defaultCase = -1;
        stmt.cases.forEach((c, i) => {
          if (!c.value) {
            defaultCase = i;
            return;
          }
          const label = this._convert(this._value(c.value), this._typeOf(value), c.span);
          const equal = this._temp(this.types.bool);
          this._emit({ op: "binary", dst: equal, kind: "==", a: value, b: label, type: this.types.bool }, c.span);
          const branch = this._emit({ op: "branch", cond: equal, then: -1, otherwise: -1 }, c.span);
          if (branch.op === "branch") branch.otherwise = this.instructions.length;
          tests.push({ branch, case: i });
        });
        const toDefault = this._emit({ op: "jump", target: -1 }, stmt.span);
        const starts: number[] = [];
        stmt.cases.forEach((c) => {
          starts.push(this.instructions.length);
          this._push();
          for (const s of c.body) this._statement(s);
          this._pop();
        });
        const after = this.instructions.length;
        for (const t of tests) if (t.branch.op === "branch") t.branch.then = starts[t.case] ?? after;
        if (toDefault.op === "jump") toDefault.target = defaultCase >= 0 ? starts[defaultCase] ?? after : after;
        this._closeLoop(after, after);
        return;
      }
    }
  }

  private _closeLoop(after: number, continueTarget: number): void {
    const loop = this._loops.pop();
    if (!loop) return;
    for (const at of loop.breaks) {
      const instr = this.instructions[at];
      if (instr.op === "jump") instr.target = after;
    }
    for (const at of loop.continues) {
      const instr = this.instructions[at];
      if (instr.op === "jump") instr.target = continueTarget;
    }
  }

  private _declare(typeNode: TypeRefNode, declarator: Declarator): void {
    let type = this._withArrayDims(this._type(typeNode), declarator.arrayDims);
    // `float a[] = {1, 2, 3}` takes its length from the initializer.
    const t = this.types.get(type);
    if (t?.kind === "array" && t.length < 0 && declarator.init?.kind === "initializer") {
      type = this.types.array(t.element, declarator.init.values.length);
    }
    const symbol = this._symbol(declarator.name, type, "local", false);
    this._currentLocals.push({ id: symbol.id, type, name: declarator.name });
    // The name is in scope only after its declarator, so `int x = x;` reads the outer one.
    const init = declarator.init ? this._initialize(declarator.init, type, declarator.span) : -1;
    this._scopes[this._scopes.length - 1].names.set(declarator.name, symbol.id);
    if (init < 0) return;
    const ptr = this._temp(this.types.pointer(type, "thread"));
    this._emit({ op: "addr", dst: ptr, variable: symbol.id, type: this.types.pointer(type, "thread") }, declarator.span);
    this._emit({ op: "store", ptr, value: init, type }, declarator.span);
  }

  /** An initializer in a context whose type is known: `{1, 2}` builds the type, anything else converts. */
  private _initialize(expr: Expr, type: number, span: Span): number {
    if (expr.kind === "initializer") return this._constructFrom(expr.values, type, span);
    return this._convert(this._value(expr), type, span);
  }

  // -------------------------------------------------------------------------------------------
  // Expressions: addresses

  /** The type a register holds. */
  private _typeOf(reg: number): number {
    return this.symbols[reg]?.type ?? this.types.void_;
  }

  /**
   * A register holding a pointer to what the expression names, with the type pointed at. Null for
   * an expression that has no address (a call's result, a swizzle — the caller handles those).
   */
  private _address(expr: Expr): { ptr: number; type: number } | null {
    switch (expr.kind) {
      case "name": {
        const id = this._lookup(expr.name);
        if (id === undefined) return null;
        const symbol = this.symbols[id];
        const t = this.types.get(symbol.type);
        // A reference parameter holds the pointer: its address *is* that value.
        if (t?.kind === "pointer" && t.reference) {
          const ptr = this._temp(symbol.type);
          this._emit({ op: "move", dst: ptr, src: id, type: symbol.type }, expr.span);
          return { ptr, type: t.pointee };
        }
        // A pointer, texture or sampler parameter is a value the invocation was handed, not
        // memory: it has no address, and its uses read it with `move` instead (_nameValue).
        if (symbol.kind === "param" && (t?.kind === "pointer" || t?.kind === "texture" || t?.kind === "sampler")) return null;
        const pointer = this.types.pointer(symbol.type, symbol.kind === "global" ? "constant" : "thread");
        const ptr = this._temp(pointer);
        this._emit({ op: "addr", dst: ptr, variable: id, type: pointer }, expr.span);
        return { ptr, type: symbol.type };
      }
      case "member": {
        const target = this._memberTarget(expr);
        if (!target) return null;
        const t = this.types.get(target.type);
        if (t?.kind !== "struct") return null;
        const index = t.members.findIndex((m) => m.name === expr.name);
        if (index < 0) {
          this._warn(expr.span, `${this.types.name(target.type)} has no member ${expr.name}`);
          return null;
        }
        const memberType = t.members[index].type;
        const pointer = this.types.pointer(memberType, this._spaceOf(target.ptr));
        const dst = this._temp(pointer);
        this._emit({ op: "member", dst, ptr: target.ptr, member: index, type: pointer }, expr.span);
        return { ptr: dst, type: memberType };
      }
      case "index": {
        const base = this._indexTarget(expr);
        if (!base) return null;
        const t = this.types.get(base.type);
        const element = t?.kind === "array" ? t.element : t?.kind === "matrix" ? t.column : t?.kind === "vector" ? t.element : null;
        if (element === null) return null;
        const at = this._value(expr.index);
        const pointer = this.types.pointer(element, this._spaceOf(base.ptr));
        const dst = this._temp(pointer);
        this._emit({ op: "index", dst, ptr: base.ptr, at, type: pointer }, expr.span);
        return { ptr: dst, type: element };
      }
      case "unary": {
        // `*p` names what p points at.
        if (expr.op !== "*") return null;
        const ptr = this._value(expr.operand);
        const t = this.types.get(this._typeOf(ptr));
        if (t?.kind !== "pointer") return null;
        return { ptr, type: t.pointee };
      }
      default:
        return null;
    }
  }

  private _spaceOf(ptr: number): "thread" | "device" | "constant" | "threadgroup" {
    const t = this.types.get(this._typeOf(ptr));
    return t?.kind === "pointer" ? (t.space as "thread") : "thread";
  }

  /** The struct a `.` or `->` reads from: the object's address, dereferenced for a pointer. */
  private _memberTarget(expr: Expr & { kind: "member" }): { ptr: number; type: number } | null {
    const direct = expr.arrow ? null : this._address(expr.object);
    if (direct) {
      const t = this.types.get(direct.type);
      // `p.x` where p is a pointer is not legal MSL, but `s.p->x` chains are: follow a pointer.
      if (t?.kind === "pointer") {
        const loaded = this._temp(direct.type);
        this._emit({ op: "load", dst: loaded, ptr: direct.ptr, type: direct.type }, expr.span);
        return { ptr: loaded, type: t.pointee };
      }
      return direct;
    }
    const value = this._value(expr.object);
    const t = this.types.get(this._typeOf(value));
    if (t?.kind === "pointer") return { ptr: value, type: t.pointee };
    return null;
  }

  private _indexTarget(expr: Expr & { kind: "index" }): { ptr: number; type: number } | null {
    const direct = this._address(expr.object);
    if (direct) {
      const t = this.types.get(direct.type);
      if (t?.kind === "pointer") {
        // `buf[i]` where buf is a `device float*`: the pointer's value is the base.
        const loaded = this._temp(direct.type);
        this._emit({ op: "load", dst: loaded, ptr: direct.ptr, type: direct.type }, expr.span);
        return { ptr: loaded, type: this.types.array(t.pointee, -1) };
      }
      return direct;
    }
    const value = this._value(expr.object);
    const t = this.types.get(this._typeOf(value));
    if (t?.kind === "pointer") return { ptr: value, type: this.types.array(t.pointee, -1) };
    return null;
  }

  // -------------------------------------------------------------------------------------------
  // Expressions: values

  /** Lowers an expression and returns the register holding its value. */
  private _value(expr: Expr): number {
    switch (expr.kind) {
      case "number": {
        const text = expr.text;
        const value = numberOf(text);
        const type = /[fF]$/.test(text) || /[.eE]/.test(text) ? this.types.float
          : /[hH]$/.test(text) ? this.types.scalar("half")
          : /[uU]/.test(text) ? this.types.uint
          : this.types.int;
        const dst = this._temp(type);
        this._emit({ op: "const", dst, value, type }, expr.span);
        return dst;
      }
      case "bool": {
        const dst = this._temp(this.types.bool);
        this._emit({ op: "const", dst, value: expr.value, type: this.types.bool }, expr.span);
        return dst;
      }
      case "name":
        return this._nameValue(expr);
      case "member":
        return this._memberValue(expr);
      case "index":
        return this._indexValue(expr);
      case "call":
        return this._call(expr);
      case "construct": {
        const type = this._type(expr.type);
        return this._constructFrom(expr.args, type, expr.span);
      }
      case "initializer":
        // An initializer with no context: the values as an array, which is the best guess available.
        return this._constructFrom(expr.values, this.types.array(this.types.float, expr.values.length), expr.span);
      case "cast": {
        const type = this._type(expr.type);
        return this._convert(this._value(expr.operand), type, expr.span);
      }
      case "typeCall":
        return this._typeCall(expr);
      case "unary":
        return this._unary(expr);
      case "binary":
        return this._binary(expr);
      case "assign":
        return this._assign(expr);
      case "conditional":
        return this._conditional(expr);
    }
  }

  private _nameValue(expr: Expr & { kind: "name" }): number {
    const id = this._lookup(expr.name);
    if (id === undefined) {
      this._warn(expr.span, `${expr.name} is not declared in this shader: it reads as zero`);
      const dst = this._temp(this.types.int);
      this._emit({ op: "const", dst, value: 0, type: this.types.int }, expr.span);
      return dst;
    }
    const symbol = this.symbols[id];
    const t = this.types.get(symbol.type);
    // Textures and samplers are handles, not memory: the parameter's value is the handle itself.
    if (t?.kind === "texture" || t?.kind === "sampler") {
      const dst = this._temp(symbol.type);
      this._emit({ op: "move", dst, src: id, type: symbol.type }, expr.span);
      return dst;
    }
    // A reference reads through itself; a plain pointer parameter reads as the pointer.
    if (t?.kind === "pointer" && !t.reference) {
      const dst = this._temp(symbol.type);
      this._emit({ op: "move", dst, src: id, type: symbol.type }, expr.span);
      return dst;
    }
    const address = this._address(expr);
    if (!address) {
      const dst = this._temp(symbol.type);
      this._emit({ op: "move", dst, src: id, type: symbol.type }, expr.span);
      return dst;
    }
    const dst = this._temp(address.type);
    this._emit({ op: "load", dst, ptr: address.ptr, type: address.type }, expr.span);
    return dst;
  }

  private _memberValue(expr: Expr & { kind: "member" }): number {
    // A swizzle reads components out of a vector rather than a member out of a struct.
    const objectType = this._peekType(expr.object);
    const swizzle = this._swizzleOf(objectType, expr.name);
    if (swizzle) {
      const value = this._value(expr.object);
      const type = swizzle.indices.length === 1 ? this.types.elementOf(objectType)
        : this.types.vector(this.types.elementOf(objectType), swizzle.indices.length);
      const dst = this._temp(type);
      this._emit({ op: "extract", dst, value, indices: swizzle.indices, type }, expr.span);
      return dst;
    }
    const address = this._address(expr);
    if (address) {
      const dst = this._temp(address.type);
      this._emit({ op: "load", dst, ptr: address.ptr, type: address.type }, expr.span);
      return dst;
    }
    // A member of a value with no address: a call's result, for instance.
    const value = this._value(expr.object);
    const t = this.types.get(this._typeOf(value));
    if (t?.kind === "struct") {
      const index = t.members.findIndex((m) => m.name === expr.name);
      if (index >= 0) {
        const type = t.members[index].type;
        const dst = this._temp(type);
        this._emit({ op: "extract", dst, value, indices: [index], type }, expr.span);
        return dst;
      }
    }
    this._warn(expr.span, `${this.types.name(this._typeOf(value))} has no member ${expr.name}`);
    return value;
  }

  private _indexValue(expr: Expr & { kind: "index" }): number {
    const address = this._address(expr);
    if (address) {
      const dst = this._temp(address.type);
      this._emit({ op: "load", dst, ptr: address.ptr, type: address.type }, expr.span);
      return dst;
    }
    const value = this._value(expr.object);
    const t = this.types.get(this._typeOf(value));
    const element = t?.kind === "vector" ? t.element : t?.kind === "matrix" ? t.column : t?.kind === "array" ? t.element : null;
    if (element === null) {
      this._warn(expr.span, `${this.types.name(this._typeOf(value))} cannot be indexed`);
      return value;
    }
    const at = this._value(expr.index);
    const dst = this._temp(element);
    this._emit({ op: "extractAt", dst, value, at, type: element }, expr.span);
    return dst;
  }

  /** The components a swizzle names, or null when the name is not one. */
  private _swizzleOf(type: number, name: string): { indices: number[] } | null {
    const t = this.types.get(type);
    if (t?.kind !== "vector") return null;
    if (!name.length || name.length > 4) return null;
    const indices: number[] = [];
    // A swizzle uses one family of names throughout: xyzw, rgba or stpq.
    const family = /^[xyzw]+$/.test(name) ? 1 : /^[rgba]+$/.test(name) ? 2 : /^[stpq]+$/.test(name) ? 3 : 0;
    if (!family) return null;
    for (const c of name) {
      const index = COMPONENT_INDEX[c];
      if (index === undefined || index >= t.count) return null;
      indices.push(index);
    }
    return { indices };
  }

  /**
   * The type an expression will have, without emitting anything. Only the cases a swizzle or an
   * overload needs are exact; anything else falls back to lowering it (which is why this is only
   * called where that is harmless).
   */
  private _peekType(expr: Expr): number {
    switch (expr.kind) {
      case "name": {
        const id = this._lookup(expr.name);
        if (id === undefined) return this.types.void_;
        const symbol = this.symbols[id];
        const t = this.types.get(symbol.type);
        return t?.kind === "pointer" && t.reference ? t.pointee : symbol.type;
      }
      case "member": {
        const objectType = this._peekType(expr.object);
        const swizzle = this._swizzleOf(objectType, expr.name);
        if (swizzle) {
          return swizzle.indices.length === 1 ? this.types.elementOf(objectType)
            : this.types.vector(this.types.elementOf(objectType), swizzle.indices.length);
        }
        const base = this.types.get(objectType);
        const struct = base?.kind === "pointer" ? this.types.get(base.pointee) : base;
        if (struct?.kind === "struct") return struct.members.find((m) => m.name === expr.name)?.type ?? this.types.void_;
        return this.types.void_;
      }
      case "index": {
        const t = this.types.get(this._peekType(expr.object));
        if (t?.kind === "vector") return t.element;
        if (t?.kind === "matrix") return t.column;
        if (t?.kind === "array") return t.element;
        if (t?.kind === "pointer") return t.pointee;
        return this.types.void_;
      }
      case "construct":
        return this._type(expr.type);
      case "cast":
      case "typeCall":
        return this._type(expr.type);
      case "number":
        return /[fF]$/.test(expr.text) || /[.eE]/.test(expr.text) ? this.types.float : this.types.int;
      case "bool":
        return this.types.bool;
      case "unary":
        return expr.op === "!" ? this.types.bool : this._peekType(expr.operand);
      case "binary":
        return this._peekType(expr.left);
      case "assign":
        return this._peekType(expr.target);
      case "conditional":
        return this._peekType(expr.then);
      default:
        return this.types.void_;
    }
  }

  // -------------------------------------------------------------------------------------------
  // Operators

  private _unary(expr: Expr & { kind: "unary" }): number {
    if (expr.op === "++" || expr.op === "--") return this._incrementOrDecrement(expr);
    if (expr.op === "&") {
      const address = this._address(expr.operand);
      if (address) return address.ptr;
      this._warn(expr.span, "the address of a value that has none was taken");
      return this._value(expr.operand);
    }
    if (expr.op === "*") {
      const address = this._address(expr);
      if (address) {
        const dst = this._temp(address.type);
        this._emit({ op: "load", dst, ptr: address.ptr, type: address.type }, expr.span);
        return dst;
      }
      return this._value(expr.operand);
    }
    if (expr.op === "sizeof" || expr.op === "alignof") {
      const type = this._peekType(expr.operand);
      const dst = this._temp(this.types.uint);
      const size = expr.op === "sizeof" ? this.types.sizeOf(type) : this.types.alignOf(type);
      this._emit({ op: "const", dst, value: size, type: this.types.uint }, expr.span);
      return dst;
    }
    const a = this._value(expr.operand);
    if (expr.op === "+") return a;
    const operandType = this._typeOf(a);
    const type = expr.op === "!" ? this.types.withScalar(operandType, "bool") : operandType;
    const dst = this._temp(type);
    this._emit({ op: "unary", dst, kind: expr.op as UnaryOp, a, type }, expr.span);
    return dst;
  }

  private _incrementOrDecrement(expr: Expr & { kind: "unary" }): number {
    const address = this._address(expr.operand);
    if (!address) {
      this._warn(expr.span, `${expr.op} needs a variable`);
      return this._value(expr.operand);
    }
    const before = this._temp(address.type);
    this._emit({ op: "load", dst: before, ptr: address.ptr, type: address.type }, expr.span);
    const one = this._temp(address.type);
    this._emit({ op: "const", dst: one, value: this._oneOf(address.type), type: address.type }, expr.span);
    const after = this._temp(address.type);
    this._emit({ op: "binary", dst: after, kind: expr.op === "++" ? "+" : "-", a: before, b: one, type: address.type }, expr.span);
    this._emit({ op: "store", ptr: address.ptr, value: after, type: address.type }, expr.span);
    return expr.prefix ? after : before;
  }

  private _oneOf(type: number): Value {
    const scalar = this.types.scalarOf(type);
    const one: Value = scalar?.width === 64 && scalar.base !== "float" ? 1n : 1;
    const t = this.types.get(type);
    if (t?.kind === "vector") return Array.from({ length: t.count }, () => one);
    return one;
  }

  private _binary(expr: Expr & { kind: "binary" }): number {
    if (expr.op === ",") {
      this._value(expr.left);
      return this._value(expr.right);
    }
    if (expr.op === "&&" || expr.op === "||") return this._shortCircuit(expr);
    let a = this._value(expr.left);
    let b = this._value(expr.right);
    const aType = this._typeOf(a);
    const bType = this._typeOf(b);
    const product = this._matrixProduct(expr, a, b, aType, bType);
    if (product >= 0) return product;
    // Bring both operands to one scalar type and one shape, so the interpreter is component-wise.
    const scalar = this._commonScalar(aType, bType);
    const shift = expr.op === "<<" || expr.op === ">>";
    const shape = this._commonShape(aType, bType);
    const operandType = shape < 0 ? aType : this._shapeWith(shape, scalar);
    a = this._convert(a, shift ? this._shapeWith(shape, this.types.scalarBase(aType) ?? scalar) : operandType, expr.span);
    b = this._convert(b, shift ? this._shapeWith(shape, this.types.scalarBase(bType) ?? scalar) : operandType, expr.span);
    const comparison = ["==", "!=", "<", ">", "<=", ">="].includes(expr.op);
    const type = comparison ? this.types.withScalar(this._typeOf(a), "bool") : this._typeOf(shift ? a : a);
    const dst = this._temp(type);
    this._emit({ op: "binary", dst, kind: expr.op as BinaryOp, a, b, type }, expr.span);
    return dst;
  }

  /** `m * v`, `v * m`, `m * m`: named operations, since they are not component-wise. */
  private _matrixProduct(expr: Expr & { kind: "binary" }, a: number, b: number, aType: number, bType: number): number {
    if (expr.op !== "*") return -1;
    const ta = this.types.get(aType);
    const tb = this.types.get(bType);
    const matrixA = ta?.kind === "matrix";
    const matrixB = tb?.kind === "matrix";
    if (!matrixA && !matrixB) return -1;
    // A matrix times a scalar stays component-wise, which the interpreter handles by shape.
    if (matrixA && tb?.kind === "scalar") return -1;
    if (matrixB && ta?.kind === "scalar") return -1;
    let type: number;
    let name: string;
    if (matrixA && matrixB) {
      // (columns of b) columns, each as tall as a's columns.
      type = this.types.matrix(ta.column, tb.columns);
      name = "matrix.multiply";
    } else if (matrixA) {
      type = ta.column;
      name = "matrix.times.vector";
    } else {
      const column = this.types.get(tb!.kind === "matrix" ? tb.column : 0);
      type = this.types.vector(column?.kind === "vector" ? column.element : this.types.float, tb!.kind === "matrix" ? tb.columns : 4);
      name = "vector.times.matrix";
    }
    const dst = this._temp(type);
    this._emit({ op: "builtin", dst, name, args: [a, b], type }, expr.span);
    return dst;
  }

  private _shortCircuit(expr: Expr & { kind: "binary" }): number {
    const result = this._symbol("", this.types.bool, "register", true).id;
    const a = this._condition(expr.left);
    this._emit({ op: "move", dst: result, src: a, type: this.types.bool }, expr.span);
    const branch = this._emit({ op: "branch", cond: a, then: -1, otherwise: -1 }, expr.span);
    // `a && b` evaluates b when a is true; `a || b` when a is false.
    const evaluate = this.instructions.length;
    const b = this._condition(expr.right);
    this._emit({ op: "move", dst: result, src: b, type: this.types.bool }, expr.span);
    const after = this.instructions.length;
    if (branch.op === "branch") {
      branch.then = expr.op === "&&" ? evaluate : after;
      branch.otherwise = expr.op === "&&" ? after : evaluate;
    }
    return result;
  }

  /** An expression as a single bool, for an `if`, a loop or a short circuit. */
  private _condition(expr: Expr): number {
    const value = this._value(expr);
    const type = this._typeOf(value);
    if (this.types.isBool(type) && this.types.get(type)?.kind === "scalar") return value;
    const t = this.types.get(type);
    if (t?.kind === "vector") {
      // A vector condition is not legal MSL; `all` is the reading that keeps a shader running.
      const dst = this._temp(this.types.bool);
      this._emit({ op: "builtin", dst, name: "all", args: [value], type: this.types.bool }, expr.span);
      return dst;
    }
    return this._convert(value, this.types.bool, expr.span);
  }

  private _assign(expr: Expr & { kind: "assign" }): number {
    // A swizzle on the left writes components back: `v.xy = w`.
    if (expr.target.kind === "member") {
      const objectType = this._peekType(expr.target.object);
      const swizzle = this._swizzleOf(objectType, expr.target.name);
      if (swizzle) return this._assignSwizzle(expr, expr.target, swizzle.indices, objectType);
    }
    const address = this._address(expr.target);
    if (!address) {
      this._warn(expr.span, "the left side of an assignment is not something that can be assigned to");
      return this._value(expr.value);
    }
    let value: number;
    if (expr.op === "=") {
      value = this._initialize(expr.value, address.type, expr.span);
    } else {
      const before = this._temp(address.type);
      this._emit({ op: "load", dst: before, ptr: address.ptr, type: address.type }, expr.span);
      value = this._compound(expr.op, before, expr.value, address.type, expr.span);
    }
    this._emit({ op: "store", ptr: address.ptr, value, type: address.type }, expr.span);
    return value;
  }

  private _assignSwizzle(expr: Expr & { kind: "assign" }, target: Expr & { kind: "member" }, indices: number[], objectType: number): number {
    const address = this._address(target.object);
    if (!address) {
      this._warn(expr.span, `${target.name} cannot be assigned to`);
      return this._value(expr.value);
    }
    const element = this.types.elementOf(objectType);
    const partType = indices.length === 1 ? element : this.types.vector(element, indices.length);
    const before = this._temp(address.type);
    this._emit({ op: "load", dst: before, ptr: address.ptr, type: address.type }, expr.span);
    let value: number;
    if (expr.op === "=") {
      value = this._convert(this._value(expr.value), partType, expr.span);
    } else {
      const part = this._temp(partType);
      this._emit({ op: "extract", dst: part, value: before, indices, type: partType }, expr.span);
      value = this._compound(expr.op, part, expr.value, partType, expr.span);
    }
    const after = this._temp(address.type);
    this._emit({ op: "insert", dst: after, value: before, element: value, indices, type: address.type }, expr.span);
    this._emit({ op: "store", ptr: address.ptr, value: after, type: address.type }, expr.span);
    return value;
  }

  /** The right side of a compound assignment applied to the value already there. */
  private _compound(op: string, before: number, rightExpr: Expr, type: number, span: Span): number {
    const right = this._value(rightExpr);
    const kind = op.slice(0, -1) as BinaryOp;
    const shift = kind === "<<" || kind === ">>";
    const operand = shift ? right : this._convert(right, type, span);
    const dst = this._temp(type);
    this._emit({ op: "binary", dst, kind, a: before, b: operand, type }, span);
    return dst;
  }

  private _conditional(expr: Expr & { kind: "conditional" }): number {
    const thenType = this._peekType(expr.then);
    const otherwiseType = this._peekType(expr.otherwise);
    const scalar = this._commonScalar(thenType, otherwiseType);
    const shape = this._commonShape(thenType, otherwiseType);
    const type = shape < 0 ? thenType : this._shapeWith(shape, scalar);
    const result = this._symbol("", type, "register", true).id;
    const cond = this._condition(expr.cond);
    const branch = this._emit({ op: "branch", cond, then: -1, otherwise: -1 }, expr.span);
    if (branch.op === "branch") branch.then = this.instructions.length;
    const a = this._convert(this._value(expr.then), type, expr.span);
    this._emit({ op: "move", dst: result, src: a, type }, expr.span);
    const skip = this._emit({ op: "jump", target: -1 }, expr.span);
    if (branch.op === "branch") branch.otherwise = this.instructions.length;
    const b = this._convert(this._value(expr.otherwise), type, expr.span);
    this._emit({ op: "move", dst: result, src: b, type }, expr.span);
    if (skip.op === "jump") skip.target = this.instructions.length;
    return result;
  }

  // -------------------------------------------------------------------------------------------
  // Conversions and construction

  private _commonScalar(a: number, b: number): ScalarBase {
    const sa = this.types.scalarBase(a);
    const sb = this.types.scalarBase(b);
    if (!sa) return sb ?? "float";
    if (!sb) return sa;
    return RANK[sa] >= RANK[sb] ? sa : sb;
  }

  /** The type whose shape both operands take: the vector or matrix among them, -1 when neither is. */
  private _commonShape(a: number, b: number): number {
    const ta = this.types.get(a);
    const tb = this.types.get(b);
    if (ta?.kind === "matrix") return a;
    if (tb?.kind === "matrix") return b;
    if (ta?.kind === "vector") return a;
    if (tb?.kind === "vector") return b;
    return -1;
  }

  private _shapeWith(shape: number, scalar: ScalarBase): number {
    return this.types.withScalar(shape, scalar);
  }

  /** A register converted to a type: a scalar conversion, a broadcast, or both. */
  private _convert(value: number, type: number, span: Span): number {
    if (value < 0) return value;
    const from = this._typeOf(value);
    if (from === type) return value;
    const tf = this.types.get(from);
    const tt = this.types.get(type);
    if (!tf || !tt) return value;
    // A pointer keeps its value; only the type it is read through changes.
    if (tt.kind === "pointer" || tt.kind === "texture" || tt.kind === "sampler" || tt.kind === "opaque") return value;
    if (tt.kind === "struct" || tt.kind === "array") {
      if (tf.kind === tt.kind) return value;
      return this._constructValues([value], type, span);
    }
    // A scalar becoming a vector or matrix is a broadcast.
    if ((tt.kind === "vector" || tt.kind === "matrix") && tf.kind === "scalar") {
      const scalar = this._convertScalar(value, this.types.elementOf(this.types.elementOf(type)), span);
      const dst = this._temp(type);
      this._emit({ op: "construct", dst, args: [scalar], type }, span);
      return dst;
    }
    if (tt.kind === "vector" && tf.kind === "vector" && tf.count !== tt.count) {
      return this._constructValues([value], type, span);
    }
    return this._convertScalar(value, type, span);
  }

  private _convertScalar(value: number, type: number, span: Span): number {
    if (this._typeOf(value) === type) return value;
    const dst = this._temp(type);
    this._emit({ op: "convert", dst, a: value, type }, span);
    return dst;
  }

  /** `float4(a, b)`, `S{...}`, `array<float,3>{...}`: the arguments lowered, then built into the type. */
  private _constructFrom(args: Expr[], type: number, span: Span): number {
    const t = this.types.get(type);
    // A single argument of the same shape is a conversion, not a build: `float4(someFloat4)`.
    if (args.length === 1 && args[0].kind !== "initializer") {
      const only = this._value(args[0]);
      const from = this.types.get(this._typeOf(only));
      if (t?.kind === "vector" && from?.kind === "vector" && from.count === t.count) return this._convertScalar(only, type, span);
      if (t?.kind === "scalar" || t?.kind === "vector" || t?.kind === "matrix") return this._convert(only, type, span);
      if (from && t && from.kind === t.kind) return only;
      return this._constructValues([only], type, span);
    }
    // Members and elements are initialized by their own type, so a nested `{...}` works.
    const registers: number[] = [];
    if (t?.kind === "struct") {
      args.forEach((a, i) => {
        const member = t.members[i];
        registers.push(member ? this._initialize(a, member.type, span) : this._value(a));
      });
    } else if (t?.kind === "array") {
      for (const a of args) registers.push(this._initialize(a, t.element, span));
    } else if (t?.kind === "matrix") {
      for (const a of args) registers.push(this._value(a));
    } else {
      for (const a of args) registers.push(this._value(a));
    }
    return this._constructValues(registers, type, span);
  }

  private _constructValues(args: number[], type: number, span: Span): number {
    const dst = this._temp(type);
    this._emit({ op: "construct", dst, args, type }, span);
    return dst;
  }

  // -------------------------------------------------------------------------------------------
  // Calls

  private _typeCall(expr: Expr & { kind: "typeCall" }): number {
    const type = this._type(expr.type);
    if (expr.name === "sizeof") {
      const dst = this._temp(this.types.uint);
      this._emit({ op: "const", dst, value: this.types.sizeOf(type), type: this.types.uint }, expr.span);
      return dst;
    }
    if (expr.name === "alignof") {
      const dst = this._temp(this.types.uint);
      this._emit({ op: "const", dst, value: this.types.alignOf(type), type: this.types.uint }, expr.span);
      return dst;
    }
    const a = expr.args.length ? this._value(expr.args[0]) : -1;
    if (a < 0) return this._constructValues([], type, expr.span);
    if (expr.name === "as_type") {
      const dst = this._temp(type);
      this._emit({ op: "bitcast", dst, a, type }, expr.span);
      return dst;
    }
    return this._convert(a, type, expr.span);
  }

  private _call(expr: Expr & { kind: "call" }): number {
    // A method on a texture or sampler: `source.sample(smp, uv)`.
    if (expr.callee.kind === "member") {
      const objectType = this._peekType(expr.callee.object);
      const t = this.types.get(objectType);
      if (t?.kind === "texture" || t?.kind === "sampler" || t?.kind === "atomic") {
        const object = this._value(expr.callee.object);
        const args = [object, ...expr.args.map((a) => this._value(a))];
        const type = this._textureResultType(t.kind === "texture" ? objectType : objectType, expr.callee.name);
        const dst = this._temp(type);
        this._emit({ op: "builtin", dst, name: `texture.${expr.callee.name}`, args, type }, expr.span);
        return dst;
      }
    }
    if (expr.callee.kind !== "name") {
      this._warn(expr.span, "a call through something other than a name is not supported");
      return this._value(expr.callee);
    }
    const name = expr.callee.name;
    const overloads = this._functionsByName.get(name);
    const args = expr.args.map((a) => this._value(a));
    if (overloads?.length) {
      const target = this._pickOverload(overloads, args);
      const decl = this._unit.functions[target];
      const converted = args.map((a, i) => {
        const param = decl.params[i];
        if (!param) return a;
        const type = this._withArrayDims(this._type(param.type), param.arrayDims);
        const t = this.types.get(type);
        // A reference parameter takes the argument's address, not its value.
        if (t?.kind === "pointer" && t.reference) {
          const address = this._address(expr.args[i]);
          return address ? address.ptr : a;
        }
        return this._convert(a, type, expr.span);
      });
      const type = this._type(decl.returnType);
      const dst = this._temp(type);
      this._emit({ op: "call", dst, target, args: converted, type }, expr.span);
      return dst;
    }
    // The standard library: the result's type follows from the arguments (stdlib.ts computes the value).
    const type = this._builtinResultType(name, args, expr.span);
    const dst = this._temp(type);
    this._emit({ op: "builtin", dst, name, args, type }, expr.span);
    return dst;
  }

  private _pickOverload(overloads: number[], args: number[]): number {
    let best = overloads[0];
    let bestScore = -1;
    for (const index of overloads) {
      const decl = this._unit.functions[index];
      if (decl.params.length !== args.length) continue;
      let score = 1;
      decl.params.forEach((p, i) => {
        const want = this._withArrayDims(this._type(p.type), p.arrayDims);
        const have = this._typeOf(args[i]);
        if (want === have) score += 4;
        else if (this.types.scalarBase(want) === this.types.scalarBase(have)) score += 2;
        else if (this.types.components(want) === this.types.components(have)) score += 1;
      });
      if (score > bestScore) {
        bestScore = score;
        best = index;
      }
    }
    return best;
  }

  private _textureResultType(textureType: number, method: string): number {
    const t = this.types.get(textureType);
    if (t?.kind === "atomic") return t.element;
    if (t?.kind !== "texture") return this.types.float;
    switch (method) {
      case "sample":
      case "read":
      case "gather":
        // A depth texture samples to one value; a colour texture to four.
        return t.depth && method !== "gather" ? t.sampled : this.types.vector(t.sampled, 4);
      case "sample_compare":
      case "gather_compare":
        return method === "gather_compare" ? this.types.vector(this.types.float, 4) : this.types.float;
      case "write":
        return this.types.void_;
      case "get_width":
      case "get_height":
      case "get_depth":
      case "get_num_mip_levels":
      case "get_array_size":
      case "get_num_samples":
        return this.types.uint;
      default:
        return this.types.vector(t.sampled, 4);
    }
  }

  /**
   * The type a standard library call gives. The rules cover the shapes the library actually has:
   * most functions give what their first argument is, the comparisons give bools, the ones that
   * reduce a vector give its element, and a handful are fixed.
   */
  private _builtinResultType(name: string, args: number[], span: Span): number {
    const first = args.length ? this._typeOf(args[0]) : this.types.float;
    switch (name) {
      case "all": case "any": case "is_null_texture":
        return this.types.bool;
      case "isnan": case "isinf": case "isfinite": case "isnormal": case "signbit":
        return this.types.withScalar(first, "bool");
      case "length": case "distance": case "length_squared": case "distance_squared":
      case "dot": case "determinant":
        return this.types.elementOf(this.types.elementOf(first));
      case "cross":
        return first;
      case "transpose": {
        const t = this.types.get(first);
        if (t?.kind !== "matrix") return first;
        const column = this.types.get(t.column);
        const rows = column?.kind === "vector" ? column.count : 1;
        const element = column?.kind === "vector" ? column.element : t.column;
        return this.types.matrix(this.types.vector(element, t.columns), rows);
      }
      case "select":
        return args.length > 1 ? this._typeOf(args[1]) : first;
      case "mix": case "clamp": case "smoothstep": case "fma": case "mad":
        // These take the shape of their widest argument: `mix(a, b, 0.5)` is a vector.
        return args.reduce((wide, a) => (this.types.components(this._typeOf(a)) > this.types.components(wide) ? this._typeOf(a) : wide), first);
      case "step":
        return args.length > 1 ? this._typeOf(args[1]) : first;
      case "min": case "max": case "pow": case "atan2": case "fmod": case "copysign": case "ldexp": case "powr":
        return args.reduce((wide, a) => (this.types.components(this._typeOf(a)) > this.types.components(wide) ? this._typeOf(a) : wide), first);
      case "popcount": case "clz": case "ctz": case "absdiff":
        return first;
      case "float4": case "float3": case "float2":
        return this.types.builtin(name) ?? first;
      case "atomic_load_explicit": case "atomic_fetch_add_explicit": case "atomic_fetch_sub_explicit":
      case "atomic_fetch_min_explicit": case "atomic_fetch_max_explicit": case "atomic_fetch_and_explicit":
      case "atomic_fetch_or_explicit": case "atomic_fetch_xor_explicit": case "atomic_exchange_explicit": {
        const t = this.types.get(first);
        const pointee = t?.kind === "pointer" ? this.types.get(t.pointee) : undefined;
        return pointee?.kind === "atomic" ? pointee.element : t?.kind === "pointer" ? t.pointee : this.types.uint;
      }
      case "atomic_store_explicit": case "threadgroup_barrier": case "simdgroup_barrier": case "atomic_compare_exchange_weak_explicit":
        return name === "atomic_compare_exchange_weak_explicit" ? this.types.bool : this.types.void_;
      case "abs": {
        // abs of a signed integer gives the unsigned type in MSL.
        const base = this.types.scalarBase(first);
        if (base === "int") return this.types.withScalar(first, "uint");
        if (base === "short") return this.types.withScalar(first, "ushort");
        if (base === "char") return this.types.withScalar(first, "uchar");
        return first;
      }
      default: {
        const t = this.types.get(first);
        if (t?.kind === "pointer") return t.pointee;
        if (first === this.types.void_) this._warn(span, `${name} is not a function this shader declares or the interpreter knows`);
        return first;
      }
    }
  }
}

/** A literal's value: an integer stays exact, a float is a number, a 64-bit integer a bigint. */
function numberOf(text: string): Value {
  const clean = text.replace(/'/g, "");
  const suffix = /[uUlLfFhH]*$/.exec(clean)?.[0] ?? "";
  const digits = clean.slice(0, clean.length - suffix.length);
  const long = /[lL]{1,2}/.test(suffix) && !/[fFhH]/.test(suffix);
  if (/^0[xX]/.test(digits) && !/[.pP]/.test(digits)) {
    const big = BigInt(digits);
    return long ? BigInt.asIntN(64, big) : Number(BigInt.asUintN(32, big)) | 0;
  }
  if (/^0[bB]/.test(digits)) return Number(BigInt(digits));
  if (long) return BigInt(digits.split(".")[0] || "0");
  const value = Number(digits);
  return Number.isFinite(value) ? value : 0;
}

/** What binds an entry point's parameter, from its attribute. */
function bindingOf(param: ParamDecl, type: number, types: TypeTable): ParamBinding | undefined {
  const t = types.get(type);
  for (const a of param.attributes) {
    if (a.name === "buffer") return { kind: "buffer", index: a.args[0] ?? 0 };
    if (a.name === "texture") return { kind: "texture", index: a.args[0] ?? 0 };
    if (a.name === "sampler") return { kind: "sampler", index: a.args[0] ?? 0 };
    if (a.name === "stage_in") return { kind: "stage_in" };
    if (a.name === "threadgroup") return { kind: "threadgroup", index: a.args[0] ?? 0 };
    if (BUILTIN_ATTRIBUTES.has(a.name)) return { kind: "builtin", name: a.name };
  }
  // A texture or sampler parameter with no attribute still binds by its position among them.
  if (t?.kind === "texture") return { kind: "texture", index: -1 };
  if (t?.kind === "sampler") return { kind: "sampler", index: -1 };
  if (t?.kind === "pointer") return { kind: "buffer", index: -1 };
  return undefined;
}

export function lowerMsl(unit: Unit): LoweredProgram {
  return new Lowering(unit).run();
}
