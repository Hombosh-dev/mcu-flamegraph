#pragma once

#include "boards.hpp"
#include "os/access.hpp"
#include "os/ports.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace malgrind::host {

enum class DeviceKind { Serial, Probe };

struct Device {
    std::string id;                    // stable, OS-neutral, [a-z0-9.-] only
    DeviceKind kind = DeviceKind::Serial;
    std::string path;                  // Serial: what SerialPort::open takes; Probe: ""
    std::string name;                  // short OS port label; Probe: probe name
    std::string title;                 // "WCH CH340/CH343 USB-UART", "ST-Link"
    std::optional<os::UsbInfo> usb;
    os::PortAccess access;             // Serial only
    std::string family_hint;           // family id from USB VID/PID; "" = unknown
};

// First non-empty family hint for a USB VID/PID across registered families, or "".
[[nodiscard]] std::string family_hint_for_usb(std::string_view vid, std::string_view pid);
[[nodiscard]] Device device_from_port(os::PortInfo port);

// Rules, in order:
//   usb with serial    -> "usb-<vid>-<pid>-sn-<serial>"
//   usb with location  -> "usb-<vid>-<pid>-at-<location>"
//   otherwise          -> "port-<name>"
// Probes use "probe-" instead of "usb-". Characters outside [a-z0-9.-] become '-', lower-cased, max 96 chars.
[[nodiscard]] std::string device_id(const os::PortInfo& port);
[[nodiscard]] std::string probe_device_id(const DebugProbe& probe);
[[nodiscard]] std::vector<Device> list_devices();          // serial ports + probes
// `ref` may be a device id, a path, or a short name. Returns nullopt if empty, absent, or ambiguous.
[[nodiscard]] std::optional<Device> resolve_device(std::string_view ref, const std::vector<Device>& now);

} // namespace malgrind::host
