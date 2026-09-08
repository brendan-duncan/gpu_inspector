// Command classifications shared by the capture panel's command list and command details.

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

/** Draws, dispatches and ray tracing launches: the commands with reconstructed state. */
export function isAction(method: string): boolean {
  return DRAW_METHODS.has(method) || DISPATCH_METHODS.has(method) || TRACE_METHODS.has(method);
}

export function bindPointOf(method: string): string {
  if (DISPATCH_METHODS.has(method)) return "VK_PIPELINE_BIND_POINT_COMPUTE";
  if (TRACE_METHODS.has(method)) return "VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR";
  return "VK_PIPELINE_BIND_POINT_GRAPHICS";
}
