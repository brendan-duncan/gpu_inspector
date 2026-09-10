#include "validation.h"

#include "frame_stats.h"
#include "json_writer.h"
#include "swizzle.h"
#include "tracker.h"
#include "transport.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdarg>
#include <cstdio>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace mtlinsp {
namespace {

struct Entry {
    uint64_t key = 0;
    std::string severity;
    std::string type;
    std::string idName;
    int64_t idNumber = 0;
    std::string message;
    uint64_t frame = 0;
    uint32_t count = 0;
    // The object concerned, as it was at the first report.
    uint64_t objectId = 0;
    std::string objectClass;
    std::string objectHandle;
    std::string objectName;
    bool dirty = false;
};

// Matches the Vulkan layer: the first two thousand distinct messages are kept, the rest counted.
constexpr size_t kMaxEntries = 2000;

std::mutex g_mutex;
std::vector<Entry> g_entries;
std::unordered_map<uint64_t, size_t> g_index;   // key -> index in g_entries
uint64_t g_dropped = 0;
bool g_droppedDirty = false;
bool g_anyDirty = false;

std::string MessageJson(const Entry &e) {
    vkinsp::JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("ValidationMessage");
    w.Key("key"); w.Uint(e.key);
    w.Key("severity"); w.String(e.severity);
    w.Key("types"); w.BeginArray(); w.String(e.type); w.EndArray();
    w.Key("idName"); w.String(e.idName);
    w.Key("idNumber"); w.Int(e.idNumber);
    w.Key("message"); w.String(e.message);
    w.Key("frame"); w.Uint(e.frame);
    w.Key("count"); w.Uint(e.count);
    w.Key("objects"); w.BeginArray();
    if (!e.objectClass.empty()) {
        w.BeginObject();
        w.Key("object");
        w.BeginObject();
        if (e.objectId != 0) { w.Key("__id"); w.Uint(e.objectId); }
        else { w.Key("__handle"); w.String(e.objectHandle); }
        w.Key("__class"); w.String(e.objectClass);
        w.EndObject();
        w.Key("class"); w.String(e.objectClass);
        w.Key("handle"); w.String(e.objectHandle);
        if (!e.objectName.empty()) { w.Key("name"); w.String(e.objectName); }
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    return std::move(w.str());
}

/** Under g_mutex. */
std::string CountsJson() {
    vkinsp::JsonWriter w;
    w.BeginObject();
    w.Key("action"); w.String("ValidationCount");
    w.Key("counts"); w.BeginArray();
    for (Entry &e : g_entries) {
        if (!e.dirty) continue;
        w.BeginArray(); w.Uint(e.key); w.Uint(e.count); w.EndArray();
        e.dirty = false;
    }
    w.EndArray();
    if (g_droppedDirty) {
        w.Key("dropped"); w.Uint(g_dropped);
        g_droppedDirty = false;
    }
    w.EndObject();
    g_anyDirty = false;
    return std::move(w.str());
}

std::string Utf8(NSString *s) {
    return s == nil ? std::string() : std::string(s.UTF8String);
}

/**
 * A command buffer completed: its error, with the encoder that faulted when the buffer was made
 * with encoder execution status, and whatever its shaders logged.
 */
void ReportCommandBufferCompletion(id<MTLCommandBuffer> cb) {
    NSError *error = cb.error;
    if (error != nil) {
        std::string message = Utf8(error.localizedDescription);
        if (@available(macOS 11.0, *)) {
            NSArray *infos = error.userInfo[MTLCommandBufferEncoderInfoErrorKey];
            for (id<MTLCommandBufferEncoderInfo> info in infos) {
                const char *state = nullptr;
                switch (info.errorState) {
                    case MTLCommandEncoderErrorStateFaulted: state = "faulted"; break;
                    case MTLCommandEncoderErrorStateAffected: state = "affected"; break;
                    case MTLCommandEncoderErrorStatePending: state = "pending"; break;
                    default: break;
                }
                if (state == nullptr) continue;
                message += "\n  encoder \"" + Utf8(info.label) + "\": " + state;
                for (NSString *signpost in info.debugSignposts) message += "\n    after signpost \"" + Utf8(signpost) + "\"";
            }
        }
        ReportValidation("error", "general", Utf8(error.domain), (int64_t)error.code, message, cb,
                         "MTLCommandBuffer");
    }
    if (@available(macOS 11.0, *)) {
        id<MTLLogContainer> logs = cb.logs;
        if (logs != nil) {
            for (id<MTLFunctionLog> log in logs) {
                std::string message = Utf8([log description]);
                if (log.encoderLabel != nil) message = "encoder \"" + Utf8(log.encoderLabel) + "\": " + message;
                ReportValidation("info", "general", "MTLFunctionLog", (int64_t)log.type, message, cb,
                                 "MTLCommandBuffer");
            }
        }
    }
}

// --------------------------------------------------------------------------------------------
// Metal's validation layer logs through NSLog when told to (MTL_DEBUG_LAYER_ERROR_MODE=nslog).
// NSLog and NSLogv are interposed for every image but this one, so Metal's calls arrive here
// and the application's own pass through untouched. A line is taken for the layer's when it
// names one of its classes or its assertion form.

bool LooksLikeMetalValidation(NSString *text) {
    static NSString *const kMarks[] = {
        @"MTLDebug", @"MTLGPUDebug", @"Metal API Validation", @"failed assertion",
        @"Shader validation", @"MTLShaderValidation",
    };
    for (NSString *mark in kMarks) {
        if ([text rangeOfString:mark].location != NSNotFound) return true;
    }
    return false;
}

void ReportDebugLayerLine(NSString *text) {
    if (text == nil || !LooksLikeMetalValidation(text)) return;
    const bool warning = [text rangeOfString:@"warning" options:NSCaseInsensitiveSearch].location != NSNotFound;
    // "-[MTLDebugRenderCommandEncoder setVertexBuffer:offset:atIndex:]:2118: failed assertion
    // '...'": the method is the message's id, the way a VUID is for Vulkan.
    std::string idName = "Metal API Validation";
    NSRange open = [text rangeOfString:@"-["];
    if (open.location != NSNotFound) {
        NSRange close = [text rangeOfString:@"]" options:0 range:NSMakeRange(open.location, text.length - open.location)];
        if (close.location != NSNotFound) idName = Utf8([text substringWithRange:NSMakeRange(open.location, close.location - open.location + 1)]);
    }
    ReportValidation(warning ? "warning" : "error", "validation", idName, 0, Utf8(text), nil, nullptr);
}

void Interposed_NSLogv(NSString *format, va_list args) {
    va_list copy;
    va_copy(copy, args);
    NSString *text = [[NSString alloc] initWithFormat:format arguments:copy];
    va_end(copy);
    NSLogv(format, args);
    ReportDebugLayerLine(text);
    [text release];
}

void Interposed_NSLog(NSString *format, ...) {
    va_list args;
    va_start(args, format);
    Interposed_NSLogv(format, args);
    va_end(args);
}

struct Interpose {
    const void *replacement;
    const void *replacee;
};

__attribute__((used, section("__DATA,__interpose"))) const Interpose kLogInterposers[] = {
    {(const void *)&Interposed_NSLog, (const void *)&NSLog},
    {(const void *)&Interposed_NSLogv, (const void *)&NSLogv},
};

}  // namespace

void ReportValidation(const char *severity, const char *type, const std::string &idName,
                      int64_t idNumber, const std::string &message, id object,
                      const char *objectClass) {
    const uint64_t key = std::hash<std::string>()(message + "\n" + idName);
    std::string toSend;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_index.find(key);
        if (it != g_index.end()) {
            Entry &e = g_entries[it->second];
            e.count++;
            e.dirty = true;
            g_anyDirty = true;
            return;
        }
        if (g_entries.size() >= kMaxEntries) {
            g_dropped++;
            g_droppedDirty = true;
            g_anyDirty = true;
            return;
        }
        Entry e;
        e.key = key;
        e.severity = severity;
        e.type = type;
        e.idName = idName;
        e.idNumber = idNumber;
        e.message = message;
        e.frame = FrameNumber();
        e.count = 1;
        if (object != nil && objectClass != nullptr) {
            e.objectId = IdOf(object);
            e.objectClass = objectClass;
            char buf[32];
            snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)(uintptr_t)object);
            e.objectHandle = buf;
            if ([object respondsToSelector:@selector(label)]) {
                NSString *label = [object performSelector:@selector(label)];
                e.objectName = Utf8(label);
            }
        }
        g_index[key] = g_entries.size();
        g_entries.push_back(std::move(e));
        Log("validation %s: %s", severity, message.c_str());
        if (Transport::Get().Connected()) toSend = MessageJson(g_entries.back());
    }
    if (!toSend.empty()) Transport::Get().SendJson(std::move(toSend));
}

void WatchCommandBuffer(id commandBuffer) {
    if (commandBuffer == nil || !Transport::Get().Connected()) return;
    Internal internal;
    [(id<MTLCommandBuffer>)commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
        @autoreleasepool {
            ReportCommandBufferCompletion(done);
        }
    }];
}

void FlushValidation() {
    if (!g_anyDirty || !Transport::Get().Connected()) return;
    std::string counts;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_anyDirty) return;
        counts = CountsJson();
    }
    Transport::Get().SendJson(std::move(counts));
}

void SendValidationSnapshot() {
    std::vector<std::string> messages;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        messages.reserve(g_entries.size() + 1);
        for (Entry &e : g_entries) {
            messages.push_back(MessageJson(e));
            e.dirty = false;
        }
        if (g_dropped != 0) {
            vkinsp::JsonWriter w;
            w.BeginObject();
            w.Key("action"); w.String("ValidationCount");
            w.Key("counts"); w.BeginArray(); w.EndArray();
            w.Key("dropped"); w.Uint(g_dropped);
            w.EndObject();
            messages.push_back(std::move(w.str()));
            g_droppedDirty = false;
        }
        g_anyDirty = false;
    }
    for (std::string &m : messages) Transport::Get().SendJson(std::move(m));
}

}  // namespace mtlinsp
