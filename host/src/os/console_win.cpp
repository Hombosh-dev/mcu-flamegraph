#include "os/console.hpp"

#include "os/paths.hpp"

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <mutex>

namespace malgrind::host::os {

namespace {

std::atomic<void (*)()> g_on_stop{nullptr};
std::once_flag g_handler_once;

BOOL WINAPI handle_console_ctrl(DWORD ctrl_type) {
    switch (ctrl_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        if (auto* cb = g_on_stop.load(std::memory_order_relaxed)) cb();
        return TRUE;
    default:
        return FALSE;
    }
}

} // namespace

void install_stop_handler(void (*on_stop)()) {
    g_on_stop.store(on_stop, std::memory_order_relaxed);
    std::call_once(g_handler_once, [] { SetConsoleCtrlHandler(handle_console_ctrl, TRUE); });
}

void binary_stdio() {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    SetConsoleOutputCP(CP_UTF8);
}

bool has_display() { return true; }

void open_url(const std::string& url) {
    if (url.empty()) return;
    const std::wstring wurl = from_utf8(url).native();
    ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace malgrind::host::os
