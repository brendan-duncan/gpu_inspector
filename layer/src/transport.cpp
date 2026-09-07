#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "transport.h"

#include "layer.h"
#include "tracker.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#if defined(_WIN32)
typedef SOCKET socket_t;
#define INVALID_SOCK INVALID_SOCKET
static void CloseSocket(socket_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int socket_t;
#define INVALID_SOCK (-1)
static void CloseSocket(socket_t s) { close(s); }
#endif

namespace vkinsp {

static const uint16_t kDefaultPort = 47531;

struct Transport::Impl {
    std::thread listener;
    std::thread sender;
    std::thread receiver;
    std::atomic<bool> connected{false};
    std::atomic<bool> stop{false};
    socket_t client = INVALID_SOCK;

    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::deque<std::string> queue;  // fully framed messages
    size_t queuedBytes = 0;

    std::mutex handlerMutex;
    std::function<void(const std::string&)> handler;

    uint16_t port = kDefaultPort;

    void Enqueue(std::string frame) {
        std::lock_guard lock(queueMutex);
        // Drop live-object traffic if nobody is connected (snapshot is sent on connect).
        if (!connected) return;
        queuedBytes += frame.size();
        queue.push_back(std::move(frame));
        queueCv.notify_one();
    }

    static bool SendAll(socket_t s, const char* data, size_t size) {
        while (size > 0) {
            int n = send(s, data, (int)std::min<size_t>(size, 1 << 20), 0);
            if (n <= 0) return false;
            data += n;
            size -= (size_t)n;
        }
        return true;
    }

    static bool RecvAll(socket_t s, char* data, size_t size) {
        while (size > 0) {
            int n = recv(s, data, (int)std::min<size_t>(size, 1 << 20), 0);
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
                std::unique_lock lock(queueMutex);
                queueCv.wait(lock, [&] { return stop || !queue.empty() || !connected; });
                if (stop) return;
                if (!connected) { queue.clear(); queuedBytes = 0; continue; }
                frame = std::move(queue.front());
                queue.pop_front();
                queuedBytes -= frame.size();
            }
            if (!SendAll(client, frame.data(), frame.size())) {
                Log("send failed; disconnecting");
                Disconnect();
            }
        }
    }

    void ReceiverLoop(socket_t s) {
        while (!stop && connected && client == s) {
            uint8_t hdr[5];
            if (!RecvAll(s, (char*)hdr, 5)) break;
            uint32_t len = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
            uint8_t kind = hdr[4];
            std::string payload(len, '\0');
            if (len && !RecvAll(s, payload.data(), len)) break;
            if (kind == 0) {
                std::function<void(const std::string&)> h;
                { std::lock_guard lock(handlerMutex); h = handler; }
                if (h) h(payload);
            }
        }
        Disconnect();
    }

    void Disconnect() {
        bool was = connected.exchange(false);
        if (!was) return;
        socket_t s = client;
        client = INVALID_SOCK;
        if (s != INVALID_SOCK) CloseSocket(s);
        queueCv.notify_all();
        Tracker::Get().OnDisconnect();
        Log("client disconnected");
    }

    void ListenerLoop() {
        socket_t listenSock = socket(AF_INET, SOCK_STREAM, 0);
        if (listenSock == INVALID_SOCK) { Log("socket() failed"); return; }
        int one = 1;
        setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) != 0) {
            Log("bind(%u) failed", port);
            CloseSocket(listenSock);
            return;
        }
        if (listen(listenSock, 1) != 0) { Log("listen failed"); CloseSocket(listenSock); return; }
        Log("listening on 127.0.0.1:%u", port);

        while (!stop) {
            socket_t s = accept(listenSock, nullptr, nullptr);
            if (s == INVALID_SOCK) continue;
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
            // Snapshot the live objects into the queue before any new events are streamed.
            Tracker::Get().SendSnapshot();
            if (receiver.joinable()) receiver.join();
            receiver = std::thread([this, s] { ReceiverLoop(s); });
        }
        CloseSocket(listenSock);
    }
};

Transport& Transport::Get() {
    static Transport* instance = new Transport();
    return *instance;
}

void Transport::Start() {
    if (_impl) return;
    _impl = new Impl();
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    if (const char* p = getenv("VKINSP_PORT")) {
        int v = atoi(p);
        if (v > 0 && v < 65536) _impl->port = (uint16_t)v;
    }
    _impl->sender = std::thread([this] { _impl->SenderLoop(); });
    _impl->listener = std::thread([this] { _impl->ListenerLoop(); });
    _impl->sender.detach();
    _impl->listener.detach();
}

bool Transport::Connected() const {
    return _impl && _impl->connected;
}

static void AppendHeader(std::string& frame, uint32_t len, uint8_t kind) {
    frame.push_back((char)(len & 0xff));
    frame.push_back((char)((len >> 8) & 0xff));
    frame.push_back((char)((len >> 16) & 0xff));
    frame.push_back((char)((len >> 24) & 0xff));
    frame.push_back((char)kind);
}

void Transport::SendJson(std::string json) {
    if (!_impl || !_impl->connected) return;
    std::string frame;
    frame.reserve(json.size() + 5);
    AppendHeader(frame, (uint32_t)json.size(), 0);
    frame += json;
    _impl->Enqueue(std::move(frame));
}

void Transport::SendBinary(std::string headerJson, const void* data, size_t size) {
    if (!_impl || !_impl->connected) return;
    std::string frame;
    frame.reserve(headerJson.size() + size + 9);
    AppendHeader(frame, (uint32_t)(4 + headerJson.size() + size), 1);
    uint32_t hl = (uint32_t)headerJson.size();
    frame.push_back((char)(hl & 0xff));
    frame.push_back((char)((hl >> 8) & 0xff));
    frame.push_back((char)((hl >> 16) & 0xff));
    frame.push_back((char)((hl >> 24) & 0xff));
    frame += headerJson;
    frame.append(static_cast<const char*>(data), size);
    _impl->Enqueue(std::move(frame));
}

void Transport::SetMessageHandler(std::function<void(const std::string&)> handler) {
    if (!_impl) Start();
    std::lock_guard lock(_impl->handlerMutex);
    _impl->handler = std::move(handler);
}

} // namespace vkinsp
