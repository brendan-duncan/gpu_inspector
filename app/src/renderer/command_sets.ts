// Command classification, per graphics API.
//
// The capture panel has to know which commands are draws, which open and close a render pass, and
// so on, and it can only tell from the method name — which is `vkCmdDraw` in a Vulkan capture and
// `drawPrimitives:vertexStart:vertexCount:` in a Metal one. Rather than one widening set of names
// from every API at once, each API contributes a table and a capture selects the one for its own
// (`CaptureData.sets`, from the `api` the capture library reported or the `.gpucap` recorded).
//
// The tables themselves live beside the rest of each API's code, in `vulkan/` and `metal/`.
import { METAL_SETS } from "./metal/command_sets.js";
import { VULKAN_SETS } from "./vulkan/command_sets.js";
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
}

/** Draws, dispatches and ray tracing launches: the commands with reconstructed state. */
export function isAction(sets: CommandSets, method: string): boolean {
  return sets.DRAW.has(method) || sets.DISPATCH.has(method) || sets.TRACE.has(method);
}

export function setsFor(api: CaptureApi): CommandSets {
  return api === "metal" ? METAL_SETS : VULKAN_SETS;
}
