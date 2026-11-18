#include "os/usb.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <string>

namespace fs = std::filesystem;

namespace malgrind::host::os {

namespace {

std::string read_line(const fs::path& file) {
    std::ifstream in(file);
    std::string line;
    std::getline(in, line);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())) != 0) line.pop_back();
    return line;
}

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::optional<UsbInfo> read_usb_device(const fs::path& dir) {
    auto vid = lower(read_line(dir / "idVendor"));
    if (vid.empty()) return std::nullopt;
    return UsbInfo{
        std::move(vid),
        lower(read_line(dir / "idProduct")),
        read_line(dir / "product"),
        read_line(dir / "manufacturer"),
        read_line(dir / "serial"),
        dir.filename().string(),
    };
}

} // namespace

std::vector<UsbInfo> list_usb_devices_linux(const fs::path& sys_root) {
    std::vector<UsbInfo> devices;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(sys_root / "bus/usb/devices", ec)) {
        if (auto info = read_usb_device(entry.path())) {
            devices.push_back(std::move(*info));
        }
    }
    std::ranges::sort(devices, {}, &UsbInfo::location);
    return devices;
}

std::vector<UsbInfo> list_usb_devices() {
    return list_usb_devices_linux("/sys");
}

} // namespace malgrind::host::os
