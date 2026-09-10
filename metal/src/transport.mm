#include "transport.h"

#include "swizzle.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace mtlinsp {
namespace {

constexpr uint16_t kDefaultPort = 47531;
constexpr int kInvalidSocket = -1;

uint16_t PortFromEnvironment() {
    const char *value = getenv("MTLINSP_PORT");
    if (value == nullptr || value[0] == '\0') return kDefaultPort;
    const int port = atoi(value);
    return port > 0 && port < 65536 ? (uint16_t)port : kDefaultPort;
}

void AppendU32(std::string &frame, uint32_t value) {
    frame.push_back((char)(value & 0xff));
    frame.push_back((char)((value >> 8) & 0xff));
    frame.push_back((char)((value >> 16) & 0xff));
    frame.push_back((char)((value >> 24) & 0xff));
}

void AppendHeader(std::string &frame, uint32_t length, uint8_t kind) {
    AppendU32(frame, length);
    frame.push_back((char)kind);
}

/** One queued message: the framing and header, then an optional payload sent right after it. */
struct Outgoing {
    std::string head;
    std::vector<uint8_t> payload;
};

}  // namespace

struct Transport::Impl {
    std::atomic<bool> connected{false};
    std::atomic<bool> started{false};
    int client = kInvalidSocket;

    std::mutex queueMutex;
    std::condition_variable queueReady;
    std::condition_variable queueDrained;
    std::deque<Outgoing> queue;
    bool sending = false;

    std::function<void()> onConnect;
    std::function<void()> onDisconnect;
    std::function<void(const std::string &)> onMessage;

    void Enqueue(Outgoing frame) {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            queue.push_back(std::move(frame));
        }
        queueReady.notify_one();
    }

    bool SendAll(const void *data, size_t size) {
        const char *bytes = static_cast<const char *>(data);
        size_t sent = 0;
        while (sent < size) {
            const ssize_t n = send(client, bytes + sent, size - sent, 0);
            if (n <= 0) return false;
            sent += (size_t)n;
        }
        return true;
    }

    /** Drains the queue onto the socket until the connection drops. */
    void SendLoop() {
        while (connected) {
            Outgoing frame;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueReady.wait(lock, [this] { return !queue.empty() || !connected; });
                if (!connected) break;
                frame = std::move(queue.front());
                queue.pop_front();
                sending = true;
            }
            const bool ok = SendAll(frame.head.data(), frame.head.size())
                && SendAll(frame.payload.data(), frame.payload.size());
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                sending = false;
            }
            queueDrained.notify_all();
            if (!ok) {
                Log("send failed; disconnecting");
                connected = false;
                break;
            }
        }
        queueDrained.notify_all();
    }

    /** Reads length-prefixed frames from the client until it goes away. */
    void ReceiveLoop() {
        std::string pending;
        size_t consumed = 0;
        char buffer[4096];
        while (connected) {
            const ssize_t n = recv(client, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            pending.append(buffer, (size_t)n);
            for (;;) {
                if (pending.size() - consumed < 5) break;
                uint32_t length = 0;
                memcpy(&length, pending.data() + consumed, 4);
                if (pending.size() - consumed < 5 + length) break;
                const uint8_t kind = (uint8_t)pending[consumed + 4];
                if (kind == 0 && onMessage) onMessage(pending.substr(consumed + 5, length));
                consumed += 5 + length;
            }
            // Erased in one go rather than per message, which for a burst of messages was
            // quadratic in the buffer.
            if (consumed > 0) {
                pending.erase(0, consumed);
                consumed = 0;
            }
        }
        connected = false;
        queueReady.notify_all();
    }

    void Listen() {
        const uint16_t port = PortFromEnvironment();
        const int listener = socket(AF_INET, SOCK_STREAM, 0);
        if (listener == kInvalidSocket) {
            Log("socket() failed (%d)", errno);
            return;
        }
        const int reuse = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (bind(listener, (sockaddr *)&address, sizeof(address)) != 0) {
            Log("bind(127.0.0.1:%u) failed (%d)", port, errno);
            close(listener);
            return;
        }
        if (listen(listener, 1) != 0) {
            Log("listen failed (%d)", errno);
            close(listener);
            return;
        }
        Log("listening on 127.0.0.1:%u", port);

        for (;;) {
            const int accepted = accept(listener, nullptr, nullptr);
            if (accepted == kInvalidSocket) break;
            const int noDelay = 1;
            setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
            // A client that goes away mid-send raises SIGPIPE by default, which would take the
            // application down with it.
            const int noSigPipe = 1;
            setsockopt(accepted, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
            client = accepted;
            connected = true;
            Log("client connected");

            std::thread sender([this] { SendLoop(); });
            // The snapshot goes out before anything the hooks produce from here on, so the UI
            // sees every object exactly once whenever it happens to connect.
            if (onConnect) onConnect();
            ReceiveLoop();
            queueReady.notify_all();
            sender.join();

            close(client);
            client = kInvalidSocket;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                queue.clear();
            }
            if (onDisconnect) onDisconnect();
            Log("client disconnected");
        }
        close(listener);
    }
};

Transport &Transport::Get() {
    static Transport instance;
    return instance;
}

void Transport::Start() {
    if (impl_ == nullptr) impl_ = new Impl();
    if (impl_->started.exchange(true)) return;
    std::thread([this] { impl_->Listen(); }).detach();
}

bool Transport::Connected() const {
    return impl_ != nullptr && impl_->connected;
}

void Transport::SendJson(std::string json) {
    if (impl_ == nullptr || !impl_->connected) return;
    Outgoing frame;
    frame.head.reserve(json.size() + 5);
    AppendHeader(frame.head, (uint32_t)json.size(), 0);
    frame.head += json;
    impl_->Enqueue(std::move(frame));
}

void Transport::Flush(uint32_t timeoutMs) {
    if (impl_ == nullptr || !impl_->connected) return;
    std::unique_lock<std::mutex> lock(impl_->queueMutex);
    impl_->queueDrained.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
        return (impl_->queue.empty() && !impl_->sending) || !impl_->connected;
    });
}

void Transport::SendBinary(std::string headerJson, std::vector<uint8_t> payload) {
    if (impl_ == nullptr || !impl_->connected) return;
    Outgoing frame;
    frame.head.reserve(headerJson.size() + 9);
    AppendHeader(frame.head, (uint32_t)(4 + headerJson.size() + payload.size()), 1);
    AppendU32(frame.head, (uint32_t)headerJson.size());
    frame.head += headerJson;
    frame.payload = std::move(payload);
    impl_->Enqueue(std::move(frame));
}

void Transport::SendBinary(std::string headerJson, const void *data, size_t size) {
    if (impl_ == nullptr || !impl_->connected) return;
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    SendBinary(std::move(headerJson), std::vector<uint8_t>(bytes, bytes + size));
}

void Transport::SetOnConnect(std::function<void()> handler) {
    if (impl_ == nullptr) impl_ = new Impl();
    impl_->onConnect = std::move(handler);
}

void Transport::SetOnDisconnect(std::function<void()> handler) {
    if (impl_ == nullptr) impl_ = new Impl();
    impl_->onDisconnect = std::move(handler);
}

void Transport::SetMessageHandler(std::function<void(const std::string &)> handler) {
    if (impl_ == nullptr) impl_ = new Impl();
    impl_->onMessage = std::move(handler);
}

}  // namespace mtlinsp
