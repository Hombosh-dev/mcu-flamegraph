#pragma once

#include "devices.hpp"
#include "os/serial.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace malgrind::host {

// Abstract stream of bytes between the host and a target (serial port, stdin, memory, etc.).
class ByteSource {
public:
    virtual ~ByteSource() = default;
    ByteSource(const ByteSource&) = delete;
    ByteSource& operator=(const ByteSource&) = delete;

    // Bytes read; 0 if nothing arrived within the timeout; nullopt at end of stream or when the device is gone.
    [[nodiscard]] virtual std::optional<std::size_t> read(std::span<std::uint8_t> out, int timeout_ms = 200) = 0;
    // Host -> target bytes (agent commands). Returns false if this source cannot send or the write failed.
    [[nodiscard]] virtual bool write(std::span<const std::uint8_t>) { return false; }
    // Short human-readable label for logs and diagnostics (e.g. port name, "stdin", "memory").
    [[nodiscard]] virtual std::string describe() const = 0;

protected:
    ByteSource() = default;
};

class SerialSource final : public ByteSource {
public:
    [[nodiscard]] static std::variant<std::unique_ptr<SerialSource>, os::OpenFailure> open(const Device& dev,
                                                                                           unsigned baud);
    [[nodiscard]] static std::variant<std::unique_ptr<SerialSource>, os::OpenFailure> open_path(const std::string& path,
                                                                                                unsigned baud,
                                                                                                std::string label = {});

    explicit SerialSource(os::SerialPort serial, std::string label = {});

    [[nodiscard]] std::optional<std::size_t> read(std::span<std::uint8_t> out, int timeout_ms = 200) override;
    [[nodiscard]] bool write(std::span<const std::uint8_t> data) override;
    [[nodiscard]] std::string describe() const override;

private:
    os::SerialPort serial_;
    std::string label_;
};

class StdinSource final : public ByteSource {
public:
    StdinSource();
    ~StdinSource() override;

    [[nodiscard]] std::optional<std::size_t> read(std::span<std::uint8_t> out, int timeout_ms = 200) override;
    [[nodiscard]] std::string describe() const override;

private:
    struct State;
    std::shared_ptr<State> state_;
};

class MemorySource final : public ByteSource {
public:
    explicit MemorySource(std::string label = "memory");

    void push(std::span<const std::uint8_t> data);
    void push(std::string_view text);
    void close();
    [[nodiscard]] std::vector<std::uint8_t> written() const;

    [[nodiscard]] std::optional<std::size_t> read(std::span<std::uint8_t> out, int timeout_ms = 200) override;
    [[nodiscard]] bool write(std::span<const std::uint8_t> data) override;
    [[nodiscard]] std::string describe() const override;

private:
    std::string label_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::uint8_t> rx_;
    std::vector<std::uint8_t> tx_;
    bool closed_ = false;
};

} // namespace malgrind::host
