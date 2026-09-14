// The MSL parser: tokens (lexer.ts) to a syntax tree (ast.ts).
//
// MSL is C++14 with address spaces, function qualifiers and `[[attributes]]`, but a shader uses a
// small part of that, and a *generated* shader — which is what a capture of an engine holds —
// uses less still. This parses what shaders are written in: structs, typedefs and `using`, free
// functions (overloaded), globals, the C expression grammar with swizzles and constructors, and
// the statements. What it does not parse — classes with methods, templates a shader defines,
// namespaces other than `metal` — is reported as a diagnostic naming the line, so the debugger can
// say why it cannot step a shader instead of stepping it wrongly.
//
// The one genuinely ambiguous piece of C++ that shaders do hit is `(T)x` versus `(expr)`: a
// parenthesized name is a cast only when the name is a type, which is why declared type names are
// collected as parsing goes.
import { tokenize, type LexDiagnostic, type Token } from "./lexer.js";
import type {
  Declarator, Expr, GlobalDecl, ParamDecl, Span, Stmt, StructDecl, StructMemberDecl, SwitchCase, TypeRefNode, Unit,
} from "./ast.js";
import { isTextureName, type Attribute } from "./types.js";

export interface ParseDiagnostic {
  line: number;
  message: string;
}

export interface ParseResult {
  unit: Unit;
  diagnostics: ParseDiagnostic[];
}

const ADDRESS_SPACES = new Set(["device", "constant", "threadgroup", "thread", "threadgroup_imageblock", "ray_data", "object_data"]);
const FUNCTION_QUALIFIERS = new Set(["vertex", "fragment", "kernel", "visible", "extern", "static", "inline", "__attribute__"]);
/** Ignorable declaration specifiers: they change nothing the interpreter does. */
const IGNORED_SPECIFIERS = new Set(["const", "constexpr", "static", "inline", "extern", "volatile", "restrict", "__restrict", "thread_local", "typename"]);

const BUILTIN_TYPE_NAMES = new Set([
  "void", "bool", "char", "uchar", "short", "ushort", "int", "uint", "long", "ulong", "half", "float", "double",
  "signed", "unsigned", "size_t", "ptrdiff_t", "uintptr_t",
  "int8_t", "uint8_t", "int16_t", "uint16_t", "int32_t", "uint32_t", "int64_t", "uint64_t",
  "sampler", "array", "atomic", "atomic_int", "atomic_uint", "atomic_bool", "atomic_float",
  "sampler_array", "texture_array", "vec", "matrix", "ray", "intersector",
]);

function isBuiltinTypeName(name: string): boolean {
  if (BUILTIN_TYPE_NAMES.has(name) || isTextureName(name)) return true;
  // floatN, halfNxM, packed_floatN and the rest.
  return /^(packed_)?(bool|char|uchar|short|ushort|int|uint|long|ulong|half|float|double)\d(x\d)?$/.test(name);
}

class Parser {
  private _tokens: Token[];
  private _at = 0;
  readonly diagnostics: ParseDiagnostic[] = [];
  /** Names that are types: the built-ins, plus every struct, typedef and using as it is declared. */
  private _typeNames = new Set<string>();
  readonly unit: Unit = { structs: [], functions: [], globals: [], aliases: [] };

  constructor(tokens: Token[]) {
    this._tokens = tokens;
  }

  // -------------------------------------------------------------------------------------------
  // Tokens

  private get _t(): Token {
    return this._tokens[this._at] ?? this._tokens[this._tokens.length - 1];
  }

  private _peek(n = 0): Token {
    return this._tokens[Math.min(this._at + n, this._tokens.length - 1)];
  }

  private get _done(): boolean {
    return this._t.kind === "end";
  }

  private _span(): Span {
    return { line: this._t.line, column: this._t.column };
  }

  private _take(): Token {
    const t = this._t;
    if (!this._done) this._at++;
    return t;
  }

  private _is(text: string, n = 0): boolean {
    const t = this._peek(n);
    return t.kind === "punct" ? t.text === text : t.kind === "identifier" && t.text === text;
  }

  private _eat(text: string): boolean {
    if (this._is(text)) {
      this._at++;
      return true;
    }
    return false;
  }

  private _expect(text: string, what: string): boolean {
    if (this._eat(text)) return true;
    this._error(`expected ${text} ${what}, found ${this._t.text || "the end of the shader"}`);
    return false;
  }

  private _error(message: string): void {
    // One diagnostic per line is enough: a cascade after a real error says nothing new.
    if (this.diagnostics[this.diagnostics.length - 1]?.line === this._t.line) return;
    this.diagnostics.push({ line: this._t.line, message });
  }

  /** Skips to after the next `;` or the end of the current braces, to keep parsing after an error. */
  private _recover(): void {
    let depth = 0;
    while (!this._done) {
      if (this._is("{")) depth++;
      if (this._is("}")) {
        if (depth === 0) return;
        depth--;
        this._at++;
        if (depth === 0) return;
        continue;
      }
      if (this._is(";") && depth === 0) {
        this._at++;
        return;
      }
      this._at++;
    }
  }

  // -------------------------------------------------------------------------------------------
  // Declarations

  parseUnit(): void {
    let guard = 0;
    while (!this._done) {
      const before = this._at;
      this._declaration();
      if (this._at === before) {
        // Nothing consumed: skip a token so a malformed shader cannot spin.
        this._at++;
        if (guard++ > 10000) break;
      }
    }
  }

  private _declaration(): void {
    // Things that are declarations but change nothing here.
    if (this._eat(";")) return;
    if (this._is("using")) {
      this._using();
      return;
    }
    if (this._is("namespace")) {
      // `namespace metal { ... }` around a shader's own helpers: parse the contents in place.
      this._take();
      if (this._t.kind === "identifier") this._take();
      if (this._eat("{")) {
        while (!this._done && !this._is("}")) this._declaration();
        this._eat("}");
      }
      return;
    }
    if (this._is("typedef")) {
      this._typedef();
      return;
    }
    if (this._is("struct") || this._is("class") || this._is("union")) {
      const struct = this._struct();
      // `struct S { ... } name;` also declares a global.
      if (struct && !this._is(";")) this._afterType(this._typeRefOf(struct.name), []);
      this._eat(";");
      return;
    }
    this._afterSpecifiers();
  }

  /** A function or a global, after its qualifiers and attributes. */
  private _afterSpecifiers(): void {
    const span = this._span();
    const attributes = this._attributes();
    let qualifier = "";
    for (;;) {
      const t = this._t;
      if (t.kind !== "identifier") break;
      if (FUNCTION_QUALIFIERS.has(t.text)) {
        if (t.text === "vertex" || t.text === "fragment" || t.text === "kernel") qualifier = t.text;
        this._take();
        // __attribute__((...)) and the like carry a parenthesized payload.
        if (t.text === "__attribute__" && this._is("(")) this._skipBalanced("(", ")");
        continue;
      }
      if (IGNORED_SPECIFIERS.has(t.text)) {
        this._take();
        continue;
      }
      break;
    }
    attributes.push(...this._attributes());
    if (this._is("struct") || this._is("class")) {
      const struct = this._struct();
      if (struct && !this._is(";")) this._afterType(this._typeRefOf(struct.name), attributes, qualifier, span);
      this._eat(";");
      return;
    }
    if (this._done) return;
    const type = this._typeRef();
    if (!type) {
      this._error(`expected a declaration, found ${this._t.text}`);
      this._recover();
      return;
    }
    this._afterType(type, attributes, qualifier, span);
  }

  /** After a declaration's type: a function when a `(` follows the name, else one or more globals. */
  private _afterType(type: TypeRefNode, attributes: Attribute[], qualifier = "", span = this._span()): void {
    if (this._eat(";")) return;
    const nameToken = this._t;
    if (nameToken.kind !== "identifier") {
      this._error(`expected a name in a declaration, found ${nameToken.text || "the end of the shader"}`);
      this._recover();
      return;
    }
    this._take();
    if (this._is("(")) {
      this._function(type, nameToken.text, qualifier, attributes, span);
      return;
    }
    // Globals, possibly several: `constant float a = 1, b = 2;`
    for (;;) {
      const dims = this._arrayDims();
      const global: GlobalDecl = { name: nameToken.text, type, attributes: [...attributes, ...this._attributes()], arrayDims: dims, span };
      if (this._eat("=")) global.init = this._assignment();
      this.unit.globals.push(global);
      if (!this._eat(",")) break;
      const next = this._t;
      if (next.kind !== "identifier") break;
      this._take();
      nameToken.text = next.text;
    }
    this._expect(";", "after a declaration");
  }

  private _function(returnType: TypeRefNode, name: string, qualifier: string, attributes: Attribute[], span: Span): void {
    const params: ParamDecl[] = [];
    this._expect("(", "before a parameter list");
    while (!this._done && !this._is(")")) {
      const param = this._parameter();
      if (param) params.push(param);
      else break;
      if (!this._eat(",")) break;
    }
    this._expect(")", "after a parameter list");
    // Trailing qualifiers and attributes: `const`, `[[clang::optnone]]`.
    while (this._t.kind === "identifier" && IGNORED_SPECIFIERS.has(this._t.text)) this._take();
    const returnAttributes = [...attributes, ...this._attributes()];
    let body: Stmt[] | null = null;
    if (this._is("{")) {
      body = this._blockBody();
    } else {
      this._expect(";", "after a function declaration");
    }
    this.unit.functions.push({ name, qualifier, returnType, returnAttributes, params, body, span });
  }

  private _parameter(): ParamDecl | null {
    const span = this._span();
    const attributes = this._attributes();
    while (this._t.kind === "identifier" && IGNORED_SPECIFIERS.has(this._t.text)) this._take();
    const type = this._typeRef();
    if (!type) {
      this._error(`expected a parameter type, found ${this._t.text}`);
      return null;
    }
    let name = "";
    if (this._t.kind === "identifier" && !this._is("[")) name = this._take().text;
    const arrayDims = this._arrayDims();
    attributes.push(...this._attributes());
    const param: ParamDecl = { name, type, attributes, arrayDims, span };
    if (this._eat("=")) param.init = this._assignment();
    return param;
  }

  private _struct(): StructDecl | null {
    const span = this._span();
    this._take();   // struct / class / union
    let name = "";
    if (this._t.kind === "identifier") name = this._take().text;
    if (name) this._typeNames.add(name);
    if (!this._is("{")) {
      // A forward declaration, or `struct S x;` naming an existing type.
      return name ? { name, members: [], span } : null;
    }
    this._take();
    const members: StructMemberDecl[] = [];
    while (!this._done && !this._is("}")) {
      if (this._eat(";")) continue;
      // A struct's own methods and nested types are beyond what a debugged shader needs.
      if (this._is("struct") || this._is("class") || this._is("union")) {
        this._struct();
        this._eat(";");
        continue;
      }
      if (this._is("typedef") || this._is("using")) {
        this._declaration();
        continue;
      }
      const memberSpan = this._span();
      const attributes = this._attributes();
      while (this._t.kind === "identifier" && IGNORED_SPECIFIERS.has(this._t.text)) this._take();
      const type = this._typeRef();
      if (!type) {
        this._error(`expected a member type in struct ${name}, found ${this._t.text}`);
        this._recover();
        continue;
      }
      for (;;) {
        if (this._t.kind !== "identifier") {
          this._error(`expected a member name in struct ${name}, found ${this._t.text}`);
          break;
        }
        const memberName = this._take().text;
        if (this._is("(")) {
          // A method: skipped, with its body, since the debugger only reads data members.
          this._skipBalanced("(", ")");
          while (this._t.kind === "identifier" && IGNORED_SPECIFIERS.has(this._t.text)) this._take();
          if (this._is("{")) this._skipBalanced("{", "}");
          else this._eat(";");
          break;
        }
        const arrayDims = this._arrayDims();
        members.push({ name: memberName, type, attributes: [...attributes, ...this._attributes()], arrayDims, span: memberSpan });
        if (!this._eat(",")) break;
      }
      this._eat(";");
    }
    this._expect("}", `after the members of struct ${name}`);
    const struct: StructDecl = { name, members, span };
    if (name) this.unit.structs.push(struct);
    return struct;
  }

  private _typedef(): void {
    const span = this._span();
    this._take();
    if (this._is("struct") || this._is("class")) {
      const struct = this._struct();
      if (this._t.kind === "identifier" && struct) {
        const alias = this._take().text;
        this._typeNames.add(alias);
        this.unit.aliases.push({ name: alias, type: this._typeRefOf(struct.name), span });
      }
      this._eat(";");
      return;
    }
    const type = this._typeRef();
    if (!type) {
      this._recover();
      return;
    }
    if (this._t.kind === "identifier") {
      const alias = this._take().text;
      this._typeNames.add(alias);
      // `typedef float Weights[4]` is an array alias.
      const dims = this._arrayDims();
      this.unit.aliases.push({ name: alias, type: dims.length ? { ...type, name: type.name } : type, span });
    }
    this._expect(";", "after a typedef");
  }

  private _using(): void {
    const span = this._span();
    this._take();
    // `using namespace metal;` brings the standard library in, which is always in scope here.
    if (this._eat("namespace")) {
      while (!this._done && !this._is(";")) this._take();
      this._eat(";");
      return;
    }
    if (this._t.kind !== "identifier") {
      this._recover();
      return;
    }
    const alias = this._take().text;
    if (!this._eat("=")) {
      this._eat(";");
      return;
    }
    const type = this._typeRef();
    if (type) {
      this._typeNames.add(alias);
      this.unit.aliases.push({ name: alias, type, span });
    }
    this._expect(";", "after a using declaration");
  }

  // -------------------------------------------------------------------------------------------
  // Types and attributes

  private _typeRefOf(name: string): TypeRefNode {
    return { name, templateArgs: [], pointers: 0, reference: false, const_: false, span: this._span() };
  }

  /** Whether the tokens at `n` begin a type name. */
  private _looksLikeType(n = 0): boolean {
    const t = this._peek(n);
    if (t.kind !== "identifier") return false;
    if (ADDRESS_SPACES.has(t.text) || IGNORED_SPECIFIERS.has(t.text)) return true;
    if (t.text === "struct" || t.text === "class") return true;
    // `metal::float4`.
    if (this._peek(n + 1).text === "::") return this._looksLikeType(n + 2);
    return isBuiltinTypeName(t.text) || this._typeNames.has(t.text);
  }

  private _typeRef(): TypeRefNode | null {
    const span = this._span();
    let addressSpace: string | undefined;
    let const_ = false;
    for (;;) {
      const t = this._t;
      if (t.kind !== "identifier") break;
      if (ADDRESS_SPACES.has(t.text)) {
        addressSpace = t.text;
        this._take();
        continue;
      }
      if (t.text === "const") {
        const_ = true;
        this._take();
        continue;
      }
      if (IGNORED_SPECIFIERS.has(t.text) || t.text === "struct" || t.text === "class" || t.text === "union") {
        this._take();
        continue;
      }
      break;
    }
    if (this._t.kind !== "identifier") return null;
    let name = this._take().text;
    // `metal::float4`, `access::read`.
    while (this._eat("::")) {
      if (this._t.kind !== "identifier") break;
      name = this._take().text;
    }
    // `unsigned int`, `long long`: the second word refines the first.
    while (this._t.kind === "identifier" && (name === "unsigned" || name === "signed" || name === "long") &&
           ["int", "char", "short", "long"].includes(this._t.text)) {
      const second = this._take().text;
      name = name === "unsigned" ? (second === "char" ? "uchar" : second === "short" ? "ushort" : second === "long" ? "ulong" : "uint")
        : name === "signed" ? (second === "char" ? "char" : second === "short" ? "short" : second === "long" ? "long" : "int")
        : "long";
    }
    const templateArgs: (TypeRefNode | number | string)[] = [];
    if (this._is("<")) {
      this._take();
      while (!this._done && !this._is(">")) {
        const arg0: Token = this._t;
        if (arg0.kind === "number") {
          templateArgs.push(Number(this._take().text.replace(/[uUlL']/g, "")));
        } else if (this._looksLikeType()) {
          const arg = this._typeRef();
          if (arg) templateArgs.push(arg);
          else break;
        } else {
          // `access::read`, `metal::sample`: the qualified name as written.
          let text = this._take().text;
          while (this._eat("::")) text = this._t.kind === "identifier" ? this._take().text : text;
          templateArgs.push(text);
        }
        if (!this._eat(",")) break;
      }
      this._expect(">", "after template arguments");
    }
    let pointers = 0;
    let reference = false;
    for (;;) {
      if (this._eat("*")) {
        pointers++;
        continue;
      }
      if (this._is("&") && !this._is("&&")) {
        this._take();
        reference = true;
        continue;
      }
      // `float* const`, `device float* device`.
      if (this._t.kind === "identifier" && (this._t.text === "const" || ADDRESS_SPACES.has(this._t.text)) && pointers > 0) {
        if (ADDRESS_SPACES.has(this._t.text)) addressSpace = this._t.text;
        this._take();
        continue;
      }
      break;
    }
    return { name, templateArgs, addressSpace, pointers, reference, const_, span };
  }

  /** `[[attribute(0)]] [[flat]]`, however many follow each other. */
  private _attributes(): Attribute[] {
    const out: Attribute[] = [];
    while (this._is("[") && this._peek(1).text === "[") {
      this._take();
      this._take();
      while (!this._done && !this._is("]")) {
        if (this._eat(",")) continue;
        if (this._t.kind !== "identifier") {
          this._take();
          continue;
        }
        let name = this._take().text;
        // `clang::optnone`, `metal::flat`: the last component names the attribute.
        while (this._eat("::")) {
          if (this._t.kind === "identifier") name = this._take().text;
        }
        const args: number[] = [];
        let text: string | undefined;
        if (this._eat("(")) {
          while (!this._done && !this._is(")")) {
            const t = this._take();
            if (t.kind === "number") args.push(Number(t.text.replace(/[uUlL']/g, "")));
            else if (t.kind === "identifier") text = text === undefined ? t.text : `${text} ${t.text}`;
            else if (t.text !== ",") text = text === undefined ? t.text : `${text}${t.text}`;
          }
          this._expect(")", "after an attribute's arguments");
        }
        out.push(text === undefined ? { name, args } : { name, args, text });
      }
      this._expect("]", "after an attribute");
      this._expect("]", "after an attribute");
    }
    return out;
  }

  private _arrayDims(): (Expr | null)[] {
    const dims: (Expr | null)[] = [];
    while (this._is("[") && this._peek(1).text !== "[") {
      this._take();
      dims.push(this._is("]") ? null : this._expression());
      this._expect("]", "after an array size");
    }
    return dims;
  }

  private _skipBalanced(open: string, close: string): void {
    if (!this._eat(open)) return;
    let depth = 1;
    while (!this._done && depth > 0) {
      if (this._is(open)) depth++;
      else if (this._is(close)) depth--;
      this._take();
    }
  }

  // -------------------------------------------------------------------------------------------
  // Statements

  private _blockBody(): Stmt[] {
    const body: Stmt[] = [];
    this._expect("{", "at the start of a block");
    while (!this._done && !this._is("}")) {
      const before = this._at;
      const stmt = this._statement();
      if (stmt) body.push(stmt);
      if (this._at === before) this._at++;
    }
    this._expect("}", "at the end of a block");
    return body;
  }

  private _statement(): Stmt | null {
    const span = this._span();
    if (this._is("{")) return { kind: "block", span, body: this._blockBody() };
    if (this._eat(";")) return { kind: "empty", span };
    const t = this._t;
    if (t.kind === "identifier") {
      switch (t.text) {
        case "if": return this._if();
        case "for": return this._for();
        case "while": return this._while();
        case "do": return this._do();
        case "switch": return this._switch();
        case "return": {
          this._take();
          const value = this._is(";") ? undefined : this._expression();
          this._expect(";", "after return");
          return { kind: "return", span, value };
        }
        case "break":
          this._take();
          this._expect(";", "after break");
          return { kind: "break", span };
        case "continue":
          this._take();
          this._expect(";", "after continue");
          return { kind: "continue", span };
        case "discard_fragment":
          this._take();
          this._skipBalanced("(", ")");
          this._expect(";", "after discard_fragment()");
          return { kind: "discard", span };
        case "typedef":
        case "using":
          this._declaration();
          return null;
        case "struct":
        case "class": {
          const struct = this._struct();
          if (struct && !this._is(";")) {
            // `struct S { ... } s;` inside a function.
            const decl = this._declarationStatement(this._typeRefOf(struct.name), span);
            return decl;
          }
          this._eat(";");
          return null;
        }
        default:
          break;
      }
      // A label (`again:`) is not something a shader needs, but skip it rather than misparse.
      if (this._peek(1).text === ":" && !isBuiltinTypeName(t.text) && !this._typeNames.has(t.text)) {
        this._take();
        this._take();
        return this._statement();
      }
      if (this._looksLikeType() && this._startsDeclaration()) {
        const type = this._typeRef();
        if (!type) {
          this._recover();
          return null;
        }
        return this._declarationStatement(type, span);
      }
    }
    const expr = this._expression();
    this._expect(";", "after an expression statement");
    return { kind: "expr", span, expr };
  }

  /**
   * Whether what follows is a declaration rather than an expression. `float4 x = ...` is;
   * `float4(1)` (a constructor call) is not, and neither is `x * y` when `x` happens to be a type
   * name — which cannot happen in a shader, so a type name followed by a name is enough.
   */
  private _startsDeclaration(): boolean {
    let n = 0;
    // Skip the type's own tokens: qualifiers, name, `::`, template arguments, `*` and `&`.
    for (;;) {
      const t = this._peek(n);
      if (t.kind === "identifier" && (ADDRESS_SPACES.has(t.text) || IGNORED_SPECIFIERS.has(t.text) || t.text === "struct")) {
        n++;
        continue;
      }
      break;
    }
    if (this._peek(n).kind !== "identifier") return false;
    n++;
    while (this._peek(n).text === "::") n += 2;
    if (this._peek(n).text === "<") {
      let depth = 0;
      for (; this._peek(n).kind !== "end"; n++) {
        if (this._peek(n).text === "<") depth++;
        else if (this._peek(n).text === ">") {
          depth--;
          if (depth === 0) {
            n++;
            break;
          }
        }
      }
    }
    while (this._peek(n).text === "*" || this._peek(n).text === "&" ||
           (this._peek(n).kind === "identifier" && (this._peek(n).text === "const" || ADDRESS_SPACES.has(this._peek(n).text)))) n++;
    return this._peek(n).kind === "identifier";
  }

  private _declarationStatement(type: TypeRefNode, span: Span): Stmt {
    const declarators: Declarator[] = [];
    for (;;) {
      if (this._t.kind !== "identifier") {
        this._error(`expected a variable name, found ${this._t.text}`);
        break;
      }
      const nameSpan = this._span();
      const name = this._take().text;
      const arrayDims = this._arrayDims();
      this._attributes();
      const declarator: Declarator = { name, arrayDims, span: nameSpan };
      if (this._eat("=")) declarator.init = this._is("{") ? this._initializer() : this._assignment();
      else if (this._is("(")) {
        // `float3 v(1, 2, 3)`: a constructor call in a declaration.
        this._take();
        const args: Expr[] = [];
        while (!this._done && !this._is(")")) {
          args.push(this._assignment());
          if (!this._eat(",")) break;
        }
        this._expect(")", "after a constructor's arguments");
        declarator.init = { kind: "construct", span: nameSpan, type, args };
      } else if (this._is("{")) {
        declarator.init = this._initializer();
      }
      declarators.push(declarator);
      if (!this._eat(",")) break;
    }
    this._expect(";", "after a declaration");
    return { kind: "decl", span, type, declarators };
  }

  private _if(): Stmt {
    const span = this._span();
    this._take();
    this._expect("(", "after if");
    const cond = this._expression();
    this._expect(")", "after an if condition");
    const then = this._statement() ?? { kind: "empty", span };
    let otherwise: Stmt | undefined;
    if (this._eat("else")) otherwise = this._statement() ?? { kind: "empty", span };
    return { kind: "if", span, cond, then, otherwise };
  }

  private _for(): Stmt {
    const span = this._span();
    this._take();
    this._expect("(", "after for");
    let init: Stmt | undefined;
    if (!this._is(";")) {
      const initSpan = this._span();
      if (this._looksLikeType() && this._startsDeclaration()) {
        const type = this._typeRef();
        init = type ? this._declarationStatement(type, initSpan) : undefined;
      } else {
        init = { kind: "expr", span: initSpan, expr: this._expression() };
        this._expect(";", "after a for initializer");
      }
    } else {
      this._take();
    }
    const cond = this._is(";") ? undefined : this._expression();
    this._expect(";", "after a for condition");
    const step = this._is(")") ? undefined : this._expression();
    this._expect(")", "after a for clause");
    const body = this._statement() ?? { kind: "empty", span };
    return { kind: "for", span, init, cond, step, body };
  }

  private _while(): Stmt {
    const span = this._span();
    this._take();
    this._expect("(", "after while");
    const cond = this._expression();
    this._expect(")", "after a while condition");
    const body = this._statement() ?? { kind: "empty", span };
    return { kind: "while", span, cond, body };
  }

  private _do(): Stmt {
    const span = this._span();
    this._take();
    const body = this._statement() ?? { kind: "empty", span };
    this._expect("while", "after a do body");
    this._expect("(", "after do ... while");
    const cond = this._expression();
    this._expect(")", "after a do ... while condition");
    this._expect(";", "after do ... while");
    return { kind: "do", span, body, cond };
  }

  private _switch(): Stmt {
    const span = this._span();
    this._take();
    this._expect("(", "after switch");
    const value = this._expression();
    this._expect(")", "after a switch value");
    this._expect("{", "at the start of a switch");
    const cases: SwitchCase[] = [];
    while (!this._done && !this._is("}")) {
      const caseSpan = this._span();
      if (this._eat("case")) {
        const label = this._conditional();
        this._expect(":", "after a case label");
        cases.push({ value: label, span: caseSpan, body: [] });
        continue;
      }
      if (this._eat("default")) {
        this._expect(":", "after default");
        cases.push({ value: null, span: caseSpan, body: [] });
        continue;
      }
      const stmt = this._statement();
      if (!cases.length) {
        // Statements before the first label are unreachable; keep parsing regardless.
        if (stmt) cases.push({ value: null, span: caseSpan, body: [stmt] });
        continue;
      }
      if (stmt) cases[cases.length - 1].body.push(stmt);
    }
    this._expect("}", "at the end of a switch");
    return { kind: "switch", span, value, cases };
  }

  // -------------------------------------------------------------------------------------------
  // Expressions

  private _expression(): Expr {
    let left = this._assignment();
    while (this._is(",")) {
      const span = this._span();
      this._take();
      const right = this._assignment();
      left = { kind: "binary", span, op: ",", left, right };
    }
    return left;
  }

  private _initializer(): Expr {
    const span = this._span();
    this._expect("{", "at the start of an initializer");
    const values: Expr[] = [];
    while (!this._done && !this._is("}")) {
      values.push(this._is("{") ? this._initializer() : this._assignment());
      if (!this._eat(",")) break;
    }
    this._expect("}", "at the end of an initializer");
    return { kind: "initializer", span, values };
  }

  private _assignment(): Expr {
    const span = this._span();
    const target = this._conditional();
    const op = this._t.text;
    if (this._t.kind === "punct" && ["=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="].includes(op)) {
      this._take();
      const value = this._is("{") ? this._initializer() : this._assignment();
      return { kind: "assign", span, op, target, value };
    }
    return target;
  }

  private _conditional(): Expr {
    const span = this._span();
    const cond = this._binary(0);
    if (!this._eat("?")) return cond;
    const then = this._assignment();
    this._expect(":", "in a conditional expression");
    const otherwise = this._assignment();
    return { kind: "conditional", span, cond, then, otherwise };
  }

  private static readonly LEVELS = [
    ["||"], ["&&"], ["|"], ["^"], ["&"], ["==", "!="], ["<", ">", "<=", ">="], ["<<", ">>"], ["+", "-"], ["*", "/", "%"],
  ];

  private _binary(level: number): Expr {
    if (level >= Parser.LEVELS.length) return this._unary();
    let left = this._binary(level + 1);
    for (;;) {
      const t = this._t;
      if (t.kind !== "punct" || !Parser.LEVELS[level].includes(t.text)) return left;
      const span = this._span();
      this._take();
      const right = this._binary(level + 1);
      left = { kind: "binary", span, op: t.text, left, right };
    }
  }

  private _unary(): Expr {
    const span = this._span();
    const t = this._t;
    if (t.kind === "punct" && ["-", "+", "!", "~", "*", "&", "++", "--"].includes(t.text)) {
      this._take();
      return { kind: "unary", span, op: t.text, operand: this._unary(), prefix: true };
    }
    if (t.kind === "identifier" && (t.text === "sizeof" || t.text === "alignof")) {
      this._take();
      if (this._is("(") && this._looksLikeType(1)) {
        this._take();
        const type = this._typeRef();
        this._expect(")", "after sizeof");
        if (type) return { kind: "typeCall", span, name: t.text, type, args: [] };
      }
      return { kind: "unary", span, op: t.text, operand: this._unary(), prefix: true };
    }
    // A cast: `(float)x`, `(device float*)p`. Only when the parenthesized tokens are a type and
    // what follows can start an expression — `(x) * y` must stay a multiplication.
    if (t.kind === "punct" && t.text === "(" && this._looksLikeType(1)) {
      const save = this._at;
      this._take();
      const type = this._typeRef();
      if (type && this._is(")")) {
        this._take();
        if (this._canStartExpression()) return { kind: "cast", span, type, operand: this._unary() };
      }
      this._at = save;
    }
    return this._postfix(this._primary());
  }

  private _canStartExpression(): boolean {
    const t = this._t;
    if (t.kind === "identifier" || t.kind === "number" || t.kind === "string" || t.kind === "char") return true;
    return t.kind === "punct" && ["(", "-", "+", "!", "~", "*", "&", "++", "--"].includes(t.text);
  }

  private _postfix(expr: Expr): Expr {
    for (;;) {
      const span = this._span();
      if (this._is(".") || this._is("->")) {
        const arrow = this._t.text === "->";
        this._take();
        if (this._t.kind !== "identifier") {
          this._error("expected a member name after .");
          return expr;
        }
        expr = { kind: "member", span, object: expr, name: this._take().text, arrow };
        continue;
      }
      if (this._is("[")) {
        this._take();
        const index = this._expression();
        this._expect("]", "after an index");
        expr = { kind: "index", span, object: expr, index };
        continue;
      }
      if (this._is("(")) {
        this._take();
        const args: Expr[] = [];
        while (!this._done && !this._is(")")) {
          args.push(this._is("{") ? this._initializer() : this._assignment());
          if (!this._eat(",")) break;
        }
        this._expect(")", "after a call's arguments");
        expr = { kind: "call", span, callee: expr, args };
        continue;
      }
      if (this._is("++") || this._is("--")) {
        const op = this._take().text;
        expr = { kind: "unary", span, op, operand: expr, prefix: false };
        continue;
      }
      return expr;
    }
  }

  private _primary(): Expr {
    const span = this._span();
    const t = this._t;
    if (t.kind === "number") {
      this._take();
      return { kind: "number", span, text: t.text };
    }
    if (t.kind === "punct" && t.text === "(") {
      this._take();
      const inner = this._expression();
      this._expect(")", "after a parenthesized expression");
      return inner;
    }
    if (t.kind === "punct" && t.text === "{") return this._initializer();
    if (t.kind === "identifier") {
      if (t.text === "true" || t.text === "false") {
        this._take();
        return { kind: "bool", span, value: t.text === "true" };
      }
      // `as_type<float4>(x)`: a template call whose type argument decides the result.
      if (this._peek(1).text === "<" && (t.text === "as_type" || t.text === "static_cast" || t.text === "reinterpret_cast")) {
        this._take();
        this._take();
        const type = this._typeRef();
        this._expect(">", "after a template argument");
        const args: Expr[] = [];
        if (this._eat("(")) {
          while (!this._done && !this._is(")")) {
            args.push(this._assignment());
            if (!this._eat(",")) break;
          }
          this._expect(")", "after a call's arguments");
        }
        if (type) return { kind: "typeCall", span, name: t.text, type, args };
        return { kind: "number", span, text: "0" };
      }
      // A type used as a constructor: `float4(...)`, `Uniforms{...}`, `array<float,4>{...}`.
      if (this._looksLikeType() && this._isConstructorCall()) {
        const type = this._typeRef();
        if (type) {
          const args: Expr[] = [];
          if (this._eat("(")) {
            while (!this._done && !this._is(")")) {
              args.push(this._is("{") ? this._initializer() : this._assignment());
              if (!this._eat(",")) break;
            }
            this._expect(")", "after a constructor's arguments");
          } else if (this._is("{")) {
            const init = this._initializer();
            if (init.kind === "initializer") args.push(...init.values);
          }
          return { kind: "construct", span, type, args };
        }
      }
      // `metal::min`, `access::read`: keep the last component, which names the function.
      let name = this._take().text;
      while (this._eat("::")) {
        if (this._t.kind === "identifier") name = this._take().text;
      }
      return { kind: "name", span, name };
    }
    if (t.kind === "string" || t.kind === "char") {
      this._take();
      // Nothing in a shader computes with these; a char literal is its code point.
      return { kind: "number", span, text: t.kind === "char" ? String(t.text.charCodeAt(1) || 0) : "0" };
    }
    this._error(`expected an expression, found ${t.text || "the end of the shader"}`);
    this._take();
    return { kind: "number", span, text: "0" };
  }

  /** Whether the type name at the cursor is being used as a constructor (`float4(...)` or `S{...}`). */
  private _isConstructorCall(): boolean {
    let n = 1;
    while (this._peek(n).text === "::") n += 2;
    if (this._peek(n).text === "<") {
      let depth = 0;
      for (; this._peek(n).kind !== "end"; n++) {
        if (this._peek(n).text === "<") depth++;
        else if (this._peek(n).text === ">") {
          depth--;
          if (depth === 0) {
            n++;
            break;
          }
        }
      }
    }
    return this._peek(n).text === "(" || this._peek(n).text === "{";
  }
}

/** Parses a shader's Metal Shading Language source. */
export function parseMsl(source: string, defines?: Map<string, string>): ParseResult & { lexDiagnostics: LexDiagnostic[] } {
  const { tokens, diagnostics: lexDiagnostics } = tokenize(source, defines);
  const parser = new Parser(tokens);
  parser.parseUnit();
  return { unit: parser.unit, diagnostics: parser.diagnostics, lexDiagnostics };
}
