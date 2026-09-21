// Command classification, per graphics API.
//
// The capture panel has to know which commands are draws, which open and close a render pass, and
// so on, and it can only tell from the method name — which is `vkCmdDraw` in a Vulkan capture and
// `drawPrimitives:vertexStart:vertexCount:` in a Metal one. Rather than one widening set of names
// from every API at once, each API contributes a table and a capture selects the one for its own
// (`CaptureData.sets`, from the `api` the capture library reported or the `.gpucap` recorded).
//
// The tables themselves live beside the rest of each API's code, in `vulkan/`, `metal/` and `d3d12/`,
// or in a plugin's backend module; backend.ts holds which table is whose.
import { backendFor } from "./backend.js";
import { d3d12PipelineOf } from "./d3d12/command_sets.js";
import { isObject, str } from "./vulkan/vulkan_object.js";
import type { ArgObject, ArgValue, CaptureApi, CaptureCommand } from "../shared/protocol.js";

/**
 * A vertex buffer a command binds. Vulkan binds a range of bindings with one command carrying
 * parallel arrays; Metal binds one per call, with the binding index as an argument. Both are
 * flattened to this.
 */
export interface BoundVertexBuffer {
  cmd: CaptureCommand;
  binding: number;
  buffer: ArgValue;
  offset: number;
  size: number | null;
  stride: number | null;
  /** Id of the CaptureBuffers entry holding the bound range's contents, 0 when not captured. */
  dataId: number;
}

export interface BoundIndexBuffer {
  cmd: CaptureCommand;
  buffer: ArgValue;
  offset: number;
  indexType: string;
  dataId: number;
}

/**
 * A buffer bound to a shader stage by index rather than through a descriptor set, which is how
 * Metal binds everything: `setVertexBuffer:offset:atIndex:`, `setFragmentBuffer:...`, the
 * compute encoder's `setBuffer:...`, and the inline `set*Bytes:` forms, whose bytes are the
 * capture's own buffer entry with no object behind it. The pipeline's reflection names the
 * struct at each stage and index.
 */
export interface BoundStageBuffer {
  cmd: CaptureCommand;
  /** "vertex", "fragment", "compute", "object", "mesh" or "tile". */
  stage: string;
  index: number;
  /** Null for inline bytes. */
  buffer: ArgValue | null;
  offset: number;
  /** Id of the CaptureBuffers entry holding the bound range's contents, 0 when not captured. */
  dataId: number;
  inline: boolean;
}

/**
 * Metal: an acceleration structure or a function table bound to a stage.
 *
 * These bind at a *buffer* index — `setAccelerationStructure:atBufferIndex:` — so the index shares
 * its namespace with the buffers above, and only the shader parameter's type says which of the two
 * a slot holds. The shader debugger's ray queries read them (msl/raytracing.ts).
 */
export interface BoundRayObject {
  cmd: CaptureCommand;
  stage: string;
  index: number;
  kind: "accelerationStructure" | "intersectionFunctionTable" | "visibleFunctionTable";
  object: ArgValue | null;
}

/**
 * A texture or a sampler bound to a stage by index, the counterpart of BoundStageBuffer:
 * `setFragmentTexture:atIndex:`, `setVertexSamplerState:atIndex:`, the compute encoder's
 * `setTexture:atIndex:`, and the plural forms that bind a range at once.
 */
export interface BoundStageTexture {
  cmd: CaptureCommand;
  /** "vertex", "fragment", "compute", "object", "mesh" or "tile". */
  stage: string;
  index: number;
  texture: ArgValue;
  /** Id of the CaptureTextureFrames entry holding the texels, 0 when they were not read back. */
  dataId: number;
}

export interface BoundStageSampler {
  cmd: CaptureCommand;
  stage: string;
  index: number;
  sampler: ArgValue;
  /** `setVertexSamplerState:lodMinClamp:lodMaxClamp:atIndex:` overrides the sampler's own clamps. */
  lodMinClamp?: number;
  lodMaxClamp?: number;
}

export interface CommandSets {
  DRAW: ReadonlySet<string>;
  DISPATCH: ReadonlySet<string>;
  /** Ray tracing launches; empty for an API without them. */
  TRACE: ReadonlySet<string>;
  PASS_BEGIN: ReadonlySet<string>;
  PASS_END: ReadonlySet<string>;
  LABEL_BEGIN: ReadonlySet<string>;
  LABEL_END: ReadonlySet<string>;
  SUBMIT: ReadonlySet<string>;
  BIND_DESCRIPTOR: ReadonlySet<string>;
  BIND_VERTEX: ReadonlySet<string>;
  BIND_INDEX: ReadonlySet<string>;
  PUSH_CONSTANT: ReadonlySet<string>;
  INDIRECT: ReadonlySet<string>;
  /**
   * Commands that close a compute pass. Vulkan has no compute pass, so the layer brackets runs of
   * dispatches and these end one. Metal has a real compute encoder, so its set is empty and
   * PASS_BEGIN/PASS_END carry the encoder instead.
   */
  COMPUTE_PASS_END: ReadonlySet<string>;
  /**
   * Where one recording of a command buffer starts and ends. The capture libraries number a
   * buffer's passes from zero within each recording, so anything counting passes has to restart
   * with them or its numbering drifts out of step (see collectPassMetrics).
   *
   * Empty for Metal, whose command buffers are used once: the next frame's is a different object
   * and starts a fresh count of its own without any marker.
   */
  RECORD_BEGIN: ReadonlySet<string>;
  RECORD_END: ReadonlySet<string>;
  /**
   * Whether a PASS_BEGIN command opens a compute pass rather than a render one. The layer and the
   * capture library key a compute pass's timings apart from a render pass's, so the UI has to
   * agree with them about which a pass is. Absent for an API whose PASS_BEGIN is render-only.
   */
  passIsCompute?(method: string): boolean;
  /** The name of the pipeline bind point a command uses, in that API's vocabulary. */
  bindPointOf(method: string): string;
  /** Commands that bind a pipeline, whose `args.pipeline` is the pipeline they bind. */
  BIND_PIPELINE: ReadonlySet<string>;
  /**
   * The bind point a pipeline-binding command targets. Vulkan has one command carrying the bind
   * point as an argument; Metal has one selector per bind point and no argument.
   */
  pipelineBindPointOf(method: string, args: ArgObject | null): string;
  /**
   * The bind point name that means "the graphics/render pipeline", in this API's vocabulary.
   * What vertex and index buffers hang off, and what a compute bind point is not.
   */
  graphicsBindPoint: string;

  // Reading a command's contents, which needs each API's own argument names rather than only its
  // method names. Vulkan's vkCmdBindVertexBuffers has `pBuffers`/`firstBinding`/`pOffsets`; Metal's
  // setVertexBuffer:offset:atIndex: has `buffer`/`index`/`offset`.

  /** The vertex buffers `cmd` binds, empty when it binds none. */
  vertexBuffersOf(cmd: CaptureCommand): BoundVertexBuffer[];
  /**
   * The index buffer `cmd` declares, or null when it declares none.
   *
   * Vulkan declares one with a binding command; Metal has no such command and names the index
   * buffer in the indexed draw itself, so for Metal this answers on the draw. Callers may ask any
   * command and rely on null.
   */
  indexBufferOf(cmd: CaptureCommand): BoundIndexBuffer | null;

  /**
   * Commands that bind a buffer to a stage by index (Metal). An API that binds through
   * descriptor sets leaves both of these out.
   */
  BIND_STAGE_BUFFER?: ReadonlySet<string>;
  /** The stage buffers `cmd` binds, empty when it binds none. */
  stageBuffersOf?(cmd: CaptureCommand): BoundStageBuffer[];

  /**
   * Commands that bind an acceleration structure or a function table to a stage (Metal). They bind
   * at a buffer index, so the index namespace is the one above and the shader parameter's type is
   * what tells a scene from bytes: the shader debugger's ray queries read these.
   */
  BIND_RAY_OBJECT?: ReadonlySet<string>;
  rayObjectsOf?(cmd: CaptureCommand): BoundRayObject[];

  /**
   * Commands that bind a texture or a sampler to a stage by index (Metal). What a draw sampled is
   * what these left bound, which is what the shader debugger reads its textures through.
   */
  BIND_STAGE_TEXTURE?: ReadonlySet<string>;
  stageTexturesOf?(cmd: CaptureCommand): BoundStageTexture[];
  BIND_STAGE_SAMPLER?: ReadonlySet<string>;
  stageSamplersOf?(cmd: CaptureCommand): BoundStageSampler[];

  /**
   * The short text shown beside a command in the tree: the arguments worth reading at a glance,
   * with `nameOf` resolving an object reference to its name. The command list shows it, and the
   * MCP server's command listing (src/mcp/) returns it. Undefined shows no summary.
   */
  summarize?(cmd: CaptureCommand, nameOf: (v: ArgValue | undefined) => string): string | undefined;

  /** The name a LABEL_BEGIN command opens its group with, for an API whose arguments name it differently. */
  labelOf?(cmd: CaptureCommand): string | undefined;

  /** What a PASS_BEGIN command's pass is called in the command tree and the thumbnail strip ("Render Pass 2: GBuffer"). */
  passLabel?(cmd: CaptureCommand, passIndex: number, nameOf: (v: ArgValue | undefined) => string): string | undefined;

  /**
   * What a draw draws, for an API whose draw arguments are not named like Vulkan's or D3D12's: the mesh
   * view reads it. `firstIndex` counts from the start of the index data the capture read back.
   */
  drawArgsOf?(cmd: CaptureCommand): DrawArgs | null;
}

/** A draw's counts, in Vulkan's terms. */
export interface DrawArgs {
  indexed: boolean;
  vertexCount?: number;
  indexCount?: number;
  firstVertex?: number;
  firstIndex?: number;
  vertexOffset?: number;
  instanceCount?: number;
}

/** Draws, dispatches and ray tracing launches: the commands with reconstructed state. */
export function isAction(sets: CommandSets, method: string): boolean {
  return sets.DRAW.has(method) || sets.DISPATCH.has(method) || sets.TRACE.has(method);
}

/**
 * The name a debug group command opens: what the API's own sets say (CommandSets.labelOf), else
 * Vulkan's label-info struct, or Metal's `label` (pushDebugGroup:).
 */
export function labelNameOf(cmd: CaptureCommand, sets?: CommandSets): string {
  const own = sets?.labelOf?.(cmd);
  if (own !== undefined) return own;
  const a = cmd.args;
  const info = a && (isObject(a.pLabelInfo) ? a.pLabelInfo : isObject(a.pMarkerInfo) ? a.pMarkerInfo : null);
  return info ? str(info.pLabelName ?? info.pMarkerName) : a && a.label !== undefined ? str(a.label) : cmd.method;
}

/**
 * The pipeline a BIND_PIPELINE command binds. Vulkan and Metal record it as `pipeline`; the D3D12
 * library may keep SetPipelineState's own parameter name.
 */
export function boundPipelineOf(a: ArgObject | null | undefined): ArgValue | undefined {
  return a ? a.pipeline ?? d3d12PipelineOf(a) : undefined;
}

export function setsFor(api: CaptureApi): CommandSets {
  return backendFor(api).sets;
}
