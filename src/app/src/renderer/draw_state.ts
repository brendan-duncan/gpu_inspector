// The state in effect at a captured command: the pipeline, descriptor sets, vertex, index and
// stage buffers, push constants and dynamic viewport state bound before it, reconstructed by
// walking back through its command buffer. The command details view (capture_command_info.ts)
// renders it and the MCP server (src/mcp/) reports it, so both read one reconstruction.
import { decodeBase64 } from "./utils/base64.js";
import {
  boundPipelineOf, type BoundIndexBuffer, type BoundStageBuffer, type BoundStageSampler, type BoundStageTexture, type BoundVertexBuffer, type CommandSets,
} from "./command_sets.js";
import { d3d12InputElements, d3d12PipelineKind, isD3D12Type } from "./d3d12/d3d12_object.js";
import { vkFormatOfDxgi } from "./d3d12/dxgi_format.js";
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
  /**
   * Vulkan: the shader objects bound instead of a pipeline (vkCmdBindShadersEXT, VK_EXT_shader_object),
   * one per stage in the order they were found, and the latest command that bound one.
   */
  shaders: VulkanObject[];
  shadersCmd: CaptureCommand | null;
  /**
   * Vulkan dynamic state, which a draw with shader objects always sets and a pipeline may leave
   * dynamic: null where no command set it.
   */
  dynamic: { cullMode: ArgValue | null; frontFace: ArgValue | null; topology: ArgValue | null; depthTest: ArgValue | null; depthCompare: ArgValue | null };
  sets: Map<number, BoundSet>;
  vertexBuffers: Map<number, BoundVertexBuffer>;
  /** Metal: buffers bound to a stage by index, keyed "stage:index". */
  stageBuffers: Map<string, BoundStageBuffer>;
  /** Metal: textures and samplers bound to a stage by index, keyed "stage:index". */
  stageTextures: Map<string, BoundStageTexture>;
  stageSamplers: Map<string, BoundStageSampler>;
  indexBuffer: BoundIndexBuffer | null;
  /** vkCmdSetVertexInputEXT arguments when the vertex layout is dynamic. */
  vertexInput: ArgObject | null;
  viewports: ArgValue | null;
  scissors: ArgValue | null;
  pushConstants: PushConstantUpdate[];   // in recording order
  /**
   * Metal: rasterizer state, which is set by commands on the encoder rather than baked into the
   * pipeline. The shader debugger's rasterizer reads these where a Vulkan draw's come from
   * `pRasterizationState` and `pDepthStencilState`.
   */
  cullMode: ArgValue | null;
  frontFace: ArgValue | null;
  depthStencil: VulkanObject | null;
}

/** The VkDynamicState each of DrawState.dynamic's values is. */
const DYNAMIC_STATES: Record<keyof DrawState["dynamic"], string> = {
  cullMode: "VK_DYNAMIC_STATE_CULL_MODE", frontFace: "VK_DYNAMIC_STATE_FRONT_FACE", topology: "VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY",
  depthTest: "VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE", depthCompare: "VK_DYNAMIC_STATE_DEPTH_COMPARE_OP",
};

/**
 * A Vulkan dynamic state value in effect at a draw: what was set, where nothing else could set it
 * (shader objects, or a pipeline declaring that state dynamic), else `baked`, the pipeline's own.
 */
export function dynamicValue(state: DrawState, key: keyof DrawState["dynamic"], baked: ArgValue | undefined): ArgValue | undefined {
  const value = state.dynamic[key];
  if (value === null) return baked;
  if (!state.pipeline) return value;
  // D3D12 bakes only the topology *type* into a pipeline: the topology itself is always set on
  // the list (IASetPrimitiveTopology), as are the viewports and scissors.
  if (isD3D12Type(state.pipeline.type)) return value;
  const d = state.pipeline.descriptor;
  const declared = isObject(d?.pDynamicState) && Array.isArray(d!.pDynamicState.pDynamicStates) ? d!.pDynamicState.pDynamicStates : [];
  return declared.some((s) => str(s) === DYNAMIC_STATES[key]) ? value : baked;
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
    bindPoint, pipelineCmd: null, pipeline: null, shaders: [], shadersCmd: null,
    dynamic: { cullMode: null, frontFace: null, topology: null, depthTest: null, depthCompare: null },
    sets: new Map(), vertexBuffers: new Map(), stageBuffers: new Map(),
    stageTextures: new Map(), stageSamplers: new Map(), indexBuffer: null,
    vertexInput: null, viewports: null, scissors: null, pushConstants: [],
    cullMode: null, frontFace: null, depthStencil: null,
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
  const shaderStages = new Set<string>();   // stages whose shader object (or its absence) was found
  for (let i = cmd.index - 1; i >= 0; i--) {
    const c = commands[i];
    if (!c || !sameStream(cmdSets, cmd, c)) break;
    if (cmd.secondary) {
      if (c.secondary !== cmd.secondary) break;
    } else if (c.secondary) {
      continue;
    }
    if (cmdSets.RECORD_BEGIN.has(c.method)) {
      // The recording starts here: nothing before it is this recording's state. A D3D12 list's
      // Reset binds its initial pipeline state (of either kind; the other kind's stays unbound).
      const initial = c.args?.pInitialState;
      if (initial !== undefined && !state.pipelineCmd && !state.shadersCmd) {
        const pipeline = db.getObject(refId(initial));
        if (pipeline && d3d12PipelineKind(pipeline) === bindPoint) {
          state.pipelineCmd = c;
          state.pipeline = pipeline;
        }
      }
      break;
    }
    const a = c.args;
    if (!a) continue;
    if (cmdSets.BIND_PIPELINE.has(c.method)) {
      // A pipeline bound after the shader objects found so far (walking back: before them) is
      // replaced by them, so it only counts when no shader object was bound since.
      if (!state.pipelineCmd && !state.shadersCmd && cmdSets.pipelineBindPointOf(c.method, a) === bindPoint) {
        state.pipelineCmd = c;
        state.pipeline = db.getObject(refId(boundPipelineOf(a)));
      }
      continue;
    }
    if (c.method === "vkCmdBindShadersEXT") {
      if (state.pipelineCmd) continue;   // a pipeline bound since replaced these
      const stages = Array.isArray(a.pStages) ? a.pStages : [];
      const shaders = Array.isArray(a.pShaders) ? a.pShaders : [];
      stages.forEach((flag, k) => {
        const stage = str(flag);
        const compute = stage.includes("COMPUTE");
        if (compute !== (bindPoint === "VK_PIPELINE_BIND_POINT_COMPUTE") || shaderStages.has(stage)) return;
        shaderStages.add(stage);
        state.shadersCmd ??= c;
        const shader = db.getObject(refId(shaders[k]));
        if (shader) state.shaders.push(shader);
      });
      continue;
    }
    if (cmdSets.BIND_STAGE_BUFFER?.has(c.method) && cmdSets.stageBuffersOf) {
      for (const sb of cmdSets.stageBuffersOf(c)) {
        const key = `${sb.stage}:${sb.index}`;
        if (!state.stageBuffers.has(key)) state.stageBuffers.set(key, sb);
      }
    }
    if (cmdSets.BIND_STAGE_TEXTURE?.has(c.method) && cmdSets.stageTexturesOf) {
      for (const st of cmdSets.stageTexturesOf(c)) {
        const key = `${st.stage}:${st.index}`;
        if (!state.stageTextures.has(key)) state.stageTextures.set(key, st);
      }
    }
    if (cmdSets.BIND_STAGE_SAMPLER?.has(c.method) && cmdSets.stageSamplersOf) {
      for (const ss of cmdSets.stageSamplersOf(c)) {
        const key = `${ss.stage}:${ss.index}`;
        if (!state.stageSamplers.has(key)) state.stageSamplers.set(key, ss);
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
      case "vkCmdSetCullMode":
      case "vkCmdSetCullModeEXT":
        state.dynamic.cullMode ??= a.cullMode ?? null;
        break;
      case "vkCmdSetFrontFace":
      case "vkCmdSetFrontFaceEXT":
        state.dynamic.frontFace ??= a.frontFace ?? null;
        break;
      case "vkCmdSetPrimitiveTopology":
      case "vkCmdSetPrimitiveTopologyEXT":
        state.dynamic.topology ??= a.primitiveTopology ?? null;
        break;
      case "vkCmdSetDepthTestEnable":
      case "vkCmdSetDepthTestEnableEXT":
        state.dynamic.depthTest ??= a.depthTestEnable ?? null;
        break;
      case "vkCmdSetDepthCompareOp":
      case "vkCmdSetDepthCompareOpEXT":
        state.dynamic.depthCompare ??= a.depthCompareOp ?? null;
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
      // Metal sets the rasterizer's state with commands on the encoder.
      case "setViewport:":
        if (!state.viewports) state.viewports = a.viewport ?? null;
        break;
      case "setViewports:count:":
        if (!state.viewports) state.viewports = a.viewports ?? null;
        break;
      case "setScissorRect:":
        if (!state.scissors) state.scissors = a.rect ?? null;
        break;
      case "setScissorRects:count:":
        if (!state.scissors) state.scissors = a.rects ?? null;
        break;
      case "setCullMode:":
        if (!state.cullMode) state.cullMode = a.cullMode ?? null;
        break;
      case "setFrontFacingWinding:":
        if (!state.frontFace) state.frontFace = a.frontFacingWinding ?? a.winding ?? null;
        break;
      case "setDepthStencilState:":
        if (!state.depthStencil) state.depthStencil = db.getObject(refId(a.depthStencilState));
        break;
      // D3D12 sets the topology, viewports and scissors on the command list.
      case "IASetPrimitiveTopology":
        state.dynamic.topology ??= a.PrimitiveTopology ?? null;
        break;
      case "RSSetViewports":
        if (!state.viewports) state.viewports = a.pViewports ?? null;
        break;
      case "RSSetScissorRects":
        if (!state.scissors) state.scissors = a.pRects ?? null;
        break;
      case "vkCmdPushConstants":
      case "vkCmdPushConstants2":
      case "vkCmdPushConstants2KHR":
      // D3D12 root constants arrive in the same shape (pValues, offset, size, stageFlags).
      case "SetGraphicsRoot32BitConstant":
      case "SetGraphicsRoot32BitConstants":
      case "SetComputeRoot32BitConstant":
      case "SetComputeRoot32BitConstants": {
        const pc = pushConstantOf(c);
        // A root constant binds one bind point; the other's are not this draw's.
        if (pc && (!pc.stageFlags || pc.stageFlags === bindPoint || !["graphics", "compute"].includes(pc.stageFlags))) state.pushConstants.unshift(pc);
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
      state.pipeline = db.getObject(refId(boundPipelineOf(c.args)));
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
  // D3D12: the pipeline's InputLayout lists elements that name their input slot; the stride is
  // the bound vertex buffer view's (a slot with no element is not read). The attribute location
  // is the element's index in the list, which is also how the mesh view names it.
  if (state.pipeline && isD3D12Type(state.pipeline.type)) {
    const elements = d3d12InputElements(state.pipeline).filter((e) => e.slot === binding);
    if (!elements.length) return null;
    const attributes = elements.map((e) => ({ location: e.location, format: vkFormatOfDxgi(e.format) ?? e.format, offset: e.offset }))
      .sort((x, y) => x.offset - y.offset);
    const perInstance = elements.some((e) => e.perInstance);
    return { stride: vb.stride ?? 0, rate: perInstance ? "VK_VERTEX_INPUT_RATE_INSTANCE" : "VK_VERTEX_INPUT_RATE_VERTEX", attributes };
  }
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
