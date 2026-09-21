// GPU Inspector plugin SDK: the connection to the inspector.
//
// A capture library is a server: it listens, the inspector connects. The wire is the one every
// built-in capture library speaks (src/vulkan/src/transport.h):
//   u32 payloadLength (little endian), u8 kind, payload
//   kind 0: UTF-8 JSON text
//   kind 1: u32 headerLength, JSON header, raw bytes (read-backs: CaptureTextureData, CaptureBufferData)
//
// Where it listens:
//   - Desktop: 127.0.0.1, on the port the inspector gave (the library's PORT setting), or when none was
//     given on the first free one of the range every capture library shares (kFirstPort and the seven
//     above), so the inspector's attach list finds it by probing those ports.
//   - Android: the abstract Unix socket "@<prefix>:<port>:<package>" (an application rarely has the
//     INTERNET permission a TCP socket needs); the inspector reaches it through
//     `adb forward tcp:<port> localabstract:<prefix>:<port>:<package>`.
//
// One client at a time; a new one replaces the old. A connection is not taken for a client until its
// first frame says it is one: a Probe is answered with a Target naming the application and dropped,
// which is how the attach list asks what is running without throwing anyone off their session.
//
// Messages are queued and written by a thread of the server's, so an API call never waits on the
// network. Nothing is queued while nobody is connected: the library sends its whole state (Snapshot,
// then AddObject for every live object) from the connect callback instead.
//
// Header-only. On Windows it needs ws2_32 and iphlpapi, which MSVC links by the pragmas below.
#pragma once

#include "json.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#if defined(_MSC_VER)
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#endif
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace gpuinsp::sdk {

/** The ports capture libraries share when the inspector named none: the attach list probes these. */
static const uint16_t kFirstPort = 47531;
static const uint16_t kLastPort = kFirstPort + 7;

namespace detail {

#if defined(_WIN32)
using socket_t = SOCKET;
static const socket_t kInvalidSocket = INVALID_SOCKET;
inline void CloseSocket(socket_t s) { closesocket(s); }
inline int LastSocketError() { return WSAGetLastError(); }
inline bool AddressInUse(int e) { return e == WSAEADDRINUSE; }
#else
using socket_t = int;
static const socket_t kInvalidSocket = -1;
inline void CloseSocket(socket_t s) { close(s); }
inline int LastSocketError() { return errno; }
inline bool AddressInUse(int e) { return e == EADDRINUSE; }
#endif

inline std::string ExecutablePath() {
#if defined(_WIN32)
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, (DWORD)sizeof(buf));
    return n > 0 && n < sizeof(buf) ? std::string(buf, n) : std::string();
#elif defined(__ANDROID__)
    if (FILE* f = fopen("/proc/self/cmdline", "rb")) {
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

inline std::string ExecutableName() {
    const std::string full = ExecutablePath();
    const size_t slash = full.find_last_of("/\\");
    return slash == std::string::npos ? full : full.substr(slash + 1);
}

inline uint32_t ProcessId() {
#if defined(_WIN32)
    return (uint32_t)GetCurrentProcessId();
#else
    return (uint32_t)getpid();
#endif
}

/**
 * Whether something already listens on this loopback port. Windows reads the listening table rather
 * than bind-testing, since SO_REUSEADDR lets a second listener bind a served address and share it
 * (src/vulkan/src/target_probe.h has the whole story); elsewhere a bind without SO_REUSEADDR decides.
 * Never a connect: that would be answered by the very server being looked for.
 */
inline bool PortIsServed(uint16_t port) {
#if defined(_WIN32)
    ULONG size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER) return false;
    std::vector<char> buffer(size);
    if (GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) return false;
    const MIB_TCPTABLE_OWNER_PID* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const MIB_TCPROW_OWNER_PID& row = table->table[i];
        if ((row.dwLocalPort & 0xFFFF) != (ULONG)htons(port)) continue;
        if (row.dwLocalAddr == (ULONG)htonl(INADDR_LOOPBACK) || row.dwLocalAddr == 0) return true;
    }
    return false;
#elif defined(__ANDROID__)
    (void)port;
    return false;
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool served = bind(s, (sockaddr*)&addr, sizeof(addr)) != 0;
    close(s);
    return served;
#endif
}

inline void AppendHeader(std::string& frame, uint32_t len, uint8_t kind) {
    frame.push_back((char)(len & 0xff));
    frame.push_back((char)((len >> 8) & 0xff));
    frame.push_back((char)((len >> 16) & 0xff));
    frame.push_back((char)((len >> 24) & 0xff));
    frame.push_back((char)kind);
}

}  // namespace detail

struct ServerOptions {
    /** The API as the attach list names it: "OpenGL ES". */
    std::string api;
    /** Android's abstract socket name starts with this: "glesinsp". */
    std::string socketPrefix;
    /** The port the inspector gave, or 0 for the first free one of the shared range. */
    uint16_t port = 0;
    /** Where the server's own messages go (connections, failures). */
    std::function<void(const std::string&)> log;
};

class Server {
public:
    static Server& Get() {
        static Server* instance = new Server();
        return *instance;
    }

    /** Starts listening, on threads of its own. Only the first call does anything. */
    void Start(ServerOptions options) {
        std::lock_guard lock(_startMutex);
        if (_started) return;
        _started = true;
        _options = std::move(options);
        _portFromConfig = _options.port != 0;
        _port = _options.port ? _options.port : kFirstPort;
#if defined(_WIN32)
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        std::thread([this] { SenderLoop(); }).detach();
        std::thread([this] { ListenerLoop(); }).detach();
    }

    bool Connected() const { return _connected; }
    uint16_t Port() const { return _port; }

    /** Called on the listener thread for each new client, before anything it sends is handled: send the state here. */
    void OnConnect(std::function<void()> f) { std::lock_guard lock(_handlerMutex); _onConnect = std::move(f); }
    /** Called on the receiver thread for each JSON message the client sends. */
    void OnMessage(std::function<void(const std::string&)> f) { std::lock_guard lock(_handlerMutex); _onMessage = std::move(f); }
    /** Called when the client goes away. */
    void OnDisconnect(std::function<void()> f) { std::lock_guard lock(_handlerMutex); _onDisconnect = std::move(f); }

    /** What the application calls itself, for the attach list (empty: its executable's name). */
    void SetTargetName(std::string name) {
        std::lock_guard lock(_nameMutex);
        _targetName = std::move(name);
    }

    void SendJson(const std::string& json) {
        if (!_connected) return;
        std::string frame;
        frame.reserve(json.size() + 5);
        detail::AppendHeader(frame, (uint32_t)json.size(), 0);
        frame += json;
        Enqueue(std::move(frame));
    }

    void SendBinary(const std::string& headerJson, const void* data, size_t size) {
        if (!_connected) return;
        std::string frame;
        frame.reserve(headerJson.size() + size + 9);
        detail::AppendHeader(frame, (uint32_t)(4 + headerJson.size() + size), 1);
        const uint32_t hl = (uint32_t)headerJson.size();
        frame.push_back((char)(hl & 0xff));
        frame.push_back((char)((hl >> 8) & 0xff));
        frame.push_back((char)((hl >> 16) & 0xff));
        frame.push_back((char)((hl >> 24) & 0xff));
        frame += headerJson;
        if (size) frame.append(static_cast<const char*>(data), size);
        Enqueue(std::move(frame));
    }

    /** Bytes waiting to be written: a library streaming a large capture may wait for this to fall. */
    size_t QueuedBytes() {
        std::lock_guard lock(_queueMutex);
        return _queuedBytes;
    }

private:
    Server() = default;

    void Log(const std::string& s) {
        if (_options.log) _options.log(s);
    }

    void Enqueue(std::string frame) {
        std::lock_guard lock(_queueMutex);
        if (!_connected) return;
        _queuedBytes += frame.size();
        _queue.push_back(std::move(frame));
        _queueCv.notify_one();
    }

    static bool SendAll(detail::socket_t s, const char* data, size_t size) {
        while (size > 0) {
            int n = send(s, data, (int)std::min<size_t>(size, 1 << 20), 0);
            if (n <= 0) return false;
            data += n;
            size -= (size_t)n;
        }
        return true;
    }

    static bool RecvAll(detail::socket_t s, char* data, size_t size) {
        while (size > 0) {
            int n = recv(s, data, (int)std::min<size_t>(size, 1 << 20), 0);
            if (n <= 0) return false;
            data += n;
            size -= (size_t)n;
        }
        return true;
    }

    static void SetRecvTimeout(detail::socket_t s, int ms) {
#if defined(_WIN32)
        DWORD t = (DWORD)ms;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
#else
        timeval t{};
        t.tv_sec = ms / 1000;
        t.tv_usec = (ms % 1000) * 1000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
#endif
    }

    static bool RecvFrame(detail::socket_t s, std::string& payload, uint8_t& kind, int timeoutMs) {
        SetRecvTimeout(s, timeoutMs);
        uint8_t hdr[5];
        bool ok = RecvAll(s, (char*)hdr, 5);
        if (ok) {
            const uint32_t len = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
            kind = hdr[4];
            payload.assign(len, '\0');
            ok = len == 0 || RecvAll(s, payload.data(), len);
        }
        SetRecvTimeout(s, 0);
        return ok;
    }

    std::string ProbeReply() {
        std::string name;
        {
            std::lock_guard lock(_nameMutex);
            name = _targetName;
        }
        JsonWriter w;
        w.BeginObject();
        w.Key("action"); w.String("Target");
        w.Key("api"); w.String(_options.api);
        w.Key("name"); w.String(name);
        w.Key("exe"); w.String(detail::ExecutableName());
        w.Key("pid"); w.Uint(detail::ProcessId());
        w.Key("port"); w.Uint(_port);
        w.Key("busy"); w.Boolean(_connected);
        w.EndObject();
        std::string frame;
        detail::AppendHeader(frame, (uint32_t)w.str().size(), 0);
        return frame + w.str();
    }

    void SenderLoop() {
        for (;;) {
            std::string frame;
            {
                std::unique_lock lock(_queueMutex);
                _queueCv.wait(lock, [&] { return !_queue.empty() || !_connected; });
                if (!_connected) {
                    _queue.clear();
                    _queuedBytes = 0;
                    _queueCv.wait(lock, [&] { return _connected.load(); });
                    continue;
                }
                frame = std::move(_queue.front());
                _queue.pop_front();
                _queuedBytes -= frame.size();
            }
            if (!SendAll(_client, frame.data(), frame.size())) {
                Log("send failed; disconnecting");
                Disconnect();
            }
        }
    }

    void Dispatch(const std::string& json) {
        std::function<void(const std::string&)> h;
        {
            std::lock_guard lock(_handlerMutex);
            h = _onMessage;
        }
        if (h) h(json);
    }

    void ReceiverLoop(detail::socket_t s) {
        while (_connected && _client == s) {
            std::string payload;
            uint8_t kind = 0;
            if (!RecvFrame(s, payload, kind, 0)) break;
            if (kind == 0) Dispatch(payload);
        }
        Disconnect();
    }

    void Disconnect() {
        if (!_connected.exchange(false)) return;
        detail::socket_t s = _client;
        _client = detail::kInvalidSocket;
        if (s != detail::kInvalidSocket) detail::CloseSocket(s);
        _queueCv.notify_all();
        std::function<void()> h;
        {
            std::lock_guard lock(_handlerMutex);
            h = _onDisconnect;
        }
        if (h) h();
        Log("client disconnected");
    }

    bool BindWithRetry(detail::socket_t s, const sockaddr* addr, socklen_t len, const std::string& name) {
        int err = 0;
        for (int attempt = 0; attempt < 120; ++attempt) {   // 30 s: a previous instance may still be dying
            if (bind(s, addr, len) == 0) return true;
            err = detail::LastSocketError();
            if (!detail::AddressInUse(err)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        Log("bind(" + name + ") failed (" + std::to_string(err) + ")");
        return false;
    }

    void ListenerLoop() {
#if defined(__ANDROID__)
        detail::socket_t listenSock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listenSock == detail::kInvalidSocket) { Log("socket() failed"); return; }
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::string name = _options.socketPrefix + ":" + std::to_string(_port);
        std::string package = detail::ExecutablePath();
        if (size_t colon = package.find(':'); colon != std::string::npos) package.resize(colon);
        if (!package.empty()) name += ":" + package;
        if (name.size() > sizeof(addr.sun_path) - 2) name.resize(sizeof(addr.sun_path) - 2);
        memcpy(addr.sun_path + 1, name.data(), name.size());   // sun_path[0] == 0: the abstract namespace
        const socklen_t addrLen = (socklen_t)(offsetof(sockaddr_un, sun_path) + 1 + name.size());
        if (!BindWithRetry(listenSock, (sockaddr*)&addr, addrLen, name)) { detail::CloseSocket(listenSock); return; }
        if (listen(listenSock, 1) != 0) { Log("listen failed"); detail::CloseSocket(listenSock); return; }
        Log("listening on the abstract socket @" + name);
#else
        if (detail::PortIsServed(_port)) {
            if (_portFromConfig) {
                Log("127.0.0.1:" + std::to_string(_port) + " is already served by another inspected application");
                return;
            }
            uint16_t free = 0;
            for (uint16_t p = (uint16_t)(_port + 1); p <= kLastPort; ++p) {
                if (!detail::PortIsServed(p)) { free = p; break; }
            }
            if (!free) { Log("every port of the shared range is served by another inspected application"); return; }
            _port = free;
        }
        detail::socket_t listenSock = socket(AF_INET, SOCK_STREAM, 0);
        if (listenSock == detail::kInvalidSocket) { Log("socket() failed"); return; }
        int one = 1;
        setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(_port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const std::string name = "127.0.0.1:" + std::to_string(_port);
        if (!BindWithRetry(listenSock, (sockaddr*)&addr, (socklen_t)sizeof(addr), name)) { detail::CloseSocket(listenSock); return; }
        if (listen(listenSock, 1) != 0) { Log("listen failed"); detail::CloseSocket(listenSock); return; }
        Log("listening on " + name);
#endif
        for (;;) {
            detail::socket_t s = accept(listenSock, nullptr, nullptr);
            if (s == detail::kInvalidSocket) continue;
            int nodelay = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
            // Not a client until its first frame says so: a Probe is answered and dropped.
            std::string first;
            uint8_t kind = 0;
            if (!RecvFrame(s, first, kind, 2000)) {
                detail::CloseSocket(s);
                continue;
            }
            JsonValue msg;
            if (kind == 0 && ParseJson(first, msg) && msg.GetString("action") == "Probe") {
                const std::string reply = ProbeReply();
                SendAll(s, reply.data(), reply.size());
                detail::CloseSocket(s);
                continue;
            }
            if (_connected) {
                Disconnect();
                if (_receiver.joinable()) _receiver.join();
            }
            {
                std::lock_guard lock(_queueMutex);
                _client = s;
                _connected = true;
            }
            _queueCv.notify_all();
            Log("client connected");
            std::function<void()> connect;
            {
                std::lock_guard lock(_handlerMutex);
                connect = _onConnect;
            }
            if (connect) connect();
            if (kind == 0) Dispatch(first);
            if (_receiver.joinable()) _receiver.join();
            _receiver = std::thread([this, s] { ReceiverLoop(s); });
        }
    }

    ServerOptions _options;
    std::mutex _startMutex;
    bool _started = false;
    bool _portFromConfig = false;
    std::atomic<uint16_t> _port{kFirstPort};

    std::atomic<bool> _connected{false};
    detail::socket_t _client = detail::kInvalidSocket;
    std::thread _receiver;

    std::mutex _queueMutex;
    std::condition_variable _queueCv;
    std::deque<std::string> _queue;
    size_t _queuedBytes = 0;

    std::mutex _handlerMutex;
    std::function<void()> _onConnect;
    std::function<void(const std::string&)> _onMessage;
    std::function<void()> _onDisconnect;

    std::mutex _nameMutex;
    std::string _targetName;
};

}  // namespace gpuinsp::sdk
