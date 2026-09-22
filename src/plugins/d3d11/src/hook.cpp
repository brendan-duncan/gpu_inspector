#include "hook.h"

#include <MinHook.h>

#include <atomic>
#include <mutex>
#include <vector>

namespace d3d11insp {

// ---------------------------------------------------------------------------------------------
// Re-entry

static thread_local int t_internalDepth = 0;

bool Internal() { return t_internalDepth > 0; }
ScopedInternal::ScopedInternal() { ++t_internalDepth; }
ScopedInternal::~ScopedInternal() { --t_internalDepth; }

// ---------------------------------------------------------------------------------------------
// Vtables

namespace {

struct PatchedVtable {
    void** vtable = nullptr;
    std::vector<void*> originals;
};

// Append-only, read without a lock: every hooked method looks its original up here, thousands of
// times a frame, and there are only ever a handful of vtables (one per class the runtime
// implements, times the debug layer's wrappers, plus the copies adopted below).
constexpr size_t kMaxVtables = 256;
PatchedVtable g_vtables[kMaxVtables];
std::atomic<size_t> g_vtableCount{0};
std::mutex g_vtableMutex;

// Every replacement we have installed, so an entry taken from an unknown vtable can be recognized
// as our own. Small and append-only, read without a lock like the table above.
constexpr size_t kMaxReplacements = 512;
void* g_replacements[kMaxReplacements];
std::atomic<size_t> g_replacementCount{0};

const PatchedVtable* Find(void** vtable) {
    size_t n = g_vtableCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i)
        if (g_vtables[i].vtable == vtable) return &g_vtables[i];
    return nullptr;
}

/** Whether `fn` is one of our own replacements rather than a runtime implementation. */
bool IsOurs(void* fn) {
    size_t n = g_replacementCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i)
        if (g_replacements[i] == fn) return true;
    return false;
}

void RememberReplacement(void* fn) {   // caller holds g_vtableMutex
    size_t n = g_replacementCount.load(std::memory_order_relaxed);
    for (size_t i = 0; i < n; ++i)
        if (g_replacements[i] == fn) return;
    if (n >= kMaxReplacements) return;
    g_replacements[n] = fn;
    g_replacementCount.store(n + 1, std::memory_order_release);
}

/**
 * An unknown vtable holding one of our replacements was copied from a vtable we had already
 * patched: a layer in the process (an engine's own wrapper, an overlay) may proxy an object that
 * way. The copy carries our hooks but is not in the table, so the fallback in OriginalEntry would
 * hand a replacement back as "the original" and recurse until the stack is gone.
 *
 * The source is found by the two entries we never patch: QueryInterface and AddRef still hold the
 * source's own implementations in the copy. The copy is then registered with the source's
 * originals, so later calls through it are an ordinary lookup.
 */
const PatchedVtable* AdoptCopy(void** vtable, uint32_t slot) {
    std::lock_guard<std::mutex> lock(g_vtableMutex);
    if (const PatchedVtable* already = Find(vtable)) return already;   // another thread got there first
    const size_t n = g_vtableCount.load(std::memory_order_relaxed);
    const PatchedVtable* source = nullptr;
    for (int tier = 0; tier < 2 && !source; ++tier) {
        for (size_t i = 0; i < n; ++i) {
            const PatchedVtable& p = g_vtables[i];
            if (p.vtable == vtable || p.originals.size() <= slot || p.originals.size() < 2) continue;
            if (p.vtable[slot] != vtable[slot]) continue;
            if (tier == 0 && (vtable[0] != p.originals[0] || vtable[1] != p.originals[1])) continue;
            source = &p;
            if (tier == 1)
                LogAlways("a copied vtable %p was matched to %p only by its replacement at slot %u, not by its "
                          "QueryInterface and AddRef: the originals may belong to another class",
                          (void*)vtable, (void*)p.vtable, slot);
            break;
        }
    }
    if (!source) return nullptr;
    if (n >= kMaxVtables) {
        LogAlways("a copied vtable could not be adopted: the table is full; %p keeps its own entries", (void*)vtable);
        return source;
    }
    PatchedVtable& copy = g_vtables[n];
    copy.vtable = vtable;
    copy.originals = source->originals;
    g_vtableCount.store(n + 1, std::memory_order_release);
    Log("adopted a copy of a patched vtable: %p copied from %p", (void*)vtable, (void*)source->vtable);
    return &g_vtables[n];
}

}  // namespace

bool VtableHooked(const void* object) {
    if (!object) return false;
    return Find(*reinterpret_cast<void** const*>(object)) != nullptr;
}

void* OriginalEntry(const void* object, uint32_t slot) {
    void** vtable = *reinterpret_cast<void** const*>(object);
    if (const PatchedVtable* p = Find(vtable)) {
        if (slot < p->originals.size()) return p->originals[slot];
        return vtable[slot];
    }
    // Not a vtable we patched. Its entry is the original unless it is one of ours, which means the
    // vtable was copied from one we had patched (see AdoptCopy).
    void* entry = vtable[slot];
    if (!IsOurs(entry)) return entry;
    if (const PatchedVtable* source = AdoptCopy(vtable, slot)) {
        if (slot < source->originals.size()) return source->originals[slot];
    }
    LogAlways("a copied vtable %p holds our replacement at slot %u and its source is unknown; the call is refused",
              (void*)vtable, slot);
    return nullptr;
}

bool HookVtable(void* object, const char* interfaceName, uint32_t count, std::initializer_list<SlotHook> hooks) {
    if (!object) return false;
    void** vtable = *reinterpret_cast<void***>(object);
    std::lock_guard<std::mutex> lock(g_vtableMutex);
    if (Find(vtable)) return false;
    size_t n = g_vtableCount.load(std::memory_order_relaxed);
    if (n >= kMaxVtables) {
        LogAlways("too many distinct vtables: %s is not hooked", interfaceName);
        return false;
    }
    PatchedVtable& p = g_vtables[n];
    p.vtable = vtable;
    p.originals.assign(vtable, vtable + count);
    // Publish the saved originals before any replacement can run and look them up.
    g_vtableCount.store(n + 1, std::memory_order_release);
    DWORD old = 0;
    if (!VirtualProtect(vtable, count * sizeof(void*), PAGE_READWRITE, &old)) {
        LogAlways("VirtualProtect failed on the vtable of %s (%lu): not hooked", interfaceName, GetLastError());
        return false;
    }
    uint32_t patched = 0;
    for (const SlotHook& h : hooks) {
        if (h.slot >= count) continue;
        if (p.originals[h.slot] == h.replacement) {
            LogAlways("%s vtable %p slot %u already holds our replacement: a copy of a patched vtable, left alone",
                      interfaceName, (void*)vtable, h.slot);
            continue;
        }
        RememberReplacement(h.replacement);
        vtable[h.slot] = h.replacement;
        ++patched;
    }
    DWORD ignored = 0;
    VirtualProtect(vtable, count * sizeof(void*), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), vtable, count * sizeof(void*));
    Log("hooked %s: %u of %u vtable entries (vtable %p)", interfaceName, patched, count, (void*)vtable);
    return true;
}

uint32_t VtableCount(IUnknown* object, const VersionCount* versions, size_t n, uint32_t base) {
    ScopedInternal internal;
    for (size_t i = 0; i < n; ++i) {
        IUnknown* newer = nullptr;
        if (SUCCEEDED(object->QueryInterface(*versions[i].iid, (void**)&newer)) && newer) {
            newer->Release();
            return versions[i].count;
        }
    }
    return base;
}

// ---------------------------------------------------------------------------------------------
// Exported functions

static std::once_flag g_minhookInit;
static bool g_minhookOk = false;

bool HookFunction(void* target, void* replacement, void** original, const char* name) {
    std::call_once(g_minhookInit, [] {
        const MH_STATUS s = MH_Initialize();
        g_minhookOk = s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED;
    });
    if (!g_minhookOk) {
        LogAlways("MinHook failed to initialize: %s is not hooked", name);
        return false;
    }
    MH_STATUS st = MH_CreateHook(target, replacement, original);
    if (st != MH_OK) {
        LogAlways("hooking %s failed: %s", name, MH_StatusToString(st));
        return false;
    }
    return true;
}

bool EnableFunctionHooks() {
    if (!g_minhookOk) return false;
    MH_STATUS st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK) LogAlways("enabling the entry point hooks failed: %s", MH_StatusToString(st));
    return st == MH_OK;
}

}  // namespace d3d11insp
