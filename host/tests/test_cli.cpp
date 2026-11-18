// Terminal mode self-check: attach to a ByteSource (plus one POSIX pty check), read the live page over HTTP, stop, check history.
#include "cli.hpp"
#include "os/paths.hpp"
#include "os/socket.hpp"
#include "source.hpp"
#include "store.hpp"
#include "stream.hpp"

#ifndef _WIN32
#include "pty.hpp"
#endif

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using namespace malgrind::host;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

unsigned free_port() {
    auto listener = os::TcpListener::bind_loopback(0);
    assert(listener);
    return listener->port();
}

// Body of GET path, or "" if nothing listens (yet).
std::string http_get(unsigned port, const std::string& path) {
    auto conn = os::TcpStream::connect_loopback(static_cast<std::uint16_t>(port), 500);
    if (!conn) return {};
    const std::string request = "GET " + path + " HTTP/1.1\r\nHost: x\r\n\r\n";
    assert(conn->write_all(request));
    std::string response;
    std::array<char, 4096> buf{};
    for (;;) {
        const auto n = conn->read(buf, 1000);
        if (!n || *n == 0) break;
        response.append(buf.data(), *n);
    }
    const auto body = response.find("\r\n\r\n");
    return body == std::string::npos ? std::string{} : response.substr(body + 4);
}

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

void attach_shows_the_live_page_and_records(const std::string& device_flag) {
    const auto root = fs::temp_directory_path() / "malgrind_cli_a";
    fs::remove_all(root);
    fs::create_directories(root / "project");
    Device dev;
    dev.id = "usb-10c4-ea60-sn-0001";
    dev.kind = DeviceKind::Serial;
    dev.path = "fake-port";
    dev.name = "ttyTest0";
    dev.title = "Fake UART";

    std::mutex src_mutex;
    MemorySource* raw_source = nullptr;
    ServeOptions opts;
    opts.data_dir = root / "runs";
    opts.home = root;
    opts.devices = [=] { return std::vector<Device>{dev}; };
    opts.open_source = [&](const Device& d, unsigned) {
        assert(d.id == dev.id);
        auto src = std::make_unique<MemorySource>(d.name);
        {
            std::scoped_lock lock(src_mutex);
            raw_source = src.get();
        }
        return std::variant<std::unique_ptr<ByteSource>, os::OpenFailure>(std::move(src));
    };

    const unsigned port = free_port();
    const std::string ref = device_flag == "--port" ? dev.path : dev.id;
    int code = -1;
    std::thread cli([&] {
        code = run_cli({"attach", device_flag, ref, "--no-open", "--http-port", std::to_string(port), "--data-dir",
                        os::to_utf8(root / "runs"), "--project", os::to_utf8(root / "project"), "--folded",
                        os::to_utf8(root / "out.folded"), "--name", "terminal run"},
                       opts);
    });

    assert(wait_for([&] { return contains(http_get(port, "/"), "<html"); })); // the page is up before any frame
    assert(contains(http_get(port, "/data.json?core=all&window=10"), R"("value":0)")); // and says "nothing yet"
    {
        std::scoped_lock lock(src_mutex);
        assert(raw_source != nullptr);
        raw_source->push(make_stream(2)); // 8 samples
    }
    assert(wait_for([&] { return contains(http_get(port, "/data.json?core=all&window=0"), R"("name":"all","value":8)"); }));
    assert(contains(http_get(port, "/data.json?core=1&window=0"), R"("name":"all","value":2)"));
    assert(http_get(port, "/other").empty());

    std::this_thread::sleep_for(20ms);
    cli_request_stop(); // Ctrl-C
    cli.join();
    assert(code == 0);

    RunStore store(root / "runs"); // the run is in the same history the app shows
    const auto runs = store.list();
    assert(runs.size() == 1 && runs[0].name == "terminal run" && runs[0].duration_ms > 0 && runs[0].ncores == 2);
    assert(runs[0].device_id == dev.id && runs[0].port == dev.name);
    std::ifstream folded(root / "out.folded");
    const std::string text((std::istreambuf_iterator<char>(folded)), {});
    assert(contains(text, "core0;0x400d2fff;0x400d1fff;0x400d1000 6\n"));
    fs::remove_all(root);
}

void exit_codes() {
    assert(run_cli({}, {}) == 2 && run_cli({"bogus"}, {}) == 2);
    assert(run_cli({"attach", "--bogus"}, {}) == 2 && run_cli({"attach", "--port"}, {}) == 2); // unknown option, missing value
    assert(run_cli({"attach", "--device"}, {}) == 2);
    assert(run_cli({"devices"}, {}) == 0);
    ServeOptions empty_opts;
    empty_opts.devices = [] { return std::vector<Device>{}; };
    assert(run_cli({"attach", "--device", "usb-missing-device-123"}, empty_opts) != 0);

    Device busy_dev;
    busy_dev.id = "usb-10c4-ea60-sn-busy";
    busy_dev.kind = DeviceKind::Serial;
    busy_dev.path = "busy-port";
    busy_dev.name = "ttyBusy0";
    busy_dev.title = "Busy UART";

    const auto busy_dir = fs::temp_directory_path() / "malgrind_cli_b";
    fs::remove_all(busy_dir);
    ServeOptions busy_opts;
    busy_opts.data_dir = busy_dir;
    busy_opts.devices = [=] { return std::vector<Device>{busy_dev}; };
    busy_opts.open_source = [](const Device&, unsigned) {
        return std::variant<std::unique_ptr<ByteSource>, os::OpenFailure>(
            os::OpenFailure{os::OpenError::Busy, false, "busy"});
    };
    assert(run_cli({"attach", "--port", busy_dev.path, "--no-open", "--data-dir", os::to_utf8(busy_dir)}, busy_opts) == 1);
    assert(run_cli({"attach", "--device", busy_dev.id, "--no-open", "--data-dir", os::to_utf8(busy_dir)}, busy_opts) == 1);
    fs::remove_all(busy_dir);
}

#ifndef _WIN32
void pty_serial_attach() {
    const auto root = fs::temp_directory_path() / "malgrind_cli_pty";
    fs::remove_all(root);
    fs::create_directories(root / "project");
    Pty pty;
    const unsigned port = free_port();
    ServeOptions pty_opts;
    pty_opts.data_dir = root / "runs";
    pty_opts.home = root;
    int code = -1;
    std::thread cli([&] {
        code = run_cli({"attach", "--port", pty.slave, "--no-open", "--http-port", std::to_string(port), "--data-dir",
                        os::to_utf8(root / "runs"), "--project", os::to_utf8(root / "project"), "--name", "pty run"},
                       pty_opts);
    });

    assert(wait_for([&] { return contains(http_get(port, "/"), "<html"); }));
    pty.send(make_stream(1)); // 4 samples
    assert(wait_for([&] { return contains(http_get(port, "/data.json?core=all&window=0"), R"("name":"all","value":4)"); }));

    cli_request_stop();
    cli.join();
    assert(code == 0);

    RunStore store(root / "runs");
    const auto runs = store.list();
    assert(runs.size() == 1 && runs[0].name == "pty run" && runs[0].ncores == 2);
    fs::remove_all(root);
}
#endif

} // namespace

int main() {
    attach_shows_the_live_page_and_records("--port");
    attach_shows_the_live_page_and_records("--device");
    exit_codes();
#ifndef _WIN32
    pty_serial_attach();
#endif
    std::puts("cli ok");
}
