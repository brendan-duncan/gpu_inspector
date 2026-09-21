// Metal's command classification. The Vulkan counterpart is ../vulkan/command_sets.ts, and the
// interface both fill in is ../command_sets.ts.
//
// The names are Objective-C selectors, which is what src/metal/src/capture.mm records as a command's
// method. Where Vulkan needs several spellings of the same command for its extensions and core
// versions, Metal needs one per overload — `drawPrimitives:` has four, differing only in whether
// instancing and base-instance arguments are present.
import type {
  BoundIndexBuffer, BoundRayObject, BoundStageBuffer, BoundStageSampler, BoundStageTexture, BoundVertexBuffer, CommandSets,
} from "../command_sets.js";
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

/**
 * Acceleration structures and function tables bound to a stage, which bind at a *buffer* index:
 * `setAccelerationStructure:atBufferIndex:` puts one where `setBuffer:offset:atIndex:` would put
 * bytes, and only the shader parameter's type says which of the two a slot holds. Kept apart from
 * the buffer table because what they bind is an object with no bytes to read, and the shader
 * debugger's ray queries read them through their own accessor (msl/raytracing.ts).
 */
const RAY_BINDING_METHODS: Record<string, { stage: string; kind: BoundRayObject["kind"]; many?: boolean }> = {
  "setAccelerationStructure:atBufferIndex:": { stage: "compute", kind: "accelerationStructure" },
  "setVertexAccelerationStructure:atBufferIndex:": { stage: "vertex", kind: "accelerationStructure" },
  "setFragmentAccelerationStructure:atBufferIndex:": { stage: "fragment", kind: "accelerationStructure" },
  "setTileAccelerationStructure:atBufferIndex:": { stage: "tile", kind: "accelerationStructure" },
  "setIntersectionFunctionTable:atBufferIndex:": { stage: "compute", kind: "intersectionFunctionTable" },
  "setVertexIntersectionFunctionTable:atBufferIndex:": { stage: "vertex", kind: "intersectionFunctionTable" },
  "setFragmentIntersectionFunctionTable:atBufferIndex:": { stage: "fragment", kind: "intersectionFunctionTable" },
  "setTileIntersectionFunctionTable:atBufferIndex:": { stage: "tile", kind: "intersectionFunctionTable" },
  "setIntersectionFunctionTables:withBufferRange:": { stage: "compute", kind: "intersectionFunctionTable", many: true },
  "setVisibleFunctionTable:atBufferIndex:": { stage: "compute", kind: "visibleFunctionTable" },
  "setVertexVisibleFunctionTable:atBufferIndex:": { stage: "vertex", kind: "visibleFunctionTable" },
  "setFragmentVisibleFunctionTable:atBufferIndex:": { stage: "fragment", kind: "visibleFunctionTable" },
  "setTileVisibleFunctionTable:atBufferIndex:": { stage: "tile", kind: "visibleFunctionTable" },
  "setVisibleFunctionTables:withBufferRange:": { stage: "compute", kind: "visibleFunctionTable", many: true },
};
const BIND_RAY_OBJECT = new Set(Object.keys(RAY_BINDING_METHODS));

// Textures and samplers bound to a stage by index, the same shape as the buffer table: "one"
// binds a single index, "many" a range given by `withRange:`. A draw samples whatever these left
// bound, which is what the shader debugger reads its textures and samplers through.
const STAGE_TEXTURE_METHODS: Record<string, { stage: string; kind: "one" | "many" }> = {
  "setVertexTexture:atIndex:": { stage: "vertex", kind: "one" },
  "setVertexTextures:withRange:": { stage: "vertex", kind: "many" },
  "setFragmentTexture:atIndex:": { stage: "fragment", kind: "one" },
  "setFragmentTextures:withRange:": { stage: "fragment", kind: "many" },
  "setTexture:atIndex:": { stage: "compute", kind: "one" },
  "setTextures:withRange:": { stage: "compute", kind: "many" },
  "setObjectTexture:atIndex:": { stage: "object", kind: "one" },
  "setObjectTextures:withRange:": { stage: "object", kind: "many" },
  "setMeshTexture:atIndex:": { stage: "mesh", kind: "one" },
  "setMeshTextures:withRange:": { stage: "mesh", kind: "many" },
  "setTileTexture:atIndex:": { stage: "tile", kind: "one" },
  "setTileTextures:withRange:": { stage: "tile", kind: "many" },
};
const BIND_STAGE_TEXTURE = new Set(Object.keys(STAGE_TEXTURE_METHODS));

const STAGE_SAMPLER_METHODS: Record<string, { stage: string; kind: "one" | "many" }> = {
  "setVertexSamplerState:atIndex:": { stage: "vertex", kind: "one" },
  "setVertexSamplerState:lodMinClamp:lodMaxClamp:atIndex:": { stage: "vertex", kind: "one" },
  "setVertexSamplerStates:withRange:": { stage: "vertex", kind: "many" },
  "setVertexSamplerStates:lodMinClamps:lodMaxClamps:withRange:": { stage: "vertex", kind: "many" },
  "setFragmentSamplerState:atIndex:": { stage: "fragment", kind: "one" },
  "setFragmentSamplerState:lodMinClamp:lodMaxClamp:atIndex:": { stage: "fragment", kind: "one" },
  "setFragmentSamplerStates:withRange:": { stage: "fragment", kind: "many" },
  "setFragmentSamplerStates:lodMinClamps:lodMaxClamps:withRange:": { stage: "fragment", kind: "many" },
  "setSamplerState:atIndex:": { stage: "compute", kind: "one" },
  "setSamplerState:lodMinClamp:lodMaxClamp:atIndex:": { stage: "compute", kind: "one" },
  "setSamplerStates:withRange:": { stage: "compute", kind: "many" },
  "setSamplerStates:lodMinClamps:lodMaxClamps:withRange:": { stage: "compute", kind: "many" },
  "setObjectSamplerState:atIndex:": { stage: "object", kind: "one" },
  "setObjectSamplerStates:withRange:": { stage: "object", kind: "many" },
  "setMeshSamplerState:atIndex:": { stage: "mesh", kind: "one" },
  "setMeshSamplerStates:withRange:": { stage: "mesh", kind: "many" },
  "setTileSamplerState:atIndex:": { stage: "tile", kind: "one" },
  "setTileSamplerStates:withRange:": { stage: "tile", kind: "many" },
};
const BIND_STAGE_SAMPLER = new Set(Object.keys(STAGE_SAMPLER_METHODS));

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
    // The acceleration structure encoder's commands. Without these the generic fallback below
    // would print the scratch buffer and an offset, which is the least interesting thing about a
    // build: what it built and out of how much is what a reader is looking for.
    case "buildAccelerationStructure:descriptor:scratchBuffer:scratchBufferOffset:":
    case "refitAccelerationStructure:descriptor:destination:scratchBuffer:scratchBufferOffset:":
    case "refitAccelerationStructure:descriptor:destination:scratchBuffer:scratchBufferOffset:options:": {
      const target = nameOf(a.accelerationStructure) || "(none)";
      const d = isObject(a.descriptor) ? a.descriptor : null;
      if (!d) return target;
      const what = str(d.kind) === "instance"
        ? `${num(d.instanceCount).toLocaleString()} instances`
        : `${num(d.primitiveCount).toLocaleString()} primitives in `
          + `${Array.isArray(d.geometries) ? d.geometries.length : 0} geometries`;
      const refit = m.startsWith("refit") ? " (refit)" : "";
      return `${target} ← ${what}${refit}`;
    }
    case "copyAccelerationStructure:toAccelerationStructure:":
    case "copyAndCompactAccelerationStructure:toAccelerationStructure:":
      return `${nameOf(a.sourceAccelerationStructure) || "(none)"} → `
           + `${nameOf(a.destinationAccelerationStructure) || "(none)"}`;
    case "writeCompactedAccelerationStructureSize:toBuffer:offset:":
    case "writeCompactedAccelerationStructureSize:toBuffer:offset:sizeDataType:":
      return `${nameOf(a.accelerationStructure) || "(none)"} → ${nameOf(a.buffer) || "(none)"}`;
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
  // A Metal command buffer is used once, so the next frame's is a different object with a
  // counter of its own: there is no recording to restart (command_sets.ts).
  RECORD_BEGIN: new Set<string>(),
  RECORD_END: new Set<string>(),
  LABEL_BEGIN: new Set(["pushDebugGroup:"]),
  LABEL_END: new Set(["popDebugGroup"]),
  // `commit` hands the command buffer to the GPU and `presentDrawable:` schedules the frame:
  // between them they are what vkQueueSubmit and vkQueuePresentKHR are in a Vulkan capture.
  // `present` is the marker the library records when the frame ends through the drawable's own
  // present rather than through the command buffer (src/metal/README.md, "Frame boundaries").
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
  // a compute encoder under the compute kind (PassKind::Compute in src/metal/src/capture.mm), which
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

  BIND_RAY_OBJECT,

  rayObjectsOf(cmd: CaptureCommand): BoundRayObject[] {
    const entry = RAY_BINDING_METHODS[cmd.method];
    const a = cmd.args;
    if (!entry || !a) return [];
    if (!entry.many) {
      // The singular forms name the object by what it is: `accelerationStructure`,
      // `intersectionFunctionTable`, `visibleFunctionTable` (src/metal/src/hooks_encoders.mm).
      const object = a.accelerationStructure ?? a.intersectionFunctionTable ?? a.visibleFunctionTable ?? null;
      return [{ cmd, stage: entry.stage, index: num(a.index), kind: entry.kind, object }];
    }
    const list = a.intersectionFunctionTables ?? a.visibleFunctionTables;
    if (!Array.isArray(list)) return [];
    const first = isObject(a.range) ? num(a.range.location) : 0;
    return list.map((object, i) => ({ cmd, stage: entry.stage, index: first + i, kind: entry.kind, object }));
  },

  BIND_STAGE_TEXTURE,

  stageTexturesOf(cmd: CaptureCommand): BoundStageTexture[] {
    const entry = STAGE_TEXTURE_METHODS[cmd.method];
    const a = cmd.args;
    if (!entry || !a) return [];
    if (entry.kind === "one") {
      return [{ cmd, stage: entry.stage, index: num(a.index), texture: a.texture ?? null, dataId: cmd.textureData?.[0] ?? 0 }];
    }
    if (!Array.isArray(a.textures)) return [];
    const first = isObject(a.range) ? num(a.range.location) : 0;
    return a.textures.map((texture, i) => ({
      cmd, stage: entry.stage, index: first + i, texture, dataId: cmd.textureData?.[i] ?? 0,
    }));
  },

  BIND_STAGE_SAMPLER,

  stageSamplersOf(cmd: CaptureCommand): BoundStageSampler[] {
    const entry = STAGE_SAMPLER_METHODS[cmd.method];
    const a = cmd.args;
    if (!entry || !a) return [];
    const clamps = (i: number): { lodMinClamp?: number; lodMaxClamp?: number } => {
      // The plural form carries parallel arrays; the singular one a pair of scalars.
      const min = Array.isArray(a.lodMinClamps) ? a.lodMinClamps[i] : a.lodMinClamp;
      const max = Array.isArray(a.lodMaxClamps) ? a.lodMaxClamps[i] : a.lodMaxClamp;
      return {
        ...(min === undefined ? {} : { lodMinClamp: num(min) }),
        ...(max === undefined ? {} : { lodMaxClamp: num(max) }),
      };
    };
    if (entry.kind === "one") {
      return [{ cmd, stage: entry.stage, index: num(a.index), sampler: a.sampler ?? null, ...clamps(0) }];
    }
    if (!Array.isArray(a.samplers)) return [];
    const first = isObject(a.range) ? num(a.range.location) : 0;
    return a.samplers.map((sampler, i) => ({ cmd, stage: entry.stage, index: first + i, sampler, ...clamps(i) }));
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
