#include "os/console.hpp"

#include "os/paths.hpp"
#include "os/process.hpp"

#include <atomic>
#include <csignal>
#include <signal.h>
#include <string_view>
#include <thread>

namespace malgrind::host::os {

namespace {

std::atomic<void (*)()> g_on_stop{nullptr};

void handle_stop_signal(int) {
    if (auto* cb = g_on_stop.load(std::memory_order_relaxed)) cb();
}

} // namespace

void install_stop_handler(void (*on_stop)()) {
    g_on_stop.store(on_stop, std::memory_order_relaxed);
    std::signal(SIGPIPE, SIG_IGN); // a closed browser connection or app must not kill us before the run is saved
    // SIGINT/SIGTERM end blocking reads (no SA_RESTART) so serve and live runs shut down cleanly
    struct sigaction stop{};
    stop.sa_handler = handle_stop_signal;
    sigemptyset(&stop.sa_mask);
    stop.sa_flags = 0;
    sigaction(SIGINT, &stop, nullptr);
    sigaction(SIGTERM, &stop, nullptr);
}

void binary_stdio() {}

bool has_display() {
    if constexpr (kind() == Kind::MacOS) {
        return !get_env("SSH_CONNECTION").has_value();
    } else {
        const auto non_empty = [](std::string_view name) {
            const auto v = get_env(name);
            return v && !v->empty();
        };
        return non_empty("DISPLAY") || non_empty("WAYLAND_DISPLAY");
    }
}

void open_url(const std::string& url) {
    if (url.empty()) return;
    const char* opener = kind() == Kind::MacOS ? "open" : "xdg-open";
    std::thread([opener, url] { (void)run({opener, url}); }).detach();
}

} // namespace malgrind::host::os
