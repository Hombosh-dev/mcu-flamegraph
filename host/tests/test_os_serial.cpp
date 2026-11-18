// OS serial port and ByteSource self-check: exclusive open, raw I/O, timeouts, hangup, error mapping, StdinSource, MemorySource.
#include "families/family.hpp"
#include "ops.hpp"
#include "os/paths.hpp"
#include "os/serial.hpp"
#include "source.hpp"
#include "stream.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#ifndef _WIN32
#include "pty.hpp"
#include <sys/select.h>
#include <unistd.h>
#endif

using namespace malgrind::host;

namespace {

void open_error_formatting_checks() {
    const auto busy = open_error("portA", {os::OpenError::Busy, false, "in use"});
    assert(std::string_view(busy.what()).find("busy:") != std::string_view::npos);
    assert(std::string_view(busy.what()).find("portA") != std::string_view::npos);

    const auto denied = open_error("portB", {os::OpenError::Denied, false, "permission denied"});
    assert(std::string_view(denied.what()).find("Grant access") != std::string_view::npos);
    assert(std::string_view(denied.what()).find("portB") != std::string_view::npos);

    const auto missing = open_error("portC", {os::OpenError::NotFound, true, "no such file"});
    assert(std::string_view(missing.what()) == "cannot open portC: no such file");
}

void source_stdin_roundtrip() {
    auto* orig = std::cin.rdbuf();
    std::stringbuf input("hello-stdin");
    std::cin.rdbuf(&input);

    auto src = open_source("-");
    assert(src);
    assert(src->describe() == "stdin");
    assert(!src->write({}));

    std::string collected;
    std::array<std::uint8_t, 32> buf{};
    for (;;) {
        const auto n = src->read(buf, 1000);
        if (!n) break;
        collected.append(reinterpret_cast<const char*>(buf.data()), *n);
    }
    assert(collected == "hello-stdin");
    std::cin.rdbuf(orig);
}

void memory_source_roundtrip() {
    using namespace std::chrono_literals;

    MemorySource mem;
    assert(mem.describe() == "memory");
    assert(MemorySource("custom-label").describe() == "custom-label");

    std::array<std::uint8_t, 16> buf{};
    const auto timed_out = mem.read(buf, 20);
    assert(timed_out.has_value() && *timed_out == 0);

    mem.push(std::string_view("hello-"));
    const std::vector<std::uint8_t> tail = {'m', 'e', 'm', 'o', 'r', 'y'};
    mem.push(tail);

    std::string rx;
    std::array<std::uint8_t, 5> small{};
    while (rx.size() < 12) {
        const auto n = mem.read(small, 1000);
        assert(n.has_value() && *n > 0);
        rx.append(reinterpret_cast<const char*>(small.data()), *n);
    }
    assert(rx == "hello-memory");
    assert(mem.read(buf, 15) == std::optional<std::size_t>(0));

    // Producer wakes a waiting reader.
    std::thread pusher([&] {
        std::this_thread::sleep_for(20ms);
        mem.push("woke");
    });
    const auto woken = mem.read(buf, 2000);
    pusher.join();
    assert(woken.has_value() && *woken == 4);
    assert(std::string(reinterpret_cast<const char*>(buf.data()), *woken) == "woke");

    // Host -> target writes accumulate in order until closed.
    const std::vector<std::uint8_t> cmd1 = {0x01, 0x02};
    const std::vector<std::uint8_t> cmd2 = {'a', 'b'};
    assert(mem.write(cmd1));
    assert(mem.write(cmd2));
    assert(mem.written() == (std::vector<std::uint8_t>{0x01, 0x02, 'a', 'b'}));

    // Buffered rx bytes drain before EOF; writes after close return false and do not append.
    mem.push("last");
    mem.close();
    assert(!mem.write(cmd1));
    assert(mem.written() == (std::vector<std::uint8_t>{0x01, 0x02, 'a', 'b'}));
    const auto drained = mem.read(buf, 1000);
    assert(drained.has_value() && *drained == 4);
    assert(std::string(reinterpret_cast<const char*>(buf.data()), *drained) == "last");
    assert(!mem.read(buf, 1000).has_value());

    // close() also wakes a blocked reader with EOF.
    MemorySource async_eof;
    std::thread closer([&] {
        std::this_thread::sleep_for(20ms);
        async_eof.close();
    });
    assert(!async_eof.read(buf, 2000).has_value());
    closer.join();
}

void source_factory_and_probe_checks() {
    Device dev;
    dev.id = "usb-10c4-ea60-sn-0001";
    dev.kind = DeviceKind::Serial;
    dev.path = "fake-port";
    dev.name = "ttyTest0";

    const auto opened = open_device_source(dev, [](const Device& d, unsigned baud) {
        assert(d.id == "usb-10c4-ea60-sn-0001");
        assert(baud == family_by_id("esp32")->agent_baud());
        auto src = std::make_unique<MemorySource>(d.name);
        src->push("hi");
        src->close();
        return std::variant<std::unique_ptr<ByteSource>, os::OpenFailure>(std::move(src));
    });
    assert(opened && opened->describe() == "ttyTest0");

    const auto with_agent = probe_device(dev, [](const Device&, unsigned) {
        auto src = std::make_unique<MemorySource>();
        src->push(make_stream(1));
        src->close();
        return std::variant<std::unique_ptr<ByteSource>, os::OpenFailure>(std::move(src));
    });
    assert(with_agent.agent && with_agent.arch == malgrind::Arch::Xtensa && with_agent.ncores == 2);

    const auto quiet = probe_device(dev, [](const Device&, unsigned) {
        auto src = std::make_unique<MemorySource>();
        src->close();
        return std::variant<std::unique_ptr<ByteSource>, os::OpenFailure>(std::move(src));
    });
    assert(!quiet.agent && !quiet.busy && !quiet.denied);

    const auto busy = probe_device(dev, [](const Device&, unsigned) {
        return std::variant<std::unique_ptr<ByteSource>, os::OpenFailure>(
            os::OpenFailure{os::OpenError::Busy, false, "held"});
    });
    assert(busy.busy && !busy.agent);

    const auto denied = probe_device(dev, [](const Device&, unsigned) {
        return std::variant<std::unique_ptr<ByteSource>, os::OpenFailure>(
            os::OpenFailure{os::OpenError::Denied, false, "denied"});
    });
    assert(denied.denied && !denied.agent);
}

#ifndef _WIN32

std::vector<std::uint8_t> read_exact_from_master(int fd, std::size_t expected) {
    std::vector<std::uint8_t> out;
    while (out.size() < expected) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(fd, &readable);
        timeval timeout{1, 0};
        const int ready = ::select(fd + 1, &readable, nullptr, nullptr, &timeout);
        assert(ready > 0);
        std::array<std::uint8_t, 256> buf{};
        const ssize_t n = ::read(fd, buf.data(), std::min(buf.size(), expected - out.size()));
        assert(n > 0);
        out.insert(out.end(), buf.begin(), buf.begin() + n);
    }
    return out;
}

std::vector<std::uint8_t> read_exact_from_serial(os::SerialPort& port, std::size_t expected) {
    std::vector<std::uint8_t> out;
    while (out.size() < expected) {
        std::array<std::uint8_t, 256> buf{};
        const auto n = port.read(buf, 1000);
        assert(n.has_value());
        assert(*n > 0);
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n));
    }
    return out;
}

void pty_roundtrip_timeout_move_and_hangup() {
    Pty pty;
    auto res = os::SerialPort::open(pty.slave, 921600);
    assert(std::holds_alternative<os::SerialPort>(res));
    auto port = std::get<os::SerialPort>(std::move(res));

    // Nothing sent yet -> timeout returns 0 (not nullopt).
    std::array<std::uint8_t, 32> buf{};
    const auto timed_out = port.read(buf, 20);
    assert(timed_out.has_value() && *timed_out == 0);

    // Move construction and move assignment transfer ownership cleanly.
    os::SerialPort moved(std::move(port));
    os::SerialPort target = std::move(moved);

    // Host -> device write.
    const std::vector<std::uint8_t> tx = {0x01, 0x02, 'h', 'i', 0x00, 0xFF};
    assert(target.write(tx));
    assert(read_exact_from_master(pty.master, tx.size()) == tx);

    // Device -> host read.
    const std::string rx = "hello-serial\n";
    pty.send(rx);
    const auto got = read_exact_from_serial(target, rx.size());
    assert(std::string(got.begin(), got.end()) == rx);

    // Closing the peer returns nullopt on read.
    pty.hang_up();
    const auto after_hangup = target.read(buf, 1000);
    assert(!after_hangup.has_value());
}

void exclusive_open_returns_busy() {
    if (!exclusive_open_is_enforced()) {
        std::puts("exclusive_open_returns_busy: skipped, this platform does not enforce TIOCEXCL on a pty");
        return;
    }
    Pty pty;
    auto first = os::SerialPort::open(pty.slave, 921600);
    assert(std::holds_alternative<os::SerialPort>(first));

    auto second = os::SerialPort::open(pty.slave, 921600);
    assert(std::holds_alternative<os::OpenFailure>(second));
    const auto& failure = std::get<os::OpenFailure>(second);
    assert(failure.kind == os::OpenError::Busy);
    assert(!failure.transient);
    assert(!failure.detail.empty());
}

void error_cases_posix() {
    const std::string missing_path = "/tmp/malgrind_nonexistent_serial_port_99999";
    auto missing = os::SerialPort::open(missing_path, 921600);
    assert(std::holds_alternative<os::OpenFailure>(missing));
    const auto& mf = std::get<os::OpenFailure>(missing);
    assert(mf.kind == os::OpenError::NotFound);
    assert(mf.transient);
    assert(!mf.detail.empty());

    auto bad0 = os::SerialPort::open(missing_path, 0);
    assert(std::holds_alternative<os::OpenFailure>(bad0));
    assert(std::get<os::OpenFailure>(bad0).kind == os::OpenError::BadBaud);
    assert(!std::get<os::OpenFailure>(bad0).transient);

    if constexpr (os::kind() == os::Kind::Linux) {
        auto bad_linux = os::SerialPort::open(missing_path, 12345);
        assert(std::holds_alternative<os::OpenFailure>(bad_linux));
        assert(std::get<os::OpenFailure>(bad_linux).kind == os::OpenError::BadBaud);
    }

    // SerialSource::open_path and SerialSource::open wrapper checks.
    {
        Pty pty;
        auto src_res = SerialSource::open_path(pty.slave, 921600);
        assert(std::holds_alternative<std::unique_ptr<SerialSource>>(src_res));
        auto src = std::get<std::unique_ptr<SerialSource>>(std::move(src_res));
        assert(src->describe() == pty.slave);
        const std::vector<std::uint8_t> ping = {'o', 'k'};
        assert(src->write(ping));
        assert(read_exact_from_master(pty.master, ping.size()) == ping);
        pty.send("pong");
        std::array<std::uint8_t, 16> rbuf{};
        std::string received;
        while (received.size() < 4) {
            const auto n = src->read(rbuf, 1000);
            assert(n.has_value() && *n > 0);
            received.append(reinterpret_cast<const char*>(rbuf.data()), *n);
        }
        assert(received == "pong");
    }
    {
        Pty pty;
        Device dev;
        dev.path = pty.slave;
        dev.name = "ttyTest0";
        auto by_dev = SerialSource::open(dev, 921600);
        assert(std::holds_alternative<std::unique_ptr<SerialSource>>(by_dev));
        assert(std::get<std::unique_ptr<SerialSource>>(by_dev)->describe() == "ttyTest0");
    }
    {
        Pty pty;
        Device dev;
        dev.path = pty.slave;
        auto by_dev_unnamed = SerialSource::open(dev, 921600);
        assert(std::holds_alternative<std::unique_ptr<SerialSource>>(by_dev_unnamed));
        assert(std::get<std::unique_ptr<SerialSource>>(by_dev_unnamed)->describe() == pty.slave);
    }

    auto src_missing = SerialSource::open_path(missing_path, 921600);
    assert(std::holds_alternative<os::OpenFailure>(src_missing));
    assert(std::get<os::OpenFailure>(src_missing).kind == os::OpenError::NotFound);
    assert(std::get<os::OpenFailure>(src_missing).transient);

    bool threw_missing = false;
    try {
        (void)open_source(missing_path);
    } catch (const std::runtime_error& e) {
        threw_missing = std::string_view(e.what()).starts_with("cannot open ");
    }
    assert(threw_missing);

    if (exclusive_open_is_enforced()) {
        Pty held;
        auto holder = os::SerialPort::open(held.slave, 921600);
        assert(std::holds_alternative<os::SerialPort>(holder));
        assert(probe_port(held.slave).busy);
        bool threw_busy = false;
        try {
            (void)open_source(held.slave);
        } catch (const std::runtime_error& e) {
            threw_busy = std::string_view(e.what()).starts_with("busy:");
        }
        assert(threw_busy);
    }
}

#else

void windows_stub_checks() {
    auto missing = os::SerialPort::open("\\\\.\\NONEXISTENT_COM99", 921600);
    assert(std::holds_alternative<os::OpenFailure>(missing));
    const auto& mf = std::get<os::OpenFailure>(missing);
    assert(mf.kind == os::OpenError::NotFound);
    assert(mf.transient);
    assert(!mf.detail.empty());

    auto bad = os::SerialPort::open("COM99", 0);
    assert(std::holds_alternative<os::OpenFailure>(bad));
    assert(std::get<os::OpenFailure>(bad).kind == os::OpenError::BadBaud);
}

#endif

} // namespace

int main() {
    open_error_formatting_checks();
    source_stdin_roundtrip();
    memory_source_roundtrip();
    source_factory_and_probe_checks();
#ifndef _WIN32
    pty_roundtrip_timeout_move_and_hangup();
    exclusive_open_returns_busy();
    error_cases_posix();
#else
    windows_stub_checks();
#endif
    std::puts("os_serial ok");
}
