#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace malgrind::host::os {

class TcpStream {
public:
    TcpStream() = default;
    TcpStream(TcpStream&& other) noexcept;
    TcpStream& operator=(TcpStream&& other) noexcept;
    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;
    ~TcpStream();

    static std::optional<TcpStream> connect_loopback(std::uint16_t port, int timeout_ms);
    [[nodiscard]] std::optional<std::size_t> read(std::span<char> out, int timeout_ms); // 0 on timeout, nullopt when closed/error
    bool write_all(std::string_view data); // no SIGPIPE (MSG_NOSIGNAL / SO_NOSIGPIPE)
    [[nodiscard]] bool valid() const noexcept { return handle_ != kInvalidHandle; }

private:
    friend class TcpListener;
    static constexpr std::uintptr_t kInvalidHandle = ~std::uintptr_t{0};
    explicit TcpStream(std::uintptr_t handle) noexcept : handle_(handle) {}
    std::uintptr_t handle_ = kInvalidHandle;
};

class TcpListener {
public:
    TcpListener() = default;
    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    ~TcpListener();

    static std::optional<TcpListener> bind_loopback(std::uint16_t port); // 0 = any free port
    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] std::optional<TcpStream> accept(int timeout_ms);
    [[nodiscard]] bool valid() const noexcept { return handle_ != kInvalidHandle; }

private:
    static constexpr std::uintptr_t kInvalidHandle = ~std::uintptr_t{0};
    explicit TcpListener(std::uintptr_t handle) noexcept : handle_(handle) {}
    std::uintptr_t handle_ = kInvalidHandle;
};

} // namespace malgrind::host::os
