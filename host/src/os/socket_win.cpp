#include "os/socket.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace malgrind::host::os {

static_assert(sizeof(std::uintptr_t) >= sizeof(SOCKET));
static_assert(static_cast<std::uintptr_t>(INVALID_SOCKET) == ~std::uintptr_t{0});

namespace {

constexpr std::uintptr_t kInvalid = ~std::uintptr_t{0};

struct WsaGuard {
    bool ok = false;
    WsaGuard() {
        WSADATA data{};
        ok = (::WSAStartup(MAKEWORD(2, 2), &data) == 0);
    }
    ~WsaGuard() {
        if (ok) ::WSACleanup();
    }
};

bool ensure_wsa() {
    static WsaGuard guard;
    return guard.ok;
}

SOCKET to_socket(std::uintptr_t handle) noexcept {
    return static_cast<SOCKET>(handle);
}

std::uintptr_t from_socket(SOCKET s) noexcept {
    return static_cast<std::uintptr_t>(s);
}

void close_handle(std::uintptr_t& handle) noexcept {
    if (handle != kInvalid) {
        ::closesocket(to_socket(handle));
        handle = kInvalid;
    }
}

} // namespace

TcpStream::TcpStream(TcpStream&& other) noexcept
    : handle_(std::exchange(other.handle_, kInvalidHandle)) {}

TcpStream& TcpStream::operator=(TcpStream&& other) noexcept {
    if (this != &other) {
        close_handle(handle_);
        handle_ = std::exchange(other.handle_, kInvalidHandle);
    }
    return *this;
}

TcpStream::~TcpStream() {
    close_handle(handle_);
}

std::optional<TcpStream> TcpStream::connect_loopback(std::uint16_t port, int timeout_ms) {
    if (!ensure_wsa()) return std::nullopt;
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return std::nullopt;

    u_long non_blocking = 1;
    if (::ioctlsocket(s, FIONBIO, &non_blocking) != 0) {
        ::closesocket(s);
        return std::nullopt;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const int rc = ::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (rc == SOCKET_ERROR) {
        const int err = ::WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
            ::closesocket(s);
            return std::nullopt;
        }
        WSAPOLLFD pfd{};
        pfd.fd = s;
        pfd.events = POLLWRNORM;
        const int prc = ::WSAPoll(&pfd, 1, timeout_ms);
        if (prc <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) || !(pfd.revents & POLLWRNORM)) {
            ::closesocket(s);
            return std::nullopt;
        }
        int so_err = 0;
        int err_len = sizeof(so_err);
        if (::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err), &err_len) != 0 || so_err != 0) {
            ::closesocket(s);
            return std::nullopt;
        }
    }

    non_blocking = 0;
    if (::ioctlsocket(s, FIONBIO, &non_blocking) != 0) {
        ::closesocket(s);
        return std::nullopt;
    }
    return TcpStream(from_socket(s));
}

std::optional<std::size_t> TcpStream::read(std::span<char> out, int timeout_ms) {
    if (!valid()) return std::nullopt;
    if (out.empty()) return std::size_t{0};

    WSAPOLLFD pfd{};
    pfd.fd = to_socket(handle_);
    pfd.events = POLLRDNORM;
    const int prc = ::WSAPoll(&pfd, 1, timeout_ms);
    if (prc == 0) return std::size_t{0};
    if (prc < 0) return std::nullopt;
    if (pfd.revents & (POLLERR | POLLNVAL)) return std::nullopt;
    if (!(pfd.revents & POLLRDNORM)) return std::nullopt;

    const int to_read = static_cast<int>(
        (std::min)<std::size_t>(out.size(), static_cast<std::size_t>((std::numeric_limits<int>::max)())));
    const int n = ::recv(to_socket(handle_), out.data(), to_read, 0);
    if (n <= 0) return std::nullopt;
    return static_cast<std::size_t>(n);
}

bool TcpStream::write_all(std::string_view data) {
    if (!valid()) return false;
    const SOCKET s = to_socket(handle_);
    while (!data.empty()) {
        const int chunk = static_cast<int>(
            (std::min)<std::size_t>(data.size(), static_cast<std::size_t>((std::numeric_limits<int>::max)())));
        const int n = ::send(s, data.data(), chunk, 0);
        if (n <= 0) return false;
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

TcpListener::TcpListener(TcpListener&& other) noexcept
    : handle_(std::exchange(other.handle_, kInvalidHandle)) {}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        close_handle(handle_);
        handle_ = std::exchange(other.handle_, kInvalidHandle);
    }
    return *this;
}

TcpListener::~TcpListener() {
    close_handle(handle_);
}

std::optional<TcpListener> TcpListener::bind_loopback(std::uint16_t port) {
    if (!ensure_wsa()) return std::nullopt;
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return std::nullopt;

    const BOOL exclusive = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(s, 8) != 0) {
        ::closesocket(s);
        return std::nullopt;
    }
    return TcpListener(from_socket(s));
}

std::uint16_t TcpListener::port() const {
    if (!valid()) return 0;
    sockaddr_in addr{};
    int len = sizeof(addr);
    if (::getsockname(to_socket(handle_), reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
    return ntohs(addr.sin_port);
}

std::optional<TcpStream> TcpListener::accept(int timeout_ms) {
    if (!valid()) return std::nullopt;
    WSAPOLLFD pfd{};
    pfd.fd = to_socket(handle_);
    pfd.events = POLLRDNORM;
    const int prc = ::WSAPoll(&pfd, 1, timeout_ms);
    if (prc <= 0) return std::nullopt;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return std::nullopt;
    if (!(pfd.revents & POLLRDNORM)) return std::nullopt;

    const SOCKET client = ::accept(to_socket(handle_), nullptr, nullptr);
    if (client == INVALID_SOCKET) return std::nullopt;
    return TcpStream(from_socket(client));
}

} // namespace malgrind::host::os
