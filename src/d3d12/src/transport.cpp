#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "transport.h"

#include "cpu_sampler.h"

#include "common.h"
#include "target_probe.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace dxinsp
{

static const uint16_t kDefaultPort = gpuinsp::kFirstPort;
/** How long a new connection has to say whether it is a probe or a client (target_probe.h). */
static const int kHandshakeTimeoutMs = 2000;

struct Transport::Impl
{
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

    void Enqueue(std::string frame)
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        // Live-object traffic with nobody connected is dropped: the snapshot is sent on connect.
        if (!connected)
            return;
        queuedBytes += frame.size();
        queue.push_back(std::move(frame));
        queueCv.notify_one();
    }

    static bool SendAll(SOCKET s, const char* data, size_t size)
    {
        while (size > 0)
        {
            int n = send(s, data, (int)(size < (1u << 20) ? size : (1u << 20)), 0);
            if (n <= 0)
                return false;
            data += n;
            size -= (size_t)n;
        }
        return true;
    }

    static bool RecvAll(SOCKET s, char* data, size_t size)
    {
        while (size > 0)
        {
            int n = recv(s, data, (int)(size < (1u << 20) ? size : (1u << 20)), 0);
            if (n <= 0)
                return false;
            data += n;
            size -= (size_t)n;
        }
        return true;
    }

    void SenderLoop()
    {
        gpuinsp::CpuSampler::Get().ExcludeCurrentThread();   // the inspector's own, not the application's
        while (!stop)
        {
            std::string frame;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCv.wait(lock, [&] { return stop || !queue.empty() || !connected; });
                if (stop)
                    return;
                if (!connected)
                {
                    queue.clear();
                    queuedBytes = 0;
                    drainedCv.notify_all();
                    continue;
                }
                frame = std::move(queue.front());
                queue.pop_front();
                queuedBytes -= frame.size();
                sending = true;
            }
            bool ok = SendAll(client, frame.data(), frame.size());
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                sending = false;
                if (queue.empty())
                    drainedCv.notify_all();
            }
            if (!ok)
            {
                Log("send failed; disconnecting");
                Disconnect();
            }
        }
    }

    void ReceiverLoop(SOCKET s)
    {
        gpuinsp::CpuSampler::Get().ExcludeCurrentThread();
        while (!stop && connected && client == s)
        {
            uint8_t hdr[5];
            if (!RecvAll(s, (char*)hdr, 5))
                break;
            uint32_t len = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
            uint8_t kind = hdr[4];
            std::string payload(len, '\0');
            if (len && !RecvAll(s, payload.data(), len))
                break;
            if (kind == 0)
            {
                std::function<void(const std::string&)> h;
                {
                    std::lock_guard<std::mutex> lock(handlerMutex);
                    h = handler;
                }
                if (h)
                    h(payload);
            }
        }
        Disconnect();
    }

    void Disconnect()
    {
        bool was = connected.exchange(false);
        if (!was)
            return;
        SOCKET s = client;
        client = INVALID_SOCKET;
        if (s != INVALID_SOCKET)
            closesocket(s);
        queueCv.notify_all();
        std::function<void()> h;
        {
            std::lock_guard<std::mutex> lock(handlerMutex);
            h = onDisconnect;
        }
        if (h)
            h();
        Log("client disconnected");
    }

    // The address is usually still held by the previous instance of the application, which the
    // launcher stopped a moment ago and which has not finished dying: keep trying for a while.
    bool BindWithRetry(SOCKET s, const sockaddr* addr, int len, const char* name)
    {
        int err = 0;
        for (int attempt = 0; attempt < 120 && !stop; ++attempt)
        {   // 30 s
            if (bind(s, addr, len) == 0)
            {
                if (attempt)
                    Log("bind(%s) succeeded after %d retries", name, attempt);
                return true;
            }
            err = WSAGetLastError();
            if (err != WSAEADDRINUSE)
                break;
            if (attempt == 0)
                Log("bind(%s): address in use (the previous instance is still shutting down?); retrying", name);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        LogAlways("bind(%s) failed (%d)", name, err);
        return false;
    }

    static void SetRecvTimeout(SOCKET s, int ms)
    {
        DWORD t = (DWORD)ms;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
    }

    /** One frame, or false when the peer said nothing in time, closed, or framed it badly. */
    static bool RecvFrame(SOCKET s, std::string& payload, uint8_t& kind, int timeoutMs)
    {
        SetRecvTimeout(s, timeoutMs);
        uint8_t hdr[5];
        bool ok = RecvAll(s, (char*)hdr, 5);
        if (ok)
        {
            const uint32_t len = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
            kind = hdr[4];
            payload.assign(len, '\0');
            ok = len == 0 || RecvAll(s, payload.data(), len);
        }
        SetRecvTimeout(s, 0);   // back to blocking, for the receiver thread
        return ok;
    }

    /** A JSON frame written straight to a socket, bypassing the queue: the probe is not a client. */
    static bool SendJsonTo(SOCKET s, const std::string& json)
    {
        std::string frame;
        frame.reserve(json.size() + 5);
        const uint32_t len = (uint32_t)json.size();
        frame.push_back((char)(len & 0xff));
        frame.push_back((char)((len >> 8) & 0xff));
        frame.push_back((char)((len >> 16) & 0xff));
        frame.push_back((char)((len >> 24) & 0xff));
        frame.push_back((char)0);
        frame += json;
        return SendAll(s, frame.data(), frame.size());
    }

    void ListenerLoop()
    {
        gpuinsp::CpuSampler::Get().ExcludeCurrentThread();
        // A port the user named is used as given: moving off it would leave whoever chose it
        // waiting on the wrong one. Only the default may step aside, so two applications started
        // by hand are both inspectable.
        if (gpuinsp::PortIsServed(port))
        {
            if (portFromConfig)
            {
                LogAlways(
                    "127.0.0.1:%u is already served by another inspected application; "
                    "set DXINSP_PORT to a free port for this one",
                    port);
                return;
            }
            uint16_t free = 0;
            for (uint16_t candidate = (uint16_t)(port + 1); candidate <= gpuinsp::kLastPort; ++candidate)
            {
                if (!gpuinsp::PortIsServed(candidate))
                {
                    free = candidate;
                    break;
                }
            }
            if (!free)
            {
                LogAlways(
                    "127.0.0.1:%u and the ports above it are all served by other "
                    "inspected applications; set DXINSP_PORT to a free port",
                    port);
                return;
            }
            LogAlways(
                "127.0.0.1:%u is already served by another inspected application; "
                "listening on %u instead (connect the inspector to that port)",
                port, free);
            port = free;
        }

        SOCKET listenSock = socket(AF_INET, SOCK_STREAM, 0);
        if (listenSock == INVALID_SOCKET)
        {
            LogAlways("socket() failed");
            return;
        }
        int one = 1;
        setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        char name[32];
        snprintf(name, sizeof(name), "127.0.0.1:%u", port);
        if (!BindWithRetry(listenSock, (sockaddr*)&addr, sizeof(addr), name))
        {
            closesocket(listenSock);
            return;
        }
        if (listen(listenSock, 1) != 0)
        {
            LogAlways("listen failed");
            closesocket(listenSock);
            return;
        }
        LogAlways("listening on 127.0.0.1:%u", port);

        while (!stop)
        {
            SOCKET s = accept(listenSock, nullptr, nullptr);
            if (s == INVALID_SOCKET)
                continue;
            int nodelay = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

            // A connection is not a client until its first frame says so (target_probe.h): a
            // probe is answered and dropped, leaving whoever is attached where they are, and a
            // connection that says nothing at all is dropped rather than taking the session from
            // them. Both clients send Ping the moment they connect, so the wait is not felt.
            std::string first;
            uint8_t kind = 0;
            if (!RecvFrame(s, first, kind, kHandshakeTimeoutMs))
            {
                Log("a connection said nothing within %d ms; dropped", kHandshakeTimeoutMs);
                closesocket(s);
                continue;
            }
            if (kind == 0 && gpuinsp::IsProbeRequest(first))
            {
                // Direct3D 12 has no name of the application's own choosing; the executable is it.
                SendJsonTo(s, gpuinsp::ProbeReply("D3D12", std::string(), port, connected));
                closesocket(s);
                continue;
            }

            if (connected)
            {
                // One client at a time; replace the old connection.
                Disconnect();
                if (receiver.joinable())
                    receiver.join();
            }
            client = s;
            connected = true;
            Log("client connected");
            std::function<void()> h;
            {
                std::lock_guard<std::mutex> lock(handlerMutex);
                h = onConnect;
            }
            // The snapshot goes into the queue before any new event is streamed.
            if (h)
                h();
            // The frame that identified it as a client still has to be acted on; it goes after the
            // snapshot, exactly where the receiver thread would have put it.
            if (kind == 0)
            {
                std::function<void(const std::string&)> mh;
                {
                    std::lock_guard<std::mutex> lock(handlerMutex);
                    mh = handler;
                }
                if (mh)
                    mh(first);
            }
            if (receiver.joinable())
                receiver.join();
            receiver = std::thread([this, s] { ReceiverLoop(s); });
        }
        closesocket(listenSock);
    }
};

Transport& Transport::Get()
{
    static Transport* instance = new Transport();
    return *instance;
}

void Transport::Start()
{
    if (_impl)
        return;
    _impl = new Impl();
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    {
        int v = atoi(ConfigValue("DXINSP_PORT").c_str());
        if (v > 0 && v < 65536)
        {
            _impl->port = (uint16_t)v;
            _impl->portFromConfig = true;
        }
    }
    _impl->sender = std::thread([this] { _impl->SenderLoop(); });
    _impl->listener = std::thread([this] { _impl->ListenerLoop(); });
    _impl->sender.detach();
    _impl->listener.detach();
}

bool Transport::Connected() const
{
    return _impl && _impl->connected;
}

static void AppendU32(std::string& frame, uint32_t v)
{
    frame.push_back((char)(v & 0xff));
    frame.push_back((char)((v >> 8) & 0xff));
    frame.push_back((char)((v >> 16) & 0xff));
    frame.push_back((char)((v >> 24) & 0xff));
}

void Transport::SendJson(std::string json)
{
    if (!_impl || !_impl->connected)
        return;
    std::string frame;
    frame.reserve(json.size() + 5);
    AppendU32(frame, (uint32_t)json.size());
    frame.push_back(0);
    frame += json;
    _impl->Enqueue(std::move(frame));
}

void Transport::SendBinary(std::string headerJson, const void* data, size_t size)
{
    if (!_impl || !_impl->connected)
        return;
    std::string frame;
    frame.reserve(headerJson.size() + size + 9);
    AppendU32(frame, (uint32_t)(4 + headerJson.size() + size));
    frame.push_back(1);
    AppendU32(frame, (uint32_t)headerJson.size());
    frame += headerJson;
    frame.append(static_cast<const char*>(data), size);
    _impl->Enqueue(std::move(frame));
}

void Transport::Flush(uint32_t timeoutMs)
{
    if (!_impl || !_impl->connected)
        return;
    std::unique_lock<std::mutex> lock(_impl->queueMutex);
    _impl->drainedCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return _impl->queue.empty() && !_impl->sending; });
}

void Transport::SetOnConnect(std::function<void()> handler)
{
    if (!_impl)
        Start();
    std::lock_guard<std::mutex> lock(_impl->handlerMutex);
    _impl->onConnect = std::move(handler);
}

void Transport::SetOnDisconnect(std::function<void()> handler)
{
    if (!_impl)
        Start();
    std::lock_guard<std::mutex> lock(_impl->handlerMutex);
    _impl->onDisconnect = std::move(handler);
}

void Transport::SetMessageHandler(std::function<void(const std::string&)> handler)
{
    if (!_impl)
        Start();
    std::lock_guard<std::mutex> lock(_impl->handlerMutex);
    _impl->handler = std::move(handler);
}

}  // namespace dxinsp
