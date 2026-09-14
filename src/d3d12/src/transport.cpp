#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include "transport.h"

#include "common.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace dxinsp {

static const uint16_t kDefaultPort = 47531;

struct Transport::Impl {
    std::thread listener;
    std::thread sender;
    std::thread receiver;
    std::atomic<bool> connected{false};
    std::atomic<bool> stop{false};
    SOCKET client = INVALID_SOCKET;

    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::condition_variable drainedCv;
    std::deque<std::string> queue;   // fully framed messages
    size_t queuedBytes = 0;
    bool sending = false;

    std::mutex handlerMutex;
    std::function<void(const std::string&)> handler;
    std::function<void()> onConnect;
    std::function<void()> onDisconnect;

    uint16_t port = kDefaultPort;
    /** Whether DXINSP_PORT named the port, which means it may not be stepped off. */
    bool portFromConfig = false;

    void Enqueue(std::string frame) {
        std::lock_guard<std::mutex> lock(queueMutex);
        // Live-object traffic with nobody connected is dropped: the snapshot is sent on connect.
        if (!connected) return;
        queuedBytes += frame.size();
        queue.push_back(std::move(frame));
        queueCv.notify_one();
    }

    static bool SendAll(SOCKET s, const char* data, size_t size) {
        while (size > 0) {
            int n = send(s, data, (int)(size < (1u << 20) ? size : (1u << 20)), 0);
            if (n <= 0) return false;
            data += n;
            size -= (size_t)n;
        }
        return true;
    }

    static bool RecvAll(SOCKET s, char* data, size_t size) {
        while (size > 0) {
            int n = recv(s, data, (int)(size < (1u << 20) ? size : (1u << 20)), 0);
            if (n <= 0) return false;
            data += n;
            size -= (size_t)n;
        }
        return true;
    }

    void SenderLoop() {
        while (!stop) {
            std::string frame;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCv.wait(lock, [&] { return stop || !queue.empty() || !connected; });
                if (stop) return;
                if (!connected) { queue.clear(); queuedBytes = 0; drainedCv.notify_all(); continue; }
                frame = std::move(queue.front());
                queue.pop_front();
                queuedBytes -= frame.size();
                sending = true;
            }
            bool ok = SendAll(client, frame.data(), frame.size());
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                sending = false;
                if (queue.empty()) drainedCv.notify_all();
            }
            if (!ok) {
                Log("send failed; disconnecting");
                Disconnect();
            }
        }
    }

    void ReceiverLoop(SOCKET s) {
        while (!stop && connected && client == s) {
            uint8_t hdr[5];
            if (!RecvAll(s, (char*)hdr, 5)) break;
            uint32_t len = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
            uint8_t kind = hdr[4];
            std::string payload(len, '\0');
            if (len && !RecvAll(s, payload.data(), len)) break;
            if (kind == 0) {
                std::function<void(const std::string&)> h;
                { std::lock_guard<std::mutex> lock(handlerMutex); h = handler; }
                if (h) h(payload);
            }
        }
        Disconnect();
    }

    void Disconnect() {
        bool was = connected.exchange(false);
        if (!was) return;
        SOCKET s = client;
        client = INVALID_SOCKET;
        if (s != INVALID_SOCKET) closesocket(s);
        queueCv.notify_all();
        std::function<void()> h;
        { std::lock_guard<std::mutex> lock(handlerMutex); h = onDisconnect; }
        if (h) h();
        Log("client disconnected");
    }

    // The address is usually still held by the previous instance of the application, which the
    // launcher stopped a moment ago and which has not finished dying: keep trying for a while.
    bool BindWithRetry(SOCKET s, const sockaddr* addr, int len, const char* name) {
        int err = 0;
        for (int attempt = 0; attempt < 120 && !stop; ++attempt) {   // 30 s
            if (bind(s, addr, len) == 0) {
                if (attempt) Log("bind(%s) succeeded after %d retries", name, attempt);
                return true;
            }
            err = WSAGetLastError();
            if (err != WSAEADDRINUSE) break;
            if (attempt == 0) Log("bind(%s): address in use (the previous instance is still shutting down?); retrying", name);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        LogAlways("bind(%s) failed (%d)", name, err);
        return false;
    }

    /**
     * Whether something is already listening on this port, which SO_REUSEADDR hides: on Windows
     * that option lets a second listener bind the same address, so bind() succeeds and the two
     * servers share the port, with a client reaching whichever the stack happens to route to.
     * Found for real twice: a Unity player beside a leftover test application, where the inspector
     * connected to the wrong one and reported its frame; and a Vulkan application whose driver
     * makes a D3D12 device, where both capture libraries live in the one process.
     *
     * The table is read rather than probed with a connect. A connect would be answered by the
     * server we are looking for, and these servers take one client at a time, so probing would
     * throw the inspector off its own connection. It also tells a live listener from a closed
     * connection still in TIME_WAIT, which a bind conflict cannot (that is what BindWithRetry is
     * for) and which SO_REUSEADDR is there to allow.
     */
    static bool PortIsServed(uint16_t p) {
        ULONG size = 0;
        if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER) {
            return false;   // Unreadable: bind anyway, which is what this did before.
        }
        std::vector<char> buffer(size);
        if (GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) return false;
        const MIB_TCPTABLE_OWNER_PID* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const MIB_TCPROW_OWNER_PID& row = table->table[i];
            // The table holds the port in network order in the low half of the field.
            if ((row.dwLocalPort & 0xFFFF) != (ULONG)htons(p)) continue;
            // Ours binds the loopback address; a wildcard listener covers it too.
            if (row.dwLocalAddr == (ULONG)htonl(INADDR_LOOPBACK) || row.dwLocalAddr == 0) return true;
        }
        return false;
    }

    void ListenerLoop() {
        // A port the user named is used as given: moving off it would leave whoever chose it
        // waiting on the wrong one. Only the default may step aside, so two applications started
        // by hand are both inspectable.
        if (PortIsServed(port)) {
            if (portFromConfig) {
                LogAlways("127.0.0.1:%u is already served by another inspected application; "
                          "set DXINSP_PORT to a free port for this one", port);
                return;
            }
            uint16_t free = 0;
            for (uint16_t candidate = port + 1; candidate < port + 9 && candidate > port; ++candidate) {
                if (!PortIsServed(candidate)) { free = candidate; break; }
            }
            if (!free) {
                LogAlways("127.0.0.1:%u and the eight ports above it are all served by other "
                          "inspected applications; set DXINSP_PORT to a free port", port);
                return;
            }
            LogAlways("127.0.0.1:%u is already served by another inspected application; "
                      "listening on %u instead (connect the inspector to that port)", port, free);
            port = free;
        }

        SOCKET listenSock = socket(AF_INET, SOCK_STREAM, 0);
        if (listenSock == INVALID_SOCKET) { LogAlways("socket() failed"); return; }
        int one = 1;
        setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        char name[32];
        snprintf(name, sizeof(name), "127.0.0.1:%u", port);
        if (!BindWithRetry(listenSock, (sockaddr*)&addr, sizeof(addr), name)) { closesocket(listenSock); return; }
        if (listen(listenSock, 1) != 0) { LogAlways("listen failed"); closesocket(listenSock); return; }
        LogAlways("listening on 127.0.0.1:%u", port);

        while (!stop) {
            SOCKET s = accept(listenSock, nullptr, nullptr);
            if (s == INVALID_SOCKET) continue;
            if (connected) {
                // One client at a time; replace the old connection.
                Disconnect();
                if (receiver.joinable()) receiver.join();
            }
            int nodelay = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
            client = s;
            connected = true;
            Log("client connected");
            std::function<void()> h;
            { std::lock_guard<std::mutex> lock(handlerMutex); h = onConnect; }
            // The snapshot goes into the queue before any new event is streamed.
            if (h) h();
            if (receiver.joinable()) receiver.join();
            receiver = std::thread([this, s] { ReceiverLoop(s); });
        }
        closesocket(listenSock);
    }
};

Transport& Transport::Get() {
    static Transport* instance = new Transport();
    return *instance;
}

void Transport::Start() {
    if (_impl) return;
    _impl = new Impl();
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    {
        int v = atoi(ConfigValue("DXINSP_PORT").c_str());
        if (v > 0 && v < 65536) { _impl->port = (uint16_t)v; _impl->portFromConfig = true; }
    }
    _impl->sender = std::thread([this] { _impl->SenderLoop(); });
    _impl->listener = std::thread([this] { _impl->ListenerLoop(); });
    _impl->sender.detach();
    _impl->listener.detach();
}

bool Transport::Connected() const {
    return _impl && _impl->connected;
}

static void AppendU32(std::string& frame, uint32_t v) {
    frame.push_back((char)(v & 0xff));
    frame.push_back((char)((v >> 8) & 0xff));
    frame.push_back((char)((v >> 16) & 0xff));
    frame.push_back((char)((v >> 24) & 0xff));
}

void Transport::SendJson(std::string json) {
    if (!_impl || !_impl->connected) return;
    std::string frame;
    frame.reserve(json.size() + 5);
    AppendU32(frame, (uint32_t)json.size());
    frame.push_back(0);
    frame += json;
    _impl->Enqueue(std::move(frame));
}

void Transport::SendBinary(std::string headerJson, const void* data, size_t size) {
    if (!_impl || !_impl->connected) return;
    std::string frame;
    frame.reserve(headerJson.size() + size + 9);
    AppendU32(frame, (uint32_t)(4 + headerJson.size() + size));
    frame.push_back(1);
    AppendU32(frame, (uint32_t)headerJson.size());
    frame += headerJson;
    frame.append(static_cast<const char*>(data), size);
    _impl->Enqueue(std::move(frame));
}

void Transport::Flush(uint32_t timeoutMs) {
    if (!_impl || !_impl->connected) return;
    std::unique_lock<std::mutex> lock(_impl->queueMutex);
    _impl->drainedCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return _impl->queue.empty() && !_impl->sending; });
}

void Transport::SetOnConnect(std::function<void()> handler) {
    if (!_impl) Start();
    std::lock_guard<std::mutex> lock(_impl->handlerMutex);
    _impl->onConnect = std::move(handler);
}

void Transport::SetOnDisconnect(std::function<void()> handler) {
    if (!_impl) Start();
    std::lock_guard<std::mutex> lock(_impl->handlerMutex);
    _impl->onDisconnect = std::move(handler);
}

void Transport::SetMessageHandler(std::function<void(const std::string&)> handler) {
    if (!_impl) Start();
    std::lock_guard<std::mutex> lock(_impl->handlerMutex);
    _impl->handler = std::move(handler);
}

}  // namespace dxinsp
