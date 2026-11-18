#include "os/socket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace malgrind::host::os {

static_assert(sizeof(std::uintptr_t) >= sizeof(int));

namespace {

constexpr std::uintptr_t kInvalid = ~std::uintptr_t{0};

int to_fd(std::uintptr_t handle) noexcept {
    return static_cast<int>(handle);
}

std::uintptr_t from_fd(int fd) noexcept {
    return fd < 0 ? kInvalid : static_cast<std::uintptr_t>(fd);
}

void set_cloexec(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

void set_nosigpipe(int fd) noexcept {
#ifdef SO_NOSIGPIPE
    const int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#else
    (void)fd;
#endif
}

int poll_single(int fd, short events, int timeout_ms, short* revents_out = nullptr) {
    using clock = std::chrono::steady_clock;
    const bool has_deadline = timeout_ms > 0;
    const auto deadline = has_deadline ? clock::now() + std::chrono::milliseconds(timeout_ms) : clock::time_point{};
    int remaining = timeout_ms;

    for (;;) {
        pollfd pfd{fd, events, 0};
        const int rc = ::poll(&pfd, 1, remaining);
        if (rc > 0) {
            if (revents_out) *revents_out = pfd.revents;
            return rc;
        }
        if (rc == 0) {
            if (revents_out) *revents_out = 0;
            return 0;
        }
        if (errno != EINTR) return -1;
        if (has_deadline) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
            if (left <= 0) {
                if (revents_out) *revents_out = 0;
                return 0;
            }
            remaining = static_cast<int>(left);
        }
    }
}

void close_handle(std::uintptr_t& handle) noexcept {
    if (handle != kInvalid) {
        ::close(to_fd(handle));
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
#ifdef SOCK_CLOEXEC
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) set_cloexec(fd);
#endif
    if (fd < 0) return std::nullopt;
    set_nosigpipe(fd);

    const int orig_flags = ::fcntl(fd, F_GETFL, 0);
    if (orig_flags < 0 || ::fcntl(fd, F_SETFL, orig_flags | O_NONBLOCK) != 0) {
        ::close(fd);
        return std::nullopt;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (rc != 0) {
        if (errno != EINPROGRESS) {
            ::close(fd);
            return std::nullopt;
        }
        short revents = 0;
        if (poll_single(fd, POLLOUT, timeout_ms, &revents) <= 0 || (revents & (POLLERR | POLLHUP | POLLNVAL))) {
            ::close(fd);
            return std::nullopt;
        }
        int so_err = 0;
        socklen_t err_len = sizeof(so_err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &err_len) != 0 || so_err != 0) {
            ::close(fd);
            return std::nullopt;
        }
    }

    if (::fcntl(fd, F_SETFL, orig_flags) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    return TcpStream(from_fd(fd));
}

std::optional<std::size_t> TcpStream::read(std::span<char> out, int timeout_ms) {
    if (!valid()) return std::nullopt;
    if (out.empty()) return std::size_t{0};

    const int fd = to_fd(handle_);
    short revents = 0;
    const int prc = poll_single(fd, POLLIN, timeout_ms, &revents);
    if (prc == 0) return std::size_t{0};
    if (prc < 0) return std::nullopt;
    if (revents & (POLLERR | POLLNVAL)) return std::nullopt;
    if (!(revents & POLLIN)) return std::nullopt;

    for (;;) {
        const ssize_t n = ::recv(fd, out.data(), out.size(), 0);
        if (n > 0) return static_cast<std::size_t>(n);
        if (n == 0) return std::nullopt;
        if (errno == EINTR) continue;
        return std::nullopt;
    }
}

bool TcpStream::write_all(std::string_view data) {
    if (!valid()) return false;
    const int fd = to_fd(handle_);
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    while (!data.empty()) {
        const ssize_t n = ::send(fd, data.data(), data.size(), flags);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
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
#ifdef SOCK_CLOEXEC
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) set_cloexec(fd);
#endif
    if (fd < 0) return std::nullopt;

    const int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, 8) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    return TcpListener(from_fd(fd));
}

std::uint16_t TcpListener::port() const {
    if (!valid()) return 0;
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(to_fd(handle_), reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
    return ntohs(addr.sin_port);
}

std::optional<TcpStream> TcpListener::accept(int timeout_ms) {
    if (!valid()) return std::nullopt;
    const int fd = to_fd(handle_);
    short revents = 0;
    const int prc = poll_single(fd, POLLIN, timeout_ms, &revents);
    if (prc <= 0) return std::nullopt;
    if (revents & (POLLERR | POLLHUP | POLLNVAL)) return std::nullopt;
    if (!(revents & POLLIN)) return std::nullopt;

    const int client = ::accept(fd, nullptr, nullptr);
    if (client < 0) return std::nullopt;
    set_cloexec(client);
    set_nosigpipe(client);
    return TcpStream(from_fd(client));
}

} // namespace malgrind::host::os
