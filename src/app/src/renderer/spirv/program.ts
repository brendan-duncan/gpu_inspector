// A SPIR-V module as the shader debugger reads it (../debug/program.ts): its embedded source, the
// line each instruction came from, the places a breakpoint can go, and how to name and print the
// values an invocation produces.
//
// Everything here was written against SpirvModule inside the debugger's stepping and its tab
// before the MSL interpreter existed; it lives behind DebugProgram so neither has to know which
// language produced the shader.
import { Op, StorageClass, type Instruction, type SpirvModule } from "./module.js";
import type { SpirvStepResult } from "./interpreter.js";
import {
  ImageValue, Pointer, SampledImageValue, SamplerValue, imageText, samplerText, scalarText, type Value,
} from "./values.js";
import {
  instructionKey, sourceKey, type DebugChild, type DebugLocation, type DebugProgram, type DebugSourceFile, type DebugStep,
  type LineMode, type StepResult, type VariableView,
} from "../debug/program.js";
import { disassemblyInstructions, sourceLanguageOf } from "../vulkan/spirv_debug.js";
import type { HighlightLanguage } from "../code_editor.js";

/** Composites longer than this open to their elements even when they are vectors. */
const OPEN_ABOVE = 8;

const _programs = new WeakMap<SpirvModule, SpirvProgram>();

export class SpirvProgram implements DebugProgram {
  readonly kind = "spirv" as const;
  readonly module: SpirvModule;
  private _names: Map<string, number[]> | null = null;
  /** How a resource's set and binding are named in the variables table; a D3D12 translation names its register ("t0"). */
  bindingName: ((set: number, binding: number) => string) | null = null;

  /** One program per module: the view holds onto it across steps, and the name index is built once. */
  static of(module: SpirvModule): SpirvProgram {
    let p = _programs.get(module);
    if (!p) {
      p = new SpirvProgram(module);
      _programs.set(module, p);
    }
    return p;
  }

  constructor(module: SpirvModule) {
    this.module = module;
  }

  get modes(): readonly LineMode[] {
    // A module without line information can only be stepped as instructions; one with it offers both.
    return hasLineInfo(this.module) ? ["source", "instruction"] : ["instruction"];
  }

  get files(): readonly DebugSourceFile[] {
    return this.module.debug?.files ?? [];
  }

  get mainFile(): number {
    return this.module.debug?.mainFile ?? -1;
  }

  get language(): HighlightLanguage | null {
    return sourceLanguageOf(this.module.debug ?? null);
  }

  /** What OpSource said the module was compiled from ("GLSL", "HLSL", "Slang", ...). */
  get languageName(): string {
    return this.module.debug?.language || "source";
  }

  hasSourceText(): boolean {
    return hasLineInfo(this.module) && this.files.some((f) => f.text != null);
  }

  locationOf(step: DebugStep | null): DebugLocation | null {
    return step ? this.module.debug?.locations[step.index] ?? null : null;
  }

  stopKeys(mode: LineMode): Set<string> {
    const keys = new Set<string>();
    for (const i of executableInstructions(this.module)) {
      if (mode === "instruction") {
        keys.add(instructionKey(i));
        continue;
      }
      const loc = this.module.debug?.locations[i];
      if (loc) keys.add(sourceKey(loc.file, loc.line));
    }
    return keys;
  }

  nameOf(id: number): string {
    return this.module.nameOf(id);
  }

  typeName(type: number): string {
    return this.module.typeName(type);
  }

  typeOfId(id: number): number {
    return valueType(this.module, id);
  }

  valueText(type: number, value: Value | undefined, limit = 16): string {
    return valueText(this.module, type, value, limit);
  }

  children(type: number, value: Value): DebugChild[] | null {
    const t = this.module.types.get(type);
    if (!Array.isArray(value)) return null;
    const opens = t?.kind === "struct" || t?.kind === "array" || t?.kind === "runtimeArray" || t?.kind === "matrix" || value.length > OPEN_ABOVE;
    if (!opens) return null;
    return value.map((child, i) => ({
      name: t?.kind === "struct" ? this.module.memberNames.get(type)?.get(i) ?? `[${i}]` : `[${i}]`,
      type: t?.kind === "struct" ? t.members[i]
        : t?.kind === "matrix" ? t.column
        : t?.kind === "array" || t?.kind === "runtimeArray" ? t.element
        : t?.kind === "vector" ? t.element : 0,
      value: child,
    }));
  }

  idsNamed(name: string): number[] {
    if (!this._names) {
      const names = new Map<string, number[]>();
      for (const source of [this.module.names, this.module.debugVariableNames]) {
        for (const [id, n] of source) {
          if (this.module.types.has(id)) continue;
          const list = names.get(n) ?? [];
          list.push(id);
          names.set(n, list);
        }
      }
      this._names = names;
    }
    return this._names.get(name) ?? [];
  }

  resultType(r: StepResult): number {
    return resultType(this.module, r as SpirvStepResult);
  }

  /** A result the shader named nothing: an SSA temporary, which the values table grays out. */
  resultTemporary(r: StepResult): boolean {
    return !this.module.names.has(r.id) && !this.module.debugVariableNames.has(r.id);
  }

  variableWhere(v: VariableView): string {
    if (v.builtin !== undefined) return "";
    if (v.location !== undefined) return `location ${v.location}`;
    if (v.binding !== undefined) return this.bindingName ? this.bindingName(v.set ?? 0, v.binding) : `set ${v.set ?? 0} binding ${v.binding}`;
    return v.storage === StorageClass.PushConstant ? "push constants" : "";
  }

  readonly disassembly = {
    bytes: (): Uint8Array => new Uint8Array(this.module.words.buffer, this.module.words.byteOffset, this.module.words.byteLength),
    /** No spirv-dis: a listing of the executable instructions. */
    listing: (): { text: string; number: string; key: string }[] =>
      executableInstructions(this.module).map((i) => {
        const inst = this.module.instructions[i];
        return {
          text: `${inst.result ? `${this.module.nameOf(inst.result)} = ` : ""}Op${inst.op}`,
          number: String(i),
          key: instructionKey(i),
        };
      }),
    mapDisassembly: (lines: string[]): { keys: (string | null)[]; numbers: string[] } => {
      const keys = new Array<string | null>(lines.length).fill(null);
      const numbers = new Array<string>(lines.length).fill("");
      const instructions = disassemblyInstructions(lines);
      // Only when the disassembly is of this module: one line group per instruction, in order.
      if (instructions.length === this.module.instructions.length) {
        instructions.forEach((ls, k) => {
          keys[ls[0]] = instructionKey(k);
          numbers[ls[0]] = String(k);
        });
      }
      return { keys, numbers };
    },
  };
}

// ---------------------------------------------------------------------------------------------

/** Whether a module's instructions map to lines of source text it (or the host) has. */
export function hasLineInfo(module: SpirvModule): boolean {
  const info = module.debug;
  return !!info && info.form !== "none" && info.locations.some((l) => l !== null);
}

/** The instructions of the module's function bodies that execute (what a breakpoint can be put on). */
export function executableInstructions(module: SpirvModule): number[] {
  const out: number[] = [];
  for (const fn of module.functions.values()) {
    for (const block of fn.blocks) {
      for (let i = block.start + 1; i <= block.end; i++) {
        const inst = module.instructions[i];
        if (inst && !isNoop(module, inst)) out.push(i);
      }
    }
  }
  return out.sort((a, b) => a - b);
}

function isNoop(module: SpirvModule, inst: Instruction): boolean {
  switch (inst.op) {
    case Op.Nop: case Op.Line: case Op.NoLine: case Op.SelectionMerge: case Op.LoopMerge: case Op.Label:
      return true;
    case Op.ExtInst:
      return module.extSets.get(inst.words[2])?.startsWith("NonSemantic.") ?? false;
    default:
      return false;
  }
}

/** The type of the value an id has: a variable's pointee, else its result type. */
export function valueType(m: SpirvModule, id: number): number {
  const type = m.idTypes.get(id) ?? m.globals.get(id)?.type ?? 0;
  const t = m.types.get(type);
  return t?.kind === "pointer" ? t.pointee : type;
}

/** A step result's type: the instruction's result type, or for a store the stored variable's. */
export function resultType(m: SpirvModule, r: SpirvStepResult): number {
  return r.inst.resultType || valueType(m, r.id);
}

/** One line of text for a value: scalars and vectors in full, composites shortened past `limit` elements. */
export function valueText(module: SpirvModule, type: number, value: Value | undefined, limit = 16): string {
  if (value === undefined || value === null) return "undefined";
  if (value instanceof Pointer) return `→ ${module.nameOf(value.variable)}${value.path.length ? `[${value.path.join("][")}]` : ""}`;
  if (value instanceof SampledImageValue) return `${imageText(value.image)}, ${samplerText(value.sampler)}`;
  if (value instanceof ImageValue) return imageText(value);
  if (value instanceof SamplerValue) return samplerText(value);
  if (!Array.isArray(value)) return scalarText(value);
  const t = module.types.get(type);
  const inner = t?.kind === "vector" ? t.element : t?.kind === "matrix" ? t.column : t?.kind === "array" || t?.kind === "runtimeArray" ? t.element : 0;
  const parts: string[] = [];
  for (let i = 0; i < Math.min(value.length, limit); i++) {
    const memberType = t?.kind === "struct" ? t.members[i] : inner;
    const text = valueText(module, memberType, value[i], limit);
    const name = t?.kind === "struct" ? module.memberNames.get(type)?.get(i) : undefined;
    parts.push(name ? `${name}: ${text}` : text);
  }
  if (value.length > limit) parts.push(`… ${value.length - limit} more`);
  return t?.kind === "struct" ? `{ ${parts.join(", ")} }` : t?.kind === "array" || t?.kind === "runtimeArray" ? `[${parts.join(", ")}]` : `(${parts.join(", ")})`;
}
