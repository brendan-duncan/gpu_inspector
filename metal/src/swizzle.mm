#include "swizzle.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>

namespace mtlinsp {
namespace {

std::mutex g_mutex;
// Method -> the implementation it had before the hook.
//
// Keyed by the Method rather than by (class, selector), which is not the same thing and matters.
// class_getInstanceMethod walks up the hierarchy, so hooking "MTLDebugRenderCommandEncoder's
// endEncoding" can really be replacing an implementation that lives on a shared ancestor. Keying
// by the class asked about then leaves a sibling that inherits the same implementation —
// MTLDebugComputeCommandEncoder — with no entry to find: its hook fires, the lookup walks its own
// ancestors, finds nothing, and the forward jumps through a null pointer. Metal's validation
// layers have exactly that shape, so it is not hypothetical.
//
// Replacing an implementation does not move or copy the Method, so the pointer stays valid and
// identifies the implementation being replaced no matter which class was named to reach it.
std::unordered_map<Method, IMP> g_originals;
// Classes already offered to the hook installers.
std::set<Class> g_seen;

bool g_logChecked = false;
bool g_logEnabled = false;

}  // namespace

bool LogEnabled() {
    if (!g_logChecked) {
        const char *value = getenv("MTLINSP_LOG");
        g_logEnabled = value != nullptr && value[0] != '\0' && value[0] != '0';
        g_logChecked = true;
    }
    return g_logEnabled;
}

void Log(const char *format, ...) {
    if (!LogEnabled()) return;
    va_list args;
    va_start(args, format);
    fprintf(stderr, "[mtlinsp] ");
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
    fflush(stderr);
}

const char *ClassName(id object) {
    return object == nil ? "(nil)" : class_getName(object_getClass(object));
}

namespace {
thread_local int g_depth = 0;
}  // namespace

Reentry::Reentry() : outermost_(g_depth++ == 0) {}
Reentry::~Reentry() { --g_depth; }

bool FirstSighting(Class cls) {
    if (cls == nil) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_seen.insert(cls).second;
}

bool Hook(Class cls, SEL sel, IMP replacement) {
    if (cls == nil) return false;
    Method method = class_getInstanceMethod(cls, sel);
    if (method == nullptr) return false;

    std::lock_guard<std::mutex> lock(g_mutex);
    // Already replaced, reached through this class or another that shares the implementation.
    if (g_originals.count(method) != 0) return true;
    g_originals[method] = method_setImplementation(method, replacement);
    return true;
}

IMP Original(id self, SEL sel) {
    Method method = class_getInstanceMethod(object_getClass(self), sel);
    if (method == nullptr) return nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_originals.find(method);
    if (it == g_originals.end()) {
        // Unreachable: a hook only runs because this Method was replaced, so it is in the map.
        // Worth saying out loud rather than returning a null the caller will jump through.
        fprintf(stderr, "[mtlinsp] no original for %s on %s\n", sel_getName(sel), ClassName(self));
        return nullptr;
    }
    return it->second;
}

}  // namespace mtlinsp
