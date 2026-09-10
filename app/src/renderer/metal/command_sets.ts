// Metal's command classification. The Vulkan counterpart is ../vulkan/command_sets.ts, and the
// interface both fill in is ../command_sets.ts.
//
// The names are Objective-C selectors, which is what metal/src/capture.mm records as a command's
// method. Where Vulkan needs several spellings of the same command for its extensions and core
// versions, Metal needs one per overload — `drawPrimitives:` has four, differing only in whether
// instancing and base-instance arguments are present.
import type { BoundIndexBuffer, BoundStageBuffer, BoundVertexBuffer, CommandSets } from "../command_sets.js";
import type { ArgValue, CaptureCommand } from "../../shared/protocol.js";
// Generic argument coercers that happen to live beside the Vulkan object model.
import { isObject, num, str } from "../vulkan/vulkan_object.js";

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

/** "MTLPrimitiveTypeTriangle" as "Triangle", given the key it came under. */
function enumShort(key: string, v: ArgValue | undefined): string {
  if (typeof v !== "string") return v === undefined || v === null ? "" : String(v);
  const prefix = `MTL${key.charAt(0).toUpperCase()}${key.slice(1)}`;
  if (v.startsWith(prefix) && v.length > prefix.length) return v.slice(prefix.length);
  return v.startsWith("MTL") ? v.slice(3) : v;
}

function size(v: ArgValue | undefined): string {
  return isObject(v) ? `${num(v.width)}x${num(v.height)}x${num(v.depth)}` : "";
}

/**
 * What to show beside a command in the tree. The common ones are spelled out; for the rest,
 * the scalar arguments and object references, a few of them, so a `setCullMode:` reads
 * "Back" and a `fillBuffer:range:value:` names its buffer.
 */
function summarize(cmd: CaptureCommand, nameOf: (v: ArgValue | undefined) => string): string {
  const a = cmd.args;
  const m = cmd.method;
  if (!a) return "";
  const quoted = (v: ArgValue | undefined): string => (typeof v === "string" && v ? `"${v}"` : "");
  const slot = (what: string): string => `[${num(a.index)}] ${what}`;
  switch (m) {
    case "setLabel:":
    case "pushDebugGroup:":
    case "insertDebugSignpost:":
      return quoted(a.label);
    case "drawPrimitives:vertexStart:vertexCount:":
    case "drawPrimitives:vertexStart:vertexCount:instanceCount:":
    case "drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:":
      return `${enumShort("primitiveType", a.primitiveType)} ${num(a.vertexCount)} verts x${num(a.instanceCount)}`;
    case "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:":
    case "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:":
    case "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:":
      return `${enumShort("primitiveType", a.primitiveType)} ${num(a.indexCount)} idx x${num(a.instanceCount)}`;
    case "drawPrimitives:indirectBuffer:indirectBufferOffset:":
    case "drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:indirectBuffer:indirectBufferOffset:":
      return `${enumShort("primitiveType", a.primitiveType)} indirect ${nameOf(a.indirectBuffer)}`;
    case "dispatchThreads:threadsPerThreadgroup:":
      return `${size(a.threadsPerGrid)} threads, ${size(a.threadsPerThreadgroup)} per group`;
    case "dispatchThreadgroups:threadsPerThreadgroup:":
      return `${size(a.threadgroupsPerGrid)} groups of ${size(a.threadsPerThreadgroup)}`;
    case "dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerThreadgroup:":
      return `indirect ${nameOf(a.indirectBuffer)}, ${size(a.threadsPerThreadgroup)} per group`;
    case "setRenderPipelineState:":
    case "setComputePipelineState:":
      return nameOf(a.pipeline);
    case "setDepthStencilState:":
      return nameOf(a.depthStencilState);
    case "setViewport:":
      return isObject(a.viewport) ? `${num(a.viewport.width)}x${num(a.viewport.height)}` : "";
    case "setScissorRect:":
      return isObject(a.rect) ? `${num(a.rect.width)}x${num(a.rect.height)} at ${num(a.rect.x)},${num(a.rect.y)}` : "";
    case "renderCommandEncoderWithDescriptor:":
    case "parallelRenderCommandEncoderWithDescriptor:": {
      const colors = Array.isArray(a.colorAttachments) ? a.colorAttachments : [];
      const first = colors.find((c) => isObject(c) && c.texture !== null);
      const target = isObject(first) ? nameOf(first.texture) : "";
      const more = colors.length > 1 ? ` +${colors.length - 1}` : "";
      const depth = isObject(a.depthAttachment) ? " + depth" : "";
      return `${target || `${colors.length} attachment${colors.length === 1 ? "" : "s"}`}${more}${depth}`;
    }
    case "presentDrawable:":
    case "presentDrawable:atTime:":
    case "presentDrawable:afterMinimumDuration:":
      return nameOf(a.texture);
    case "present":
      return "";
    default:
      break;
  }
  if (a.buffer !== undefined && a.index !== undefined) {
    // set<Stage>Buffer:offset:atIndex:
    return slot(`${nameOf(a.buffer) || "(none)"}${num(a.offset) ? ` +${num(a.offset)}` : ""}`);
  }
  if (a.pValues !== undefined && a.index !== undefined) return slot(`${num(a.size)} bytes`);
  if (a.texture !== undefined && a.index !== undefined) return slot(nameOf(a.texture) || "(none)");
  if (a.sampler !== undefined && a.index !== undefined) return slot(nameOf(a.sampler) || "(none)");
  if (Array.isArray(a.buffers) && isObject(a.range)) return `[${num(a.range.location)}] +${num(a.range.length)}`;
  // Everything else: the scalars and references, a few of them.
  const parts: string[] = [];
  for (const [key, value] of Object.entries(a)) {
    if (parts.length >= 4) break;
    if (value === null || value === undefined) continue;
    if (typeof value === "number") parts.push(`${key} ${value}`);
    else if (typeof value === "boolean") parts.push(`${key} ${value}`);
    else if (typeof value === "string") parts.push(value.startsWith("MTL") ? enumShort(key, value) : `${key} ${str(value)}`);
    else if (isObject(value) && typeof value.__id === "number") { const n = nameOf(value); if (n) parts.push(n); }
    else if (isObject(value) && value.width !== undefined && value.height !== undefined) parts.push(`${key} ${size(value)}`);
  }
  return parts.join(", ");
}

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

  // Every encoder is a pass and they share one counter per command buffer, but the library times
  // a compute encoder under the compute kind (PassKind::Compute in metal/src/capture.mm), which
  // is a separate key. A blit or resource-state encoder is timed as a render pass.
  passIsCompute(method: string): boolean {
    return method.startsWith("computeCommandEncoder");
  },
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

  summarize,

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
