// Answering "which applications are inspectable right now?", for the inspector's attach list.
//
// The capture libraries listen on one port each, the first free one in a small range (kFirstPort
// and the seven above it), so several applications started by hand are all reachable without
// anybody choosing port numbers. To list them the inspector connects to each port in turn and
// sends a Probe frame; the library answers with a Target frame naming itself and closes the
// connection, without becoming the client.
//
// That handshake is why a probe is safe. These servers take one client at a time and replace the
// old connection on accept, so a bare connect -- the obvious way to ask "is anybody there?" --
// would throw the inspector off its own session. A connection is therefore not taken for a client
// until its first frame says it is one: Probe is answered and dropped, anything else attaches.
//
// Header-only and API-neutral, shared by all three backends the way json_writer.h is (see
// src/d3d12/CMakeLists.txt and src/metal/CMakeLists.txt, which put this directory on the include
// path for that reason).
#pragma once

#include "json_parse.h"
#include "json_writer.h"

#include <cstdint>
#include <cstdio>
#include <string>

#if defined(_WIN32)
// winsock2.h before windows.h: the other order pulls in winsock.h and the two do not agree.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <vector>
#elif defined(__APPLE__)
// The sockets as well as the executable path: PortIsServed's POSIX branch bind-tests a port, and
// macOS takes that branch (the Metal library calls it, src/metal/src/transport.mm).
#include <arpa/inet.h>
#include <mach-o/dyld.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace gpuinsp
{

/** The first port a capture library tries, and how many it may step through. */
static const uint16_t kFirstPort = 47531;
static const uint16_t kPortCount = 8;
static const uint16_t kLastPort = kFirstPort + kPortCount - 1;

/** The full path of the running executable, or "" when it cannot be read. */
inline std::string ExecutablePath()
{
#if defined(_WIN32)
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, (DWORD)sizeof(buf));
    return n > 0 && n < sizeof(buf) ? std::string(buf, n) : std::string();
#elif defined(__ANDROID__)
    // No /proc/self/exe worth showing (it is the zygote-forked app_process); the package name
    // from the command line is what the user recognizes.
    if (FILE* f = fopen("/proc/self/cmdline", "rb"))
    {
        char cmd[256] = {};
        size_t got = fread(cmd, 1, sizeof(cmd) - 1, f);
        fclose(f);
        cmd[got] = 0;
        return std::string(cmd);
    }
    return std::string();
#elif defined(__APPLE__)
    char buf[1024];
    uint32_t size = (uint32_t)sizeof(buf);
    return _NSGetExecutablePath(buf, &size) == 0 ? std::string(buf) : std::string();
#else
    char buf[1024];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf));
    return n > 0 ? std::string(buf, (size_t)n) : std::string();
#endif
}

/** The executable's name without its directory: what the attach list shows. */
inline std::string ExecutableName()
{
    const std::string full = ExecutablePath();
    const size_t slash = full.find_last_of("/\\");
    return slash == std::string::npos ? full : full.substr(slash + 1);
}

inline uint32_t CurrentProcessId()
{
#if defined(_WIN32)
    return (uint32_t)GetCurrentProcessId();
#else
    return (uint32_t)getpid();
#endif
}

/**
 * Whether something is already listening on this loopback port.
 *
 * On Windows the listening table is read rather than the port being bind-tested, because
 * SO_REUSEADDR -- which the listeners set, to get past a previous instance's leftovers -- lets a
 * second listener bind an address that is already served, so bind() succeeds and the two share
 * the port, with a client reaching whichever the stack happens to route to. That was found for
 * real twice: a Unity player beside a leftover test application, where the inspector connected to
 * the wrong one and reported its frame; and a Vulkan application whose driver makes a Direct3D 12
 * device, where both capture libraries live in the one process. The table also tells a live
 * listener from a closed connection still in TIME_WAIT, which a bind conflict cannot.
 *
 * Elsewhere a bind decides it, on a socket without SO_REUSEADDR so that a held address fails:
 * POSIX does not let two live listeners share a port, so the answer is exact except for a
 * leftover in TIME_WAIT, which costs at most a step to the next port.
 *
 * Never probed with a connect: a connect would be answered by the very server being looked for,
 * and these servers take one client at a time, so probing would throw the inspector off its own
 * connection. That is what the Probe handshake at the top of this file is for.
 */
inline bool PortIsServed(uint16_t port)
{
#if defined(_WIN32)
    ULONG size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER)
    {
        return false;   // Unreadable: bind anyway, which is what this did before.
    }
    std::vector<char> buffer(size);
    if (GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR)
        return false;
    const MIB_TCPTABLE_OWNER_PID* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i)
    {
        const MIB_TCPROW_OWNER_PID& row = table->table[i];
        // The table holds the port in network order in the low half of the field.
        if ((row.dwLocalPort & 0xFFFF) != (ULONG)htons(port))
            continue;
        // Ours binds the loopback address; a wildcard listener covers it too.
        if (row.dwLocalAddr == (ULONG)htonl(INADDR_LOOPBACK) || row.dwLocalAddr == 0)
            return true;
    }
    return false;
#elif defined(__ANDROID__)
    // Android listens on an abstract socket whose name carries the package, not on a TCP port, so
    // two packages on the same port number never collide and there is nothing to step off.
    (void)port;
    return false;
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool served = bind(s, (sockaddr*)&addr, sizeof(addr)) != 0;
    close(s);
    return served;
#endif
}

/** Whether a client's first frame is a probe rather than the start of a session. */
inline bool IsProbeRequest(const std::string& json)
{
    vkinsp::JsonValue msg;
    if (!vkinsp::JsonParser::Parse(json, msg))
        return false;
    return msg.GetString("action") == "Probe";
}

/**
 * The answer to a probe: what this application is, so the inspector can list it.
 *
 * `name` is the application's own name for itself when it has one (Vulkan's
 * VkApplicationInfo::pApplicationName), which is usually friendlier than the executable; it may
 * be empty, both because Direct3D 12 has nothing of the kind and because the probe can arrive
 * before the application has created its device. `busy` says an inspector is already attached,
 * which the list shows rather than hides: the connection would be taken from whoever holds it.
 */
inline std::string ProbeReply(const char* api, const std::string& name, uint16_t port, bool busy)
{
    vkinsp::JsonWriter w;
    w.BeginObject();
    w.Key("action");
    w.String("Target");
    w.Key("api");
    w.String(api);
    w.Key("name");
    w.String(name);
    w.Key("exe");
    w.String(ExecutableName());
    w.Key("pid");
    w.Uint(CurrentProcessId());
    w.Key("port");
    w.Uint(port);
    w.Key("busy");
    w.Boolean(busy);
    w.EndObject();
    return w.str();
}

}  // namespace gpuinsp
