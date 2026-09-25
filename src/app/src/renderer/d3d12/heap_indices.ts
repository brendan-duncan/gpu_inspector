// Which slots of a directly indexed heap a DXIL shader takes (shader model 6.6), read from its
// disassembly. Every `ResourceDescriptorHeap[i]` / `SamplerDescriptorHeap[i]` is a call to
// dx.op.createHandleFromHeap(index, samplerHeap, nonUniformIndex); the index is an SSA value, and
// following its definition back gives either a constant, or an expression over constant buffer
// loads -- the usual way a bindless engine passes indices, in root constants or a per-draw
// constant buffer -- which the draw's captured constants turn into a slot. Anything else (a vertex
// input, a loaded buffer element, a loop) is computed at run time and reported as such, with the
// instruction that made it so.
//
// The disassembly is dxc's text form (`dxinsp_shader.exe --disassemble`):
//   %1 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %Frame_cbuffer, ...)
//   %18 = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %1, i32 0)
//   %19 = extractvalue %dx.types.CBufRet.i32 %18, 1
//   %20 = lshr i32 %19, 8
//   %21 = call %dx.types.Handle @dx.op.createHandleFromHeap(i32 218, i32 %20, i1 false, i1 false), !dbg !145 ; line:49 col:25
// with `%Frame_cbuffer` made by dx.op.createHandleFromBinding from a ResBind { lower, upper, space, class }.

/** A constant buffer's contents at the draw, by register and space: the 32 bits at a byte offset, or null when not known. */
export type ConstantReader = (register: number, space: number, byteOffset: number) => number | null;

export interface HeapAccess {
  samplers: boolean;
  /** The slot, when the index could be worked out; null when it is computed at run time. */
  slot: number | null;
  /** How the index is made: "10", "(b1 byte 4) >> 8", or why it cannot be known. */
  expression: string;
  /** The shader marked the index NonUniformResourceIndex (it varies across a wave). */
  nonUniform: boolean;
  /** The HLSL line of the access, from the debug information when the shader has it. */
  line: number | null;
}

interface Value {
  /** The value: an integer (as an unsigned 32-bit number) or a float; null when not known. */
  value: number | null;
  float: boolean;
  text: string;
}

const BINARY = new Map<string, [string, (a: number, b: number) => number]>([
  ["add", ["+", (a, b) => (a + b) >>> 0]],
  ["sub", ["-", (a, b) => (a - b) >>> 0]],
  ["mul", ["*", (a, b) => Math.imul(a, b) >>> 0]],
  ["udiv", ["/", (a, b) => (b ? Math.floor(a / b) >>> 0 : NaN)]],
  ["sdiv", ["/", (a, b) => (b ? Math.trunc((a | 0) / (b | 0)) >>> 0 : NaN)]],
  ["urem", ["%", (a, b) => (b ? (a % b) >>> 0 : NaN)]],
  ["shl", ["<<", (a, b) => (a << (b & 31)) >>> 0]],
  ["lshr", [">>", (a, b) => a >>> (b & 31)]],
  ["ashr", [">>", (a, b) => (a >> (b & 31)) >>> 0]],
  ["and", ["&", (a, b) => (a & b) >>> 0]],
  ["or", ["|", (a, b) => (a | b) >>> 0]],
  ["xor", ["^", (a, b) => (a ^ b) >>> 0]],
]);

/** Why a value is computed at run time, by the instruction that made it. */
function runtimeReason(rhs: string): string {
  if (rhs.includes("@dx.op.loadInput")) return "a shader input";
  if (rhs.includes("@dx.op.bufferLoad") || rhs.includes("@dx.op.rawBufferLoad") || rhs.includes("@dx.op.textureLoad")) return "a value loaded from a resource";
  if (/@dx\.op\.(threadId|groupId|threadIdInGroup|flattenedThreadIdInGroup|dispatchRaysIndex|instanceID|instanceIndex|primitiveID|viewID)/.test(rhs)) return "a system value (thread, instance or primitive id)";
  if (rhs.startsWith("phi ")) return "a value that depends on control flow (a loop or a branch)";
  if (rhs.startsWith("select ")) return "a conditional choice";
  if (rhs.startsWith("load ")) return "a value read from shader memory";
  const op = rhs.match(/@dx\.op\.(\w+)/)?.[1] ?? rhs.split(/\s+/)[0];
  return `computed at run time (${op})`;
}

/** The accesses of every createHandleFromHeap in a DXIL disassembly, in the order they appear. */
export function heapAccesses(disassembly: string, constants: ConstantReader): HeapAccess[] {
  // Every SSA definition by name, with the HLSL line its debug comment names.
  const defs = new Map<string, string>();
  const heapCalls: { rhs: string; line: number | null }[] = [];
  for (const raw of disassembly.split(/\r?\n/)) {
    const m = raw.match(/^\s*(%[^\s=]+) = (.*)$/);
    if (!m) continue;
    const comment = m[2].indexOf(" ; ");
    const rhs = (comment >= 0 ? m[2].slice(0, comment) : m[2]).replace(/, !dbg !\d+.*$/, "").trim();
    defs.set(m[1], rhs);
    if (rhs.includes("@dx.op.createHandleFromHeap(")) {
      const line = raw.match(/; line:(\d+)/);
      heapCalls.push({ rhs, line: line ? Number(line[1]) : null });
    }
  }

  const memo = new Map<string, Value>();
  const unknown = (text: string): Value => ({ value: null, float: false, text });

  const operand = (token: string, depth: number): Value => {
    const t = token.trim();
    if (/^-?\d+$/.test(t)) return { value: Number(t) >>> 0, float: false, text: String(Number(t)) };
    if (t === "true") return { value: 1, float: false, text: "1" };
    if (t === "false") return { value: 0, float: false, text: "0" };
    if (/^-?\d+(\.\d+)?e[+-]\d+$/.test(t)) return { value: Number(t), float: true, text: String(Number(t)) };
    if (t.startsWith("%")) return evaluate(t, depth + 1);
    return unknown(`an operand dxc wrote as ${t}`);
  };

  /** The constant buffer a handle names: its register and space, from the binding that created it. */
  const bufferOf = (handle: string, depth: number): { register: number; space: number } | null => {
    if (depth > 16) return null;
    const rhs = defs.get(handle);
    if (!rhs) return null;
    const annotated = rhs.match(/@dx\.op\.annotateHandle\(i32 216, %dx\.types\.Handle (%[^\s,]+)/);
    if (annotated) return bufferOf(annotated[1], depth + 1);
    const bound = rhs.match(/@dx\.op\.createHandleFromBinding\(i32 217, %dx\.types\.ResBind (zeroinitializer|\{[^}]*\}), i32 ([^,]+),/);
    if (!bound) return null;
    const fields = bound[1] === "zeroinitializer" ? [0, 0, 0, 0] : [...bound[1].matchAll(/i\d+ (-?\d+)/g)].map((f) => Number(f[1]));
    const index = operand(bound[2].replace(/^i32\s+/, ""), depth);
    // The index is the register itself (the binding's lower bound plus the element of an array).
    return index.value === null ? null : { register: index.value, space: fields[2] ?? 0 };
  };

  const evaluate = (name: string, depth: number): Value => {
    const known = memo.get(name);
    if (known) return known;
    if (depth > 64) return unknown("an expression too deep to follow");
    const rhs = defs.get(name);
    let out: Value;
    if (!rhs) {
      out = unknown(`${name}, defined outside the function`);
    } else {
      out = evaluateRhs(rhs, depth);
    }
    memo.set(name, out);
    return out;
  };

  const evaluateRhs = (rhs: string, depth: number): Value => {
    // A binary operation on 32-bit integers.
    const bin = rhs.match(/^(\w+)\s+(?:(?:nuw|nsw|exact)\s+)*i32\s+([^,]+),\s*(.+)$/);
    if (bin && BINARY.has(bin[1])) {
      const [symbol, fn] = BINARY.get(bin[1])!;
      const a = operand(bin[2], depth);
      const b = operand(bin[3], depth);
      const text = `(${a.text} ${symbol} ${b.text})`;
      if (a.value === null || b.value === null) return { value: null, float: false, text: a.value === null ? a.text : b.text };
      const v = fn(a.value, b.value);
      return Number.isNaN(v) ? unknown("a division by zero") : { value: v, float: false, text };
    }
    // A conversion that keeps the number: widening, narrowing, float to integer.
    const conv = rhs.match(/^(zext|sext|trunc|fptoui|fptosi|bitcast)\s+\w+\s+(\S+)\s+to\s+(\w+)$/);
    if (conv) {
      const v = operand(conv[2], depth);
      if (v.value === null) return v;
      if (conv[1] === "fptoui" || conv[1] === "fptosi") return { value: Math.trunc(v.value) >>> 0, float: false, text: `uint(${v.text})` };
      if (conv[1] === "bitcast" && v.float && conv[3] === "i32") {
        const view = new DataView(new ArrayBuffer(4));
        view.setFloat32(0, v.value, true);
        return { value: view.getUint32(0, true), float: false, text: `asuint(${v.text})` };
      }
      return v;
    }
    // One element of a constant buffer row: the register's 16 bytes at `row`, element `k` of them.
    const element = rhs.match(/^extractvalue %dx\.types\.CBufRet\.(i32|f32) (%[^\s,]+), (\d+)$/);
    if (element) {
      const load = defs.get(element[2]) ?? "";
      const call = load.match(/@dx\.op\.cbufferLoadLegacy\.(?:i32|f32)\(i32 59, %dx\.types\.Handle (%[^\s,]+), i32 ([^)]+)\)/);
      if (!call) return unknown("a constant buffer read dxc wrote in a form not followed here");
      const buffer = bufferOf(call[1], depth);
      const row = operand(call[2], depth);
      if (!buffer) return unknown("a constant buffer whose binding is not a fixed register");
      if (row.value === null) return unknown("a constant buffer row chosen at run time");
      const byteOffset = row.value * 16 + Number(element[3]) * 4;
      const text = `(b${buffer.register}${buffer.space ? ` space${buffer.space}` : ""} byte ${byteOffset})`;
      const bits = constants(buffer.register, buffer.space, byteOffset);
      if (bits === null) return { value: null, float: false, text: `${text}, whose value the capture does not hold` };
      if (element[1] === "i32") return { value: bits >>> 0, float: false, text };
      const view = new DataView(new ArrayBuffer(4));
      view.setUint32(0, bits, true);
      return { value: view.getFloat32(0, true), float: true, text };
    }
    return unknown(runtimeReason(rhs));
  };

  const out: HeapAccess[] = [];
  for (const call of heapCalls) {
    const args = call.rhs.match(/@dx\.op\.createHandleFromHeap\(i32 218, i32 ([^,]+), i1 (\w+), i1 (\w+)\)/);
    if (!args) continue;
    const index = operand(args[1], 0);
    const slot = index.value !== null && !index.float ? index.value : null;
    out.push({
      samplers: args[2] === "true",
      slot,
      // A constant index reads as itself; an expression shows what it was made of, and a runtime one why.
      expression: slot !== null && index.text !== String(slot) ? `${index.text} = ${slot}` : index.text,
      nonUniform: args[3] === "true",
      line: call.line,
    });
  }
  return out;
}
