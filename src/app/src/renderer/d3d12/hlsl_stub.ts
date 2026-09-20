// A compilable HLSL stage written from a D3D12 pipeline's reflection, for the shader editor to
// start from when the bytecode carries no source (src/d3d12/README.md, "Shaders"): there is no
// DXIL decompiler, so a shipped shader built without -Zi and without a PDB on this machine has no
// original text to edit. What the capture does have is the full reflection the library took at
// pipeline creation (reflection.ts) — every constant buffer with its members at their real
// offsets, every SRV, UAV and sampler with its register and space, the input and output
// signatures with their semantics, a compute stage's thread group size — and that is enough to
// write a stage that declares exactly what the original declared.
//
// The point is binding compatibility: the declarations keep the registers and spaces the original
// used, so the result compiles with dxc (compileDxil) and the D3D12 library's ReplaceShader can
// build a pipeline with the stage swapped against the application's own root signature. Only the
// body is invented, and it says so.
import type { EntryPoint, ReflType, ShaderReflection, ShaderResource, ShaderStage, ShaderVariable, StructType } from "../vulkan/spirv_reflect.js";

export interface HlslStubOptions {
  /** The pipeline the stage belongs to, for the header comment. */
  pipelineName?: string;
  /** Why the original source could not be recovered, for the header comment. */
  reason?: string;
}

/** An identifier HLSL accepts: the reflected name where it is one ("$Globals" is not), else a fallback. */
function identifier(name: string, fallback: string): string {
  const clean = (name || "").replace(/[^A-Za-z0-9_]/g, "_").replace(/^(\d)/, "_$1");
  return clean && clean !== "_" ? clean : fallback;
}

function scalarName(base: string, width: number): string {
  if (base === "bool") return "bool";
  const prefix = base === "float" ? "float" : base === "int" ? "int" : "uint";
  if (width === 64) return base === "float" ? "double" : `${prefix}64_t`;
  // 16-bit types need -enable-16bit-types; the min-precision spellings compile everywhere.
  if (width === 16) return `min16${prefix}`;
  return prefix;
}

/** The struct types a stage declares, by the shape they were collected under: see collectStructs. */
type StructNames = Map<string, { name: string; type: StructType }>;

/** The key a struct type is collected and looked up under: its name and the members it holds. */
function structKey(t: StructType): string {
  return `${identifier(t.name, "Struct")}:${t.members.map((m) => `${m.name}@${m.offset}`).join(",")}`;
}

/** The HLSL name of a type, without the array dimensions (those belong to the declarator). */
function hlslType(t: ReflType, structs?: StructNames): string {
  switch (t.kind) {
    case "scalar": return scalarName(t.base, t.width);
    case "vector": return `${scalarName(t.element.base, t.element.width)}${t.count}`;
    // HLSL spells a matrix rows by columns, which is how the reflection counts them.
    case "matrix": return `${scalarName(t.element.base, t.element.width)}${t.rows}x${t.columns}`;
    case "array": return hlslType(t.element, structs);
    case "struct": return structs?.get(structKey(t))?.name ?? identifier(t.name, "Struct");
    case "opaque": return t.name || "float4";
    case "format": return "float4";
  }
}

/** "float4 name[2][3]": the type, the name, and every array dimension the type carries. */
function declarator(t: ReflType, name: string, structs?: StructNames): string {
  const dims: number[] = [];
  let cur: ReflType = t;
  while (cur.kind === "array") {
    // A runtime-sized array in a buffer layout cannot be declared; one element stands for it.
    dims.push(cur.count || 1);
    cur = cur.element;
  }
  return `${hlslType(cur, structs)} ${name}${dims.map((d) => `[${d}]`).join("")}`;
}

/**
 * Every struct type reachable from `t`, innermost first, so a declaration comes after the types it
 * uses. Structs are collected by name; a second shape under a name already taken is renamed.
 */
function collectStructs(t: ReflType | null | undefined, out: StructNames): void {
  if (!t) return;
  if (t.kind === "array") { collectStructs(t.element, out); return; }
  if (t.kind !== "struct") return;
  for (const m of t.members) collectStructs(m.type, out);
  const key = structKey(t);
  if (out.has(key)) return;
  const wanted = identifier(t.name, "Struct");
  let name = wanted;
  for (let n = 2; [...out.values()].some((s) => s.name === name); n++) name = `${wanted}_${n}`;
  out.set(key, { name, type: t });
}

/** `packoffset(c1.y)`: the constant register and component a member's byte offset falls on. */
function packOffset(offset: number): string {
  const register = Math.floor(offset / 16);
  const component = (offset % 16) / 4;
  return `packoffset(c${register}${component ? `.${"xyzw"[component]}` : ""})`;
}

/** Whether a resource is bound as a UAV (register u) rather than an SRV (register t). */
function isUav(r: ShaderResource): boolean {
  if (r.kind === "storageImage" || r.kind === "storageTexelBuffer") return true;
  return r.kind === "storage" && !r.readOnly;
}

/** "register(t3, space1)", with the array count a bound array of resources declares. */
function registerOf(letter: string, r: ShaderResource): string {
  return `register(${letter}${r.binding}, space${r.set})`;
}

function arraySuffix(r: ShaderResource): string {
  if (r.count === 0) return "[]";          // runtime-sized: unbounded, as the original declared it
  return r.count > 1 ? `[${r.count}]` : "";
}

/** A signature entry's field name: its semantic with the index, which is how the UI names it too. */
function fieldName(v: ShaderVariable, index: number): string {
  return identifier(v.name, `attribute${index}`);
}

// Some system values have a type the reflection's component mask cannot express: the signature
// reports SV_IsFrontFace as a uint, and HLSL will only take a bool. Keyed by the semantic without
// its index, uppercased, which is how DXC's reflection spells them.
const SYSTEM_VALUE_TYPES: Record<string, string> = {
  SV_ISFRONTFACE: "bool",
  SV_COVERAGE: "uint",
  SV_INNERCOVERAGE: "uint",
  SV_SAMPLEINDEX: "uint",
  SV_PRIMITIVEID: "uint",
  SV_INSTANCEID: "uint",
  SV_VERTEXID: "uint",
  SV_GSINSTANCEID: "uint",
  SV_OUTPUTCONTROLPOINTID: "uint",
  SV_RENDERTARGETARRAYINDEX: "uint",
  SV_VIEWPORTARRAYINDEX: "uint",
  SV_SHADINGRATE: "uint",
  SV_DEPTH: "float",
  SV_DEPTHGREATEREQUAL: "float",
  SV_DEPTHLESSEQUAL: "float",
  SV_STENCILREF: "uint",
};

/** The semantic of a signature entry without its index: "SV_TARGET1" -> "SV_TARGET". */
function semanticBase(name: string): string {
  return name.toUpperCase().replace(/\d+$/, "");
}

function variableType(v: ShaderVariable): string {
  return SYSTEM_VALUE_TYPES[semanticBase(v.name)] ?? hlslType(v.type);
}

/** A struct of the signature's entries, each field named and semantically tagged by its semantic. */
function signatureStruct(name: string, vars: ShaderVariable[]): string {
  const lines = vars.map((v, i) => `    ${variableType(v)} ${fieldName(v, i)} : ${fieldName(v, i)};`);
  return `struct ${name} {\n${lines.join("\n")}\n};\n`;
}

/** A value of the type that is visible in a frame capture: magenta for a color, 1 otherwise. */
function constantOf(type: string): string {
  if (type === "float4") return "float4(1.0, 0.0, 1.0, 1.0)";
  if (type === "float3") return "float3(1.0, 0.0, 1.0)";
  if (type === "float2") return "float2(1.0, 0.0)";
  if (type === "float") return "1.0";
  return `(${type})1`;
}

/** The declarations of every resource the stage binds, in register order, with their struct types. */
function resourceDeclarations(resources: ShaderResource[]): string[] {
  const structs: StructNames = new Map();
  for (const r of resources) {
    if (r.kind === "uniform" && r.type.kind === "struct") for (const m of r.type.members) collectStructs(m.type, structs);
    else collectStructs(r.type, structs);
  }
  const out: string[] = [];
  for (const s of structs.values()) {
    const members = s.type.members.map((m, i) => `    ${declarator(m.type, identifier(m.name, `member${i}`), structs)};`);
    out.push(`struct ${s.name} {\n${members.join("\n")}\n};`);
  }
  for (const r of resources) {
    const name = identifier(r.name, `resource${r.set}_${r.binding}`);
    if (r.kind === "uniform") {
      const members = r.type.kind === "struct" ? r.type.members : [];
      // packoffset puts every member back at the offset the original had; HLSL takes it for all
      // of a buffer's members or for none, so a layout it cannot spell is left to fall out of the
      // declaration order (which is what the compiler did in the first place).
      const exact = members.every((m) => m.offset % 4 === 0);
      const lines = members.map((m, i) =>
        `    ${declarator(m.type, identifier(m.name, `member${i}`), structs)}${exact ? ` : ${packOffset(m.offset)}` : ""};`);
      out.push(`cbuffer ${name} : ${registerOf("b", r)} {\n${lines.join("\n")}\n};`);
      continue;
    }
    if (r.kind === "sampler") {
      out.push(`SamplerState ${name}${arraySuffix(r)} : ${registerOf("s", r)};`);
      continue;
    }
    // reflection.ts already spells the HLSL object type, RW prefix and element type included.
    out.push(`${r.typeName} ${name}${arraySuffix(r)} : ${registerOf(isUav(r) ? "u" : "t", r)};`);
  }
  return out;
}

/** The entry point: the real signature, and a body that compiles and does something harmless. */
function entryDeclaration(stage: ShaderStage, entry: EntryPoint, name: string): string {
  const inputs = entry.inputs;
  const outputs = entry.outputs;
  const inputType = `${name}Input`;
  const outputType = `${name}Output`;
  const parts: string[] = [];
  const parameter = inputs.length ? `${inputType} input` : "";
  if (inputs.length) parts.push(signatureStruct(inputType, inputs));

  if (stage === "compute") {
    const [x, y, z] = entry.workgroupSize ?? [1, 1, 1];
    return `[numthreads(${x}, ${y}, ${z})]\nvoid ${name}(uint3 threadId : SV_DispatchThreadID) {\n}\n`;
  }

  if (stage === "fragment") {
    // One color target is the common case and reads best as a return value with its semantic.
    if (outputs.length === 1 && semanticBase(outputs[0].name) === "SV_TARGET") {
      const type = variableType(outputs[0]);
      parts.push(`${type} ${name}(${parameter}) : ${fieldName(outputs[0], 0)} {\n    return ${constantOf(type)};\n}\n`);
      return parts.join("\n");
    }
    if (!outputs.length) {
      parts.push(`void ${name}(${parameter}) {\n}\n`);
      return parts.join("\n");
    }
    parts.push(signatureStruct(outputType, outputs));
    const assignments = outputs.map((v, i) => semanticBase(v.name) === "SV_TARGET"
      ? `    output.${fieldName(v, i)} = ${constantOf(variableType(v))};` : null).filter(Boolean);
    parts.push(`${outputType} ${name}(${parameter}) {\n    ${outputType} output = (${outputType})0;\n${assignments.join("\n")}${assignments.length ? "\n" : ""}    return output;\n}\n`);
    return parts.join("\n");
  }

  if (stage === "vertex") {
    if (!outputs.length) {
      parts.push(`void ${name}(${parameter}) {\n}\n`);
      return parts.join("\n");
    }
    parts.push(signatureStruct(outputType, outputs));
    const position = outputs.findIndex((v) => semanticBase(v.name) === "SV_POSITION");
    const source = inputs.findIndex((v) => semanticBase(v.name) === "POSITION");
    const body: string[] = [];
    if (position >= 0 && source >= 0) {
      const from = `input.${fieldName(inputs[source], source)}`;
      const t = inputs[source].type;
      const count = t.kind === "vector" ? t.count : 1;
      const value = count >= 4 ? from : count === 3 ? `float4(${from}, 1.0)` : count === 2 ? `float4(${from}, 0.0, 1.0)` : `float4(${from}, 0.0, 0.0, 1.0)`;
      body.push(`    output.${fieldName(outputs[position], position)} = ${value};`);
    } else if (position >= 0) {
      body.push(`    output.${fieldName(outputs[position], position)} = float4(0.0, 0.0, 0.0, 1.0);`);
    }
    parts.push(`${outputType} ${name}(${parameter}) {\n    ${outputType} output = (${outputType})0;\n${body.join("\n")}${body.length ? "\n" : ""}    return output;\n}\n`);
    return parts.join("\n");
  }

  // Geometry, tessellation and mesh stages carry attributes the reflection does not record (the
  // domain and partitioning of a hull shader, a geometry shader's maximum vertex count, a mesh
  // shader's output topology), so their entry point is left for whoever edits this to complete.
  if (outputs.length) parts.push(signatureStruct(outputType, outputs));
  const returns = outputs.length ? outputType : "void";
  const body = outputs.length ? `    ${outputType} output = (${outputType})0;\n    return output;\n` : "";
  // A mesh or amplification stage's thread group size is reflected even though its topology is not.
  const threads = entry.workgroupSize ? `[numthreads(${entry.workgroupSize.join(", ")})]\n` : "";
  parts.push(`// The ${stage} stage's attributes are not in the reflection: add them (for example\n`
    + `// [maxvertexcount(n)], [domain(...)], [outputtopology(...)]) before this compiles.\n`
    + `${threads}${returns} ${name}(${parameter}) {\n${body}}\n`);
  return parts.join("\n");
}

/**
 * A compilable HLSL stage generated from `reflection`, declaring what the original declared: the
 * constant buffers with their members at their real offsets, the resources at their registers and
 * spaces, and the entry point with the real input and output signature. The body is a placeholder
 * and the header comment says so.
 */
export function hlslStub(reflection: ShaderReflection | null, stage: ShaderStage, entryPoint: string, options: HlslStubOptions = {}): string {
  const name = identifier(entryPoint, "main");
  const where = options.pipelineName ? ` for ${options.pipelineName}` : "";
  const why = options.reason ? ` (${options.reason.replace(/\s+/g, " ").trim()})` : "";
  const head = `// Replacement ${stage} shader${where}, written from the pipeline's reflection: D3D12 has no HLSL\n`
    + `// decompiler, and the original source could not be recovered${why}.\n`
    + `// Everything below — the constant buffer members at their real offsets, every resource at its\n`
    + `// register and space, the entry point's input and output semantics — is what the bytecode\n`
    + `// reports, so this compiles into a binding-compatible replacement. Only the body is invented.\n`;
  if (!reflection) {
    return `${head}// There is no reflection for this stage either, so even the declarations are a guess.\n\n`
      + `void ${name}() {\n}\n`;
  }
  const entry = reflection.entryPoint(entryPoint) ?? reflection.entryPoints[0]
    ?? { name, stage, inputs: [], outputs: [], workgroupSize: null };
  const declarations = resourceDeclarations(reflection.resources);
  const sections = [head];
  if (declarations.length) sections.push(`${declarations.join("\n")}\n`);
  sections.push(entryDeclaration(stage, entry, name));
  return sections.join("\n");
}
