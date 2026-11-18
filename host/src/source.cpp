#include "source.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <streambuf>
#include <thread>
#include <utility>

namespace malgrind::host {

namespace {

std::optional<std::size_t> read_from_queue(std::mutex& mutex, std::condition_variable& cv,
                                           std::deque<std::uint8_t>& queue, const bool& eof,
                                           std::span<std::uint8_t> out, int timeout_ms) {
    if (out.empty()) return std::size_t{0};
    std::unique_lock lock(mutex);
    if (queue.empty() && !eof) {
        const auto wait = std::chrono::milliseconds(std::max(0, timeout_ms));
        cv.wait_for(lock, wait, [&] { return !queue.empty() || eof; });
    }
    if (queue.empty()) {
        return eof ? std::nullopt : std::optional<std::size_t>(0);
    }
    const std::size_t n = std::min(out.size(), queue.size());
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = queue.front();
        queue.pop_front();
    }
    return n;
}

} // namespace

// ---- SerialSource --------------------------------------------------------------------------------------------------

SerialSource::SerialSource(os::SerialPort serial, std::string label)
    : serial_(std::move(serial)), label_(std::move(label)) {}

std::variant<std::unique_ptr<SerialSource>, os::OpenFailure> SerialSource::open(const Device& dev, unsigned baud) {
    return open_path(dev.path, baud, dev.name.empty() ? dev.path : dev.name);
}

std::variant<std::unique_ptr<SerialSource>, os::OpenFailure> SerialSource::open_path(const std::string& path,
                                                                                    unsigned baud,
                                                                                    std::string label) {
    auto res = os::SerialPort::open(path, baud);
    if (auto* serial = std::get_if<os::SerialPort>(&res)) {
        if (label.empty()) label = path;
        return std::make_unique<SerialSource>(std::move(*serial), std::move(label));
    }
    return std::get<os::OpenFailure>(std::move(res));
}

std::optional<std::size_t> SerialSource::read(std::span<std::uint8_t> out, int timeout_ms) {
    return serial_.read(out, timeout_ms);
}

bool SerialSource::write(std::span<const std::uint8_t> data) {
    return serial_.write(data);
}

std::string SerialSource::describe() const {
    return label_;
}

// ---- StdinSource ---------------------------------------------------------------------------------------------------

struct StdinSource::State {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::uint8_t> buf;
    bool eof = false;
    bool closed = false;

    static std::shared_ptr<State> start() {
        auto state = std::make_shared<State>();
        std::thread([state]() {
            std::streambuf* sb = std::cin.rdbuf();
            if (!sb) {
                std::scoped_lock lock(state->mutex);
                state->eof = true;
                state->cv.notify_all();
                return;
            }
            constexpr auto kEof = std::char_traits<char>::eof();
            for (;;) {
                {
                    std::scoped_lock lock(state->mutex);
                    if (state->closed) break;
                }
                const int ch = sb->sbumpc();
                if (ch == kEof) {
                    std::scoped_lock lock(state->mutex);
                    state->eof = true;
                    state->cv.notify_all();
                    break;
                }
                std::array<char, 4096> tmp;
                tmp[0] = static_cast<char>(ch);
                std::size_t count = 1;
                const std::streamsize avail = sb->in_avail();
                if (avail > 0) {
                    const std::streamsize extra =
                        std::min<std::streamsize>(avail, static_cast<std::streamsize>(tmp.size() - 1));
                    const std::streamsize got = sb->sgetn(tmp.data() + 1, extra);
                    if (got > 0) count += static_cast<std::size_t>(got);
                }
                {
                    std::scoped_lock lock(state->mutex);
                    if (state->closed) break;
                    for (std::size_t i = 0; i < count; ++i) {
                        state->buf.push_back(static_cast<std::uint8_t>(tmp[i]));
                    }
                    state->cv.notify_one();
                }
            }
        }).detach();
        return state;
    }
};

StdinSource::StdinSource() : state_(State::start()) {}

StdinSource::~StdinSource() {
    if (state_) {
        std::scoped_lock lock(state_->mutex);
        state_->closed = true;
        state_->cv.notify_all();
    }
}

std::optional<std::size_t> StdinSource::read(std::span<std::uint8_t> out, int timeout_ms) {
    if (!state_) return std::nullopt;
    return read_from_queue(state_->mutex, state_->cv, state_->buf, state_->eof, out, timeout_ms);
}

std::string StdinSource::describe() const {
    return "stdin";
}

// ---- MemorySource --------------------------------------------------------------------------------------------------

MemorySource::MemorySource(std::string label) : label_(std::move(label)) {}

void MemorySource::push(std::span<const std::uint8_t> data) {
    if (data.empty()) return;
    {
        std::scoped_lock lock(mutex_);
        if (closed_) return;
        rx_.insert(rx_.end(), data.begin(), data.end());
    }
    cv_.notify_all();
}

void MemorySource::push(std::string_view text) {
    push(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

void MemorySource::close() {
    {
        std::scoped_lock lock(mutex_);
        closed_ = true;
    }
    cv_.notify_all();
}

std::vector<std::uint8_t> MemorySource::written() const {
    std::scoped_lock lock(mutex_);
    return tx_;
}

std::optional<std::size_t> MemorySource::read(std::span<std::uint8_t> out, int timeout_ms) {
    return read_from_queue(mutex_, cv_, rx_, closed_, out, timeout_ms);
}

bool MemorySource::write(std::span<const std::uint8_t> data) {
    std::scoped_lock lock(mutex_);
    if (closed_) return false;
    tx_.insert(tx_.end(), data.begin(), data.end());
    return true;
}

std::string MemorySource::describe() const {
    return label_;
}

} // namespace malgrind::host
