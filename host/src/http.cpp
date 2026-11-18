#include "http.hpp"

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <stdexcept>
#include <utility>

namespace malgrind::host {

HttpServer::HttpServer(unsigned port, Handler handler) : handler_(std::move(handler)) {
    if (port > 65535) {
        throw std::runtime_error(std::format("cannot listen on 127.0.0.1:{}", port));
    }
    auto listener = os::TcpListener::bind_loopback(static_cast<std::uint16_t>(port));
    if (!listener) {
        throw std::runtime_error(std::format("cannot listen on 127.0.0.1:{}", port));
    }
    listener_ = std::move(*listener);
    thread_ = std::thread([this] { serve(); });
}

HttpServer::~HttpServer() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void HttpServer::serve() {
    while (!stop_) {
        auto client = listener_.accept(200);
        if (!client) continue;

        std::string request(8192, '\0');
        std::size_t got = 0;
        while (got < request.size()) {
            const auto n = client->read(std::span<char>(request.data() + got, request.size() - got), 1000);
            if (!n || *n == 0) break;
            got += *n;
            if (std::string_view(request.data(), got).find("\r\n\r\n") != std::string_view::npos) break;
        }
        const std::string_view head(request.data(), got);
        std::optional<Response> response;
        if (head.starts_with("GET ")) {
            const auto space = head.find(' ', 4);
            const auto target = space == std::string_view::npos ? head.substr(4) : head.substr(4, space - 4);
            const auto q = target.find('?');
            response = handler_(target.substr(0, q), q == std::string_view::npos ? "" : target.substr(q + 1));
        }
        if (response) {
            if (client->write_all(std::format("HTTP/1.1 200 OK\r\nContent-Type: {}\r\nContent-Length: {}\r\n"
                                              "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                                              response->content_type, response->body.size()))) {
                client->write_all(response->body);
            }
        } else {
            client->write_all("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        }
    }
}

} // namespace malgrind::host
