#pragma once

// Test helpers shared by the tests that need a serial port without a board.

#include "source.hpp"
#include "stream.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <string>
#include <unistd.h>
#include <variant>
#include <vector>

// The "board": the master side of a pty. Code under test opens `slave` exactly as it opens /dev/ttyUSB0.
struct Pty {
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    std::string slave;
    Pty() {
        assert(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0);
        slave = ptsname(master);
    }
    ~Pty() { hang_up(); }
    void send(const std::vector<std::uint8_t>& bytes) const {
        assert(write(master, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
    }
    void send(const std::string& text) const { send(std::vector<std::uint8_t>(text.begin(), text.end())); }
    void hang_up() {
        if (master >= 0) close(master), master = -1;
    }
};

// Whether a second open of a port is refused while the first holds it (TIOCEXCL). Linux enforces it on a pty; a macOS pty does
// not, although its real serial ports do. Tests of the "port is busy" paths run only where it is enforced (elsewhere they would
// attach successfully, and a live attach never ends by itself). Probes a pty of its own: the flag can outlive the close.
inline bool exclusive_open_is_enforced() {
    Pty scratch;
    auto first = malgrind::host::SerialSource::open_path(scratch.slave, 921600);
    return std::holds_alternative<std::unique_ptr<malgrind::host::SerialSource>>(first) &&
           std::holds_alternative<malgrind::host::os::OpenFailure>(
               malgrind::host::SerialSource::open_path(scratch.slave, 921600));
}
