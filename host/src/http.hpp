#pragma once

#include "os/socket.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace malgrind::host {

// Minimal one-connection-at-a-time HTTP/1.1 GET server on 127.0.0.1, enough for the viewer page.
class HttpServer {
public:
    struct Response {
        std::string content_type;
        std::string body;
    };
    using Handler = std::function<std::optional<Response>(std::string_view path, std::string_view query)>;

    HttpServer(unsigned port, Handler handler); // throws std::runtime_error if the port is unavailable
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return listener_.port(); }

private:
    void serve();

    os::TcpListener listener_;
    Handler handler_;
    std::atomic<bool> stop_{false};
    std::thread thread_; // std::jthread is still experimental in Apple's libc++
};

} // namespace malgrind::host
