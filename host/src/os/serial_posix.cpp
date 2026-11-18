#include "os/serial.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <utility>

#ifdef __APPLE__
#include <IOKit/serial/ioss.h>
#endif

namespace malgrind::host::os {

static_assert(sizeof(std::uintptr_t) >= sizeof(int));

namespace {

int to_fd(std::uintptr_t handle) noexcept {
    return static_cast<int>(handle);
}

std::uintptr_t from_fd(int fd) noexcept {
    return fd < 0 ? ~std::uintptr_t{0} : static_cast<std::uintptr_t>(fd);
}

std::string errno_detail(int err) {
    const char* msg = std::strerror(err);
    return msg ? std::string(msg) : ("errno " + std::to_string(err));
}

OpenFailure failure_from_errno(int err) {
    switch (err) {
        case EBUSY:
            return {OpenError::Busy, false, errno_detail(err)};
        case EACCES:
        case EPERM:
            return {OpenError::Denied, false, errno_detail(err)};
        case ENOENT:
        case ENODEV:
        case ENXIO:
        case EIO:
            return {OpenError::NotFound, true, errno_detail(err)};
        default:
            return {OpenError::Other, false, errno_detail(err)};
    }
}

#ifndef __APPLE__
speed_t speed_for(unsigned baud) noexcept {
    switch (baud) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        case 460800: return B460800;
        case 921600: return B921600;
        case 1000000: return B1000000;
        case 1500000: return B1500000;
        case 2000000: return B2000000;
        default: return B0;
    }
}
#endif

} // namespace

std::variant<SerialPort, OpenFailure> SerialPort::open(const std::string& path, unsigned baud) {
#ifdef __APPLE__
    if (baud == 0) {
        return OpenFailure{OpenError::BadBaud, false, "unsupported baud rate"};
    }
#else
    const speed_t speed = speed_for(baud);
    if (speed == B0) {
        return OpenFailure{OpenError::BadBaud, false, "unsupported baud rate"};
    }
#endif

    const int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return failure_from_errno(errno);
    }

    termios tty{};
    if (::tcgetattr(fd, &tty) != 0) {
        const int err = errno;
        ::close(fd);
        return failure_from_errno(err);
    }

    ::cfmakeraw(&tty);
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cflag &= ~CSTOPB;
#ifdef CRTSCTS
    tty.c_cflag &= ~CRTSCTS;
#endif

#ifdef __APPLE__
    bool custom_baud_set = false;
    ::cfsetspeed(&tty, static_cast<speed_t>(baud));
    if (::tcsetattr(fd, TCSANOW, &tty) != 0) {
        // Real macOS serial drivers often reject high/non-standard bauds in tcsetattr and require IOSSIOSPEED afterwards.
        ::cfsetspeed(&tty, B115200);
        if (::tcsetattr(fd, TCSANOW, &tty) != 0) {
            const int err = errno;
            ::close(fd);
            return failure_from_errno(err);
        }
        speed_t ios_speed = static_cast<speed_t>(baud);
        if (::ioctl(fd, IOSSIOSPEED, &ios_speed) != 0) {
            const int err = errno;
            ::close(fd);
            if (err == EINVAL) return OpenFailure{OpenError::BadBaud, false, errno_detail(err)};
            return failure_from_errno(err);
        }
        custom_baud_set = true;
    }
    if (!custom_baud_set) {
        // Apply IOSSIOSPEED even when tcsetattr succeeded so hardware drivers honor non-standard rates; ignore on ptys (ENOTTY).
        speed_t ios_speed = static_cast<speed_t>(baud);
        (void)::ioctl(fd, IOSSIOSPEED, &ios_speed);
    }
#else
    ::cfsetspeed(&tty, speed);
    if (::tcsetattr(fd, TCSANOW, &tty) != 0) {
        const int err = errno;
        ::close(fd);
        return failure_from_errno(err);
    }
#endif

    // Exclusive: a second open of a real serial port (and Linux ptys) fails with EBUSY instead of stealing bytes.
    (void)::ioctl(fd, TIOCEXCL);
    return SerialPort(from_fd(fd));
}

SerialPort::SerialPort(SerialPort&& other) noexcept
    : handle_(std::exchange(other.handle_, kInvalidHandle)),
      last_timeout_ms_(std::exchange(other.last_timeout_ms_, -1)) {}

SerialPort& SerialPort::operator=(SerialPort&& other) noexcept {
    if (this != &other) {
        if (handle_ != kInvalidHandle) ::close(to_fd(handle_));
        handle_ = std::exchange(other.handle_, kInvalidHandle);
        last_timeout_ms_ = std::exchange(other.last_timeout_ms_, -1);
    }
    return *this;
}

SerialPort::~SerialPort() {
    if (handle_ != kInvalidHandle) ::close(to_fd(handle_));
}

std::optional<std::size_t> SerialPort::read(std::span<std::uint8_t> out, int timeout_ms) {
    if (handle_ == kInvalidHandle) return std::nullopt;
    if (out.empty()) return std::size_t{0};

    const int fd = to_fd(handle_);
    if (fd < 0 || fd >= FD_SETSIZE) return std::nullopt;

    const int clamped_ms = std::max(0, timeout_ms);
    // select(), not poll(): macOS poll() does not work on device files such as serial ports.
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(fd, &readable);
    timeval timeout{clamped_ms / 1000, (clamped_ms % 1000) * 1000};
    const int ready = ::select(fd + 1, &readable, nullptr, nullptr, &timeout);
    if (ready < 0) return errno == EINTR ? std::optional<std::size_t>(0) : std::nullopt;
    if (ready == 0) return std::size_t{0};

    const ssize_t n = ::read(fd, out.data(), out.size());
    if (n > 0) return static_cast<std::size_t>(n);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return std::size_t{0};
    return std::nullopt; // EOF or device gone (EIO/ENXIO)
}

bool SerialPort::write(std::span<const std::uint8_t> data) {
    if (handle_ == kInvalidHandle) return false;
    if (data.empty()) return true;

    const int fd = to_fd(handle_);
    while (!data.empty()) {
        const ssize_t n = ::write(fd, data.data(), data.size());
        if (n > 0) {
            data = data.subspan(static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) return false;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (fd < 0 || fd >= FD_SETSIZE) return false;
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(fd, &writable);
            timeval timeout{5, 0};
            const int ready = ::select(fd + 1, nullptr, &writable, nullptr, &timeout);
            if (ready < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (ready == 0) return false;
            continue;
        }
        return false;
    }
    return true;
}

} // namespace malgrind::host::os
