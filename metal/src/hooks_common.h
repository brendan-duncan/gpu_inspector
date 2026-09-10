// Shared by the hook sources: the JSON argument builder, the reference writer, the descriptor
// serializers, and the per-frame counters the log line reports.
//
// The hooks are split by the class they intercept — hooks_device.mm for what the device and its
// resources create, hooks_command_buffer.mm for queues and command buffers, hooks_encoders.mm for
// the encoders — because there are some two hundred of them. Every one has the same shape: guard
// against re-entry, record the call if this is the application's own rather than one of Metal's
// wrappers calling the next (see Reentry in swizzle.h), forward to the implementation that was
// replaced, and hook the class of any Metal object that came back so the next level down is
// covered too. Nothing is wrapped and nothing is withheld from the application: the forward
// happens on every path.
//
// Compiled without ARC on purpose. The hooks forward to the original implementation through a raw
// IMP, and the `new*` methods return an object the caller owns (+1); handing that straight back
// is only obviously correct when the compiler is not also inserting retains and releases.
#pragma once

#include "capture.h"
#include "formats.h"
#include "json_writer.h"
#include "swizzle.h"
#include "tracker.h"

#import <Metal/Metal.h>

#include <atomic>
#include <string>

/** The original implementation of the hooked method, as a function pointer of the given type. */
#define ORIG(...) ((__VA_ARGS__)reentry.original())

namespace mtlinsp {

extern std::atomic<uint64_t> g_frame;
extern std::atomic<uint32_t> g_drawsThisFrame;
extern std::atomic<uint32_t> g_dispatchesThisFrame;
extern std::atomic<uint32_t> g_encodersThisFrame;

/** Whether this hook invocation is the application's call and a capture is recording. */
inline bool Rec(const Reentry &reentry) { return reentry.outermost() && Recording(); }

const char *LabelOf(id object);

/** A tracked object as the UI's `{"__id", "__class"}` reference, or null. */
void WriteRef(vkinsp::JsonWriter &w, id object, const char *type);

/** An enum as its Metal name, falling back to the raw value for one the tables do not know. */
void WriteEnum(vkinsp::JsonWriter &w, const char *name, uint64_t value);

/**
 * A command's arguments, built in the order the hook receives them. Every hook that records
 * builds one and hands `str()` to RecordCommand; the writer is exposed for the few with nested
 * structure.
 */
class Args {
public:
    Args() { w_.BeginObject(); }
    Args &u(const char *key, uint64_t value) { w_.Key(key); w_.Uint(value); return *this; }
    Args &i(const char *key, int64_t value) { w_.Key(key); w_.Int(value); return *this; }
    Args &d(const char *key, double value) { w_.Key(key); w_.Double(value); return *this; }
    Args &b(const char *key, bool value) { w_.Key(key); w_.Boolean(value); return *this; }
    Args &s(const char *key, NSString *value) {
        w_.Key(key);
        if (value == nil) w_.Null(); else w_.String(value.UTF8String);
        return *this;
    }
    Args &c(const char *key, const char *value) {
        w_.Key(key);
        if (value == nullptr) w_.Null(); else w_.String(value);
        return *this;
    }
    Args &e(const char *key, const char *name, uint64_t value) {
        w_.Key(key);
        WriteEnum(w_, name, value);
        return *this;
    }
    Args &ref(const char *key, id object, const char *type) {
        w_.Key(key);
        WriteRef(w_, object, type);
        return *this;
    }
    Args &refs(const char *key, const id *objects, NSUInteger count, const char *type) {
        w_.Key(key);
        w_.BeginArray();
        for (NSUInteger n = 0; objects != nullptr && n < count; n++) WriteRef(w_, objects[n], type);
        w_.EndArray();
        return *this;
    }
    Args &uints(const char *key, const NSUInteger *values, NSUInteger count) {
        w_.Key(key);
        w_.BeginArray();
        for (NSUInteger n = 0; values != nullptr && n < count; n++) w_.Uint(values[n]);
        w_.EndArray();
        return *this;
    }
    Args &size(const char *key, MTLSize v) {
        w_.Key(key);
        w_.BeginObject();
        w_.Key("width"); w_.Uint(v.width);
        w_.Key("height"); w_.Uint(v.height);
        w_.Key("depth"); w_.Uint(v.depth);
        w_.EndObject();
        return *this;
    }
    Args &origin(const char *key, MTLOrigin v) {
        w_.Key(key);
        w_.BeginObject();
        w_.Key("x"); w_.Uint(v.x);
        w_.Key("y"); w_.Uint(v.y);
        w_.Key("z"); w_.Uint(v.z);
        w_.EndObject();
        return *this;
    }
    Args &region(const char *key, MTLRegion v) {
        w_.Key(key);
        w_.BeginObject();
        w_.Key("origin");
        w_.BeginObject();
        w_.Key("x"); w_.Uint(v.origin.x);
        w_.Key("y"); w_.Uint(v.origin.y);
        w_.Key("z"); w_.Uint(v.origin.z);
        w_.EndObject();
        w_.Key("size");
        w_.BeginObject();
        w_.Key("width"); w_.Uint(v.size.width);
        w_.Key("height"); w_.Uint(v.size.height);
        w_.Key("depth"); w_.Uint(v.size.depth);
        w_.EndObject();
        w_.EndObject();
        return *this;
    }
    Args &range(const char *key, NSRange v) {
        w_.Key(key);
        w_.BeginObject();
        w_.Key("location"); w_.Uint(v.location);
        w_.Key("length"); w_.Uint(v.length);
        w_.EndObject();
        return *this;
    }
    Args &viewport(const char *key, const MTLViewport &v) {
        w_.Key(key);
        w_.BeginObject();
        w_.Key("originX"); w_.Double(v.originX);
        w_.Key("originY"); w_.Double(v.originY);
        w_.Key("width"); w_.Double(v.width);
        w_.Key("height"); w_.Double(v.height);
        w_.Key("znear"); w_.Double(v.znear);
        w_.Key("zfar"); w_.Double(v.zfar);
        w_.EndObject();
        return *this;
    }
    Args &scissor(const char *key, const MTLScissorRect &v) {
        w_.Key(key);
        w_.BeginObject();
        w_.Key("x"); w_.Uint(v.x);
        w_.Key("y"); w_.Uint(v.y);
        w_.Key("width"); w_.Uint(v.width);
        w_.Key("height"); w_.Uint(v.height);
        w_.EndObject();
        return *this;
    }
    Args &bytes(const char *key, const void *data, size_t length) {
        w_.Key(key);
        w_.Bytes(data, length);
        return *this;
    }
    Args &raw(const char *key, const std::string &json) {
        w_.Key(key);
        if (json.empty()) w_.Null(); else w_.Raw(json);
        return *this;
    }
    vkinsp::JsonWriter &writer() { return w_; }
    /** Closes the object. The Args is spent afterwards. */
    std::string str() {
        w_.EndObject();
        return std::move(w_.str());
    }

private:
    vkinsp::JsonWriter w_;
};

// The "args" of an AddObject: the descriptor the object was created from, in the same shape the
// Vulkan layer serializes a VkCreateInfo into. Written by hand, one per creating call — there is
// no vk.xml for Metal to generate them from, which is the main cost of this backend.
// (hooks_descriptors.mm)
std::string BufferArgs(id buffer, NSUInteger length, MTLResourceOptions options);
std::string TextureDescriptorArgs(MTLTextureDescriptor *descriptor, id texture);
std::string TextureObjectArgs(id<MTLTexture> texture);
/**
 * A buffer's GPU address and a texture's or sampler's GPU resource id, as hex strings: what an
 * argument buffer holds for them, so the UI can match its bytes back to objects.
 */
void WriteGpuIds(Args &a, id object);
/**
 * What a resource occupies: `allocatedSize` (what Metal set aside, alignment and padding
 * included), the heap it was sub-allocated from with its `heapOffset`, and `aliasable`. The
 * memory meter in Inspect sums the sizes; a texture view carries none, its storage is its
 * parent's.
 */
void WriteMemoryInfo(Args &a, id resource);
std::string TextureViewArgs(id<MTLTexture> view, MTLPixelFormat format, MTLTextureType type,
                            NSRange levels, NSRange slices);
// A pipeline's descriptor carries the reflection the hooks asked for with it (reflection.h),
// which is what makes a captured buffer readable as fields rather than bytes.
std::string RenderPipelineArgs(MTLRenderPipelineDescriptor *descriptor,
                               MTLRenderPipelineReflection *reflection);
std::string TileRenderPipelineArgs(MTLTileRenderPipelineDescriptor *descriptor,
                                   MTLRenderPipelineReflection *reflection);
std::string MeshRenderPipelineArgs(id descriptor, MTLRenderPipelineReflection *reflection);
std::string ComputePipelineFunctionArgs(id<MTLFunction> function, id<MTLComputePipelineState> state,
                                        MTLComputePipelineReflection *reflection);
std::string ComputePipelineDescriptorArgs(MTLComputePipelineDescriptor *descriptor,
                                          id<MTLComputePipelineState> state,
                                          MTLComputePipelineReflection *reflection);

/**
 * The pipeline options every creation asks for: argument info and buffer type info, so the
 * pipeline comes back with reflection. Spelled numerically because the first was renamed
 * (MTLPipelineOptionArgumentInfo became MTLPipelineOptionBindingInfo in macOS 14) without
 * changing value.
 */
constexpr MTLPipelineOption kReflectionOptions = (MTLPipelineOption)((1 << 0) | (1 << 1));
std::string LibraryArgs(id<MTLLibrary> library, const char *origin, uint64_t sourceLength);
std::string FunctionArgs(id<MTLFunction> function);
std::string SamplerArgs(MTLSamplerDescriptor *descriptor, id sampler);
std::string DepthStencilArgs(MTLDepthStencilDescriptor *descriptor);
std::string HeapArgs(MTLHeapDescriptor *descriptor, id heap);
/** A heap's `usedSize` and `currentAllocatedSize`, sent as an update after each sub-allocation. */
std::string HeapUsageArgs(id heap);
std::string RenderPassArgs(MTLRenderPassDescriptor *descriptor);
std::string DeviceArgs(id<MTLDevice> device, const char *origin);

/**
 * Registers an object and hooks `setLabel:` on its class, so later labelling is streamed.
 * Returns the id, 0 for nil or for an object the library made for itself.
 */
uint64_t Track(id object, const char *type, const char *cmd, id parent, const std::string &args);

/** The one `setLabel:` replacement, for tracked objects and for encoders and command buffers. */
void Replaced_setLabel(id self, SEL _cmd, NSString *label);

/** endEncoding, the debug groups and setLabel:, which every encoder class has. */
void HookCommonEncoderMethods(Class cls);

}  // namespace mtlinsp
