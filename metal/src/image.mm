#include "image.h"

#include "capture.h"
#include "formats.h"
#include "json_writer.h"
#include "swizzle.h"
#include "tracker.h"
#include "transport.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <mutex>
#include <string>

namespace mtlinsp {
namespace {

// One queue of our own, made on first use. The application's queues are its own to schedule on,
// and a read-back submitted to one would sit behind whatever it has already queued.
std::mutex g_queueMutex;
id<MTLCommandQueue> g_readbackQueue = nil;
id<MTLDevice> g_readbackDevice = nil;

id<MTLCommandQueue> ReadbackQueue(id<MTLDevice> device) {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    if (g_readbackQueue != nil && g_readbackDevice == device) return g_readbackQueue;
    [g_readbackQueue release];
    g_readbackDevice = device;
    g_readbackQueue = [device newCommandQueue];
    g_readbackQueue.label = @"gpu-inspector readback";
    return g_readbackQueue;
}

void SendError(uint64_t objectId, uint32_t mip, uint32_t layer, const std::string &error) {
    vkinsp::JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("ImageData");
    w.Key("id"); w.Uint(objectId);
    w.Key("mip"); w.Uint(mip);
    w.Key("layer"); w.Uint(layer);
    w.Key("format"); w.String("");
    w.Key("aspect"); w.String("color");
    w.Key("width"); w.Uint(0);
    w.Key("height"); w.Uint(0);
    w.Key("depth"); w.Uint(0);
    w.Key("layers"); w.Uint(0);
    w.Key("size"); w.Uint(0);
    w.Key("error"); w.String(error);
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
    Log("image %llu: %s", (unsigned long long)objectId, error.c_str());
}

}  // namespace

void SendImageData(uint64_t objectId, uint32_t mip, uint32_t layer) {
    // Nothing this does should end up in a capture that happens to be recording.
    Internal internal;

    id object = LiveObject(objectId);
    if (object == nil) {
        SendError(objectId, mip, layer, "the application has released this texture");
        return;
    }
    if (![object conformsToProtocol:@protocol(MTLTexture)]) {
        SendError(objectId, mip, layer, "not a texture");
        return;
    }
    id<MTLTexture> texture = (id<MTLTexture>)object;

    const PixelFormatInfo info = PixelFormatDetails(texture.pixelFormat);
    if (info.name[0] == '\0') {
        const char *enumName = PixelFormatEnumName(texture.pixelFormat);
        SendError(objectId, mip, layer, std::string("cannot read back ")
                  + (enumName[0] != '\0' ? enumName : std::to_string((int)texture.pixelFormat)));
        return;
    }
    if (mip >= texture.mipmapLevelCount) {
        SendError(objectId, mip, layer, "no such mip level");
        return;
    }

    // Mip dimensions, floored at one the way the API defines them.
    const uint32_t width = (uint32_t)std::max<NSUInteger>(1, texture.width >> mip);
    const uint32_t height = (uint32_t)std::max<NSUInteger>(1, texture.height >> mip);
    uint64_t rowBytes = 0;
    const uint64_t size = PixelFormatImageSize(info, width, height, &rowBytes);
    const NSUInteger bytesPerRow = (NSUInteger)rowBytes;

    id<MTLDevice> device = texture.device;
    id<MTLBuffer> staging = [device newBufferWithLength:size options:MTLResourceStorageModeShared];
    if (staging == nil) {
        SendError(objectId, mip, layer, "could not allocate a staging buffer");
        return;
    }

    // A blit rather than -getBytes:, which only works for a texture the CPU can already see: a
    // render target or anything else in private storage — most of what is worth looking at — has
    // no mapped contents at all.
    id<MTLCommandBuffer> commandBuffer = [ReadbackQueue(device) commandBuffer];
    commandBuffer.label = @"gpu-inspector image read-back";
    id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
    [blit copyFromTexture:texture
              sourceSlice:layer
              sourceLevel:mip
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(width, height, 1)
                 toBuffer:staging
        destinationOffset:0
   destinationBytesPerRow:bytesPerRow
 destinationBytesPerImage:size];
    [blit endEncoding];
    [commandBuffer commit];
    // On the transport's receiver thread, so blocking here delays only further UI messages.
    [commandBuffer waitUntilCompleted];

    if (commandBuffer.error != nil) {
        SendError(objectId, mip, layer, commandBuffer.error.localizedDescription.UTF8String);
        [staging release];
        return;
    }

    vkinsp::JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("ImageData");
    w.Key("id"); w.Uint(objectId);
    w.Key("mip"); w.Uint(mip);
    w.Key("layer"); w.Uint(layer);
    w.Key("format"); w.String(info.name);
    w.Key("aspect"); w.String("color");
    w.Key("width"); w.Uint(width);
    w.Key("height"); w.Uint(height);
    w.Key("depth"); w.Uint(1);
    w.Key("layers"); w.Uint(texture.arrayLength);
    w.Key("size"); w.Uint(size);
    w.EndObject();
    Transport::Get().SendBinary(std::move(w.str()), staging.contents, size);
    Log("image %llu: %ux%u mip %u layer %u, %zu bytes", (unsigned long long)objectId, width,
        height, mip, layer, (size_t)size);
    [staging release];
}

}  // namespace mtlinsp
