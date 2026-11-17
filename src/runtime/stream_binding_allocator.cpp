#include "gr4cp/runtime/stream_binding_allocator.hpp"

#include <format>
#include <stdexcept>

#if defined(__EMSCRIPTEN__)
#include <atomic>
#else
#include <string>
#include <system_error>
#if defined(_WIN32)
#include <atomic>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <cerrno>
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

#if defined(_WIN32)
using native_socket_t = SOCKET;
using socket_length_t = int;
constexpr native_socket_t invalid_socket = INVALID_SOCKET;
#else
using native_socket_t = int;
using socket_length_t = socklen_t;
constexpr native_socket_t invalid_socket = -1;
#endif

// Why the last socket call failed. Winsock keeps this per thread and the next call may overwrite it, so it is read
// straight after the failing call.
std::string last_socket_error() {
#if defined(_WIN32)
    return std::system_category().message(::WSAGetLastError());
#else
    return std::generic_category().message(errno);
#endif
}

class SocketHandle {
public:
    SocketHandle() {
#if defined(_WIN32)
        // Winsock refuses every call until the process has started it. WSAStartup is reference counted, so a start
        // is harmless when cpp-httplib has already made one; the reference is kept for the life of the process, and a
        // start that fails is tried again by the next socket rather than remembered.
        static std::atomic<bool> winsock_started{false};
        if (!winsock_started.load()) {
            WSADATA data{};
            if (const int result = ::WSAStartup(MAKEWORD(2, 2), &data); result != 0) {
                throw std::runtime_error("failed to initialize Winsock for stream binding allocation: " +
                                         std::system_category().message(result));
            }
            winsock_started = true;
        }
#endif
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ == invalid_socket) {
            throw std::runtime_error("failed to create stream binding allocation socket: " + last_socket_error());
        }
    }

    ~SocketHandle() {
        if (fd_ != invalid_socket) {
#if defined(_WIN32)
            ::closesocket(fd_);
#else
            ::close(fd_);
#endif
        }
    }

    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    native_socket_t get() const {
        return fd_;
    }

private:
    native_socket_t fd_{invalid_socket};
};

int reserve_ephemeral_loopback_port() {
    SocketHandle socket;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;

    if (::bind(socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        throw std::runtime_error("failed to bind ephemeral loopback port for stream: " + last_socket_error());
    }

    sockaddr_in bound{};
    socket_length_t bound_size = sizeof(bound);
    if (::getsockname(socket.get(), reinterpret_cast<sockaddr*>(&bound), &bound_size) != 0) {
        throw std::runtime_error("failed to inspect allocated stream port: " + last_socket_error());
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
