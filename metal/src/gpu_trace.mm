#include "gpu_trace.h"

#include "frame_stats.h"
#include "json_writer.h"
#include "swizzle.h"
#include "transport.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <mutex>

namespace mtlinsp {
namespace {

std::mutex g_mutex;
std::string g_pendingPath;   // a request waiting for the next boundary
bool g_pending = false;
bool g_active = false;
std::string g_activePath;
uint64_t g_activeFrame = 0;

void SendResult(bool ok, const std::string &path, const std::string &error, uint64_t frame) {
    vkinsp::JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("GpuTrace");
    w.Key("ok"); w.Boolean(ok);
    w.Key("path"); w.String(path);
    w.Key("frame"); w.Uint(frame);
    if (!error.empty()) { w.Key("error"); w.String(error); }
    w.EndObject();
    Transport::Get().SendJson(std::move(w.str()));
    if (ok) Log("gpu trace: frame %llu written to %s", (unsigned long long)frame, path.c_str());
    else Log("gpu trace: %s", error.c_str());
}

std::string DefaultPath(uint64_t frame) {
    NSString *name = NSProcessInfo.processInfo.processName;
    NSString *desktop = [NSHomeDirectory() stringByAppendingPathComponent:@"Desktop"];
    NSString *file = [NSString stringWithFormat:@"%@-frame%llu.gputrace", name, (unsigned long long)frame];
    return std::string([desktop stringByAppendingPathComponent:file].UTF8String);
}

}  // namespace

void RequestGpuTrace(const std::string &path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pendingPath = path;
    g_pending = true;
    Log("gpu trace requested%s%s", path.empty() ? "" : ": ", path.c_str());
}

void GpuTraceAtFrameBoundary(id device) {
    bool start = false, stop = false;
    std::string path;
    uint64_t frame = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_active) {
            stop = true;
            path = g_activePath;
            frame = g_activeFrame;
            g_active = false;
        } else if (g_pending) {
            start = true;
            g_pending = false;
            path = g_pendingPath;
        }
    }
    if (!start && !stop) return;
    @autoreleasepool {
        Internal internal;
        MTLCaptureManager *manager = [MTLCaptureManager sharedCaptureManager];
        if (stop) {
            [manager stopCapture];
            SendResult(true, path, "", frame);
            return;
        }
        frame = FrameNumber() + 1;
        if (path.empty()) path = DefaultPath(frame);
        if (@available(macOS 10.15, *)) {
            if (![manager supportsDestination:MTLCaptureDestinationGPUTraceDocument]) {
                SendResult(false, path,
                           "GPU trace documents are not enabled for this process: it has to be started "
                           "with METAL_CAPTURE_ENABLED=1 (the inspector's launch does that)", frame);
                return;
            }
            if (manager.isCapturing) {
                SendResult(false, path, "a Metal capture is already in progress", frame);
                return;
            }
            MTLCaptureDescriptor *descriptor = [[[MTLCaptureDescriptor alloc] init] autorelease];
            descriptor.captureObject = device;
            descriptor.destination = MTLCaptureDestinationGPUTraceDocument;
            descriptor.outputURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
            NSError *error = nil;
            if (![manager startCaptureWithDescriptor:descriptor error:&error]) {
                SendResult(false, path, error == nil ? "startCapture failed" : error.localizedDescription.UTF8String, frame);
                return;
            }
            std::lock_guard<std::mutex> lock(g_mutex);
            g_active = true;
            g_activePath = path;
            g_activeFrame = frame;
            Log("gpu trace: capturing frame %llu", (unsigned long long)frame);
        } else {
            SendResult(false, path, "GPU trace documents need macOS 10.15", frame);
        }
    }
}

}  // namespace mtlinsp
