// The state in effect at a captured command: the pipeline, descriptor sets, vertex, index and
// stage buffers, push constants and dynamic viewport state bound before it, reconstructed by
// walking back through its command buffer. The command details view (capture_command_info.ts)
// renders it and the MCP server (src/mcp/) reports it, so both read one reconstruction.
import { decodeBase64 } from "./utils/base64.js";
import type { BoundIndexBuffer, BoundStageBuffer, BoundVertexBuffer, CommandSets } from "./command_sets.js";
import { isObject, num, refId, str, type ObjectLookup, type VulkanObject } from "./vulkan/vulkan_object.js";
import type { CaptureData } from "./capture_data.js";
import type { ArgObject, ArgValue, CaptureCommand, CaptureDescriptorSet } from "../shared/protocol.js";

export interface BoundSet { cmd: CaptureCommand; set: CaptureDescriptorSet }

export interface PushConstantUpdate {
  cmd: CaptureCommand;
  stageFlags: string;
  offset: number;
  size: number;
  data: Uint8Array | null;
}

/** State in effect at a command, reconstructed by walking back through its command buffer. */
export interface DrawState {
  bindPoint: string;
  pipelineCmd: CaptureCommand | null;
  pipeline: VulkanObject | null;
  sets: Map<number, BoundSet>;
  vertexBuffers: Map<number, BoundVertexBuffer>;
  /** Metal: buffers bound to a stage by index, keyed "stage:index". */
  stageBuffers: Map<string, BoundStageBuffer>;
  indexBuffer: BoundIndexBuffer | null;
  /** vkCmdSetVertexInputEXT arguments when the vertex layout is dynamic. */
  vertexInput: ArgObject | null;
  viewports: ArgValue | null;
  scissors: ArgValue | null;
  pushConstants: PushConstantUpdate[];   // in recording order
}

/** The vertex layout of one binding: stride, input rate and the attributes read from it. */
export interface VertexLayout {
  stride: number;
  rate: string;
  attributes: { location: number; format: string; offset: number }[];
}

/** The bytes a push constants command carries. */
export function pushConstantBytes(a: ArgObject | null): Uint8Array | null {
  const v = a && isObject(a.pValues) ? a.pValues : null;
  if (!v || typeof v.base64 !== "string") return null;
  try {
    return decodeBase64(v.base64);
  } catch {
    return null;
  }
}

export function sameStream(cmdSets: CommandSets, cmd: CaptureCommand, c: CaptureCommand): boolean {
  if (c.object?.__id !== cmd.object?.__id) return false;
  if (cmdSets.SUBMIT.has(c.method)) return false;
  return true;
}

export function emptyDrawState(bindPoint: string): DrawState {
  return {
    bindPoint, pipelineCmd: null, pipeline: null, sets: new Map(), vertexBuffers: new Map(), stageBuffers: new Map(), indexBuffer: null,
    vertexInput: null, viewports: null, scissors: null, pushConstants: [],
  };
}

export function pushConstantOf(c: CaptureCommand): PushConstantUpdate | null {
  let a = c.args;
  if (!a) return null;
  if (isObject(a.pPushConstantsInfo)) a = a.pPushConstantsInfo;
  return { cmd: c, stageFlags: str(a.stageFlags), offset: num(a.offset), size: num(a.size), data: pushConstantBytes(a) };
}

/**
 * Walks back from a command through its command buffer, collecting the state bound before it.
 * Commands inlined from a secondary command buffer see only that buffer's own commands (a
 * secondary starts with no state); commands of a primary skip the inlined ones.
 */
export function drawState(data: CaptureData, db: ObjectLookup, cmd: CaptureCommand, bindPoint = data.sets.bindPointOf(cmd.method)): DrawState {
  const cmdSets = data.sets;
  const commands = data.commands;
  const state = emptyDrawState(bindPoint);
  // An API without an index-buffer binding command names it in the draw itself (Metal).
  // indexBufferOf answers null for a command that declares none, so this is safe to ask always.
  state.indexBuffer = cmdSets.indexBufferOf(cmd);
  for (let i = cmd.index - 1; i >= 0; i--) {
    const c = commands[i];
    if (!c || !sameStream(cmdSets, cmd, c)) break;
    if (cmd.secondary) {
      if (c.secondary !== cmd.secondary) break;
    } else if (c.secondary) {
      continue;
    }
    const a = c.args;
    if (!a) continue;
    if (cmdSets.BIND_PIPELINE.has(c.method)) {
      if (!state.pipelineCmd && cmdSets.pipelineBindPointOf(c.method, a) === bindPoint) {
        state.pipelineCmd = c;
        state.pipeline = db.getObject(refId(a.pipeline));
      }
      continue;
    }
    if (cmdSets.BIND_STAGE_BUFFER?.has(c.method) && cmdSets.stageBuffersOf) {
      for (const sb of cmdSets.stageBuffersOf(c)) {
        const key = `${sb.stage}:${sb.index}`;
        if (!state.stageBuffers.has(key)) state.stageBuffers.set(key, sb);
      }
    }
    if (cmdSets.BIND_VERTEX.has(c.method)) {
      for (const vb of cmdSets.vertexBuffersOf(c)) {
        if (!state.vertexBuffers.has(vb.binding)) state.vertexBuffers.set(vb.binding, vb);
      }
      continue;
    }
    if (cmdSets.BIND_INDEX.has(c.method)) {
      if (!state.indexBuffer) state.indexBuffer = cmdSets.indexBufferOf(c);
      continue;
    }
    switch (c.method) {
      case "vkCmdSetVertexInputEXT":
        if (!state.vertexInput) state.vertexInput = a;
        break;
      case "vkCmdSetViewport":
      case "vkCmdSetViewportWithCount":
      case "vkCmdSetViewportWithCountEXT":
        if (!state.viewports) state.viewports = a.pViewports ?? null;
        break;
      case "vkCmdSetScissor":
      case "vkCmdSetScissorWithCount":
      case "vkCmdSetScissorWithCountEXT":
        if (!state.scissors) state.scissors = a.pScissors ?? null;
        break;
      case "vkCmdPushConstants":
      case "vkCmdPushConstants2":
      case "vkCmdPushConstants2KHR": {
        const pc = pushConstantOf(c);
        if (pc) state.pushConstants.unshift(pc);
        break;
      }
      default:
        break;
    }
    if (c.descriptors && c.descriptors.bindPoint === bindPoint) {
      for (const set of c.descriptors.sets) {
        if (!state.sets.has(set.set)) state.sets.set(set.set, { cmd: c, set });
      }
    }
  }
  return state;
}

/** State for a binding command: what is bound before it, plus the pipeline bound next if none was bound before. */
export function bindingState(data: CaptureData, db: ObjectLookup, cmd: CaptureCommand, bindPoint: string): DrawState {
  const cmdSets = data.sets;
  const state = drawState(data, db, cmd, bindPoint);
  if (state.pipeline) return state;
  const commands = data.commands;
  for (let i = cmd.index + 1; i < commands.length; i++) {
    const c = commands[i];
    if (!c || !sameStream(cmdSets, cmd, c)) break;
    if (cmd.secondary ? c.secondary !== cmd.secondary : c.secondary) {
      if (cmd.secondary) break;
      continue;
    }
    if (cmdSets.BIND_PIPELINE.has(c.method) &&
        cmdSets.pipelineBindPointOf(c.method, c.args) === bindPoint) {
      state.pipelineCmd = c;
      state.pipeline = db.getObject(refId(c.args?.pipeline));
      break;
    }
  }
  return state;
}

/** Pass containing (or begun / ended by) a command. */
export function findPass(data: CaptureData, cmd: CaptureCommand): { passBegin: CaptureCommand; passIndex: number } | null {
  const cmdSets = data.sets;
  const commands = data.commands;
  let depth = 0;
  for (let i = cmd.index; i >= 0; i--) {
    const c = commands[i];
    if (!c || !sameStream(cmdSets, cmd, c)) break;
    if (c.secondary) continue;   // passes are begun and ended by the primary
    if (i !== cmd.index && cmdSets.PASS_END.has(c.method)) depth++;
    if (cmdSets.PASS_BEGIN.has(c.method)) {
      if (depth === 0) {
        let passIndex = 0;
        for (let j = i - 1; j >= 0; j--) {
          const p = commands[j];
          if (!p || !sameStream(cmdSets, cmd, p)) break;
          if (cmdSets.PASS_BEGIN.has(p.method)) passIndex++;
        }
        return { passBegin: c, passIndex };
      }
      depth--;
    }
  }
  return null;
}

/** The vertex layout of one binding: stride, input rate and its attributes, from the pipeline or dynamic state. */
export function vertexLayout(state: DrawState, binding: number, vb: BoundVertexBuffer): VertexLayout | null {
  // Metal: the pipeline's MTLVertexDescriptor, with a layout per buffer index and attributes
  // that name their buffer. Each attribute carries the protocol's format name beside Metal's,
  // which is what the decoder understands.
  const vd = state.pipeline?.descriptor?.vertexDescriptor;
  if (isObject(vd)) {
    const layouts = Array.isArray(vd.layouts) ? vd.layouts : [];
    const attrs = Array.isArray(vd.attributes) ? vd.attributes : [];
    const layout = layouts.find((l) => isObject(l) && num(l.index) === binding);
    if (!isObject(layout)) return null;
    const attributes = attrs.filter((a): a is ArgObject => isObject(a) && num(a.bufferIndex) === binding)
      .map((a) => ({ location: num(a.index), format: str(a.vkFormat ?? a.format), offset: num(a.offset) }))
      .sort((x, y) => x.offset - y.offset);
    return { stride: vb.stride ?? num(layout.stride), rate: str(layout.stepFunction).includes("PerInstance") ? "VK_VERTEX_INPUT_RATE_INSTANCE" : "VK_VERTEX_INPUT_RATE_VERTEX", attributes };
  }
  const vi = state.vertexInput ?? (isObject(state.pipeline?.descriptor?.pVertexInputState) ? state.pipeline!.descriptor!.pVertexInputState : null);
  if (!isObject(vi)) return null;
  const bindings = Array.isArray(vi.pVertexBindingDescriptions) ? vi.pVertexBindingDescriptions : [];
  const attrs = Array.isArray(vi.pVertexAttributeDescriptions) ? vi.pVertexAttributeDescriptions : [];
  const b = bindings.find((x) => isObject(x) && num(x.binding) === binding);
  if (!isObject(b)) return null;
  const attributes = attrs.filter((a): a is ArgObject => isObject(a) && num(a.binding) === binding)
    .map((a) => ({ location: num(a.location), format: str(a.format), offset: num(a.offset) }))
    .sort((x, y) => x.offset - y.offset);
  return { stride: vb.stride ?? num(b.stride), rate: str(b.inputRate), attributes };
}
