// Loopback TCP socket abstraction and HttpServer self-check.
#include "http.hpp"
#include "os/socket.hpp"

#include <array>
#include <cassert>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace malgrind::host;

namespace {

std::string read_until_eof(os::TcpStream& stream) {
    std::string out;
    std::array<char, 512> buf{};
    for (;;) {
        const auto n = stream.read(buf, 1000);
        if (!n) break;
        assert(*n > 0); // 1s timeout should not fire before peer sends/closes
        out.append(buf.data(), *n);
    }
    return out;
}

void tcp_loopback_echo_and_lifecycle_checks() {
    auto listener = os::TcpListener::bind_loopback(0);
    assert(listener.has_value());
    assert(listener->valid());
    const std::uint16_t port = listener->port();
    assert(port != 0);

    // No pending connection -> accept times out with nullopt.
    assert(!listener->accept(20).has_value());

    // Move TcpListener and verify handle transfers cleanly.
    os::TcpListener moved_listener = std::move(*listener);
    assert(!listener->valid());
    assert(moved_listener.valid());
    assert(moved_listener.port() == port);

    auto client = os::TcpStream::connect_loopback(port, 1000);
    assert(client.has_value());
    assert(client->valid());

    auto server_conn = moved_listener.accept(1000);
    assert(server_conn.has_value());
    assert(server_conn->valid());

    // Nothing sent yet -> read timeout returns 0 (not nullopt).
    std::array<char, 64> buf{};
    const auto timed_out = server_conn->read(buf, 30);
    assert(timed_out.has_value() && *timed_out == 0);

    // Echo client -> server -> client.
    const std::string_view msg = "hello malgrind socket";
    assert(client->write_all(msg));
    const auto got = server_conn->read(buf, 1000);
    assert(got.has_value() && *got == msg.size());
    assert(std::string_view(buf.data(), *got) == msg);

    assert(server_conn->write_all(std::string_view(buf.data(), *got)));
    const auto echo = client->read(buf, 1000);
    assert(echo.has_value() && *echo == msg.size());
    assert(std::string_view(buf.data(), *echo) == msg);

    // Closing one end causes read on the peer to return std::nullopt.
    client.reset();
    const auto eof = server_conn->read(buf, 1000);
    assert(!eof.has_value());

    // Writing to closed peer returns false (must not raise SIGPIPE).
    // Note: first write after peer close may succeed before RST arrives, second fails; ensure no crash.
    (void)server_conn->write_all("after-close");
}

void http_server_checks() {
    HttpServer server(0, [](std::string_view path, std::string_view query) -> std::optional<HttpServer::Response> {
        if (path == "/hello") {
            return HttpServer::Response{"text/plain", std::string("hi:") + std::string(query)};
        }
        return std::nullopt;
    });
    const std::uint16_t port = server.port();
    assert(port != 0);

    // Binding the same port again must throw std::runtime_error.
    bool threw = false;
    try {
        HttpServer duplicate(port, [](std::string_view, std::string_view) { return std::nullopt; });
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    // 200 OK on /hello?core=0
    {
        auto conn = os::TcpStream::connect_loopback(port, 1000);
        assert(conn.has_value());
        assert(conn->write_all("GET /hello?core=0 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
        const std::string resp = read_until_eof(*conn);
        assert(resp.find("HTTP/1.1 200 OK\r\n") == 0);
        assert(resp.find("Content-Type: text/plain\r\n") != std::string::npos);
        assert(resp.find("Content-Length: 9\r\n") != std::string::npos);
        assert(resp.ends_with("\r\n\r\nhi:core=0"));
    }

    // 404 Not Found on unknown path
    {
        auto conn = os::TcpStream::connect_loopback(port, 1000);
        assert(conn.has_value());
        assert(conn->write_all("GET /missing HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
        const std::string resp = read_until_eof(*conn);
        assert(resp == "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    }
}

} // namespace

int main() {
    tcp_loopback_echo_and_lifecycle_checks();
    http_server_checks();
    std::puts("os_socket ok");
}
