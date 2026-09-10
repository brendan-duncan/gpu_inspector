#include "swizzle.h"

#include <atomic>
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

// (class, selector) -> the implementation calls on that class reached before the hook.
//
// The entry is only ever written for a class that now carries the hook *itself*, which is what
// makes the key sound. See Hook() for why the obvious approach is not.
using Table = std::unordered_map<std::pair<const void *, const void *>, IMP, PairHash>;

// Writers (Hook) serialize on g_mutex and publish a fresh copy; readers (every intercepted call)
// load the pointer and never lock. Superseded tables are deliberately leaked: a reader may still
// be inside one, and there are tens of hooks over the life of a process, not thousands.
std::atomic<const Table *> g_table{nullptr};
std::mutex g_mutex;
// Classes already offered to the hook installers.
std::set<Class> g_seen;

bool g_logChecked = false;
bool g_logEnabled = false;

const Table *Snapshot() { return g_table.load(std::memory_order_acquire); }

/** Under g_mutex. */
void Publish(const void *cls, const void *sel, IMP original) {
    const Table *old = g_table.load(std::memory_order_relaxed);
    Table *next = old != nullptr ? new Table(*old) : new Table();
    (*next)[{cls, sel}] = original;
    g_table.store(next, std::memory_order_release);
}

// One intercepted call in progress on this thread: which object and selector, and the class the
// original was resolved on, so a nested observation of the same call resolves above it.
struct Frame {
    const void *self;
    const void *sel;
    Class resolved;
};
constexpr int kMaxFrames = 32;
thread_local int g_depth = 0;
thread_local Frame g_frames[kMaxFrames];
thread_local int g_internalDepth = 0;

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

Reentry::Reentry(id self, SEL sel) : outermost_(g_depth == 0), original_(nullptr) {
    Class start = object_getClass(self);
    // The same selector on the same object, already on the stack: an override calling super into
    // an ancestor that is hooked too. Resolve above where that outer call resolved.
    const int frames = g_depth < kMaxFrames ? g_depth : kMaxFrames;
    for (int i = frames - 1; i >= 0; i--) {
        if (g_frames[i].self == (const void *)self && g_frames[i].sel == (const void *)sel) {
            start = g_frames[i].resolved != nil ? class_getSuperclass(g_frames[i].resolved) : nil;
            break;
        }
    }
    Class resolved = nil;
    if (const Table *table = Snapshot()) {
        for (Class cls = start; cls != nil; cls = class_getSuperclass(cls)) {
            auto it = table->find({(const void *)cls, (const void *)sel});
            if (it != table->end()) {
                original_ = it->second;
                resolved = cls;
                break;
            }
        }
    }
    if (original_ == nullptr) {
        // Unreachable by construction: a hook only runs because it was installed on the
        // receiver's class or one it inherits from, and the original is published before the
        // hook is. Said out loud rather than left to the null the caller would jump through.
        fprintf(stderr, "[mtlinsp] no original for %s on %s\n", sel_getName(sel), ClassName(self));
    }
    if (g_depth < kMaxFrames) g_frames[g_depth] = {(const void *)self, (const void *)sel, resolved};
    g_depth++;
}

Reentry::~Reentry() { --g_depth; }

Internal::Internal() { ++g_internalDepth; }
Internal::~Internal() { --g_internalDepth; }
bool IsInternal() { return g_internalDepth > 0; }

bool FirstSighting(Class cls) {
    if (cls == nil) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_seen.insert(cls).second;
}

bool Hook(Class cls, SEL sel, IMP replacement) {
    if (cls == nil) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
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

    const Table *table = Snapshot();
    if (table != nullptr && table->count({(const void *)cls, (const void *)sel}) != 0) {
        // A second, different replacement for a selector this class already carries. Chaining
        // them would make the second's "original" the first, which Reentry's resolution then
        // skips over. Every selector has one hook; the first one installed wins.
        fprintf(stderr, "[mtlinsp] %s on %s is already hooked; keeping the first replacement\n",
                sel_getName(sel), class_getName(cls));
        return false;
    }

    // Published first, so a call on another thread that lands on the replacement below has an
    // original to forward to.
    Publish((const void *)cls, (const void *)sel, current);
    if (!class_addMethod(cls, sel, replacement, method_getTypeEncoding(method))) {
        method_setImplementation(class_getInstanceMethod(cls, sel), replacement);
    }
    return true;
}

}  // namespace mtlinsp
