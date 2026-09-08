#include "stacktrace.h"

#include "json_writer.h"
#include "layer.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#else
#include <cxxabi.h>
#include <dlfcn.h>
#include <unwind.h>
#endif

namespace vkinsp {

bool StackTracesEnabled() {
    static int enabled = -1;
    if (enabled < 0) enabled = ConfigFlag("VKINSP_STACKTRACES") ? 1 : 0;
    return enabled == 1;
}

static std::string BaseName(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? path : path.substr(p + 1);
}

// Frames of the Vulkan loader, this layer and other layers are noise between the application
// and the driver call.
static bool IsInternalModule(const std::string& module) {
    std::string m = module;
    for (char& c : m) c = (char)tolower((unsigned char)c);
    return m.rfind("vklayer_", 0) == 0 || m.rfind("libvklayer_", 0) == 0 || m.rfind("vulkan-1", 0) == 0 ||
           m.rfind("libvulkan", 0) == 0;
}

// A symbol this far from the address is the nearest export of a module without symbols (a
// driver, a stripped engine): module+offset says more than a wrong name.
constexpr uint64_t kMaxSymbolDisplacement = 64 * 1024;

// Everything from the innermost frame up to the outermost loader/layer frame is the Vulkan call
// chain (the driver's frames sit between the layers and the loader); the application starts
// after it.
static void MarkInternal(std::vector<StackFrame>& frames) {
    size_t last = SIZE_MAX;
    for (size_t i = 0; i < frames.size(); ++i)
        if (IsInternalModule(frames[i].module)) last = i;
    if (last == SIZE_MAX) return;
    for (size_t i = 0; i <= last; ++i) frames[i].internal = true;
}

// ---------------------------------------------------------------------------------------------
#if defined(_WIN32)

StackTrace CaptureStack(unsigned skip) {
    void* frames[kMaxStackFrames];
    USHORT n = CaptureStackBackTrace(skip + 1, (DWORD)kMaxStackFrames, frames, nullptr);
    StackTrace out(n);
    for (USHORT i = 0; i < n; ++i) out[i] = (uint64_t)(uintptr_t)frames[i];
    return out;
}

// DbgHelp is not thread-safe: one lock around every call.
static std::mutex g_symMutex;
static bool g_symInit = false;

static void EnsureSymbols() {
    if (g_symInit) return;
    g_symInit = true;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
    if (!SymInitialize(GetCurrentProcess(), nullptr, TRUE)) Log("stack traces: SymInitialize failed (%lu)", GetLastError());
}

std::vector<StackFrame> Symbolize(const StackTrace& addresses) {
    std::lock_guard<std::mutex> lock(g_symMutex);
    EnsureSymbols();
    HANDLE process = GetCurrentProcess();
    // Modules loaded since the last call (the application keeps loading DLLs).
    SymRefreshModuleList(process);
    std::vector<StackFrame> out;
    out.reserve(addresses.size());
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
    for (uint64_t addr : addresses) {
        StackFrame f;
        f.address = addr;
        DWORD64 base = SymGetModuleBase64(process, addr);
        if (base) {
            char path[MAX_PATH];
            if (GetModuleFileNameA((HMODULE)(uintptr_t)base, path, MAX_PATH)) f.module = BaseName(path);
            f.offset = addr - base;
        }
        SYMBOL_INFO* sym = (SYMBOL_INFO*)buffer;
        memset(sym, 0, sizeof(SYMBOL_INFO));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 disp = 0;
        if (SymFromAddr(process, addr, &disp, sym) && disp < kMaxSymbolDisplacement) {
            f.function = sym->Name;
            f.offset = disp;
        }
        IMAGEHLP_LINE64 line;
        memset(&line, 0, sizeof(line));
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        if (SymGetLineFromAddr64(process, addr, &lineDisp, &line) && line.FileName) {
            f.file = line.FileName;
            f.line = line.LineNumber;
        }
        out.push_back(std::move(f));
    }
    MarkInternal(out);
    return out;
}

// ---------------------------------------------------------------------------------------------
#else

namespace {
struct UnwindState {
    StackTrace* out;
    unsigned skip;
};
_Unwind_Reason_Code UnwindStep(struct _Unwind_Context* ctx, void* arg) {
    UnwindState* s = (UnwindState*)arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (!pc) return _URC_NO_REASON;
    if (s->skip) { s->skip--; return _URC_NO_REASON; }
    s->out->push_back((uint64_t)pc);
    return s->out->size() >= kMaxStackFrames ? _URC_END_OF_STACK : _URC_NO_REASON;
}
}  // namespace

StackTrace CaptureStack(unsigned skip) {
    StackTrace out;
    out.reserve(kMaxStackFrames);
    UnwindState s{&out, skip + 1};
    _Unwind_Backtrace(UnwindStep, &s);
    return out;
}

std::vector<StackFrame> Symbolize(const StackTrace& addresses) {
    std::vector<StackFrame> out;
    out.reserve(addresses.size());
    for (uint64_t addr : addresses) {
        StackFrame f;
        f.address = addr;
        Dl_info info;
        memset(&info, 0, sizeof(info));
        if (dladdr((void*)(uintptr_t)addr, &info)) {
            if (info.dli_fname) f.module = BaseName(info.dli_fname);
            if (info.dli_fbase) f.offset = addr - (uint64_t)(uintptr_t)info.dli_fbase;
            uint64_t disp = info.dli_saddr ? addr - (uint64_t)(uintptr_t)info.dli_saddr : 0;
            if (info.dli_sname && disp < kMaxSymbolDisplacement) {
                int status = 0;
                char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
                f.function = status == 0 && demangled ? demangled : info.dli_sname;
                free(demangled);
                f.offset = disp;
            }
        }
        out.push_back(std::move(f));
    }
    MarkInternal(out);
    return out;
}

#endif

// ---------------------------------------------------------------------------------------------

static std::string Hex(uint64_t v) {
    char buf[24];
    snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)v);
    return buf;
}

void WriteStackFrames(JsonWriter& w, const std::vector<StackFrame>& frames) {
    w.BeginArray();
    for (const StackFrame& f : frames) {
        w.BeginObject();
        w.Key("address"); w.String(Hex(f.address));
        if (!f.module.empty()) { w.Key("module"); w.String(f.module); }
        if (!f.function.empty()) { w.Key("function"); w.String(f.function); }
        if (!f.file.empty()) { w.Key("file"); w.String(f.file); w.Key("line"); w.Uint(f.line); }
        w.Key("offset"); w.Uint(f.offset);
        if (f.internal) { w.Key("internal"); w.Boolean(true); }
        w.EndObject();
    }
    w.EndArray();
}

void WriteStackAddresses(JsonWriter& w, const StackTrace& stack) {
    w.BeginArray();
    for (uint64_t a : stack) w.String(Hex(a));
    w.EndArray();
}

std::string StackExtraJson(const StackTrace& stack) {
    if (stack.empty()) return std::string();
    std::string s = ",\"stack\":[";
    for (size_t i = 0; i < stack.size(); ++i) {
        if (i) s += ',';
        s += '"';
        s += Hex(stack[i]);
        s += '"';
    }
    s += ']';
    return s;
}

}  // namespace vkinsp
