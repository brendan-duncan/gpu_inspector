// One description of each Metal descriptor a capture holds, read by two visitors: the filler
// (FillVisitor in mtl_replayer.mm), which sets the descriptor's properties from the JSON the
// capture library wrote (src/metal/src/hooks_descriptors.mm), and the source emitter (EmitVisitor),
// which spells the filled descriptor as Objective-C++ for Export to C++. Metal has no registry to
// generate these from, as vk.xml is for the Vulkan replay, so they are written once here and the
// two directions stay in step — the same bargain dx_reflect.h makes for D3D12.
//
// Where D3D12's descriptors are C structs whose members a visitor can bind to, Metal's are objects
// whose properties are messages. So a property is passed to the visitor as three things: the value
// it holds now, the value a freshly allocated descriptor of the same class holds, and a block that
// sets it. That is enough for both jobs, and it is what lets the emitter leave out every property
// the application never touched — a pipeline's description stays the handful of lines it was
// written as, rather than fifty lines of Metal's defaults.
//
// A visitor V provides, for a property `name`:
//   Uint / Int / Float / Bool (name, value, default, setter)
//   Enum / Flags              (name, value, default, table, setter)
//   Object                    (name, value, setter)          a tracked Metal object, by capture id
//   Label                     (value, setter)                the `label` every descriptor has
#pragma once

#include <iterator>

#import <Metal/Metal.h>

#include "metal_enums.gen.h"

#include "mtl_source.h"

namespace mtlreplay {

#define MTL_TABLE(T) ::mtlreplay::EnumTable{ ::mtlinsp::kEnum_##T, std::size(::mtlinsp::kEnum_##T), #T }

#define R_UINT(f)     v.Uint(#f, (uint64_t)o.f, (uint64_t)d.f, ^(uint64_t x) { o.f = (__typeof__(o.f))x; })
#define R_INT(f)      v.Int(#f, (int64_t)o.f, (int64_t)d.f, ^(int64_t x) { o.f = (__typeof__(o.f))x; })
#define R_FLOAT(f)    v.Float(#f, (double)o.f, (double)d.f, ^(double x) { o.f = (__typeof__(o.f))x; })
#define R_BOOL(f)     v.Bool(#f, (bool)o.f, (bool)d.f, ^(bool x) { o.f = x; })
#define R_ENUM(f, T)  v.Enum(#f, (int64_t)o.f, (int64_t)d.f, MTL_TABLE(T), ^(int64_t x) { o.f = (__typeof__(o.f))x; })
#define R_FLAGS(f, T) v.Flags(#f, (uint64_t)o.f, (uint64_t)d.f, MTL_TABLE(T), ^(uint64_t x) { o.f = (__typeof__(o.f))x; })
#define R_OBJECT(f, C) v.Object(#f, o.f, ^(id x) { o.f = (C)x; })
#define R_LABEL()     v.Label(o.label, ^(NSString* x) { o.label = x; })

// ---------------------------------------------------------------------------------------------
// Resources

/** TextureDescriptorArgs / TextureObjectArgs. A texture read back off the device reports the same
 *  property names its descriptor was built from, so one description serves both. */
template <typename V> void Reflect(V& v, MTLTextureDescriptor* o, MTLTextureDescriptor* d) {
    R_ENUM(textureType, MTLTextureType);
    R_ENUM(pixelFormat, MTLPixelFormat);
    R_UINT(width);
    R_UINT(height);
    R_UINT(depth);
    R_UINT(mipmapLevelCount);
    R_UINT(sampleCount);
    R_UINT(arrayLength);
    R_FLAGS(usage, MTLTextureUsage);
    R_ENUM(storageMode, MTLStorageMode);
    R_ENUM(cpuCacheMode, MTLCPUCacheMode);
    R_ENUM(hazardTrackingMode, MTLHazardTrackingMode);
    R_BOOL(allowGPUOptimizedContents);
}

/** HeapArgs. `allocatedSize` and the rest are the heap's own figures, not the descriptor's. */
template <typename V> void Reflect(V& v, MTLHeapDescriptor* o, MTLHeapDescriptor* d) {
    R_UINT(size);
    R_ENUM(storageMode, MTLStorageMode);
    R_ENUM(cpuCacheMode, MTLCPUCacheMode);
    R_ENUM(hazardTrackingMode, MTLHazardTrackingMode);
    R_ENUM(type, MTLHeapType);
}

/** SamplerArgs. Every member is written unconditionally, most of them as bare numbers. */
template <typename V> void Reflect(V& v, MTLSamplerDescriptor* o, MTLSamplerDescriptor* d) {
    R_LABEL();
    R_ENUM(minFilter, MTLSamplerMinMagFilter);
    R_ENUM(magFilter, MTLSamplerMinMagFilter);
    R_ENUM(mipFilter, MTLSamplerMipFilter);
    R_UINT(maxAnisotropy);
    R_ENUM(sAddressMode, MTLSamplerAddressMode);
    R_ENUM(tAddressMode, MTLSamplerAddressMode);
    R_ENUM(rAddressMode, MTLSamplerAddressMode);
    R_ENUM(borderColor, MTLSamplerBorderColor);
    R_BOOL(normalizedCoordinates);
    R_FLOAT(lodMinClamp);
    R_FLOAT(lodMaxClamp);
    R_ENUM(compareFunction, MTLCompareFunction);
    R_BOOL(supportArgumentBuffers);
}

// ---------------------------------------------------------------------------------------------
// Pipeline state

template <typename V> void Reflect(V& v, MTLStencilDescriptor* o, MTLStencilDescriptor* d) {
    R_ENUM(stencilCompareFunction, MTLCompareFunction);
    R_ENUM(stencilFailureOperation, MTLStencilOperation);
    R_ENUM(depthFailureOperation, MTLStencilOperation);
    R_ENUM(depthStencilPassOperation, MTLStencilOperation);
    R_UINT(readMask);
    R_UINT(writeMask);
}

/** DepthStencilArgs. The two stencil faces are objects of their own, handled by the caller. */
template <typename V> void Reflect(V& v, MTLDepthStencilDescriptor* o, MTLDepthStencilDescriptor* d) {
    R_LABEL();
    R_ENUM(depthCompareFunction, MTLCompareFunction);
    R_BOOL(depthWriteEnabled);
}

/** One entry of RenderPipelineArgs' `colorAttachments`, without its `index`. */
template <typename V>
void Reflect(V& v, MTLRenderPipelineColorAttachmentDescriptor* o, MTLRenderPipelineColorAttachmentDescriptor* d) {
    R_ENUM(pixelFormat, MTLPixelFormat);
    R_BOOL(blendingEnabled);
    R_ENUM(sourceRGBBlendFactor, MTLBlendFactor);
    R_ENUM(destinationRGBBlendFactor, MTLBlendFactor);
    R_ENUM(rgbBlendOperation, MTLBlendOperation);
    R_ENUM(sourceAlphaBlendFactor, MTLBlendFactor);
    R_ENUM(destinationAlphaBlendFactor, MTLBlendFactor);
    R_ENUM(alphaBlendOperation, MTLBlendOperation);
    R_FLAGS(writeMask, MTLColorWriteMask);
}

template <typename V> void Reflect(V& v, MTLVertexBufferLayoutDescriptor* o, MTLVertexBufferLayoutDescriptor* d) {
    R_UINT(stride);
    R_ENUM(stepFunction, MTLVertexStepFunction);
    R_UINT(stepRate);
}

template <typename V> void Reflect(V& v, MTLVertexAttributeDescriptor* o, MTLVertexAttributeDescriptor* d) {
    R_ENUM(format, MTLVertexFormat);
    R_UINT(offset);
    R_UINT(bufferIndex);
}

/** RenderPipelineArgs' scalars; the functions, vertex descriptor and attachments are the caller's. */
template <typename V> void Reflect(V& v, MTLRenderPipelineDescriptor* o, MTLRenderPipelineDescriptor* d) {
    R_LABEL();
    R_UINT(rasterSampleCount);
    R_BOOL(alphaToCoverageEnabled);
    R_BOOL(alphaToOneEnabled);
    R_BOOL(rasterizationEnabled);
    R_ENUM(inputPrimitiveTopology, MTLPrimitiveTopologyClass);
    R_ENUM(depthAttachmentPixelFormat, MTLPixelFormat);
    R_ENUM(stencilAttachmentPixelFormat, MTLPixelFormat);
    R_BOOL(supportIndirectCommandBuffers);
    R_UINT(maxTessellationFactor);
    R_ENUM(tessellationPartitionMode, MTLTessellationPartitionMode);
}

/** ComputePipelineArgs' scalars; the function is the caller's. */
template <typename V> void Reflect(V& v, MTLComputePipelineDescriptor* o, MTLComputePipelineDescriptor* d) {
    R_LABEL();
    R_BOOL(threadGroupSizeIsMultipleOfThreadExecutionWidth);
    R_UINT(maxTotalThreadsPerThreadgroup);
    R_BOOL(supportIndirectCommandBuffers);
}

/** TileRenderPipelineArgs' scalars. */
template <typename V> void Reflect(V& v, MTLTileRenderPipelineDescriptor* o, MTLTileRenderPipelineDescriptor* d) {
    R_LABEL();
    R_UINT(rasterSampleCount);
    R_BOOL(threadgroupSizeMatchesTileSize);
    R_UINT(maxTotalThreadsPerThreadgroup);
}

/** IndirectCommandBufferArgs. */
template <typename V>
void Reflect(V& v, MTLIndirectCommandBufferDescriptor* o, MTLIndirectCommandBufferDescriptor* d) {
    R_FLAGS(commandTypes, MTLIndirectCommandType);
    R_BOOL(inheritPipelineState);
    R_BOOL(inheritBuffers);
    R_UINT(maxVertexBufferBindCount);
    R_UINT(maxFragmentBufferBindCount);
    R_UINT(maxKernelBufferBindCount);
}

// ---------------------------------------------------------------------------------------------
// Passes

/** WritePassAttachment's shared members; `clearColor` / `clearDepth` / `clearStencil` and the
 *  attachment's `index` are the caller's, since they differ by aspect. */
template <typename V> void Reflect(V& v, MTLRenderPassAttachmentDescriptor* o, MTLRenderPassAttachmentDescriptor* d) {
    R_OBJECT(texture, id<MTLTexture>);
    R_UINT(level);
    R_UINT(slice);
    R_UINT(depthPlane);
    R_ENUM(loadAction, MTLLoadAction);
    R_ENUM(storeAction, MTLStoreAction);
    R_FLAGS(storeActionOptions, MTLStoreActionOptions);
    R_OBJECT(resolveTexture, id<MTLTexture>);
    R_UINT(resolveLevel);
    R_UINT(resolveSlice);
}

/** RenderPassArgs' scalars; the attachments are the caller's. */
template <typename V> void Reflect(V& v, MTLRenderPassDescriptor* o, MTLRenderPassDescriptor* d) {
    R_OBJECT(visibilityResultBuffer, id<MTLBuffer>);
    R_UINT(renderTargetArrayLength);
    R_UINT(renderTargetWidth);
    R_UINT(renderTargetHeight);
    R_UINT(defaultRasterSampleCount);
    R_UINT(imageblockSampleLength);
    R_UINT(threadgroupMemoryLength);
    R_UINT(tileWidth);
    R_UINT(tileHeight);
}

template <typename V> void Reflect(V& v, MTLComputePassDescriptor* o, MTLComputePassDescriptor* d) {
    R_ENUM(dispatchType, MTLDispatchType);
}

#undef R_UINT
#undef R_INT
#undef R_FLOAT
#undef R_BOOL
#undef R_ENUM
#undef R_FLAGS
#undef R_OBJECT
#undef R_LABEL

} // namespace mtlreplay
