#include "os/serial.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <string_view>
#include <utility>

namespace malgrind::host::os {

static_assert(sizeof(std::uintptr_t) >= sizeof(HANDLE));

namespace {

HANDLE to_handle(std::uintptr_t handle) noexcept {
    return reinterpret_cast<HANDLE>(handle);
}

std::uintptr_t from_handle(HANDLE h) noexcept {
    return reinterpret_cast<std::uintptr_t>(h);
}

std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) return {};
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

std::string wide_to_utf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::string format_win_error(DWORD err) {
    LPWSTR raw = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD len = ::FormatMessageW(flags, nullptr, err, 0, reinterpret_cast<LPWSTR>(&raw), 0, nullptr);
    if (len == 0 || !raw) return "Windows error " + std::to_string(err);
    std::wstring_view msg(raw, len);
    while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' || msg.back() == L' ' || msg.back() == L'\t')) {
        msg.remove_suffix(1);
    }
    std::string out = wide_to_utf8(msg);
    ::LocalFree(raw);
    return out.empty() ? ("Windows error " + std::to_string(err)) : out;
}

OpenFailure failure_from_win_error(DWORD err) {
    switch (err) {
        case ERROR_ACCESS_DENIED:
            // On Windows, opening a COM port already held by another process yields ERROR_ACCESS_DENIED.
            return {OpenError::Busy, false, format_win_error(err)};
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_DEVICE_NOT_CONNECTED:
        case ERROR_GEN_FAILURE:
            return {OpenError::NotFound, true, format_win_error(err)};
        default:
            return {OpenError::Other, false, format_win_error(err)};
    }
}

bool is_bare_com_port(std::string_view path) noexcept {
    if (path.size() < 4 || path.starts_with("\\\\")) return false;
    const auto c0 = static_cast<char>(std::tolower(static_cast<unsigned char>(path[0])));
    const auto c1 = static_cast<char>(std::tolower(static_cast<unsigned char>(path[1])));
    const auto c2 = static_cast<char>(std::tolower(static_cast<unsigned char>(path[2])));
    if (c0 != 'c' || c1 != 'o' || c2 != 'm') return false;
    for (std::size_t i = 3; i < path.size(); ++i) {
        if (path[i] < '0' || path[i] > '9') return false;
    }
    return true;
}

} // namespace

std::variant<SerialPort, OpenFailure> SerialPort::open(const std::string& path, unsigned baud) {
    if (baud == 0) {
        return OpenFailure{OpenError::BadBaud, false, "unsupported baud rate"};
    }

    const std::string device_path = is_bare_com_port(path) ? ("\\\\.\\" + path) : path;
    const std::wstring wpath = utf8_to_wide(device_path);
    if (wpath.empty()) {
        return OpenFailure{OpenError::NotFound, true, "empty port path"};
    }

    const HANDLE h = ::CreateFileW(
        wpath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0, // no sharing: exclusive open
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) {
        return failure_from_win_error(::GetLastError());
    }

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!::GetCommState(h, &dcb)) {
        const DWORD err = ::GetLastError();
        ::CloseHandle(h);
        return failure_from_win_error(err);
    }

    dcb.BaudRate = static_cast<DWORD>(baud);
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    // Keep DTR and RTS in the same state so USB-UART auto-reset circuits do not pulse reset/boot on open.
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fTXContinueOnXoff = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fAbortOnError = FALSE;

    if (!::SetCommState(h, &dcb)) {
        const DWORD err = ::GetLastError();
        ::CloseHandle(h);
        if (err == ERROR_INVALID_PARAMETER) {
            return OpenFailure{OpenError::BadBaud, false, format_win_error(err)};
        }
        return failure_from_win_error(err);
    }

    ::PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return SerialPort(from_handle(h));
}

SerialPort::SerialPort(SerialPort&& other) noexcept
    : handle_(std::exchange(other.handle_, kInvalidHandle)),
      last_timeout_ms_(std::exchange(other.last_timeout_ms_, -1)) {}

SerialPort& SerialPort::operator=(SerialPort&& other) noexcept {
    if (this != &other) {
        if (handle_ != kInvalidHandle) ::CloseHandle(to_handle(handle_));
        handle_ = std::exchange(other.handle_, kInvalidHandle);
        last_timeout_ms_ = std::exchange(other.last_timeout_ms_, -1);
    }
    return *this;
}

SerialPort::~SerialPort() {
    if (handle_ != kInvalidHandle) ::CloseHandle(to_handle(handle_));
}

std::optional<std::size_t> SerialPort::read(std::span<std::uint8_t> out, int timeout_ms) {
    if (handle_ == kInvalidHandle) return std::nullopt;
    if (out.empty()) return std::size_t{0};

    const HANDLE h = to_handle(handle_);
    const int clamped_ms = timeout_ms < 0 ? 0 : timeout_ms;
    if (clamped_ms != last_timeout_ms_) {
        COMMTIMEOUTS timeouts{};
        timeouts.ReadIntervalTimeout = MAXDWORD;
        if (clamped_ms == 0) {
            timeouts.ReadTotalTimeoutMultiplier = 0;
            timeouts.ReadTotalTimeoutConstant = 0;
        } else {
            timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
            timeouts.ReadTotalTimeoutConstant = static_cast<DWORD>(clamped_ms);
        }
        timeouts.WriteTotalTimeoutMultiplier = 0;
        timeouts.WriteTotalTimeoutConstant = 0;
        if (!::SetCommTimeouts(h, &timeouts)) {
            return std::nullopt;
        }
        last_timeout_ms_ = clamped_ms;
    }

    const DWORD to_read = static_cast<DWORD>(
        (std::min)<std::size_t>(out.size(), static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
    DWORD got = 0;
    if (!::ReadFile(h, out.data(), to_read, &got, nullptr)) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(got);
}

bool SerialPort::write(std::span<const std::uint8_t> data) {
    if (handle_ == kInvalidHandle) return false;
    if (data.empty()) return true;

    const HANDLE h = to_handle(handle_);
    while (!data.empty()) {
        const DWORD chunk = static_cast<DWORD>(
            (std::min)<std::size_t>(data.size(), static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!::WriteFile(h, data.data(), chunk, &written, nullptr) || written == 0) {
            return false;
        }
        data = data.subspan(static_cast<std::size_t>(written));
    }
    return true;
}

} // namespace malgrind::host::os
