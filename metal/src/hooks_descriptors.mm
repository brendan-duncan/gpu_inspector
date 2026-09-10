// The descriptors: what an AddObject's "args" and a pass's arguments look like. See hooks_common.h.
#include "hooks_common.h"
#include "reflection.h"

#import <objc/message.h>

#include <cstring>

namespace mtlinsp {

std::atomic<uint64_t> g_frame{0};
std::atomic<uint32_t> g_drawsThisFrame{0};
std::atomic<uint32_t> g_dispatchesThisFrame{0};
std::atomic<uint32_t> g_encodersThisFrame{0};

const char *LabelOf(id object) {
    if (object == nil || ![object respondsToSelector:@selector(label)]) return "";
    NSString *label = [object performSelector:@selector(label)];
    return label == nil ? "" : label.UTF8String;
}

void WriteRef(vkinsp::JsonWriter &w, id object, const char *type) {
    const uint64_t id = IdOf(object);
    if (id == 0) {
        w.Null();
        return;
    }
    w.BeginObject();
    w.Key("__id"); w.Uint(id);
    w.Key("__class"); w.String(type);
    w.EndObject();
}

void WriteEnum(vkinsp::JsonWriter &w, const char *name, uint64_t value) {
    if (name != nullptr && name[0] != '\0') w.String(name);
    else w.Uint(value);
}

namespace {

/** MTLTextureUsage as "ShaderRead|RenderTarget", the way the Vulkan layer spells flag sets. */
std::string UsageFlags(MTLTextureUsage usage) {
    if (usage == MTLTextureUsageUnknown) return "Unknown";
    std::string out;
    auto add = [&](MTLTextureUsage bit, const char *name) {
        if ((usage & bit) == 0) return;
        if (!out.empty()) out += '|';
        out += name;
    };
    add(MTLTextureUsageShaderRead, "ShaderRead");
    add(MTLTextureUsageShaderWrite, "ShaderWrite");
    add(MTLTextureUsageRenderTarget, "RenderTarget");
    add(MTLTextureUsagePixelFormatView, "PixelFormatView");
    add((MTLTextureUsage)0x20, "ShaderAtomic");
    return out;
}

/**
 * A pipeline's function: a reference to the tracked MTLFunction when the application made it
 * through a hooked library, so the Inspect panel links the pipeline to it, and its name beside
 * either way, since the reference alone reads as a number.
 */
void WriteFunction(Args &a, const char *key, id<MTLFunction> function) {
    std::string nameKey = std::string(key) + "Name";
    if (function == nil) {
        a.c(key, nullptr).c(nameKey.c_str(), nullptr);
        return;
    }
    if (IdOf(function) != 0) a.ref(key, function, "MTLFunction");
    else a.s(key, function.name);
    a.s(nameKey.c_str(), function.name);
}

id<MTLFunction> FunctionOf(id descriptor, SEL selector) {
    if (descriptor == nil || ![descriptor respondsToSelector:selector]) return nil;
    return [descriptor performSelector:selector];
}

void WriteColorAttachmentBlend(vkinsp::JsonWriter &w, MTLRenderPipelineColorAttachmentDescriptor *a,
                               NSUInteger index) {
    w.BeginObject();
    w.Key("index"); w.Uint(index);
    w.Key("pixelFormat"); WriteEnum(w, PixelFormatEnumName(a.pixelFormat), (uint64_t)a.pixelFormat);
    w.Key("blendingEnabled"); w.Boolean(a.blendingEnabled);
    if (a.blendingEnabled) {
        w.Key("sourceRGBBlendFactor"); w.Uint((uint64_t)a.sourceRGBBlendFactor);
        w.Key("destinationRGBBlendFactor"); w.Uint((uint64_t)a.destinationRGBBlendFactor);
        w.Key("rgbBlendOperation"); w.Uint((uint64_t)a.rgbBlendOperation);
        w.Key("sourceAlphaBlendFactor"); w.Uint((uint64_t)a.sourceAlphaBlendFactor);
        w.Key("destinationAlphaBlendFactor"); w.Uint((uint64_t)a.destinationAlphaBlendFactor);
        w.Key("alphaBlendOperation"); w.Uint((uint64_t)a.alphaBlendOperation);
    }
    w.Key("writeMask"); w.Uint((uint64_t)a.writeMask);
    w.EndObject();
}

/**
 * The vertex layout, which is what makes a captured vertex buffer readable: without the strides
 * and attribute formats the UI has nothing but bytes. Each attribute carries Metal's name and the
 * protocol's, so the Inspect panel shows what the application wrote and the vertex decoder gets
 * the name it already understands.
 */
void WriteVertexDescriptor(vkinsp::JsonWriter &w, MTLVertexDescriptor *v) {
    if (v == nil) {
        w.Null();
        return;
    }
    w.BeginObject();
    w.Key("layouts"); w.BeginArray();
    for (NSUInteger i = 0; i < 31; i++) {
        MTLVertexBufferLayoutDescriptor *layout = v.layouts[i];
        if (layout == nil || layout.stride == 0) continue;
        w.BeginObject();
        w.Key("index"); w.Uint(i);
        w.Key("stride"); w.Uint(layout.stride);
        w.Key("stepFunction");
        switch (layout.stepFunction) {
            case MTLVertexStepFunctionConstant: w.String("MTLVertexStepFunctionConstant"); break;
            case MTLVertexStepFunctionPerVertex: w.String("MTLVertexStepFunctionPerVertex"); break;
            case MTLVertexStepFunctionPerInstance: w.String("MTLVertexStepFunctionPerInstance"); break;
            case MTLVertexStepFunctionPerPatch: w.String("MTLVertexStepFunctionPerPatch"); break;
            case MTLVertexStepFunctionPerPatchControlPoint:
                w.String("MTLVertexStepFunctionPerPatchControlPoint"); break;
            default: w.Uint((uint64_t)layout.stepFunction); break;
        }
        w.Key("stepRate"); w.Uint(layout.stepRate);
        w.EndObject();
    }
    w.EndArray();
    w.Key("attributes"); w.BeginArray();
    for (NSUInteger i = 0; i < 31; i++) {
        MTLVertexAttributeDescriptor *attribute = v.attributes[i];
        if (attribute == nil || attribute.format == MTLVertexFormatInvalid) continue;
        w.BeginObject();
        w.Key("index"); w.Uint(i);
        w.Key("format"); WriteEnum(w, VertexFormatEnumName(attribute.format), (uint64_t)attribute.format);
        w.Key("vkFormat"); w.String(VertexFormatCanonicalName(attribute.format));
        w.Key("offset"); w.Uint(attribute.offset);
        w.Key("bufferIndex"); w.Uint(attribute.bufferIndex);
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
}

void WriteStencil(vkinsp::JsonWriter &w, MTLStencilDescriptor *s) {
    if (s == nil) {
        w.Null();
        return;
    }
    w.BeginObject();
    w.Key("stencilCompareFunction"); w.Uint((uint64_t)s.stencilCompareFunction);
    w.Key("stencilFailureOperation"); w.Uint((uint64_t)s.stencilFailureOperation);
    w.Key("depthFailureOperation"); w.Uint((uint64_t)s.depthFailureOperation);
    w.Key("depthStencilPassOperation"); w.Uint((uint64_t)s.depthStencilPassOperation);
    w.Key("readMask"); w.Uint(s.readMask);
    w.Key("writeMask"); w.Uint(s.writeMask);
    w.EndObject();
}

void WritePassAttachment(vkinsp::JsonWriter &w, MTLRenderPassAttachmentDescriptor *a) {
    w.Key("texture"); WriteRef(w, a.texture, "MTLTexture");
    w.Key("level"); w.Uint(a.level);
    w.Key("slice"); w.Uint(a.slice);
    if (a.depthPlane != 0) { w.Key("depthPlane"); w.Uint(a.depthPlane); }
    w.Key("loadAction"); WriteEnum(w, LoadActionEnumName(a.loadAction), (uint64_t)a.loadAction);
    w.Key("storeAction"); WriteEnum(w, StoreActionEnumName(a.storeAction), (uint64_t)a.storeAction);
    if (a.storeActionOptions != MTLStoreActionOptionNone) {
        w.Key("storeActionOptions"); w.Uint((uint64_t)a.storeActionOptions);
    }
    if (a.resolveTexture != nil) {
        w.Key("resolveTexture"); WriteRef(w, a.resolveTexture, "MTLTexture");
        w.Key("resolveLevel"); w.Uint(a.resolveLevel);
        w.Key("resolveSlice"); w.Uint(a.resolveSlice);
    }
}

}  // namespace

void WriteGpuIds(Args &a, id object) {
    if (object == nil) return;
    char hex[32];
    const SEL address = sel_registerName("gpuAddress");
    if ([object respondsToSelector:address]) {
        const uint64_t value = ((uint64_t (*)(id, SEL))objc_msgSend)(object, address);
        snprintf(hex, sizeof(hex), "0x%llx", (unsigned long long)value);
        a.c("gpuAddress", hex);
    }
#if defined(__MAC_13_0) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_13_0
    const SEL resourceId = sel_registerName("gpuResourceID");
    if ([object respondsToSelector:resourceId]) {
        const MTLResourceID value = ((MTLResourceID (*)(id, SEL))objc_msgSend)(object, resourceId);
        snprintf(hex, sizeof(hex), "0x%llx", (unsigned long long)value._impl);
        a.c("gpuResourceID", hex);
    }
#endif
}

void WriteMemoryInfo(Args &a, id resource) {
    if (resource == nil || ![resource respondsToSelector:@selector(allocatedSize)]) return;
    id<MTLResource> r = (id<MTLResource>)resource;
    a.u("allocatedSize", r.allocatedSize);
    if (r.heap != nil) {
        a.ref("heap", r.heap, "MTLHeap");
        if (@available(macOS 10.15, *)) a.u("heapOffset", r.heapOffset);
    }
    if (r.isAliasable) a.b("aliasable", true);
}

std::string HeapUsageArgs(id heap) {
    Args a;
    id<MTLHeap> h = (id<MTLHeap>)heap;
    a.u("usedSize", h.usedSize).u("currentAllocatedSize", h.currentAllocatedSize);
    return a.str();
}

std::string BufferArgs(id buffer, NSUInteger length, MTLResourceOptions options) {
    Args a;
    a.u("length", length).u("options", (uint64_t)options);
    const MTLStorageMode storage = (MTLStorageMode)((options & MTLResourceStorageModeMask) >> MTLResourceStorageModeShift);
    a.e("storageMode", StorageModeEnumName(storage), (uint64_t)storage);
    WriteGpuIds(a, buffer);
    WriteMemoryInfo(a, buffer);
    return a.str();
}

std::string TextureDescriptorArgs(MTLTextureDescriptor *d, id texture) {
    Args a;
    a.e("textureType", TextureTypeEnumName(d.textureType), (uint64_t)d.textureType)
     .e("pixelFormat", PixelFormatEnumName(d.pixelFormat), (uint64_t)d.pixelFormat)
     .u("width", d.width).u("height", d.height).u("depth", d.depth)
     .u("mipmapLevelCount", d.mipmapLevelCount).u("sampleCount", d.sampleCount)
     .u("arrayLength", d.arrayLength)
     .c("usage", UsageFlags(d.usage).c_str())
     .e("storageMode", StorageModeEnumName(d.storageMode), (uint64_t)d.storageMode)
     .u("cpuCacheMode", (uint64_t)d.cpuCacheMode)
     .u("hazardTrackingMode", (uint64_t)d.hazardTrackingMode)
     .b("allowGPUOptimizedContents", d.allowGPUOptimizedContents);
    WriteGpuIds(a, texture);
    WriteMemoryInfo(a, texture);
    return a.str();
}

std::string TextureObjectArgs(id<MTLTexture> t) {
    Args a;
    a.e("textureType", TextureTypeEnumName(t.textureType), (uint64_t)t.textureType)
     .e("pixelFormat", PixelFormatEnumName(t.pixelFormat), (uint64_t)t.pixelFormat)
     .u("width", t.width).u("height", t.height).u("depth", t.depth)
     .u("mipmapLevelCount", t.mipmapLevelCount).u("sampleCount", t.sampleCount)
     .u("arrayLength", t.arrayLength)
     .c("usage", UsageFlags(t.usage).c_str())
     .e("storageMode", StorageModeEnumName(t.storageMode), (uint64_t)t.storageMode)
     .b("framebufferOnly", t.framebufferOnly);
    WriteGpuIds(a, t);
    WriteMemoryInfo(a, t);
    return a.str();
}

std::string TextureViewArgs(id<MTLTexture> view, MTLPixelFormat format, MTLTextureType type,
                            NSRange levels, NSRange slices) {
    Args a;
    a.b("view", true)
     .ref("parentTexture", view.parentTexture, "MTLTexture")
     .e("pixelFormat", PixelFormatEnumName(format), (uint64_t)format)
     .e("textureType", TextureTypeEnumName(type), (uint64_t)type)
     .range("levels", levels).range("slices", slices);
    if (view != nil) {
        a.u("width", view.width).u("height", view.height).u("depth", view.depth)
         .u("mipmapLevelCount", view.mipmapLevelCount).u("arrayLength", view.arrayLength)
         .c("usage", UsageFlags(view.usage).c_str());
        WriteGpuIds(a, view);
    }
    return a.str();
}

std::string RenderPipelineArgs(MTLRenderPipelineDescriptor *d, MTLRenderPipelineReflection *reflection) {
    Args a;
    a.s("label", d.label);
    WriteFunction(a, "vertexFunction", d.vertexFunction);
    WriteFunction(a, "fragmentFunction", d.fragmentFunction);
    a.writer().Key("vertexDescriptor");
    WriteVertexDescriptor(a.writer(), d.vertexDescriptor);
    a.u("rasterSampleCount", d.rasterSampleCount)
     .b("alphaToCoverageEnabled", d.alphaToCoverageEnabled)
     .b("alphaToOneEnabled", d.alphaToOneEnabled)
     .b("rasterizationEnabled", d.rasterizationEnabled)
     .u("inputPrimitiveTopology", (uint64_t)d.inputPrimitiveTopology);
    a.writer().Key("colorAttachments");
    a.writer().BeginArray();
    for (NSUInteger i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
        if (c == nil || c.pixelFormat == MTLPixelFormatInvalid) continue;
        WriteColorAttachmentBlend(a.writer(), c, i);
    }
    a.writer().EndArray();
    a.e("depthAttachmentPixelFormat", PixelFormatEnumName(d.depthAttachmentPixelFormat),
        (uint64_t)d.depthAttachmentPixelFormat)
     .e("stencilAttachmentPixelFormat", PixelFormatEnumName(d.stencilAttachmentPixelFormat),
        (uint64_t)d.stencilAttachmentPixelFormat)
     .b("supportIndirectCommandBuffers", d.supportIndirectCommandBuffers);
    if (d.tessellationPartitionMode != MTLTessellationPartitionModePow2 || d.maxTessellationFactor != 16) {
        a.u("maxTessellationFactor", d.maxTessellationFactor)
         .u("tessellationPartitionMode", (uint64_t)d.tessellationPartitionMode);
    }
    a.raw("reflection", RenderReflectionJson(reflection));
    return a.str();
}

std::string TileRenderPipelineArgs(MTLTileRenderPipelineDescriptor *d, MTLRenderPipelineReflection *reflection) {
    Args a;
    a.s("label", d.label);
    WriteFunction(a, "tileFunction", d.tileFunction);
    a.u("rasterSampleCount", d.rasterSampleCount)
     .b("threadgroupSizeMatchesTileSize", d.threadgroupSizeMatchesTileSize)
     .u("maxTotalThreadsPerThreadgroup", d.maxTotalThreadsPerThreadgroup);
    a.writer().Key("colorAttachments");
    a.writer().BeginArray();
    for (NSUInteger i = 0; i < 8; i++) {
        MTLTileRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
        if (c == nil || c.pixelFormat == MTLPixelFormatInvalid) continue;
        a.writer().BeginObject();
        a.writer().Key("index"); a.writer().Uint(i);
        a.writer().Key("pixelFormat");
        WriteEnum(a.writer(), PixelFormatEnumName(c.pixelFormat), (uint64_t)c.pixelFormat);
        a.writer().EndObject();
    }
    a.writer().EndArray();
    a.raw("reflection", RenderReflectionJson(reflection));
    return a.str();
}

std::string MeshRenderPipelineArgs(id d, MTLRenderPipelineReflection *reflection) {
    // MTLMeshRenderPipelineDescriptor needs the macOS 13 SDK; read through selectors so the file
    // builds against an older one and still describes the object on a newer system.
    Args a;
    NSString *label = [d respondsToSelector:@selector(label)] ? [d performSelector:@selector(label)] : nil;
    a.s("label", label);
    WriteFunction(a, "objectFunction", FunctionOf(d, sel_registerName("objectFunction")));
    WriteFunction(a, "meshFunction", FunctionOf(d, sel_registerName("meshFunction")));
    WriteFunction(a, "fragmentFunction", FunctionOf(d, sel_registerName("fragmentFunction")));
    const SEL rasterSampleCount = sel_registerName("rasterSampleCount");
    if ([d respondsToSelector:rasterSampleCount]) {
        a.u("rasterSampleCount", ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rasterSampleCount));
    }
    a.raw("reflection", RenderReflectionJson(reflection));
    return a.str();
}

namespace {
void WriteComputeState(Args &a, id<MTLComputePipelineState> state) {
    if (state == nil) return;
    a.u("maxTotalThreadsPerThreadgroup", state.maxTotalThreadsPerThreadgroup)
     .u("threadExecutionWidth", state.threadExecutionWidth)
     .u("staticThreadgroupMemoryLength", state.staticThreadgroupMemoryLength);
}
}  // namespace

std::string ComputePipelineFunctionArgs(id<MTLFunction> function, id<MTLComputePipelineState> state,
                                        MTLComputePipelineReflection *reflection) {
    Args a;
    WriteFunction(a, "function", function);
    WriteComputeState(a, state);
    a.raw("reflection", ComputeReflectionJson(reflection));
    return a.str();
}

std::string ComputePipelineDescriptorArgs(MTLComputePipelineDescriptor *d,
                                          id<MTLComputePipelineState> state,
                                          MTLComputePipelineReflection *reflection) {
    Args a;
    a.s("label", d.label);
    WriteFunction(a, "function", d.computeFunction);
    a.b("threadGroupSizeIsMultipleOfThreadExecutionWidth", d.threadGroupSizeIsMultipleOfThreadExecutionWidth)
     .u("maxTotalThreadsPerThreadgroupRequested", d.maxTotalThreadsPerThreadgroup)
     .b("supportIndirectCommandBuffers", d.supportIndirectCommandBuffers);
    WriteComputeState(a, state);
    a.raw("reflection", ComputeReflectionJson(reflection));
    return a.str();
}

/**
 * A library's descriptor: how it was made and what is in it.
 *
 * `functionNames` is the useful part and is always available, whether the library was compiled
 * from source here or loaded precompiled — it is the only way to see what a shipped metallib
 * contains without a disassembler.
 */
std::string LibraryArgs(id<MTLLibrary> library, const char *origin, uint64_t sourceLength) {
    Args a;
    a.c("origin", origin);
    if (sourceLength != 0) a.u("sourceLength", sourceLength);
    a.writer().Key("functionNames");
    a.writer().BeginArray();
    if (library != nil) {
        for (NSString *name in library.functionNames) a.writer().String(name.UTF8String);
    }
    a.writer().EndArray();
    return a.str();
}

std::string FunctionArgs(id<MTLFunction> function) {
    Args a;
    if (function == nil) return a.str();
    a.s("name", function.name);
    switch (function.functionType) {
        case MTLFunctionTypeVertex: a.c("functionType", "vertex"); break;
        case MTLFunctionTypeFragment: a.c("functionType", "fragment"); break;
        case MTLFunctionTypeKernel: a.c("functionType", "kernel"); break;
        default: a.u("functionType", (uint64_t)function.functionType); break;
    }
    a.u("patchType", (uint64_t)function.patchType);
    return a.str();
}

std::string SamplerArgs(MTLSamplerDescriptor *d, id sampler) {
    Args a;
    a.s("label", d.label)
     .u("minFilter", (uint64_t)d.minFilter).u("magFilter", (uint64_t)d.magFilter)
     .u("mipFilter", (uint64_t)d.mipFilter)
     .u("maxAnisotropy", d.maxAnisotropy)
     .u("sAddressMode", (uint64_t)d.sAddressMode).u("tAddressMode", (uint64_t)d.tAddressMode)
     .u("rAddressMode", (uint64_t)d.rAddressMode)
     .u("borderColor", (uint64_t)d.borderColor)
     .b("normalizedCoordinates", d.normalizedCoordinates)
     .d("lodMinClamp", d.lodMinClamp).d("lodMaxClamp", d.lodMaxClamp)
     .u("compareFunction", (uint64_t)d.compareFunction)
     .b("supportArgumentBuffers", d.supportArgumentBuffers);
    WriteGpuIds(a, sampler);
    return a.str();
}

std::string DepthStencilArgs(MTLDepthStencilDescriptor *d) {
    Args a;
    a.s("label", d.label)
     .u("depthCompareFunction", (uint64_t)d.depthCompareFunction)
     .b("depthWriteEnabled", d.depthWriteEnabled);
    a.writer().Key("frontFaceStencil");
    WriteStencil(a.writer(), d.frontFaceStencil);
    a.writer().Key("backFaceStencil");
    WriteStencil(a.writer(), d.backFaceStencil);
    return a.str();
}

std::string HeapArgs(MTLHeapDescriptor *d, id heap) {
    Args a;
    a.u("size", d.size)
     .e("storageMode", StorageModeEnumName(d.storageMode), (uint64_t)d.storageMode)
     .u("cpuCacheMode", (uint64_t)d.cpuCacheMode)
     .u("hazardTrackingMode", (uint64_t)d.hazardTrackingMode)
     .u("type", (uint64_t)d.type);
    if (heap != nil) {
        // The descriptor's size is a request; the heap's own is what was set aside.
        id<MTLHeap> h = (id<MTLHeap>)heap;
        a.u("allocatedSize", h.size).u("usedSize", h.usedSize).u("currentAllocatedSize", h.currentAllocatedSize);
    }
    return a.str();
}

std::string RenderPassArgs(MTLRenderPassDescriptor *d) {
    Args a;
    vkinsp::JsonWriter &w = a.writer();
    w.Key("colorAttachments"); w.BeginArray();
    for (NSUInteger i = 0; i < 8; i++) {
        MTLRenderPassColorAttachmentDescriptor *c = d.colorAttachments[i];
        if (c == nil || c.texture == nil) continue;
        w.BeginObject();
        w.Key("index"); w.Uint(i);
        WritePassAttachment(w, c);
        if (c.loadAction == MTLLoadActionClear) {
            const MTLClearColor color = c.clearColor;
            w.Key("clearColor"); w.BeginArray();
            w.Double(color.red); w.Double(color.green); w.Double(color.blue); w.Double(color.alpha);
            w.EndArray();
        }
        w.EndObject();
    }
    w.EndArray();
    w.Key("depthAttachment");
    if (d.depthAttachment.texture == nil) {
        w.Null();
    } else {
        w.BeginObject();
        WritePassAttachment(w, d.depthAttachment);
        w.Key("clearDepth"); w.Double(d.depthAttachment.clearDepth);
        w.EndObject();
    }
    w.Key("stencilAttachment");
    if (d.stencilAttachment.texture == nil) {
        w.Null();
    } else {
        w.BeginObject();
        WritePassAttachment(w, d.stencilAttachment);
        w.Key("clearStencil"); w.Uint(d.stencilAttachment.clearStencil);
        w.EndObject();
    }
    if (d.visibilityResultBuffer != nil) {
        a.ref("visibilityResultBuffer", d.visibilityResultBuffer, "MTLBuffer");
    }
    if (d.renderTargetArrayLength != 0) a.u("renderTargetArrayLength", d.renderTargetArrayLength);
    if (d.renderTargetWidth != 0 || d.renderTargetHeight != 0) {
        a.u("renderTargetWidth", d.renderTargetWidth).u("renderTargetHeight", d.renderTargetHeight);
    }
    if (d.defaultRasterSampleCount != 0) a.u("defaultRasterSampleCount", d.defaultRasterSampleCount);
    if (d.imageblockSampleLength != 0) a.u("imageblockSampleLength", d.imageblockSampleLength);
    if (d.threadgroupMemoryLength != 0) a.u("threadgroupMemoryLength", d.threadgroupMemoryLength);
    if (d.tileWidth != 0 || d.tileHeight != 0) {
        a.u("tileWidth", d.tileWidth).u("tileHeight", d.tileHeight);
    }
    return a.str();
}

std::string DeviceArgs(id<MTLDevice> device, const char *origin) {
    Args a;
    a.s("name", device.name)
     .c("origin", origin)
     .u("registryID", device.registryID)
     .b("hasUnifiedMemory", device.hasUnifiedMemory)
     .b("lowPower", device.lowPower)
     .b("headless", device.headless)
     .b("removable", device.removable)
     .u("recommendedMaxWorkingSetSize", device.recommendedMaxWorkingSetSize)
     .u("maxBufferLength", device.maxBufferLength)
     .u("maxThreadgroupMemoryLength", device.maxThreadgroupMemoryLength)
     .size("maxThreadsPerThreadgroup", device.maxThreadsPerThreadgroup)
     .u("argumentBuffersSupport", (uint64_t)device.argumentBuffersSupport);
    if (@available(macOS 11.0, *)) {
        a.b("supportsRaytracing", device.supportsRaytracing)
         .b("supportsFunctionPointers", device.supportsFunctionPointers);
    }
    return a.str();
}

}  // namespace mtlinsp
