#include "hook.h"

#include <MinHook.h>

#include <atomic>
#include <mutex>
#include <vector>

namespace dxinsp {

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
// implements, times the debug layer's wrappers).
constexpr size_t kMaxVtables = 64;
PatchedVtable g_vtables[kMaxVtables];
std::atomic<size_t> g_vtableCount{0};
std::mutex g_vtableMutex;

const PatchedVtable* Find(void** vtable) {
    size_t n = g_vtableCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i)
        if (g_vtables[i].vtable == vtable) return &g_vtables[i];
    return nullptr;
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
    }
    return vtable[slot];
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
        vtable[h.slot] = h.replacement;
        ++patched;
    }
    DWORD ignored = 0;
    VirtualProtect(vtable, count * sizeof(void*), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), vtable, count * sizeof(void*));
    Log("hooked %s: %u of %u vtable entries (vtable %p)", interfaceName, patched, count, (void*)vtable);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Exported functions

static std::once_flag g_minhookInit;
static bool g_minhookOk = false;

bool HookFunction(void* target, void* replacement, void** original, const char* name) {
    std::call_once(g_minhookInit, [] { g_minhookOk = MH_Initialize() == MH_OK; });
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

}  // namespace dxinsp
