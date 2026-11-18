#include "os/ports.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

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

std::optional<UsbInfo> usb_info_for_tty(const std::string& name, const fs::path& sys_root) {
    std::error_code ec;
    // /sys/class/tty/<tty>/device points at the interface (or usb-serial port); the USB device is a few parents up.
    auto dir = fs::canonical(sys_root / "class/tty" / name / "device", ec);
    for (int level = 0; !ec && level < 4 && !dir.empty() && dir != dir.root_path(); ++level, dir = dir.parent_path()) {
        if (auto info = read_usb_device(dir)) return info;
    }
    return std::nullopt;
}

} // namespace

std::vector<PortInfo> list_serial_ports_linux(const fs::path& dev_root, const fs::path& sys_root) {
    std::vector<PortInfo> ports;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dev_root, ec)) {
        const std::string name = entry.path().filename().string();
        if (!name.starts_with("ttyUSB") && !name.starts_with("ttyACM")) continue;
        ports.push_back(PortInfo{
            entry.path().string(),
            name,
            {},
            usb_info_for_tty(name, sys_root),
        });
    }
    std::ranges::sort(ports, {}, &PortInfo::name);
    return ports;
}

std::vector<PortInfo> list_serial_ports() {
    return list_serial_ports_linux("/dev", "/sys");
}

} // namespace malgrind::host::os
