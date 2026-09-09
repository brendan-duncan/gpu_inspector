// Metal's command classification. The Vulkan counterpart is ../vulkan/command_sets.ts, and the
// interface both fill in is ../command_sets.ts.
//
// The names are Objective-C selectors, which is what metal/src/capture.mm records as a command's
// method. Where Vulkan needs several spellings of the same command for its extensions and core
// versions, Metal needs one per overload — `drawPrimitives:` has four, differing only in whether
// instancing and base-instance arguments are present.
import type { BoundIndexBuffer, BoundVertexBuffer, CommandSets } from "../command_sets.js";
import type { CaptureCommand } from "../../shared/protocol.js";
// Generic argument coercers that happen to live beside the Vulkan object model.
import { num } from "../vulkan/vulkan_object.js";

const DRAW = new Set([
  "drawPrimitives:vertexStart:vertexCount:",
  "drawPrimitives:vertexStart:vertexCount:instanceCount:",
  "drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:",
  "drawPrimitives:indirectBuffer:indirectBufferOffset:",
  "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:",
  "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:",
  "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:",
  "drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:indirectBuffer:indirectBufferOffset:",
  "drawMeshThreadgroups:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
  "drawMeshThreads:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
]);

const DISPATCH = new Set([
  "dispatchThreads:threadsPerThreadgroup:",
  "dispatchThreadgroups:threadsPerThreadgroup:",
  "dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerThreadgroup:",
]);

const INDIRECT = new Set([
  "drawPrimitives:indirectBuffer:indirectBufferOffset:",
  "drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:indirectBuffer:indirectBufferOffset:",
  "dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerThreadgroup:",
]);

// A Metal encoder *is* the pass: creating one begins it and endEncoding closes it, for render and
// compute alike. That is why COMPUTE_PASS_END is empty — there is no run of dispatches to bracket
// the way the Vulkan layer has to.
const PASS_BEGIN = new Set([
  "renderCommandEncoderWithDescriptor:",
  "computeCommandEncoder",
  "computeCommandEncoderWithDescriptor:",
  "computeCommandEncoderWithDispatchType:",
  "blitCommandEncoder",
  "blitCommandEncoderWithDescriptor:",
]);
const PASS_END = new Set(["endEncoding"]);

export const METAL_SETS: CommandSets = {
  DRAW,
  DISPATCH,
  TRACE: new Set(),
  PASS_BEGIN,
  PASS_END,
  LABEL_BEGIN: new Set(["pushDebugGroup:"]),
  LABEL_END: new Set(["popDebugGroup"]),
  // `commit` hands the command buffer to the GPU and `presentDrawable:` schedules the frame:
  // between them they are what vkQueueSubmit and vkQueuePresentKHR are in a Vulkan capture.
  SUBMIT: new Set(["commit", "presentDrawable:"]),
  // Metal binds resources to an encoder directly rather than through a descriptor set object;
  // argument buffers are the closest thing and are not captured yet.
  BIND_DESCRIPTOR: new Set(),
  BIND_VERTEX: new Set([
    "setVertexBuffer:offset:atIndex:",
    "setVertexBuffers:offsets:withRange:",
    "setVertexBytes:length:atIndex:",
  ]),
  // Metal has no separate index-buffer binding: the index buffer is an argument of the draw.
  BIND_INDEX: new Set(),
  PUSH_CONSTANT: new Set(["setVertexBytes:length:atIndex:", "setFragmentBytes:length:atIndex:"]),
  INDIRECT,
  COMPUTE_PASS_END: new Set(),
  bindPointOf(method: string): string {
    return DISPATCH.has(method) ? "compute" : "render";
  },
  BIND_PIPELINE: new Set(["setRenderPipelineState:", "setComputePipelineState:"]),
  pipelineBindPointOf(method: string): string {
    return method === "setComputePipelineState:" ? "compute" : "render";
  },

  graphicsBindPoint: "render",

  vertexBuffersOf(cmd: CaptureCommand): BoundVertexBuffer[] {
    const a = cmd.args;
    if (!a || a.buffer === undefined) return [];
    // One buffer per call. Metal's vertex stride lives in the pipeline's vertex descriptor rather
    // than in the binding, so there is nothing to report for it here.
    return [{
      cmd,
      binding: num(a.index),
      buffer: a.buffer,
      offset: num(a.offset),
      size: null,
      stride: null,
      dataId: cmd.bufferData?.[0] ?? 0,
    }];
  },

  indexBufferOf(cmd: CaptureCommand): BoundIndexBuffer | null {
    // Metal has no index-buffer binding command: an indexed draw names its own index buffer.
    const a = cmd.args;
    if (!a || a.indexBuffer === undefined) return null;
    return {
      cmd,
      buffer: a.indexBuffer,
      offset: num(a.indexBufferOffset),
      // MTLIndexType: 0 = UInt16, 1 = UInt32.
      indexType: num(a.indexType) === 1 ? "MTLIndexTypeUInt32" : "MTLIndexTypeUInt16",
      // bufferData is [vertex..., index] per command; an indexed draw captures only its index buffer.
      dataId: cmd.bufferData?.[0] ?? 0,
    };
  },
};
