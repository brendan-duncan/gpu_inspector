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

struct PairHash {
    size_t operator()(const std::pair<const void *, const void *> &p) const {
        return std::hash<const void *>()(p.first) * 31 + std::hash<const void *>()(p.second);
    }
};

std::mutex g_mutex;
// (class, selector) -> the implementation calls on that class reached before the hook.
//
// The entry is only ever written for a class that now carries the hook *itself*, which is what
// makes the key sound. See Hook() for why the obvious approach is not.
std::unordered_map<std::pair<const void *, const void *>, IMP, PairHash> g_originals;
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
    if (method == nullptr) return false;  // no such method: optional protocol members, API levels

    // class_getInstanceMethod walks up, so `method` may belong to an ancestor rather than to cls.
    // Calling method_setImplementation on it would rewrite that ancestor's implementation for
    // *every* class that inherits it — hooking a render encoder would silently hook the compute
    // encoder and the command buffer beside it. Metal's class trees are shaped exactly that way,
    // so this is the normal case, not a corner.
    //
    // class_addMethod instead installs an override on cls alone, leaving the ancestor untouched;
    // the implementation being overridden is what calls used to reach, which is the original.
    // When cls already has its own implementation, class_addMethod declines and there is no
    // sharing to worry about, so replacing it in place is right.
    const IMP current = method_getImplementation(method);
    // Already hooked, here or on an ancestor this class inherits from. Adding an override whose
    // "original" is our own replacement would call it in a loop.
    if (current == replacement) return true;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (class_addMethod(cls, sel, replacement, method_getTypeEncoding(method))) {
        g_originals[{(const void *)cls, (const void *)sel}] = current;
    } else {
        g_originals[{(const void *)cls, (const void *)sel}] =
            method_setImplementation(class_getInstanceMethod(cls, sel), replacement);
    }
    return true;
}

IMP Original(id self, SEL sel) {
    std::lock_guard<std::mutex> lock(g_mutex);
    // Up from the receiver's class: a hook lives on the exact class it was recorded against, and
    // an instance of a subclass reaches it by inheritance.
    for (Class cls = object_getClass(self); cls != nil; cls = class_getSuperclass(cls)) {
        auto it = g_originals.find({(const void *)cls, (const void *)sel});
        if (it != g_originals.end()) return it->second;
    }
    // Unreachable: a hook only runs because it was installed on the receiver's class or one it
    // inherits from. Worth saying out loud rather than returning a null the caller jumps through.
    fprintf(stderr, "[mtlinsp] no original for %s on %s\n", sel_getName(sel), ClassName(self));
    return nullptr;
}

}  // namespace mtlinsp
