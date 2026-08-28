#include "gr4cp/runtime/stream_binding_allocator.hpp"

#include <format>
#include <stdexcept>

#if defined(__EMSCRIPTEN__)
#include <atomic>
#else
#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#endif

namespace gr4cp::runtime {

namespace {

#if defined(__EMSCRIPTEN__)

int reserve_ephemeral_loopback_port() {
    static std::atomic<int> next_port{49152};

    const int port = next_port.fetch_add(1);
    if (port > 65535) {
        throw std::runtime_error("exhausted synthetic stream ports for the WASM build");
    }
    return port;
}

#else

class SocketHandle {
public:
    SocketHandle() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            throw std::runtime_error("failed to create stream binding allocation socket");
        }
    }

    ~SocketHandle() {
        if (fd_ >= 0) {
#if defined(_WIN32)
            ::closesocket(fd_);
#else
            ::close(fd_);
#endif
        }
    }

    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    int get() const {
        return fd_;
    }

private:
#if defined(_WIN32)
    SOCKET fd_{INVALID_SOCKET};
#else
    int fd_{-1};
#endif
};

int reserve_ephemeral_loopback_port() {
    SocketHandle socket;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;

    if (::bind(socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        throw std::runtime_error("failed to bind ephemeral loopback port for stream");
    }

    sockaddr_in bound{};
    socklen_t bound_size = sizeof(bound);
    if (::getsockname(socket.get(), reinterpret_cast<sockaddr*>(&bound), &bound_size) != 0) {
        throw std::runtime_error("failed to inspect allocated stream port");
    }

    return ntohs(bound.sin_port);
}

#endif  // defined(__EMSCRIPTEN__)

}  // namespace

domain::InternalStreamBinding StreamBindingAllocator::allocate_http() {
    std::lock_guard lock(mutex_);

    for (int attempt = 0; attempt < 64; ++attempt) {
        const auto port = reserve_ephemeral_loopback_port();
        if (!allocated_ports_.insert(port).second) {
            continue;
        }

        auto path = std::string("/snapshot");
        return domain::InternalStreamBinding{
            .protocol = "http",
            .host = "127.0.0.1",
            .port = port,
            .path = path,
            .endpoint = std::format("http://127.0.0.1:{}{}", port, path),
        };
    }

    throw std::runtime_error("failed to allocate unique internal HTTP stream binding");
}

domain::InternalStreamBinding StreamBindingAllocator::allocate_websocket() {
    std::lock_guard lock(mutex_);

    for (int attempt = 0; attempt < 64; ++attempt) {
        const auto port = reserve_ephemeral_loopback_port();
        if (!allocated_ports_.insert(port).second) {
            continue;
        }

        const auto path = std::string("/stream");
        return domain::InternalStreamBinding{
            .protocol = "websocket",
            .host = "127.0.0.1",
            .port = port,
            .path = path,
            .endpoint = std::format("ws://127.0.0.1:{}{}", port, path),
        };
    }

    throw std::runtime_error("failed to allocate unique internal websocket stream binding");
}

void StreamBindingAllocator::release(const domain::InternalStreamBinding& binding) {
    std::lock_guard lock(mutex_);
    allocated_ports_.erase(binding.port);
}

}  // namespace gr4cp::runtime
