// Vulkan's command classification. The Metal counterpart is ../metal/command_sets.ts, and the
// interface both fill in is ../command_sets.ts.
import type { CommandSets } from "../command_sets.js";
import type { ArgObject } from "../../shared/protocol.js";

export const DRAW_METHODS = new Set([
  "vkCmdDraw", "vkCmdDrawIndexed", "vkCmdDrawIndirect", "vkCmdDrawIndexedIndirect", "vkCmdDrawIndirectCount",
  "vkCmdDrawIndexedIndirectCount", "vkCmdDrawMeshTasksEXT", "vkCmdDrawMeshTasksIndirectEXT", "vkCmdDrawMeshTasksNV",
  "vkCmdDrawMultiEXT", "vkCmdDrawMultiIndexedEXT",
]);
export const DISPATCH_METHODS = new Set(["vkCmdDispatch", "vkCmdDispatchIndirect", "vkCmdDispatchBase"]);
export const TRACE_METHODS = new Set(["vkCmdTraceRaysKHR", "vkCmdTraceRaysIndirectKHR", "vkCmdTraceRaysIndirect2KHR"]);
export const PASS_BEGIN = new Set(["vkCmdBeginRenderPass", "vkCmdBeginRenderPass2", "vkCmdBeginRenderPass2KHR", "vkCmdBeginRendering", "vkCmdBeginRenderingKHR"]);
export const PASS_END = new Set(["vkCmdEndRenderPass", "vkCmdEndRenderPass2", "vkCmdEndRenderPass2KHR", "vkCmdEndRendering", "vkCmdEndRenderingKHR"]);
export const LABEL_BEGIN = new Set(["vkCmdBeginDebugUtilsLabelEXT", "vkCmdDebugMarkerBeginEXT"]);
export const LABEL_END = new Set(["vkCmdEndDebugUtilsLabelEXT", "vkCmdDebugMarkerEndEXT"]);
export const SUBMIT_METHODS = new Set(["vkQueueSubmit", "vkQueueSubmit2", "vkQueueSubmit2KHR", "vkQueuePresentKHR", "vkQueueBindSparse"]);

export const BIND_DESCRIPTOR_METHODS = new Set([
  "vkCmdBindDescriptorSets", "vkCmdBindDescriptorSets2", "vkCmdBindDescriptorSets2KHR",
  "vkCmdPushDescriptorSet", "vkCmdPushDescriptorSetKHR", "vkCmdPushDescriptorSet2", "vkCmdPushDescriptorSet2KHR",
]);
export const BIND_VERTEX_METHODS = new Set(["vkCmdBindVertexBuffers", "vkCmdBindVertexBuffers2", "vkCmdBindVertexBuffers2EXT"]);
export const BIND_INDEX_METHODS = new Set(["vkCmdBindIndexBuffer", "vkCmdBindIndexBuffer2", "vkCmdBindIndexBuffer2KHR"]);
export const PUSH_CONSTANT_METHODS = new Set(["vkCmdPushConstants", "vkCmdPushConstants2", "vkCmdPushConstants2KHR"]);
export const INDIRECT_METHODS = new Set(["vkCmdDrawIndirect", "vkCmdDrawIndexedIndirect", "vkCmdDispatchIndirect"]);

/**
 * Commands that end a compute pass (a run of dispatches outside a render pass) in the layer's
 * bracketing: barriers and event waits, secondary execution. Render pass begins, debug labels
 * and the end of the command buffer end one too (handled where those are processed).
 */
export const COMPUTE_PASS_END = new Set([
  "vkCmdPipelineBarrier", "vkCmdPipelineBarrier2", "vkCmdPipelineBarrier2KHR",
  "vkCmdWaitEvents", "vkCmdWaitEvents2", "vkCmdWaitEvents2KHR", "vkCmdExecuteCommands",
]);

/** Draws, dispatches and ray tracing launches: the commands with reconstructed state. */
export function isAction(method: string): boolean {
  return DRAW_METHODS.has(method) || DISPATCH_METHODS.has(method) || TRACE_METHODS.has(method);
}

export function bindPointOf(method: string): string {
  if (DISPATCH_METHODS.has(method)) return "VK_PIPELINE_BIND_POINT_COMPUTE";
  if (TRACE_METHODS.has(method)) return "VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR";
  return "VK_PIPELINE_BIND_POINT_GRAPHICS";
}

// The same tables as the API-keyed record the capture panel selects by `CaptureData.sets`
// (../command_sets.ts). The named exports above stay for Vulkan-only code that has no capture in
// hand — vulkan/frame_analysis.ts, which only ever runs on a Vulkan capture.
export const VULKAN_SETS: CommandSets = {
  DRAW: DRAW_METHODS,
  DISPATCH: DISPATCH_METHODS,
  TRACE: TRACE_METHODS,
  PASS_BEGIN,
  PASS_END,
  LABEL_BEGIN,
  LABEL_END,
  SUBMIT: SUBMIT_METHODS,
  BIND_DESCRIPTOR: BIND_DESCRIPTOR_METHODS,
  BIND_VERTEX: BIND_VERTEX_METHODS,
  BIND_INDEX: BIND_INDEX_METHODS,
  PUSH_CONSTANT: PUSH_CONSTANT_METHODS,
  INDIRECT: INDIRECT_METHODS,
  COMPUTE_PASS_END,
  bindPointOf,
  BIND_PIPELINE: new Set(["vkCmdBindPipeline"]),
  pipelineBindPointOf: (_method: string, args: ArgObject | null): string =>
    typeof args?.pipelineBindPoint === "string" ? args.pipelineBindPoint : "",
};
