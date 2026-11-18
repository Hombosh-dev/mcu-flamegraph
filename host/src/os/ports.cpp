#include "os/ports.hpp"
#include "os/paths.hpp"

#include <cctype>
#include <cstddef>

namespace malgrind::host::os {

namespace {

bool is_word_char(char c) {
    const auto uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) != 0 || c == '_';
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool istarts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

std::optional<std::string> find_hex4_tag(std::string_view s, std::string_view prefix) {
    for (std::size_t i = 0; i + prefix.size() + 4 <= s.size(); ++i) {
        if (i > 0 && is_word_char(s[i - 1])) continue;
        if (!iequals(s.substr(i, prefix.size()), prefix)) continue;

        const std::size_t hex_start = i + prefix.size();
        if (hex_start + 4 < s.size() && is_word_char(s[hex_start + 4])) continue;

        std::string out;
        out.reserve(4);
        bool ok = true;
        for (std::size_t j = 0; j < 4; ++j) {
            const auto ch = static_cast<unsigned char>(s[hex_start + j]);
            if (std::isxdigit(ch) == 0) {
                ok = false;
                break;
            }
            out.push_back(static_cast<char>(std::tolower(ch)));
        }
        if (ok) return out;
    }
    return std::nullopt;
}

std::vector<std::string_view> split_sv(std::string_view s, char delim) {
    std::vector<std::string_view> parts;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t next = s.find(delim, pos);
        if (next == std::string_view::npos) {
            parts.push_back(s.substr(pos));
            break;
        }
        parts.push_back(s.substr(pos, next - pos));
        pos = next + 1;
    }
    return parts;
}

} // namespace

std::string_view serial_search_hint() noexcept {
    switch (kind()) {
        case Kind::Linux: return "/dev/ttyUSB*, /dev/ttyACM*";
        case Kind::MacOS: return "/dev/cu.usb*";
        case Kind::Windows: return "COM ports of USB devices";
    }
    return {};
}

std::optional<VidPid> parse_vid_pid(std::string_view hardware_id) {
    auto vid = find_hex4_tag(hardware_id, "vid_");
    if (!vid) return std::nullopt;
    auto pid = find_hex4_tag(hardware_id, "pid_");
    if (!pid) return std::nullopt;
    return VidPid{std::move(*vid), std::move(*pid)};
}

std::string parse_windows_serial(std::string_view instance_id) {
    // Windows device instance IDs have the form "<enumerator>\<device-id>\<instance-id>".
    const auto parts = split_sv(instance_id, '\\');
    if (parts.size() != 3 || parts[0].empty() || parts[1].empty() || parts[2].empty()) return {};

    if (iequals(parts[0], "FTDIBUS")) {
        // FTDI serial ports encode the adapter serial in the middle segment: "VID_xxxx+PID_yyyy+<serial>".
        const auto fields = split_sv(parts[1], '+');
        if (fields.size() != 3) return {};
        if (!istarts_with(fields[0], "VID_") || fields[0].size() <= 4) return {};
        if (!istarts_with(fields[1], "PID_") || fields[1].size() <= 4) return {};
        const std::string_view serial = fields[2];
        if (serial.empty() || serial.find('&') != std::string_view::npos) return {};
        return std::string(serial);
    }

    const std::string_view serial = parts[2];
    // Windows synthesizes instance IDs containing '&' when the device has no USB serial descriptor.
    if (serial.find('&') != std::string_view::npos) return {};
    return std::string(serial);
}

} // namespace malgrind::host::os
