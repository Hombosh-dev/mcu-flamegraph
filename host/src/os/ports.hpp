#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace malgrind::host::os {

struct UsbInfo {
    std::string vid, pid;               // lower-case hex, 4 digits
    std::string product, manufacturer;
    std::string serial;                 // adapter's serial number, "" if none or OS-generated
    std::string location;               // OS-specific but stable on this machine
};

struct PortInfo {
    std::string path;                   // what SerialPort::open takes
    std::string name;                   // short label: "ttyUSB0", "cu.usbserial-1410", "COM5"
    std::string description;            // OS friendly name or ""
    std::optional<UsbInfo> usb;         // nullopt for non-USB ports
};

[[nodiscard]] std::vector<PortInfo> list_serial_ports();     // USB ports only, sorted by name
[[nodiscard]] std::string_view serial_search_hint() noexcept;
[[nodiscard]] std::vector<PortInfo> list_serial_ports_linux(const std::filesystem::path& dev_root,
                                                            const std::filesystem::path& sys_root);

struct VidPid {
    std::string vid, pid;
};
[[nodiscard]] std::optional<VidPid> parse_vid_pid(std::string_view hardware_id);
[[nodiscard]] std::string parse_windows_serial(std::string_view instance_id);

} // namespace malgrind::host::os
