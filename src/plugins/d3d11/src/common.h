// Shared by every source of the Direct3D 11 capture library: logging, the re-entry guard for the
// library's own D3D11 calls, and small helpers.
//
// The library lives in the application's process (injected by the inspector's launcher, see
// README.md) and speaks the inspector's protocol through the plugin SDK (gpu_inspector/sdk).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>

#include <gpu_inspector/sdk/json.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace d3d11insp
{

using gpuinsp::sdk::JsonWriter;

// ---------------------------------------------------------------------------------------------
// Logging (D3D11INSP_LOG=1, D3D11INSP_LOG_FILE=<path>)

/** D3D11INSP_LOG=1: every intercepted call worth a line goes to stderr, the debugger and the log file. */
void Log(const char* fmt, ...);
bool LogEnabled();
/** Always logged, whatever D3D11INSP_LOG says: the library loading, failures, the port. */
void LogAlways(const char* fmt, ...);

// ---------------------------------------------------------------------------------------------
// Re-entry (hook.cpp)

/**
 * Whether the current thread is inside one of the library's own D3D11 calls: staging copies,
 * timestamp queries, the read-backs. Every hook forwards without recording or tracking when this
 * is true, so the library's work never shows as the application's.
 */
bool Internal();

/** Marks the scope as the library's own. Nests. */
struct ScopedInternal
{
    ScopedInternal();
    ~ScopedInternal();
    ScopedInternal(const ScopedInternal&) = delete;
    ScopedInternal& operator=(const ScopedInternal&) = delete;
};

// ---------------------------------------------------------------------------------------------
// Helpers

inline std::string Hex(uint64_t v)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)v);
    return buf;
}

/** UTF-16 to UTF-8 (object names, adapter descriptions). */
std::string Narrow(const wchar_t* s);
std::string Narrow(const wchar_t* s, size_t length);

/** Releases and nulls a COM pointer. */
template <typename T>
inline void SafeRelease(T*& p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}

/** A COM pointer that releases on scope exit, for the library's own objects. */
template <typename T>
class ComPtr
{
public:
    ComPtr() = default;
    explicit ComPtr(T* p) : _p(p) {}
    ComPtr(const ComPtr& o) : _p(o._p)
    {
        if (_p)
            _p->AddRef();
    }
    ComPtr(ComPtr&& o) noexcept : _p(o._p) { o._p = nullptr; }
    ~ComPtr()
    {
        if (_p)
            _p->Release();
    }
    ComPtr& operator=(const ComPtr& o)
    {
        if (this != &o)
        {
            if (o._p)
                o._p->AddRef();
            if (_p)
                _p->Release();
            _p = o._p;
        }
        return *this;
    }
    ComPtr& operator=(ComPtr&& o) noexcept
    {
        if (this != &o)
        {
            if (_p)
                _p->Release();
            _p = o._p;
            o._p = nullptr;
        }
        return *this;
    }
    T* get() const { return _p; }
    T* operator->() const { return _p; }
    T** put()
    {
        if (_p)
        {
            _p->Release();
            _p = nullptr;
        }
        return &_p;
    }
    void** putVoid() { return reinterpret_cast<void**>(put()); }
    T* detach()
    {
        T* p = _p;
        _p = nullptr;
        return p;
    }
    void reset(T* p = nullptr)
    {
        if (_p)
            _p->Release();
        _p = p;
    }
    explicit operator bool() const { return _p != nullptr; }

private:
    T* _p = nullptr;
};

}  // namespace d3d11insp
