// Shared by every source of the D3D12 capture library: logging, configuration, the re-entry guard
// for the library's own D3D12 calls, and small helpers.
//
// The library lives in the application's process (injected by dxinsp_launch.exe, see README.md)
// and speaks the inspector's protocol through transport.h. Its namespace is dxinsp; the Vulkan
// layer's json_writer.h is shared as is (namespace vkinsp), since the JSON it writes is what the
// UI reads whichever API produced it.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <string>

#include "json_writer.h"

namespace dxinsp {

using vkinsp::JsonWriter;

// ---------------------------------------------------------------------------------------------
// Logging and configuration (log.cpp)

/** DXINSP_LOG=1: every intercepted call worth a line goes to stderr, the debugger and DXINSP_LOG_FILE. */
void Log(const char* fmt, ...);
bool LogEnabled();
/** Always logged, whatever DXINSP_LOG says: the library loading, failures, the port. */
void LogAlways(const char* fmt, ...);

/** A DXINSP_* environment variable, "" when unset. */
std::string ConfigValue(const char* name);
/** True when the variable is set and not "0". */
bool ConfigFlag(const char* name);

// ---------------------------------------------------------------------------------------------
// Re-entry (hook.cpp)

/**
 * Whether the current thread is inside one of the library's own D3D12 calls: read-back command
 * lists, staging resources, query heaps, the reflection of a shader. Every hook forwards without
 * recording or tracking when this is true, so the library's work never shows as the application's.
 */
bool Internal();

/** Marks the scope as the library's own. Nests. */
struct ScopedInternal {
    ScopedInternal();
    ~ScopedInternal();
    ScopedInternal(const ScopedInternal&) = delete;
    ScopedInternal& operator=(const ScopedInternal&) = delete;
};

// ---------------------------------------------------------------------------------------------
// Helpers

inline std::string Hex(uint64_t v) {
    char buf[24];
    snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)v);
    return buf;
}

/** The interface pointer as the tracker's key: the object's identity, whatever version it is seen as. */
inline uint64_t Key(const void* object) { return (uint64_t)(uintptr_t)object; }

/** UTF-16 to UTF-8 (object names, adapter descriptions). */
std::string Narrow(const wchar_t* s);
std::string Narrow(const wchar_t* s, size_t length);

/** An HRESULT as "0x8007000e" for the log. */
inline std::string HrText(HRESULT hr) { return Hex((uint32_t)hr); }

/** Releases and nulls a COM pointer. */
template <typename T>
inline void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

/** A COM pointer that releases on scope exit, for the library's own objects. */
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    explicit ComPtr(T* p) : _p(p) {}
    ComPtr(const ComPtr& o) : _p(o._p) { if (_p) _p->AddRef(); }
    ComPtr(ComPtr&& o) noexcept : _p(o._p) { o._p = nullptr; }
    ~ComPtr() { if (_p) _p->Release(); }
    ComPtr& operator=(const ComPtr& o) {
        if (this != &o) { if (o._p) o._p->AddRef(); if (_p) _p->Release(); _p = o._p; }
        return *this;
    }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) { if (_p) _p->Release(); _p = o._p; o._p = nullptr; }
        return *this;
    }
    T* get() const { return _p; }
    T* operator->() const { return _p; }
    T** put() { if (_p) { _p->Release(); _p = nullptr; } return &_p; }
    void** putVoid() { return reinterpret_cast<void**>(put()); }
    T* detach() { T* p = _p; _p = nullptr; return p; }
    void reset(T* p = nullptr) { if (_p) _p->Release(); _p = p; }
    explicit operator bool() const { return _p != nullptr; }

private:
    T* _p = nullptr;
};

}  // namespace dxinsp
