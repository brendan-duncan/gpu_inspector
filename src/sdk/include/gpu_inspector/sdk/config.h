// GPU Inspector plugin SDK: where a capture library reads its settings from.
//
// The inspector configures a library through environment variables (plugin.json's `capture.<platform>.env`,
// with ${port} and the rest filled in; docs/PLUGINS.md). Two cases need more than getenv:
//   - Windows, a process the inspector did not start (a watch, dxinsp_launch.exe --watch): the launcher
//     cannot set its environment, so it hands the settings to the library's initializer as an
//     environment block instead (GpuInspectorInitialize's argument). ApplySettingsBlock takes them.
//   - Android: an application inherits no environment. Each variable is read from a system property
//     instead, "debug.<prefix>.<rest in lower case>" (GLESINSP_PORT -> debug.glesinsp.port), which
//     `adb shell setprop` sets without root.
#pragma once

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace gpuinsp::sdk {

class Config {
public:
    static Config& Get() {
        static Config* instance = new Config();
        return *instance;
    }

    /** A setting given directly (the injector's block), which wins over the environment. */
    void Set(const std::string& name, const std::string& value) {
        std::lock_guard lock(_mutex);
        _values[name] = value;
    }

    /** The setting's value: given directly, else the environment (Android: the system property). "" when unset. */
    std::string Value(const char* name) {
        {
            std::lock_guard lock(_mutex);
            auto it = _values.find(name);
            if (it != _values.end()) return it->second;
        }
#if defined(__ANDROID__)
        // GLESINSP_PORT -> debug.glesinsp.port: the prefix before the first underscore, then the rest.
        std::string prop = "debug.";
        const char* underscore = strchr(name, '_');
        for (const char* p = name; *p; ++p) {
            if (p == underscore) prop.push_back('.');
            else prop.push_back((char)tolower((unsigned char)*p));
        }
        char value[PROP_VALUE_MAX] = {};
        int len = __system_property_get(prop.c_str(), value);
        if (len > 0) return std::string(value, (size_t)len);
#endif
        const char* v = getenv(name);
        return v ? std::string(v) : std::string();
    }

    /** A flag: set and not "0". */
    bool Flag(const char* name) {
        const std::string v = Value(name);
        return !v.empty() && v != "0";
    }

    /** A number, or `def` when unset or not one. */
    long Number(const char* name, long def) {
        const std::string v = Value(name);
        if (v.empty()) return def;
        char* end = nullptr;
        long n = strtol(v.c_str(), &end, 10);
        return end && *end == 0 ? n : def;
    }

    /**
     * The launcher's settings for a process it did not start: NAME=VALUE entries of wide characters,
     * each terminated by a null and the block by another, exactly as a Win32 environment block is
     * written; null when there are none. Returns the names applied, for the log.
     */
    std::string ApplySettingsBlock(const wchar_t* block) {
        std::string applied;
        if (!block) return applied;
        constexpr size_t kMaxEntries = 64;
        constexpr size_t kMaxChars = 16 * 1024;
        size_t chars = 0;
        for (size_t n = 0; n < kMaxEntries && *block; ++n) {
            size_t length = 0;
            while (length < kMaxChars - chars && block[length]) ++length;
            if (length == 0 || length >= kMaxChars - chars) break;
            std::string entry;
            for (size_t i = 0; i < length; ++i) entry.push_back(block[i] < 0x80 ? (char)block[i] : '?');
            const size_t eq = entry.find('=');
            if (eq != std::string::npos && eq > 0) {
                Set(entry.substr(0, eq), entry.substr(eq + 1));
                if (!applied.empty()) applied += ' ';
                applied += entry.substr(0, eq);
            }
            block += length + 1;
            chars += length + 1;
        }
        return applied;
    }

private:
    std::mutex _mutex;
    std::map<std::string, std::string> _values;
};

}  // namespace gpuinsp::sdk
