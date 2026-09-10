#include "stacktrace.h"

#include "json_writer.h"

#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>

#include <cstdlib>
#include <cstring>

namespace mtlinsp {
namespace {

std::string BaseName(const char *path) {
    if (path == nullptr) return {};
    const char *slash = strrchr(path, '/');
    return slash != nullptr ? slash + 1 : path;
}

/**
 * Frames of this library, Metal, its driver and the Objective-C runtime are the call chain
 * between the application and the GPU: noise, the way the Vulkan loader's are. Everything from
 * the innermost frame up to the outermost such frame is marked, so the application's own call
 * is the first frame shown.
 */
bool IsInternalModule(const std::string &module) {
    return module.rfind("libmtlinsp", 0) == 0 || module == "Metal" || module.rfind("AGX", 0) == 0
        || module.rfind("libobjc", 0) == 0 || module.rfind("MTLCompiler", 0) == 0
        || module.rfind("MTLDebug", 0) == 0 || module.rfind("libMTL", 0) == 0;
}

void MarkInternal(std::vector<StackFrame> &frames) {
    size_t last = SIZE_MAX;
    for (size_t i = 0; i < frames.size(); ++i) {
        if (IsInternalModule(frames[i].module)) last = i;
    }
    if (last == SIZE_MAX) return;
    for (size_t i = 0; i <= last; ++i) frames[i].internal = true;
}

// A symbol this far from the address is the nearest export of a module without symbols (a
// driver, a stripped engine): module+offset says more than a wrong name.
constexpr uint64_t kMaxSymbolDisplacement = 64 * 1024;

}  // namespace

bool StackTracesEnabled() {
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("MTLINSP_STACKTRACES");
        enabled = value != nullptr && value[0] != '\0' && value[0] != '0' ? 1 : 0;
    }
    return enabled == 1;
}

StackTrace CaptureStack(unsigned skip) {
    void *raw[kMaxStackFrames + 8];
    const int count = backtrace(raw, (int)(kMaxStackFrames + 8));
    StackTrace out;
    // One frame for this function, then what the caller asked to drop.
    const unsigned drop = 1 + skip;
    for (int i = (int)drop; i < count && out.size() < kMaxStackFrames; ++i) {
        out.push_back((uint64_t)(uintptr_t)raw[i]);
    }
    return out;
}

std::vector<StackFrame> Symbolize(const StackTrace &addresses) {
    std::vector<StackFrame> out;
    out.reserve(addresses.size());
    for (uint64_t address : addresses) {
        StackFrame f;
        f.address = address;
        Dl_info info;
        memset(&info, 0, sizeof(info));
        // The return address is one past the call; symbolizing the byte before lands inside the
        // calling function rather than at the start of the next one.
        if (dladdr((void *)(uintptr_t)(address - 1), &info)) {
            f.module = BaseName(info.dli_fname);
            if (info.dli_fbase != nullptr) f.offset = address - (uint64_t)(uintptr_t)info.dli_fbase;
            const uint64_t displacement = info.dli_saddr != nullptr ? address - (uint64_t)(uintptr_t)info.dli_saddr : 0;
            if (info.dli_sname != nullptr && displacement < kMaxSymbolDisplacement) {
                int status = 0;
                char *demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
                f.function = status == 0 && demangled != nullptr ? demangled : info.dli_sname;
                free(demangled);
            }
        }
        out.push_back(std::move(f));
    }
    MarkInternal(out);
    return out;
}

std::string HexAddress(uint64_t address) {
    char buf[24];
    snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)address);
    return buf;
}

void WriteStackFrames(vkinsp::JsonWriter &w, const std::vector<StackFrame> &frames) {
    w.BeginArray();
    for (const StackFrame &f : frames) {
        w.BeginObject();
        w.Key("address"); w.String(HexAddress(f.address));
        if (!f.module.empty()) { w.Key("module"); w.String(f.module); }
        if (!f.function.empty()) { w.Key("function"); w.String(f.function); }
        w.Key("offset"); w.Uint(f.offset);
        if (f.internal) { w.Key("internal"); w.Boolean(true); }
        w.EndObject();
    }
    w.EndArray();
}

}  // namespace mtlinsp
