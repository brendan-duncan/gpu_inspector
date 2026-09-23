// Getting between the application and Direct3D 11: inline hooks on the exported entry points, and
// vtable patches on the COM objects that come out of them.
//
// Nothing is wrapped. The first object of each kind that a creation call returns has the entries
// of its vtable replaced, once per distinct vtable (all buffers share one, all immediate and
// deferred contexts another, and the debug layer's wrapper classes have their own, found the same
// way), and the originals are kept so a replacement can forward. Per-object state lives in side
// tables keyed by the interface pointer (state.h). That is the decision the D3D12 library makes,
// for the same reasons: a proxy would have to survive everything the runtime, DXGI and the debug
// layer do with the objects they are handed.
//
// The slot numbers and the signatures of the originals are generated from the SDK's C-style
// vtable structs (gen/d3d11_vtables.gen.h, tools/gen_d3d11.py).
#pragma once

#include "common.h"

#include <initializer_list>

namespace d3d11insp
{

/** One replacement: the vtable slot and the function to put in it. */
struct SlotHook
{
    uint32_t slot;
    void* replacement;
};

/**
 * Patches the vtable of `object` (a COM interface pointer) with the given replacements, unless
 * that vtable was patched already. `count` is the number of entries the interface's vtable has
 * (slot::<Interface>_Count), all of which are saved first so any of them can be called through
 * Original(). Returns true when the vtable was patched by this call.
 */
bool HookVtable(void* object, const char* interfaceName, uint32_t count, std::initializer_list<SlotHook> hooks);

/** Whether the object's vtable has been patched (the object needs no further hooking). */
bool VtableHooked(const void* object);

/** The entry `slot` of the object's vtable held before it was patched; the current entry when it was not. */
void* OriginalEntry(const void* object, uint32_t slot);

/** OriginalEntry as a typed function pointer: `Orig<PFN_ID3D11DeviceContext4_Draw>(This, slot)`. */
template <typename Fn>
inline Fn Orig(const void* object, uint32_t slot)
{
    return reinterpret_cast<Fn>(OriginalEntry(object, slot));
}

/**
 * How many slots the object's vtable really has: the count of the newest of `versions` the object
 * answers QueryInterface for, else `base`. A runtime implementing an older interface has a shorter
 * vtable, and writing past its end would patch whatever follows it.
 */
struct VersionCount
{
    const IID* iid;
    uint32_t count;
};
uint32_t VtableCount(IUnknown* object, const VersionCount* versions, size_t n, uint32_t base);

/**
 * Installs an inline hook on an exported function (MinHook). `target` is the function's address,
 * `replacement` the function to run instead, and `original` receives a trampoline to the
 * original. Returns false, logging why, when the function cannot be hooked.
 */
bool HookFunction(void* target, void* replacement, void** original, const char* name);

/** Enables every hook installed so far. Called once at initialization, after the entry points are hooked. */
bool EnableFunctionHooks();

}  // namespace d3d11insp
