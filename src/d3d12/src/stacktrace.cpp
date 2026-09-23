#include "stacktrace.h"

#include "common.h"

#include <dbghelp.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace dxinsp
{

bool StackTracesEnabled()
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = ConfigFlag("DXINSP_STACKTRACES") ? 1 : 0;
    return enabled == 1;
}

static std::string BaseName(const std::string& path)
{
    size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? path : path.substr(p + 1);
}

// Frames of this library, the D3D12 runtime, DXGI and the drivers' user-mode DLLs are noise
// between the application and its call.
static bool IsInternalModule(const std::string& module)
{
    std::string m = module;
    for (char& c : m)
        c = (char)tolower((unsigned char)c);
    return m.rfind("dxinsp_", 0) == 0 || m == "d3d12.dll" || m == "d3d12core.dll" || m == "dxgi.dll" ||
        m == "d3d12sdklayers.dll" || m == "dxgidebug.dll" || m.rfind("nvwgf2um", 0) == 0 || m.rfind("amdxc64", 0) == 0 ||
        m.rfind("igd12", 0) == 0 || m.rfind("nvldumdx", 0) == 0 || m.rfind("amdvlk", 0) == 0 || m == "dxcore.dll";
}

// A symbol this far from the address is the nearest export of a module without symbols (a
// driver, a stripped engine): module+offset says more than a wrong name.
constexpr uint64_t kMaxSymbolDisplacement = 64 * 1024;

// Everything from the innermost frame up to the outermost runtime frame is the D3D12 call chain;
// the application starts after it.
static void MarkInternal(std::vector<StackFrame>& frames)
{
    size_t last = SIZE_MAX;
    for (size_t i = 0; i < frames.size(); ++i)
        if (IsInternalModule(frames[i].module))
            last = i;
    if (last == SIZE_MAX)
        return;
    for (size_t i = 0; i <= last; ++i)
        frames[i].internal = true;
}

StackTrace CaptureStack(unsigned skip)
{
    void* frames[kMaxStackFrames];
    USHORT n = CaptureStackBackTrace(skip + 1, (DWORD)kMaxStackFrames, frames, nullptr);
    StackTrace out(n);
    for (USHORT i = 0; i < n; ++i)
        out[i] = (uint64_t)(uintptr_t)frames[i];
    return out;
}

// DbgHelp is not thread-safe: one lock around every call.
static std::mutex g_symMutex;
static bool g_symInit = false;

static void EnsureSymbols()
{
    if (g_symInit)
        return;
    g_symInit = true;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
    if (!SymInitialize(GetCurrentProcess(), nullptr, TRUE))
        Log("stack traces: SymInitialize failed (%lu)", GetLastError());
    // DXINSP_SYMBOL_PATH (the launch dialog's symbol directories): where to look for PDBs that are
    // not beside their modules, which is every build that keeps its symbols somewhere else. Added
    // in front of what DbgHelp works out for itself (the module directories and _NT_SYMBOL_PATH)
    // rather than instead of it.
    std::string dirs = ConfigValue("DXINSP_SYMBOL_PATH");
    if (dirs.empty())
        return;
    std::string path = dirs;
    char current[4096] = {};
    if (SymGetSearchPath(GetCurrentProcess(), current, (DWORD)sizeof(current)) && current[0])
    {
        path += ';';
        path += current;
    }
    if (!SymSetSearchPath(GetCurrentProcess(), path.c_str()))
    {
        Log("stack traces: SymSetSearchPath(%s) failed (%lu)", path.c_str(), GetLastError());
        return;
    }
    Log("stack traces: symbol search path is %s", path.c_str());
}

/**
 * The source functions a return address stands for. The compiler emits one function and inlines
 * others into it, so DbgHelp's answer for the address is the emitted one; the inlined callers are
 * a separate walk, innermost first. `f` already carries the emitted function, which becomes the
 * outermost caller: the frame itself takes the innermost, which is the one the reader means.
 */
static void ResolveInlineFrames(HANDLE process, uint64_t addr, StackFrame& f)
{
    DWORD count = SymAddrIncludeInlineTrace(process, addr);
    if (!count)
        return;
    DWORD context = 0;
    DWORD frameIndex = 0;
    if (!SymQueryInlineTrace(process, addr, 0, addr, addr, &context, &frameIndex))
        return;
    std::vector<InlinedCaller> frames;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
    for (DWORD i = 0; i < count; ++i)
    {
        InlinedCaller c;
        SYMBOL_INFO* sym = (SYMBOL_INFO*)buffer;
        memset(sym, 0, sizeof(SYMBOL_INFO));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 disp = 0;
        if (SymFromInlineContext(process, addr, context + i, &disp, sym))
            c.function = sym->Name;
        IMAGEHLP_LINE64 line;
        memset(&line, 0, sizeof(line));
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        if (SymGetLineFromInlineContext(process, addr, context + i, 0, &lineDisp, &line) && line.FileName)
        {
            c.file = line.FileName;
            c.line = line.LineNumber;
        }
        if (c.function.empty() && c.file.empty())
            continue;
        frames.push_back(std::move(c));
    }
    if (frames.empty())
        return;
    // The emitted function is the last caller, after the inlined ones.
    InlinedCaller emitted{f.function, f.file, f.line};
    f.function = frames.front().function;
    f.file = frames.front().file;
    f.line = frames.front().line;
    f.inlinedInto.assign(frames.begin() + 1, frames.end());
    if (!emitted.function.empty() || !emitted.file.empty())
        f.inlinedInto.push_back(std::move(emitted));
}

std::vector<StackFrame> Symbolize(const StackTrace& addresses)
{
    std::lock_guard<std::mutex> lock(g_symMutex);
    EnsureSymbols();
    HANDLE process = GetCurrentProcess();
    SymRefreshModuleList(process);
    std::vector<StackFrame> out;
    out.reserve(addresses.size());
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
    for (uint64_t addr : addresses)
    {
        StackFrame f;
        f.address = addr;
        DWORD64 base = SymGetModuleBase64(process, addr);
        if (base)
        {
            char path[MAX_PATH];
            if (GetModuleFileNameA((HMODULE)(uintptr_t)base, path, MAX_PATH))
                f.module = BaseName(path);
            f.offset = addr - base;
        }
        SYMBOL_INFO* sym = (SYMBOL_INFO*)buffer;
        memset(sym, 0, sizeof(SYMBOL_INFO));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 disp = 0;
        if (SymFromAddr(process, addr, &disp, sym) && disp < kMaxSymbolDisplacement)
            f.function = sym->Name;
        IMAGEHLP_LINE64 line;
        memset(&line, 0, sizeof(line));
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        if (SymGetLineFromAddr64(process, addr, &lineDisp, &line) && line.FileName)
        {
            f.file = line.FileName;
            f.line = line.LineNumber;
        }
        ResolveInlineFrames(process, addr, f);
        out.push_back(std::move(f));
    }
    MarkInternal(out);
    return out;
}

void WriteStackFrames(JsonWriter& w, const std::vector<StackFrame>& frames)
{
    w.BeginArray();
    for (const StackFrame& f : frames)
    {
        w.BeginObject();
        w.Key("address");
        w.String(Hex(f.address));
        if (!f.module.empty())
        {
            w.Key("module");
            w.String(f.module);
        }
        if (!f.function.empty())
        {
            w.Key("function");
            w.String(f.function);
        }
        if (!f.file.empty())
        {
            w.Key("file");
            w.String(f.file);
            w.Key("line");
            w.Uint(f.line);
        }
        w.Key("offset");
        w.Uint(f.offset);
        if (f.internal)
        {
            w.Key("internal");
            w.Boolean(true);
        }
        if (!f.inlinedInto.empty())
        {
            w.Key("inlinedInto");
            w.BeginArray();
            for (const InlinedCaller& c : f.inlinedInto)
            {
                w.BeginObject();
                if (!c.function.empty())
                {
                    w.Key("function");
                    w.String(c.function);
                }
                if (!c.file.empty())
                {
                    w.Key("file");
                    w.String(c.file);
                    w.Key("line");
                    w.Uint(c.line);
                }
                w.EndObject();
            }
            w.EndArray();
        }
        w.EndObject();
    }
    w.EndArray();
}

void WriteStackAddresses(JsonWriter& w, const StackTrace& stack)
{
    w.BeginArray();
    for (uint64_t a : stack)
        w.String(Hex(a));
    w.EndArray();
}

std::string StackExtraJson(const StackTrace& stack)
{
    if (stack.empty())
        return std::string();
    std::string s = ",\"stack\":[";
    for (size_t i = 0; i < stack.size(); ++i)
    {
        if (i)
            s += ',';
        s += '"';
        s += Hex(stack[i]);
        s += '"';
    }
    s += ']';
    return s;
}

}  // namespace dxinsp
