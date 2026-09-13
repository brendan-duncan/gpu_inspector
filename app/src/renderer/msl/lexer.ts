// Tokens of Metal Shading Language, with the preprocessor a captured shader needs.
//
// A capture holds the text the application handed `newLibraryWithSource:`, which is what the
// debugger steps, so every token carries the line it came from in *that* text — a token produced
// by expanding a macro keeps the line of the invocation, not of the definition, so a breakpoint
// lands where the reader sees the code.
//
// The preprocessor is deliberately small: `#include` is dropped (the Metal standard library is
// built into types.ts and stdlib.ts, and a capture has no include paths anyway), `#define` /
// `#undef` / `#if` / `#ifdef` / `#ifndef` / `#elif` / `#else` / `#endif` work, and `#pragma`,
// `#line` and `#error` are skipped. That covers what engines emit; anything else is reported as a
// diagnostic rather than silently mis-parsed.

export type TokenKind = "identifier" | "number" | "string" | "char" | "punct" | "end";

export interface Token {
  kind: TokenKind;
  text: string;
  /** 1-based line in the shader's source text. */
  line: number;
  column: number;
  /** The token came out of a macro expansion, so it is not clickable as itself. */
  expanded?: boolean;
  /** Nothing but whitespace precedes it on its line: what makes a `#` a directive. */
  first?: boolean;
}

export interface LexDiagnostic {
  line: number;
  message: string;
}

interface Macro {
  name: string;
  /** Object-like macros have no parameters. */
  params: string[] | null;
  variadic: boolean;
  body: Token[];
}

// Longest first, so ">>=" wins over ">>" and ">".
const PUNCTUATORS = [
  ">>=", "<<=", "...", "->*",
  "++", "--", "->", "::", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
  "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", ".*",
  "+", "-", "*", "/", "%", "=", "<", ">", "!", "~", "&", "|", "^", "?", ":",
  ";", ",", ".", "(", ")", "[", "]", "{", "}",
];

function isIdentStart(c: string): boolean {
  return /[A-Za-z_$]/.test(c);
}

function isIdent(c: string): boolean {
  return /[A-Za-z0-9_$]/.test(c);
}

function isDigit(c: string): boolean {
  return c >= "0" && c <= "9";
}

/** Splits one line's worth of text into tokens; used by the preprocessor for directive bodies too. */
class Scanner {
  private _text: string;
  private _at = 0;
  private _line = 1;
  private _lineStart = 0;

  constructor(text: string) {
    this._text = text;
  }

  get line(): number {
    return this._line;
  }

  get done(): boolean {
    return this._at >= this._text.length;
  }

  /** Skips whitespace and comments; returns true if a newline was crossed. */
  skipTrivia(stopAtNewline: boolean): boolean {
    let newline = false;
    for (;;) {
      const c = this._text[this._at];
      if (c === undefined) return newline;
      if (c === "\n") {
        if (stopAtNewline) return true;
        newline = true;
        this._advance();
        continue;
      }
      if (c === " " || c === "\t" || c === "\r" || c === "\v" || c === "\f") {
        this._advance();
        continue;
      }
      // A backslash before a newline joins the lines, inside a directive as well as outside it.
      if (c === "\\" && (this._text[this._at + 1] === "\n" || this._text.slice(this._at + 1, this._at + 3) === "\r\n")) {
        this._advance();
        while (this._text[this._at] === "\r") this._advance();
        this._advance();
        continue;
      }
      if (c === "/" && this._text[this._at + 1] === "/") {
        while (this._at < this._text.length && this._text[this._at] !== "\n") this._advance();
        continue;
      }
      if (c === "/" && this._text[this._at + 1] === "*") {
        this._advance();
        this._advance();
        while (this._at < this._text.length && !(this._text[this._at] === "*" && this._text[this._at + 1] === "/")) this._advance();
        this._advance();
        this._advance();
        if (stopAtNewline) continue;
        continue;
      }
      return newline;
    }
  }

  /** The next token, or null at the end (or at a newline when `stopAtNewline`). */
  next(stopAtNewline = false): Token | null {
    const crossed = this.skipTrivia(stopAtNewline);
    if (stopAtNewline && (crossed || this._text[this._at] === "\n")) return null;
    if (this._at >= this._text.length) return null;
    const line = this._line;
    const column = this._at - this._lineStart + 1;
    // A `#` only opens a directive when nothing but whitespace precedes it on its line.
    let first = true;
    for (let i = this._at - 1; i >= this._lineStart; i--) {
      if (!/[ \t]/.test(this._text[i])) {
        first = false;
        break;
      }
    }
    const c = this._text[this._at];

    if (isIdentStart(c)) {
      const start = this._at;
      while (this._at < this._text.length && isIdent(this._text[this._at])) this._advance();
      return { kind: "identifier", text: this._text.slice(start, this._at), line, column, first };
    }
    if (isDigit(c) || (c === "." && isDigit(this._text[this._at + 1] ?? ""))) {
      return { kind: "number", text: this._number(), line, column, first };
    }
    if (c === '"' || c === "'") {
      const start = this._at;
      this._advance();
      while (this._at < this._text.length && this._text[this._at] !== c) {
        if (this._text[this._at] === "\\") this._advance();
        this._advance();
      }
      this._advance();
      return { kind: c === '"' ? "string" : "char", text: this._text.slice(start, this._at), line, column, first };
    }
    if (c === "#") {
      this._advance();
      return { kind: "punct", text: "#", line, column, first };
    }
    for (const p of PUNCTUATORS) {
      if (this._text.startsWith(p, this._at)) {
        for (let i = 0; i < p.length; i++) this._advance();
        return { kind: "punct", text: p, line, column, first };
      }
    }
    // Anything else (a stray character) becomes a one-character punctuator rather than stopping.
    this._advance();
    return { kind: "punct", text: c, line, column, first };
  }

  private _number(): string {
    const start = this._at;
    const hex = this._text[this._at] === "0" && /[xX]/.test(this._text[this._at + 1] ?? "");
    if (hex) {
      this._advance();
      this._advance();
    }
    const digits = hex ? /[0-9a-fA-F]/ : /[0-9]/;
    for (;;) {
      const c = this._text[this._at];
      if (c === undefined) break;
      if (digits.test(c) || c === "." || c === "'") {
        this._advance();
        continue;
      }
      // An exponent's sign is part of the number: 1e-5, 0x1p+3.
      if ((!hex && /[eE]/.test(c)) || (hex && /[pP]/.test(c))) {
        this._advance();
        if (/[+-]/.test(this._text[this._at] ?? "")) this._advance();
        continue;
      }
      if (/[uUlLfFhH]/.test(c)) {
        this._advance();
        continue;
      }
      break;
    }
    return this._text.slice(start, this._at);
  }

  private _advance(): void {
    if (this._text[this._at] === "\n") {
      this._line++;
      this._lineStart = this._at + 1;
    }
    this._at++;
  }
}

/**
 * The whole source as tokens, with the preprocessor run.
 *
 * `defines` are the macros in effect before the text (none, for a captured library: the
 * application compiled it with whatever `MTLCompileOptions` said, which the capture does not
 * record — a preprocessor macro the shader depends on is reported as a diagnostic when it is
 * used in a conditional).
 */
export function tokenize(text: string, defines: Map<string, string> = new Map()): { tokens: Token[]; diagnostics: LexDiagnostic[] } {
  const scanner = new Scanner(text);
  const out: Token[] = [];
  const diagnostics: LexDiagnostic[] = [];
  const macros = new Map<string, Macro>();
  for (const [name, body] of defines) {
    macros.set(name, { name, params: null, variadic: false, body: tokensOf(body) });
  }
  // One entry per #if: whether this branch is being taken, and whether any branch of it was.
  const conditionals: { taking: boolean; taken: boolean; parent: boolean }[] = [];
  const active = (): boolean => conditionals.every((c) => c.taking && c.parent);

  for (;;) {
    const token = scanner.next();
    if (!token) break;
    if (token.kind === "punct" && token.text === "#" && token.first) {
      directive(scanner, token, macros, conditionals, diagnostics, active);
      continue;
    }
    if (!active()) continue;
    if (token.kind === "identifier" && macros.has(token.text)) {
      expand(token, scanner, macros, out, diagnostics);
      continue;
    }
    out.push(token);
  }
  if (conditionals.length) diagnostics.push({ line: scanner.line, message: `${conditionals.length} unterminated #if` });
  out.push({ kind: "end", text: "", line: scanner.line, column: 1 });
  return { tokens: out, diagnostics };
}

function tokensOf(text: string): Token[] {
  const scanner = new Scanner(text);
  const out: Token[] = [];
  for (;;) {
    const t = scanner.next();
    if (!t) break;
    out.push(t);
  }
  return out;
}

/** The rest of the directive's line. */
function restOfLine(scanner: Scanner): Token[] {
  const out: Token[] = [];
  for (;;) {
    const t = scanner.next(true);
    if (!t) break;
    out.push(t);
  }
  return out;
}

function directive(
  scanner: Scanner, hash: Token, macros: Map<string, Macro>,
  conditionals: { taking: boolean; taken: boolean; parent: boolean }[],
  diagnostics: LexDiagnostic[], active: () => boolean,
): void {
  const name = scanner.next(true);
  if (!name) return;
  const parentActive = conditionals.every((c) => c.taking && c.parent);
  switch (name.text) {
    case "define": {
      const rest = restOfLine(scanner);
      if (!active() || !rest.length) return;
      define(rest, macros, hash.line, diagnostics);
      return;
    }
    case "undef": {
      const rest = restOfLine(scanner);
      if (active() && rest.length) macros.delete(rest[0].text);
      return;
    }
    case "ifdef":
    case "ifndef": {
      const rest = restOfLine(scanner);
      const has = rest.length ? macros.has(rest[0].text) : false;
      const taking = name.text === "ifdef" ? has : !has;
      conditionals.push({ taking, taken: taking, parent: parentActive });
      return;
    }
    case "if": {
      const rest = restOfLine(scanner);
      const taking = evaluate(rest, macros, hash.line, diagnostics, parentActive);
      conditionals.push({ taking, taken: taking, parent: parentActive });
      return;
    }
    case "elif": {
      const rest = restOfLine(scanner);
      const c = conditionals[conditionals.length - 1];
      if (!c) {
        diagnostics.push({ line: hash.line, message: "#elif without #if" });
        return;
      }
      const taking = !c.taken && evaluate(rest, macros, hash.line, diagnostics, c.parent);
      c.taking = taking;
      c.taken = c.taken || taking;
      return;
    }
    case "else": {
      restOfLine(scanner);
      const c = conditionals[conditionals.length - 1];
      if (!c) {
        diagnostics.push({ line: hash.line, message: "#else without #if" });
        return;
      }
      c.taking = !c.taken;
      c.taken = true;
      return;
    }
    case "endif":
      restOfLine(scanner);
      if (!conditionals.pop()) diagnostics.push({ line: hash.line, message: "#endif without #if" });
      return;
    case "include":
    case "pragma":
    case "line":
      restOfLine(scanner);
      return;
    case "error": {
      const rest = restOfLine(scanner);
      if (active()) diagnostics.push({ line: hash.line, message: `#error ${rest.map((t) => t.text).join(" ")}` });
      return;
    }
    default:
      restOfLine(scanner);
      if (active()) diagnostics.push({ line: hash.line, message: `unknown directive #${name.text}` });
  }
}

function define(rest: Token[], macros: Map<string, Macro>, line: number, diagnostics: LexDiagnostic[]): void {
  const name = rest[0];
  if (name.kind !== "identifier") {
    diagnostics.push({ line, message: `#define of ${name.text}, which is not a name` });
    return;
  }
  // Function-like only when the "(" touches the name: `#define F(x)` differs from `#define F (x)`.
  const open = rest[1];
  if (open && open.kind === "punct" && open.text === "(" && open.line === name.line && open.column === name.column + name.text.length) {
    const params: string[] = [];
    let variadic = false;
    let i = 2;
    for (; i < rest.length && rest[i].text !== ")"; i++) {
      if (rest[i].text === ",") continue;
      if (rest[i].text === "...") {
        variadic = true;
        params.push("__VA_ARGS__");
        continue;
      }
      params.push(rest[i].text);
    }
    macros.set(name.text, { name: name.text, params, variadic, body: rest.slice(i + 1) });
    return;
  }
  macros.set(name.text, { name: name.text, params: null, variadic: false, body: rest.slice(1) });
}

const MAX_EXPANSIONS = 4000;

/** Expands a macro use into `out`, taking a function-like macro's arguments from the scanner. */
function expand(token: Token, scanner: Scanner, macros: Map<string, Macro>, out: Token[], diagnostics: LexDiagnostic[]): void {
  const pending: Token[] = [token];
  let guard = 0;
  // Expansion is iterative so a macro whose body uses another macro resolves, with a budget
  // rather than a "currently expanding" set: the budget also catches a self-referential define.
  // Everything produced is reported at the use's line, which is where the reader sees it.
  while (pending.length) {
    if (guard++ > MAX_EXPANSIONS) {
      diagnostics.push({ line: token.line, message: `macro ${token.text} expands without end` });
      break;
    }
    const t = pending.shift()!;
    const macro = t.kind === "identifier" ? macros.get(t.text) : undefined;
    if (!macro) {
      out.push(t === token ? t : { ...t, line: token.line, column: token.column, expanded: true });
      continue;
    }
    if (!macro.params) {
      pending.unshift(...macro.body);
      continue;
    }
    const args = macroArguments(pending, scanner);
    if (!args) {
      // A function-like macro's name without a call is just the name.
      out.push({ ...t, line: token.line, column: token.column, expanded: true });
      continue;
    }
    pending.unshift(...substitute(macro, args));
  }
}

/** The argument lists of a function-like macro use: from `pending` first, then the scanner. */
function macroArguments(pending: Token[], scanner: Scanner): Token[][] | null {
  const take = (): Token | null => pending.shift() ?? scanner.next();
  const open = take();
  if (!open || open.text !== "(") {
    if (open) pending.unshift(open);
    return null;
  }
  const args: Token[][] = [[]];
  let depth = 1;
  for (;;) {
    const t = take();
    if (!t || t.kind === "end") break;
    if (t.text === "(") depth++;
    if (t.text === ")") {
      depth--;
      if (depth === 0) break;
    }
    if (t.text === "," && depth === 1) {
      args.push([]);
      continue;
    }
    args[args.length - 1].push(t);
  }
  return args;
}

function substitute(macro: Macro, args: Token[][]): Token[] {
  const params = macro.params ?? [];
  const byName = new Map<string, Token[]>();
  params.forEach((p, i) => {
    if (macro.variadic && p === "__VA_ARGS__") {
      const rest: Token[] = [];
      for (let k = i; k < args.length; k++) {
        if (k > i) rest.push({ kind: "punct", text: ",", line: 0, column: 0 });
        rest.push(...args[k]);
      }
      byName.set(p, rest);
      return;
    }
    byName.set(p, args[i] ?? []);
  });
  const out: Token[] = [];
  for (let i = 0; i < macro.body.length; i++) {
    const t = macro.body[i];
    // Stringizing: #x becomes "x".
    if (t.text === "#" && t.kind === "punct") {
      const next = macro.body[i + 1];
      const arg = next ? byName.get(next.text) : undefined;
      if (arg) {
        out.push({ kind: "string", text: JSON.stringify(arg.map((a) => a.text).join(" ")), line: t.line, column: t.column });
        i++;
        continue;
      }
    }
    // Token pasting: a ## b becomes one token when the halves join into one.
    if (macro.body[i + 1]?.text === "##" && macro.body[i + 2]) {
      const left = byName.get(t.text) ?? [t];
      const rightToken = macro.body[i + 2];
      const right = byName.get(rightToken.text) ?? [rightToken];
      const a = left[left.length - 1], b = right[0];
      out.push(...left.slice(0, -1));
      if (a && b) out.push({ kind: a.kind, text: `${a.text}${b.text}`, line: t.line, column: t.column });
      else if (a) out.push(a);
      else if (b) out.push(b);
      out.push(...right.slice(1));
      i += 2;
      continue;
    }
    const arg = byName.get(t.text);
    if (arg) {
      out.push(...arg);
      continue;
    }
    out.push(t);
  }
  return out;
}

// ---------------------------------------------------------------------------------------------
// #if expressions

/**
 * The value of a `#if` expression. Handles what shaders use: integer literals, `defined(X)`,
 * macros that expand to constants, the comparison, arithmetic and logical operators. A name that
 * is not a macro is 0, as the C preprocessor has it — but one that came from a `-D` the capture
 * does not record would also read 0, so the first such name is reported.
 */
function evaluate(tokens: Token[], macros: Map<string, Macro>, line: number, diagnostics: LexDiagnostic[], report: boolean): boolean {
  const flat: Token[] = [];
  for (let i = 0; i < tokens.length; i++) {
    const t = tokens[i];
    if (t.kind === "identifier" && t.text === "defined") {
      let k = i + 1;
      const paren = tokens[k]?.text === "(";
      if (paren) k++;
      const name = tokens[k];
      flat.push({ kind: "number", text: name && macros.has(name.text) ? "1" : "0", line: t.line, column: t.column });
      i = paren ? k + 1 : k;
      continue;
    }
    if (t.kind === "identifier" && macros.has(t.text)) {
      const macro = macros.get(t.text)!;
      if (!macro.params) {
        flat.push(...macro.body);
        continue;
      }
    }
    flat.push(t);
  }
  let at = 0;
  const peek = (): Token | undefined => flat[at];
  const eat = (text: string): boolean => {
    if (flat[at]?.text === text) {
      at++;
      return true;
    }
    return false;
  };
  const primary = (): number => {
    const t = flat[at];
    if (!t) return 0;
    if (eat("(")) {
      const v = ternary();
      eat(")");
      return v;
    }
    if (eat("!")) return primary() ? 0 : 1;
    if (eat("-")) return -primary();
    if (eat("+")) return primary();
    if (eat("~")) return ~primary();
    at++;
    if (t.kind === "number") return Number(t.text.replace(/[uUlL']/g, "")) || 0;
    if (t.kind === "identifier") {
      if (report && !macros.has(t.text)) {
        diagnostics.push({ line, message: `#if uses ${t.text}, which the capture does not record a value for: it is taken as 0` });
      }
      return 0;
    }
    return 0;
  };
  const binary = (level: number): number => {
    const levels = [["||"], ["&&"], ["|"], ["^"], ["&"], ["==", "!="], ["<", ">", "<=", ">="], ["<<", ">>"], ["+", "-"], ["*", "/", "%"]];
    if (level >= levels.length) return primary();
    let left = binary(level + 1);
    for (;;) {
      const op = peek()?.text;
      if (!op || !levels[level].includes(op)) return left;
      at++;
      const right = binary(level + 1);
      switch (op) {
        case "||": left = left || right ? 1 : 0; break;
        case "&&": left = left && right ? 1 : 0; break;
        case "|": left = left | right; break;
        case "^": left = left ^ right; break;
        case "&": left = left & right; break;
        case "==": left = left === right ? 1 : 0; break;
        case "!=": left = left !== right ? 1 : 0; break;
        case "<": left = left < right ? 1 : 0; break;
        case ">": left = left > right ? 1 : 0; break;
        case "<=": left = left <= right ? 1 : 0; break;
        case ">=": left = left >= right ? 1 : 0; break;
        case "<<": left = left << right; break;
        case ">>": left = left >> right; break;
        case "+": left += right; break;
        case "-": left -= right; break;
        case "*": left *= right; break;
        case "/": left = right ? Math.trunc(left / right) : 0; break;
        case "%": left = right ? left % right : 0; break;
        default: return left;
      }
    }
  };
  const ternary = (): number => {
    const cond = binary(0);
    if (!eat("?")) return cond;
    const a = ternary();
    eat(":");
    const b = ternary();
    return cond ? a : b;
  };
  return ternary() !== 0;
}
