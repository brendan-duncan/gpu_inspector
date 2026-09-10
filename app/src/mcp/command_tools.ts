// The MCP server's command and object tools: the command list, one command with the state bound
// at it (draw_state.ts reconstructs it, as for the command details view), the object graph, and
// the validation messages.
import { isAction, type BoundIndexBuffer, type BoundStageBuffer, type BoundVertexBuffer } from "../renderer/command_sets.js";
import { bindingState, drawState, emptyDrawState, findPass, pushConstantOf, vertexLayout, type BoundSet, type DrawState, type VertexLayout } from "../renderer/draw_state.js";
import { metalBufferResource, metalStages } from "../renderer/metal/reflection.js";
import { pipelineStages } from "../renderer/shader_cache.js";
import { imageOfView } from "../renderer/vulkan/pass_info.js";
import type { ReflType, ShaderReflection, ShaderResource, ShaderVariable, StructMember, StructType } from "../renderer/vulkan/spirv_reflect.js";
import { vertexFormat } from "../renderer/vulkan/vk_format.js";
import { isObject, num, objectMemoryBytes, refId, type VulkanObject } from "../renderer/vulkan/vulkan_object.js";
import type { ArgValue, CaptureCommand, CaptureDescriptor, CaptureDescriptorBinding, ValidationSeverity } from "../shared/protocol.js";
import type { Capture, CaptureStore } from "./capture_store.js";
import {
  CAPTURE_PARAM, PAGE_PARAMS, boolArg, compact, enumArg, jsonResult, optionalInt, page, readTyped, refText, regexArg, requireInt, round, schema,
  stackLines, stringArg, textureBrief, validationBrief,
} from "./describe.js";
import type { ToolDefinition } from "./stdio_server.js";

const KINDS = ["all", "draw", "dispatch", "action", "pass", "bind", "label", "submit", "issue", "validation"] as const;
const SEVERITY_ORDER: ValidationSeverity[] = ["error", "warning", "info", "verbose"];

/** The indirect command structs, by the draw or dispatch that reads them. */
const INDIRECT_FIELDS: Record<string, string[]> = {
  vkCmdDrawIndirect: ["vertexCount", "instanceCount", "firstVertex", "firstInstance"],
  vkCmdDrawIndexedIndirect: ["indexCount", "instanceCount", "firstIndex", "vertexOffset", "firstInstance"],
  vkCmdDispatchIndirect: ["x", "y", "z"],
};

/** A binding's vertex as a struct of VkFormat-typed attributes, named from the vertex shader's inputs. */
export function vertexStruct(layout: VertexLayout, inputs: ShaderVariable[]): StructType {
  return {
    kind: "struct", name: "Vertex", size: layout.stride,
    members: layout.attributes.map((a): StructMember => ({
      name: inputs.find((i) => i.location === a.location)?.name || `location${a.location}`,
      offset: a.offset,
      type: { kind: "format", format: a.format, size: vertexFormat(a.format)?.size ?? 0 },
    })),
  };
}

/** The inputs of a Vulkan pipeline's vertex shader, from its SPIR-V. */
export function vertexInputs(c: Capture, pipeline: VulkanObject | null): ShaderVariable[] {
  if (!pipeline || pipeline.type.startsWith("MTL")) return [];
  const vs = pipelineStages(pipeline, c.db).find((s) => s.stage === "vertex");
  return (vs && c.reflection(vs.object, vs.blobIndex)?.entryPoint(vs.entryPoint)?.inputs) ?? [];
}

/** Bytes per index of an index type ("VK_INDEX_TYPE_UINT16", "MTLIndexTypeUInt32"); 0 when unknown. */
export function indexSize(indexType: string): number {
  if (/uint8/i.test(indexType)) return 1;
  if (/16/.test(indexType)) return 2;
  if (/32/.test(indexType)) return 4;
  return 0;
}

export function readIndex(view: DataView, at: number, size: number): number {
  return size === 1 ? view.getUint8(at) : size === 2 ? view.getUint16(at, true) : view.getUint32(at, true);
}

function pick(o: ArgValue | undefined, keys: string[]): Record<string, ArgValue> | undefined {
  if (!isObject(o)) return undefined;
  const out: Record<string, ArgValue> = {};
  for (const k of keys) if (o[k] !== undefined) out[k] = o[k];
  return out;
}

/** The fixed-function state of a Vulkan graphics pipeline that decides what its draws write. */
function fixedFunctionState(p: VulkanObject): Record<string, unknown> | undefined {
  const d = p.descriptor;
  if (p.type !== "VkPipeline" || !d || !Array.isArray(d.pStages)) return undefined;
  const blend = isObject(d.pColorBlendState) ? d.pColorBlendState : null;
  const dynamic = isObject(d.pDynamicState) ? d.pDynamicState.pDynamicStates : undefined;
  return {
    topology: isObject(d.pInputAssemblyState) ? d.pInputAssemblyState.topology : undefined,
    rasterization: pick(d.pRasterizationState, ["polygonMode", "cullMode", "frontFace", "depthClampEnable", "depthBiasEnable", "rasterizerDiscardEnable"]),
    depthStencil: pick(d.pDepthStencilState, ["depthTestEnable", "depthWriteEnable", "depthCompareOp", "depthBoundsTestEnable", "stencilTestEnable"]),
    blend: blend && Array.isArray(blend.pAttachments)
      ? blend.pAttachments.map((a) => pick(a, ["blendEnable", "srcColorBlendFactor", "dstColorBlendFactor", "colorBlendOp", "srcAlphaBlendFactor", "dstAlphaBlendFactor", "alphaBlendOp", "colorWriteMask"]))
      : undefined,
    samples: isObject(d.pMultisampleState) ? d.pMultisampleState.rasterizationSamples : undefined,
    dynamicStates: Array.isArray(dynamic) ? dynamic : undefined,
  };
}

interface StageInfo { stage: string; entryPoint: string; object: VulkanObject; blobIndex: number; reflection: ShaderReflection | null }

/** The state bound at a command, in the terms get_command reports it. */
class StateReader {
  private _stages: StageInfo[] | null = null;

  constructor(private readonly c: Capture, private readonly state: DrawState, private readonly values: boolean) {}

  private get stages(): StageInfo[] {
    if (!this._stages) {
      const p = this.state.pipeline;
      this._stages = p && !p.type.startsWith("MTL")
        ? pipelineStages(p, this.c.db).map((s) => ({ stage: s.stage, entryPoint: s.entryPoint, object: s.object, blobIndex: s.blobIndex, reflection: this.c.reflection(s.object, s.blobIndex) }))
        : [];
    }
    return this._stages;
  }

  action(cmd: CaptureCommand): Record<string, unknown> {
    const sets = this.c.data.sets;
    const state = this.state;
    const graphics = state.bindPoint === sets.graphicsBindPoint;
    const stageBuffers = [...state.stageBuffers.values()].filter((sb) => (graphics ? sb.stage !== "compute" : sb.stage === "compute"));
    return {
      bindPoint: state.bindPoint,
      pipeline: this.pipeline(),
      descriptorSets: state.sets.size ? this.sets([...state.sets.values()].sort((a, b) => a.set.set - b.set.set)) : undefined,
      vertexBuffers: graphics && state.vertexBuffers.size ? this.vertexBuffers([...state.vertexBuffers.values()].sort((a, b) => a.binding - b.binding)) : undefined,
      indexBuffer: graphics && state.indexBuffer ? this.indexBuffer(state.indexBuffer, cmd) : undefined,
      stageBuffers: stageBuffers.length ? this.stageBuffers(stageBuffers) : undefined,
      pushConstants: state.pushConstants.length ? this.pushConstants() : undefined,
      viewports: state.viewports ? compact(state.viewports, this.c.db) : undefined,
      scissors: state.scissors ? compact(state.scissors, this.c.db) : undefined,
      indirect: sets.INDIRECT.has(cmd.method) ? this.indirect(cmd) : undefined,
      renderTargets: sets.DRAW.has(cmd.method) ? this.targetsOf(cmd) : undefined,
    };
  }

  pipeline(): Record<string, unknown> | undefined {
    const p = this.state.pipeline;
    if (!p) return undefined;
    const db = this.c.db;
    const metal = p.type.startsWith("MTL");
    return {
      pipeline: refText(db, p.id), boundAt: this.state.pipelineCmd?.index, summary: p.summary(db) || undefined,
      stages: metal
        ? metalStages(p).map((s) => ({ stage: s.stage, buffers: s.buffers.size, textures: s.textures.size, samplers: s.samplers.size }))
        : this.stages.map((s) => ({ stage: s.stage, entryPoint: s.entryPoint, shader: refText(db, s.object.id), blob: s.blobIndex })),
      functions: metal ? [...p.dependencies].filter((o) => o.type === "MTLFunction").map((o) => refText(db, o.id)) : undefined,
      fixedFunction: fixedFunctionState(p),
    };
  }

  sets(bound: BoundSet[]): unknown[] {
    const db = this.c.db;
    return bound.map(({ cmd, set }) => ({
      set: set.set, boundAt: cmd.index,
      descriptorSet: set.descriptorSet ? refText(db, set.descriptorSet) : "push descriptors", layout: refText(db, set.layout ?? undefined),
      bindings: set.bindings.map((b) => this.binding(set.set, b)),
    }));
  }

  private binding(set: number, b: CaptureDescriptorBinding): Record<string, unknown> {
    let res: ShaderResource | null = null;
    for (const s of this.stages) {
      res = s.reflection?.findResource(set, b.binding) ?? null;
      if (res) break;
    }
    const shown = b.descriptors.slice(0, 16);
    return {
      binding: b.binding, type: b.type, stages: b.stages, name: res?.name || undefined, shaderType: res?.typeName || undefined,
      descriptors: shown.map((d) => (d ? this.descriptor(d, res) : null)),
      more: b.descriptors.length > shown.length ? b.descriptors.length - shown.length : undefined,
    };
  }

  private descriptor(d: CaptureDescriptor, res: ShaderResource | null): Record<string, unknown> {
    const c = this.c;
    const db = c.db;
    if (d.buffer !== undefined) {
      return { buffer: refText(db, d.buffer), offset: d.offset, range: d.range, dynamicOffset: d.dynamicOffset, ...this.captured(d.data, res && res.type.kind !== "opaque" ? res.type : null) };
    }
    if (d.imageView !== undefined || d.sampler !== undefined) {
      const texture = c.data.capturedImage(d.data);
      return {
        imageView: refText(db, d.imageView), image: refText(db, imageOfView(db, refId(d.imageView ?? undefined))), layout: d.imageLayout,
        sampler: refText(db, d.sampler), texture: texture ? c.data.textures.indexOf(texture) : undefined,
      };
    }
    return compact(d, db) as Record<string, unknown>;
  }

  /** What the capture read back of a bound range: its size, and its values when the type is known. */
  private captured(dataId: number | undefined, type: ReflType | null): Record<string, unknown> {
    if (!dataId) return {};
    const b = this.c.data.buffer(dataId);
    if (!b) return { data: dataId };
    if (b.info.error) return { data: dataId, captureError: b.info.error };
    const out: Record<string, unknown> = { data: dataId, capturedBytes: b.data?.byteLength ?? 0, truncatedFrom: b.info.originalSize };
    if (this.values && type && b.data?.byteLength) {
      out.values = readTyped(type, new DataView(b.data.buffer, b.data.byteOffset, b.data.byteLength), 0, { values: 256 });
    }
    return out;
  }

  vertexBuffers(buffers: BoundVertexBuffer[]): unknown[] {
    const db = this.c.db;
    const vs = this.stages.find((s) => s.stage === "vertex");
    const inputs = vs?.reflection?.entryPoint(vs.entryPoint)?.inputs ?? [];
    const out: unknown[] = [];
    for (const vb of buffers) {
      const layout = vertexLayout(this.state, vb.binding, vb);
      // Metal binds constant blocks through the same call: a slot the vertex descriptor does not lay out is a stage buffer.
      if (!layout && this.c.data.sets.BIND_STAGE_BUFFER) continue;
      let firstVertices: ReflType | null = null;
      if (layout?.stride) firstVertices = { kind: "array", element: vertexStruct(layout, inputs), count: 3, stride: layout.stride, size: 3 * layout.stride };
      out.push({
        binding: vb.binding, buffer: refText(db, vb.buffer), offset: vb.offset, boundAt: vb.cmd.index,
        stride: layout?.stride, perInstance: layout?.rate.includes("INSTANCE") || undefined,
        attributes: layout?.attributes.map((a) => ({ location: a.location, name: inputs.find((i) => i.location === a.location)?.name || undefined, format: a.format, offset: a.offset })),
        ...this.captured(vb.dataId, firstVertices),
      });
    }
    return out;
  }

  indexBuffer(ib: BoundIndexBuffer, draw: CaptureCommand | null): Record<string, unknown> {
    const out: Record<string, unknown> = { buffer: refText(this.c.db, ib.buffer), offset: ib.offset, indexType: ib.indexType, boundAt: ib.cmd.index, ...this.captured(ib.dataId, null) };
    const data = this.c.data.buffer(ib.dataId)?.data;
    const size = indexSize(ib.indexType);
    if (this.values && data && size) {
      const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
      const first = draw ? num(draw.args?.firstIndex) : 0;
      const values: number[] = [];
      for (let i = first; i < first + 12 && (i + 1) * size <= data.byteLength; i++) values.push(readIndex(view, i * size, size));
      out.indices = { first, values };
    }
    return out;
  }

  stageBuffers(list: BoundStageBuffer[]): Record<string, unknown> {
    const db = this.c.db;
    const pipeline = this.state.pipeline;
    const reflected = metalStages(pipeline).length > 0;
    const slots: unknown[] = [];
    let unread = 0;
    for (const sb of [...list].sort((a, b) => a.stage.localeCompare(b.stage) || a.index - b.index)) {
      // Vertex-stage slots the vertex descriptor lays out are vertex buffers, listed as those.
      if (sb.stage === "vertex" && !sb.inline && vertexLayout(this.state, sb.index, { cmd: sb.cmd, binding: sb.index, buffer: sb.buffer, offset: sb.offset, size: null, stride: null, dataId: sb.dataId })) continue;
      const res = metalBufferResource(pipeline, sb.stage, sb.index);
      // With reflection, only the slots the pipeline's shaders read; without, every slot with something bound.
      if (reflected ? !res : !sb.buffer && !sb.inline) {
        unread++;
        continue;
      }
      slots.push({
        stage: sb.stage, index: sb.index, name: res?.name || undefined, shaderType: res?.typeName || undefined,
        buffer: sb.inline ? "inline bytes" : refText(db, sb.buffer), offset: sb.offset, boundAt: sb.cmd.index,
        ...this.captured(sb.dataId, res?.type ?? null),
      });
    }
    return { slots, unreadSlots: unread || undefined };
  }

  pushConstants(): Record<string, unknown> {
    const updates = this.state.pushConstants;
    const block = this.stages.map((s) => s.reflection?.pushConstants[0]).find((r): r is ShaderResource => !!r);
    const out: Record<string, unknown> = {
      name: block?.name || undefined, shaderType: block?.typeName || undefined,
      updates: updates.map((pc) => ({ stageFlags: pc.stageFlags, offset: pc.offset, size: pc.size, boundAt: pc.cmd.index })),
    };
    if (!this.values) return out;
    const blockType = block?.type;
    const size = Math.max(blockType?.kind === "struct" ? blockType.size : 0, ...updates.map((pc) => pc.offset + pc.size));
    const bytes = new Uint8Array(size);
    for (const pc of updates) {
      if (pc.data) bytes.set(pc.data.subarray(0, Math.max(0, Math.min(pc.data.byteLength, size - pc.offset))), pc.offset);
    }
    if (blockType?.kind === "struct") out.values = readTyped(blockType, new DataView(bytes.buffer), 0, { values: 256 });
    else out.bytes = Buffer.from(bytes).toString("hex");
    return out;
  }

  indirect(cmd: CaptureCommand): Record<string, unknown> | undefined {
    const fields = INDIRECT_FIELDS[cmd.method];
    const b = this.c.data.buffer(cmd.bufferData?.[0]);
    if (!fields || !b) return undefined;
    if (!b.data) return { data: b.info.id, captureError: b.info.error };
    const stride = Math.max(fields.length * 4, num(cmd.args?.stride));
    const count = Math.min(8, Math.max(1, num(cmd.args?.drawCount)), Math.floor(b.data.byteLength / stride));
    const view = new DataView(b.data.buffer, b.data.byteOffset, b.data.byteLength);
    const entries: Record<string, number>[] = [];
    for (let i = 0; i < count; i++) {
      const e: Record<string, number> = {};
      fields.forEach((f, k) => {
        e[f] = f === "vertexOffset" ? view.getInt32(i * stride + k * 4, true) : view.getUint32(i * stride + k * 4, true);
      });
      entries.push(e);
    }
    return { data: b.info.id, drawCount: num(cmd.args?.drawCount) || undefined, entries };
  }

  /** The read-back targets of the pass a command is in (or begins). */
  targetsOf(cmd: CaptureCommand): unknown[] | undefined {
    const pass = findPass(this.c.data, cmd);
    if (!pass) return undefined;
    return this.c.data.texturesForPass(cmd.frame, cmd.object?.__id ?? 0, pass.passIndex).map((t) => textureBrief(this.c, t));
  }
}

function commandDetail(c: Capture, cmd: CaptureCommand, values: boolean): Record<string, unknown> {
  const d = c.data;
  const db = c.db;
  const sets = d.sets;
  const passIndex = c.passOf(cmd.index);
  const pass = passIndex >= 0 ? c.metrics.passes[passIndex] : null;
  const issues = c.analysis.byCommand.get(cmd.index);
  const validation = db.validationForCommand(cmd.secondary ?? cmd.object?.__id, cmd.slot);
  const out: Record<string, unknown> = {
    capture: c.id, index: cmd.index, frame: d.frames > 1 ? cmd.frame : undefined, method: cmd.method,
    object: refText(db, cmd.object), secondary: refText(db, cmd.secondary),
    labels: c.labelsOf(cmd.index) || undefined,
    pass: pass ? { pass: passIndex, label: c.passName(passIndex), begin: pass.commandIndex, end: pass.endIndex, ms: round(pass.durationMs) } : undefined,
    result: cmd.result || undefined,
    args: compact(cmd.args, db),
    issues: issues?.map((f) => ({ rule: f.rule, severity: f.severity, confidence: f.confidence, message: f.message })),
    validation: validation.length ? validation.map((v) => validationBrief(c, v)) : undefined,
    stack: cmd.stack?.length ? stackLines(cmd.stack.map((a) => db.symbols.get(a) ?? { address: a, offset: 0 })) : undefined,
  };
  const m = cmd.method;
  if (isAction(sets, m)) {
    out.state = new StateReader(c, drawState(d, db, cmd), values).action(cmd);
  } else if (sets.BIND_PIPELINE.has(m)) {
    const state = emptyDrawState(sets.pipelineBindPointOf(m, cmd.args));
    state.pipelineCmd = cmd;
    state.pipeline = db.getObject(refId(cmd.args?.pipeline));
    out.pipeline = new StateReader(c, state, values).pipeline();
  } else if (sets.BIND_DESCRIPTOR.has(m) && cmd.descriptors) {
    const reader = new StateReader(c, bindingState(d, db, cmd, cmd.descriptors.bindPoint), values);
    out.descriptorSets = reader.sets(cmd.descriptors.sets.map((set) => ({ cmd, set })));
  } else if (sets.BIND_STAGE_BUFFER?.has(m) && sets.stageBuffersOf) {
    const bound = sets.stageBuffersOf(cmd);
    const reader = new StateReader(c, bindingState(d, db, cmd, bound.some((sb) => sb.stage === "compute") ? "compute" : sets.graphicsBindPoint), values);
    if (sets.BIND_VERTEX.has(m)) out.vertexBuffers = reader.vertexBuffers(sets.vertexBuffersOf(cmd));
    out.stageBuffers = reader.stageBuffers(bound);
  } else if (sets.BIND_VERTEX.has(m)) {
    out.vertexBuffers = new StateReader(c, bindingState(d, db, cmd, sets.graphicsBindPoint), values).vertexBuffers(sets.vertexBuffersOf(cmd));
  } else if (sets.BIND_INDEX.has(m)) {
    const ib = sets.indexBufferOf(cmd);
    if (ib) out.indexBuffer = new StateReader(c, emptyDrawState(sets.graphicsBindPoint), values).indexBuffer(ib, null);
  } else if (sets.PUSH_CONSTANT.has(m)) {
    const pc = pushConstantOf(cmd);
    const state = bindingState(d, db, cmd, pc?.stageFlags.includes("COMPUTE") ? "VK_PIPELINE_BIND_POINT_COMPUTE" : "VK_PIPELINE_BIND_POINT_GRAPHICS");
    if (pc) state.pushConstants.push(pc);
    out.pushConstants = new StateReader(c, state, values).pushConstants();
  } else if (sets.PASS_BEGIN.has(m)) {
    out.renderTargets = new StateReader(c, emptyDrawState(""), values).targetsOf(cmd);
  }
  return out;
}

export function commandTools(store: CaptureStore): ToolDefinition[] {
  return [
    {
      name: "list_commands",
      description: "List a capture's commands in submission order (secondary command buffers inlined where they executed), with " +
        "each command's key arguments, its pass, the debug groups around it, and the Frame Issues rules and validation messages " +
        "that apply to it. Filter by kind, method, pass, debug group label or frame; page with offset and limit.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        kind: { type: "string", enum: KINDS, description: "draw, dispatch, action (draws, dispatches, ray tracing), pass (pass begins), bind, label (debug group begins), submit, issue (commands a Frame Issues finding names), validation (commands a validation message fired on), or all (default)." },
        method: { type: "string", description: "Regular expression on the method name (\"DrawIndexed\", \"^vkCmdBind\")." },
        pass: { type: "integer", minimum: 0, description: "Only the commands of this pass (the pass numbers of get_bottlenecks and get_command)." },
        label: { type: "string", description: "Regular expression on the debug group path (\"Shadows\", \"Opaque / Terrain\")." },
        frame: { type: "integer", minimum: 0, description: "Only this captured frame (0-based), for multi-frame captures." },
        ...PAGE_PARAMS,
      }),
      readOnly: true,
      handler: (args) => {
        const c = store.resolve(stringArg(args, "capture"));
        const d = c.data;
        const db = c.db;
        const sets = d.sets;
        const kind = enumArg(args, "kind", KINDS, "all");
        const method = regexArg(args, "method");
        const label = regexArg(args, "label");
        const pass = optionalInt(args, "pass");
        const frame = optionalInt(args, "frame");
        const byCommand = c.analysis.byCommand;
        const validationOf = (cmd: CaptureCommand) => db.validationForCommand(cmd.secondary ?? cmd.object?.__id, cmd.slot);
        const matches = d.commands.filter((cmd) => {
          if (frame !== undefined && cmd.frame !== frame) return false;
          if (pass !== undefined && c.passOf(cmd.index) !== pass) return false;
          if (method && !method.test(cmd.method)) return false;
          if (label && !label.test(c.labelsOf(cmd.index))) return false;
          const m = cmd.method;
          switch (kind) {
            case "draw": return sets.DRAW.has(m);
            case "dispatch": return sets.DISPATCH.has(m);
            case "action": return isAction(sets, m);
            case "pass": return sets.PASS_BEGIN.has(m);
            case "bind": return sets.BIND_PIPELINE.has(m) || sets.BIND_DESCRIPTOR.has(m) || sets.BIND_VERTEX.has(m) || sets.BIND_INDEX.has(m) || sets.PUSH_CONSTANT.has(m) || !!sets.BIND_STAGE_BUFFER?.has(m);
            case "label": return sets.LABEL_BEGIN.has(m);
            case "submit": return sets.SUBMIT.has(m);
            case "issue": return byCommand.has(cmd.index);
            case "validation": return validationOf(cmd).length > 0;
            default: return true;
          }
        });
        const p = page(matches, args, 100, 500);
        const nameOf = (v: ArgValue | undefined): string => refText(db, v) ?? "";
        return jsonResult({
          capture: c.id, total: p.total, offset: p.offset, nextOffset: p.nextOffset,
          commands: p.items.map((cmd) => {
            const passIndex = c.passOf(cmd.index);
            const issues = byCommand.get(cmd.index);
            const validation = validationOf(cmd);
            return {
              i: cmd.index, method: cmd.method, args: sets.summarize?.(cmd, nameOf) || undefined,
              frame: d.frames > 1 ? cmd.frame : undefined, pass: passIndex >= 0 ? passIndex : undefined,
              labels: c.labelsOf(cmd.index) || undefined,
              issues: issues ? [...new Set(issues.map((f) => f.rule))] : undefined,
              validation: validation.length ? SEVERITY_ORDER.find((s) => validation.some((v) => v.severity === s)) : undefined,
            };
          }),
        });
      },
    },
    {
      name: "get_command",
      description: "One command in full: its arguments (object references as Type#id), pass and debug groups, the Frame Issues " +
        "and validation messages on it, its recording stack when captured, and for a draw, dispatch or binding command the " +
        "state bound at it — the pipeline with its shader stages and fixed-function state (topology, cull mode, depth test, " +
        "blending), every descriptor set with each binding's shader name and its uniform or storage buffer values decoded " +
        "by the shader's reflection, sampled images (with their read-back texture numbers), vertex buffers with the layout " +
        "and first vertices, the index buffer with the first indices, push constants with values, viewports and scissors, " +
        "indirect arguments, and the pass's render targets. Use it to see what a draw actually read.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        index: { type: "integer", minimum: 0, description: "The command's index (from list_commands, a finding or a message)." },
        values: { type: "boolean", description: "Decode buffer and push constant values (default true); false for the structure alone." },
      }, ["index"]),
      readOnly: true,
      handler: (args) => {
        const c = store.resolve(stringArg(args, "capture"));
        const index = requireInt(args, "index");
        const cmd = c.data.commands[index];
        if (!cmd) throw new Error(`No command ${index}: ${c.id} has ${c.data.commands.length} commands (0-${c.data.commands.length - 1}).`);
        return jsonResult(commandDetail(c, cmd, boolArg(args, "values", true)));
      },
    },
    {
      name: "list_objects",
      description: "List the objects a capture references (images, buffers, pipelines, shader modules, render passes, Metal " +
        "textures, libraries and pipeline states...), with a one-line summary each. Without a type filter it also counts them " +
        "by type. Objects destroyed before the capture was saved are kept when something still references them.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        type: { type: "string", description: "Only this type: \"VkImage\", \"VkPipeline\", \"MTLTexture\" (the Vk prefix may be left out)." },
        name: { type: "string", description: "Regular expression on the object's name or label." },
        ...PAGE_PARAMS,
      }),
      readOnly: true,
      handler: (args) => {
        const c = store.resolve(stringArg(args, "capture"));
        const db = c.db;
        const type = stringArg(args, "type")?.toLowerCase();
        const name = regexArg(args, "name");
        const all = [...db.allObjects.values(), ...db.destroyedObjects.values()].sort((a, b) => a.id - b.id);
        const list = all.filter((o) => (!type || o.type.toLowerCase() === type || o.shortType.toLowerCase() === type) && (!name || name.test(o.name) || name.test(o.label)));
        const types: Record<string, number> = {};
        if (!type) for (const o of all) types[o.type] = (types[o.type] ?? 0) + 1;
        const p = page(list, args, 100, 500);
        return jsonResult({
          capture: c.id, types: type ? undefined : types, total: p.total, offset: p.offset, nextOffset: p.nextOffset,
          objects: p.items.map((o) => ({
            id: o.id, type: o.type, name: o.name !== `${o.shortType} ${o.id}` ? o.name : undefined,
            summary: o.summary(db) || undefined, destroyed: o.isDeleted || undefined,
          })),
        });
      },
    },
    {
      name: "get_object",
      description: "One object in full: the call that created it with its arguments (the create info), later updates (memory " +
        "bindings, descriptor contents, device properties), its owner, what it depends on and what depends on it, its payloads, " +
        "its read-back images or buffer ranges in the capture, the validation messages naming it, and its creation stack " +
        "when captured. Shader-bearing objects point at get_shader.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        id: { type: "integer", minimum: 0, description: "The object's id: the number after # in a reference like VkImage#12." },
      }, ["id"]),
      readOnly: true,
      handler: (args) => {
        const c = store.resolve(stringArg(args, "capture"));
        const db = c.db;
        const d = c.data;
        const id = requireInt(args, "id");
        const o = db.getObject(id);
        if (!o) throw new Error(`No object ${id} in ${c.id}.`);
        const dependencies = [...o.dependencies];
        const dependents = [...o.dependents];
        const stack = db.stacks.get(o.id);
        const shaderTypes = ["VkPipeline", "VkShaderModule", "MTLLibrary", "MTLFunction", "MTLRenderPipelineState", "MTLComputePipelineState"];
        const images = d.textures.filter((t) => t.info.id === o.id).map((t) => d.textures.indexOf(t));
        const ranges = [...d.buffers.values()].filter((b) => b.info.buffer === o.id);
        const validation = db.validationFor(o.id);
        return jsonResult({
          capture: c.id, id: o.id, type: o.type, name: o.name !== `${o.shortType} ${o.id}` ? o.name : undefined, handle: o.handle,
          createdBy: o.cmd, parent: refText(db, o.parentId), destroyed: o.isDeleted || undefined, invalid: o.invalidReason ?? undefined,
          summary: o.summary(db) || undefined, memoryBytes: objectMemoryBytes(o, db) || undefined,
          args: compact(o.args, db),
          updates: Object.keys(o.updates).length ? compact(o.updates, db) : undefined,
          fixedFunction: fixedFunctionState(o),
          dependsOn: dependencies.length ? dependencies.slice(0, 100).map((x) => refText(db, x.id)) : undefined,
          dependents: dependents.length ? dependents.slice(0, 100).map((x) => refText(db, x.id)) : undefined,
          moreDependents: dependents.length > 100 ? dependents.length - 100 : undefined,
          payloads: o.blobs.length ? o.blobs.map((b, i) => ({ index: i, name: b.name, bytes: b.size, inFile: db.blobData.has(`${o.id}:${i}`) })) : undefined,
          shader: shaderTypes.includes(o.type) ? "get_shader shows this object's code, reflection and analysis." : undefined,
          textures: images.length ? images : undefined,
          bufferRanges: ranges.length ? ranges.slice(0, 50).map((b) => ({ data: b.info.id, offset: b.info.offset, bytes: b.info.size, error: b.info.error })) : undefined,
          validation: validation.length ? validation.slice(0, 20).map((v) => validationBrief(c, v, 600)) : undefined,
          creationStack: stack?.length ? stackLines(stack) : undefined,
        });
      },
    },
    {
      name: "get_validation",
      description: "The validation messages the capture carries (from the Khronos validation layer, the driver, or Metal's " +
        "validation layer, when enabled at launch), with repeat counts, the objects they name, and the captured command each " +
        "fired on when the layer could tell. Errors are real bugs; report them first.",
      inputSchema: schema({
        capture: CAPTURE_PARAM,
        severity: { type: "string", enum: ["error", "warning", "info", "verbose", "all"], description: "Only this severity (default all)." },
        ...PAGE_PARAMS,
      }),
      readOnly: true,
      handler: (args) => {
        const c = store.resolve(stringArg(args, "capture"));
        const db = c.db;
        const severity = enumArg(args, "severity", ["error", "warning", "info", "verbose", "all"] as const, "all");
        const list = db.validation.filter((v) => severity === "all" || v.severity === severity);
        const [errors, warnings] = db.validationCounts;
        const p = page(list, args, 50, 200);
        return jsonResult({
          capture: c.id, errors, warnings, total: p.total, offset: p.offset, nextOffset: p.nextOffset, dropped: db.validationDropped || undefined,
          messages: p.items.map((v) => validationBrief(c, v)),
          note: db.validation.length ? undefined : "No messages. The capture only has them when the application was launched with GPU Inspector's \"Validation layer\" option.",
        });
      },
    },
  ];
}
