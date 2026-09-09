#include "transport.h"

#include "swizzle.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
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

void AppendHeader(std::string &frame, uint32_t length, uint8_t kind) {
    frame.push_back((char)(length & 0xff));
    frame.push_back((char)((length >> 8) & 0xff));
    frame.push_back((char)((length >> 16) & 0xff));
    frame.push_back((char)((length >> 24) & 0xff));
    frame.push_back((char)kind);
}

}  // namespace

struct Transport::Impl {
    std::atomic<bool> connected{false};
    std::atomic<bool> started{false};
    int client = kInvalidSocket;

    std::mutex queueMutex;
    std::condition_variable queueReady;
    std::deque<std::string> queue;

    std::function<void()> onConnect;
    std::function<void()> onDisconnect;
    std::function<void(const std::string &)> onMessage;

    void Enqueue(std::string frame) {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            queue.push_back(std::move(frame));
        }
        queueReady.notify_one();
    }

    /** Drains the queue onto the socket until the connection drops. */
    void SendLoop() {
        while (connected) {
            std::string frame;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueReady.wait(lock, [this] { return !queue.empty() || !connected; });
                if (!connected) break;
                frame = std::move(queue.front());
                queue.pop_front();
            }
            size_t sent = 0;
            while (sent < frame.size()) {
                const ssize_t n = send(client, frame.data() + sent, frame.size() - sent, 0);
                if (n <= 0) {
                    Log("send failed; disconnecting");
                    connected = false;
                    break;
                }
                sent += (size_t)n;
            }
        }
    }

    /** Reads length-prefixed frames from the client until it goes away. */
    void ReceiveLoop() {
        std::string pending;
        char buffer[4096];
        while (connected) {
            const ssize_t n = recv(client, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            pending.append(buffer, (size_t)n);
            for (;;) {
                if (pending.size() < 5) break;
                uint32_t length = 0;
                memcpy(&length, pending.data(), 4);
                if (pending.size() < 5 + length) break;
                const uint8_t kind = (uint8_t)pending[4];
                if (kind == 0 && onMessage) onMessage(pending.substr(5, length));
                pending.erase(0, 5 + length);
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
    std::string frame;
    frame.reserve(json.size() + 5);
    AppendHeader(frame, (uint32_t)json.size(), 0);
    frame += json;
    impl_->Enqueue(std::move(frame));
}

void Transport::SendBinary(std::string headerJson, const void *data, size_t size) {
    if (impl_ == nullptr || !impl_->connected) return;
    std::string frame;
    frame.reserve(headerJson.size() + size + 9);
    AppendHeader(frame, (uint32_t)(4 + headerJson.size() + size), 1);
    const uint32_t headerLength = (uint32_t)headerJson.size();
    frame.push_back((char)(headerLength & 0xff));
    frame.push_back((char)((headerLength >> 8) & 0xff));
    frame.push_back((char)((headerLength >> 16) & 0xff));
    frame.push_back((char)((headerLength >> 24) & 0xff));
    frame += headerJson;
    frame.append(static_cast<const char *>(data), size);
    impl_->Enqueue(std::move(frame));
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
