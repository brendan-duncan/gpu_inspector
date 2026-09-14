#include "common.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace dxinsp {

std::string ConfigValue(const char* name) {
    char buf[1024];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return std::string();
    return std::string(buf, n);
}

bool ConfigFlag(const char* name) {
    std::string v = ConfigValue(name);
    return !v.empty() && v != "0";
}

bool LogEnabled() {
    static int enabled = -1;
    if (enabled < 0) enabled = ConfigFlag("DXINSP_LOG") ? 1 : 0;
    return enabled == 1;
}

static std::mutex g_logMutex;

static void Write(const char* fmt, va_list ap) {
    char buf[4096];
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    buf[n] = '\n';
    buf[n + 1] = 0;
    std::lock_guard<std::mutex> lock(g_logMutex);
    fputs("dxinsp: ", stderr);
    fputs(buf, stderr);
    fflush(stderr);
    OutputDebugStringA("dxinsp: ");
    OutputDebugStringA(buf);
    // A GUI application (a Unity player) has no stderr anyone can read: DXINSP_LOG_FILE appends
    // the same lines to a file, the way VKINSP_LOG_FILE does for the Vulkan layer.
    static std::string file = ConfigValue("DXINSP_LOG_FILE");
    if (!file.empty()) {
        if (FILE* f = fopen(file.c_str(), "ab")) {
            fputs(buf, f);
            fclose(f);
        }
    }
}

void Log(const char* fmt, ...) {
    if (!LogEnabled()) return;
    va_list ap;
    va_start(ap, fmt);
    Write(fmt, ap);
    va_end(ap);
}

void LogAlways(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Write(fmt, ap);
    va_end(ap);
}

std::string Narrow(const wchar_t* s, size_t length) {
    if (!s || !length) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s, (int)length, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, (int)length, out.data(), n, nullptr, nullptr);
    return out;
}

std::string Narrow(const wchar_t* s) {
    return s ? Narrow(s, wcslen(s)) : std::string();
}

}  // namespace dxinsp
