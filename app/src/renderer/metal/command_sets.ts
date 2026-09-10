// Metal's command classification. The Vulkan counterpart is ../vulkan/command_sets.ts, and the
// interface both fill in is ../command_sets.ts.
//
// The names are Objective-C selectors, which is what metal/src/capture.mm records as a command's
// method. Where Vulkan needs several spellings of the same command for its extensions and core
// versions, Metal needs one per overload — `drawPrimitives:` has four, differing only in whether
// instancing and base-instance arguments are present.
import type { BoundIndexBuffer, BoundStageBuffer, BoundVertexBuffer, CommandSets } from "../command_sets.js";
import type { CaptureCommand } from "../../shared/protocol.js";
// Generic argument coercers that happen to live beside the Vulkan object model.
import { isObject, num } from "../vulkan/vulkan_object.js";

const DRAW = new Set([
  "drawPrimitives:vertexStart:vertexCount:",
  "drawPrimitives:vertexStart:vertexCount:instanceCount:",
  "drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:",
  "drawPrimitives:indirectBuffer:indirectBufferOffset:",
  "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:",
  "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:",
  "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:",
  "drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:indirectBuffer:indirectBufferOffset:",
  "drawPatches:patchStart:patchCount:patchIndexBuffer:patchIndexBufferOffset:instanceCount:baseInstance:",
  "drawPatches:patchIndexBuffer:patchIndexBufferOffset:indirectBuffer:indirectBufferOffset:",
  "drawIndexedPatches:patchStart:patchCount:patchIndexBuffer:patchIndexBufferOffset:controlPointIndexBuffer:controlPointIndexBufferOffset:instanceCount:baseInstance:",
  "drawIndexedPatches:patchIndexBuffer:patchIndexBufferOffset:controlPointIndexBuffer:controlPointIndexBufferOffset:indirectBuffer:indirectBufferOffset:",
  "drawMeshThreadgroups:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
  "drawMeshThreads:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
  "drawMeshThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
  // An indirect command buffer executed in a render pass is a batch of draws.
  "executeCommandsInBuffer:withRange:",
  "executeCommandsInBuffer:indirectBuffer:indirectBufferOffset:",
]);

const DISPATCH = new Set([
  "dispatchThreads:threadsPerThreadgroup:",
  "dispatchThreadgroups:threadsPerThreadgroup:",
  "dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerThreadgroup:",
  "dispatchThreadsPerTile:",
]);

const INDIRECT = new Set([
  "drawPrimitives:indirectBuffer:indirectBufferOffset:",
  "drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:indirectBuffer:indirectBufferOffset:",
  "drawPatches:patchIndexBuffer:patchIndexBufferOffset:indirectBuffer:indirectBufferOffset:",
  "drawIndexedPatches:patchIndexBuffer:patchIndexBufferOffset:controlPointIndexBuffer:controlPointIndexBufferOffset:indirectBuffer:indirectBufferOffset:",
  "drawMeshThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
  "dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerThreadgroup:",
  "executeCommandsInBuffer:withRange:",
  "executeCommandsInBuffer:indirectBuffer:indirectBufferOffset:",
]);

// A Metal encoder *is* the pass: creating one begins it and endEncoding closes it, for render,
// compute and blit alike. That is why COMPUTE_PASS_END is empty — there is no run of dispatches
// to bracket the way the Vulkan layer has to. A parallel render encoder's sub-encoders share its
// pass, so their creation (`renderCommandEncoder`) is deliberately not here.
const PASS_BEGIN = new Set([
  "renderCommandEncoderWithDescriptor:",
  "parallelRenderCommandEncoderWithDescriptor:",
  "computeCommandEncoder",
  "computeCommandEncoderWithDescriptor:",
  "computeCommandEncoderWithDispatchType:",
  "blitCommandEncoder",
  "blitCommandEncoderWithDescriptor:",
  "resourceStateCommandEncoder",
  "resourceStateCommandEncoderWithDescriptor:",
  "accelerationStructureCommandEncoder",
  "accelerationStructureCommandEncoderWithDescriptor:",
]);
const PASS_END = new Set(["endEncoding"]);

// Buffers bound to a stage by index, and the inline-bytes forms that stand in for one. The
// stage is what the pipeline's reflection is keyed by (metal/reflection.ts).
const STAGE_BUFFER_METHODS: Record<string, { stage: string; kind: "one" | "many" | "bytes" }> = {
  "setVertexBuffer:offset:atIndex:": { stage: "vertex", kind: "one" },
  "setVertexBuffers:offsets:withRange:": { stage: "vertex", kind: "many" },
  "setVertexBytes:length:atIndex:": { stage: "vertex", kind: "bytes" },
  "setFragmentBuffer:offset:atIndex:": { stage: "fragment", kind: "one" },
  "setFragmentBuffers:offsets:withRange:": { stage: "fragment", kind: "many" },
  "setFragmentBytes:length:atIndex:": { stage: "fragment", kind: "bytes" },
  "setBuffer:offset:atIndex:": { stage: "compute", kind: "one" },
  "setBuffers:offsets:withRange:": { stage: "compute", kind: "many" },
  "setBytes:length:atIndex:": { stage: "compute", kind: "bytes" },
  "setObjectBuffer:offset:atIndex:": { stage: "object", kind: "one" },
  "setObjectBytes:length:atIndex:": { stage: "object", kind: "bytes" },
  "setMeshBuffer:offset:atIndex:": { stage: "mesh", kind: "one" },
  "setMeshBytes:length:atIndex:": { stage: "mesh", kind: "bytes" },
  "setTileBuffer:offset:atIndex:": { stage: "tile", kind: "one" },
  "setTileBytes:length:atIndex:": { stage: "tile", kind: "bytes" },
};
const BIND_STAGE_BUFFER = new Set(Object.keys(STAGE_BUFFER_METHODS));

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
  // `present` is the marker the library records when the frame ends through the drawable's own
  // present rather than through the command buffer (metal/README.md, "Frame boundaries").
  SUBMIT: new Set(["commit", "presentDrawable:", "presentDrawable:atTime:",
                   "presentDrawable:afterMinimumDuration:", "present"]),
  // Metal binds resources to an encoder directly rather than through a descriptor set object;
  // argument buffers are the closest thing and are not captured yet.
  BIND_DESCRIPTOR: new Set(),
  BIND_VERTEX: new Set([
    "setVertexBuffer:offset:atIndex:",
    "setVertexBuffers:offsets:withRange:",
  ]),
  // Metal has no separate index-buffer binding: the index buffer is an argument of the draw.
  BIND_INDEX: new Set(),
  // Inline constant blocks, on every stage that has them.
  PUSH_CONSTANT: new Set([
    "setVertexBytes:length:atIndex:", "setFragmentBytes:length:atIndex:", "setBytes:length:atIndex:",
    "setObjectBytes:length:atIndex:", "setMeshBytes:length:atIndex:", "setTileBytes:length:atIndex:",
  ]),
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
    if (!a) return [];
    // Metal's vertex stride lives in the pipeline's vertex descriptor rather than in the
    // binding, so there is nothing to report for it here.
    if (a.buffer !== undefined) {
      return [{
        cmd,
        binding: num(a.index),
        buffer: a.buffer,
        offset: num(a.offset),
        size: null,
        stride: null,
        dataId: cmd.bufferData?.[0] ?? 0,
      }];
    }
    // setVertexBuffers:offsets:withRange:, a range of slots in one call, like Vulkan's.
    if (Array.isArray(a.buffers)) {
      const first = isObject(a.range) ? num(a.range.location) : 0;
      const offsets = Array.isArray(a.offsets) ? a.offsets : [];
      return a.buffers.map((buffer, i) => ({
        cmd,
        binding: first + i,
        buffer,
        offset: num(offsets[i]),
        size: null,
        stride: null,
        dataId: cmd.bufferData?.[i] ?? 0,
      }));
    }
    return [];
  },

  BIND_STAGE_BUFFER,

  stageBuffersOf(cmd: CaptureCommand): BoundStageBuffer[] {
    const entry = STAGE_BUFFER_METHODS[cmd.method];
    const a = cmd.args;
    if (!entry || !a) return [];
    if (entry.kind === "bytes") {
      return [{ cmd, stage: entry.stage, index: num(a.index), buffer: null, offset: 0, dataId: cmd.bufferData?.[0] ?? 0, inline: true }];
    }
    if (entry.kind === "one") {
      return [{ cmd, stage: entry.stage, index: num(a.index), buffer: a.buffer ?? null, offset: num(a.offset), dataId: cmd.bufferData?.[0] ?? 0, inline: false }];
    }
    if (!Array.isArray(a.buffers)) return [];
    const first = isObject(a.range) ? num(a.range.location) : 0;
    const offsets = Array.isArray(a.offsets) ? a.offsets : [];
    return a.buffers.map((buffer, i) => ({
      cmd, stage: entry.stage, index: first + i, buffer, offset: num(offsets[i]), dataId: cmd.bufferData?.[i] ?? 0, inline: false,
    }));
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
