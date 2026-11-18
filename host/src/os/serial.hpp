#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>

namespace malgrind::host::os {

enum class OpenError { Busy, Denied, NotFound, BadBaud, Other };

struct OpenFailure {
    OpenError kind = OpenError::Other;
    bool transient = false;   // device absent for now (re-enumerating after a reset): retrying makes sense
    std::string detail;       // OS message, for "cannot open X: <detail>"
};

class SerialPort {
public:
    // Exclusive, raw 8N1, no flow control. DTR and RTS are left in the same state as each other (never differing),
    // so USB-UART auto-reset circuits do not reset the board on open.
    [[nodiscard]] static std::variant<SerialPort, OpenFailure> open(const std::string& path, unsigned baud);

    SerialPort(SerialPort&&) noexcept;
    SerialPort& operator=(SerialPort&&) noexcept;
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;
    ~SerialPort();

    // Bytes read; 0 on timeout; nullopt when the device is gone.
    [[nodiscard]] std::optional<std::size_t> read(std::span<std::uint8_t> out, int timeout_ms);
    // All bytes or false. May wait until a concurrent read() returns (Windows serialises I/O on one handle).
    [[nodiscard]] bool write(std::span<const std::uint8_t> data);

private:
    static constexpr std::uintptr_t kInvalidHandle = ~std::uintptr_t{0};
    explicit SerialPort(std::uintptr_t handle) noexcept : handle_(handle) {}
    std::uintptr_t handle_ = kInvalidHandle;
    int last_timeout_ms_ = -1;
};

} // namespace malgrind::host::os
