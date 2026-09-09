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
import type { ArgObject, CaptureApi } from "../shared/protocol.js";

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
}

/** Draws, dispatches and ray tracing launches: the commands with reconstructed state. */
export function isAction(sets: CommandSets, method: string): boolean {
  return sets.DRAW.has(method) || sets.DISPATCH.has(method) || sets.TRACE.has(method);
}

export function setsFor(api: CaptureApi): CommandSets {
  return api === "metal" ? METAL_SETS : VULKAN_SETS;
}
