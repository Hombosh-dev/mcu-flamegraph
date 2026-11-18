#pragma once

// USB-serial adapter naming and USB debug probe identification on top of os::UsbInfo.
// The adapter is NOT the chip: a CH343 or CP210x bridge sits in front of any MCU.

#include "os/ports.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace malgrind::host {

using os::UsbInfo;

// A readable name for a USB-serial adapter: "WCH CH340/CH343 USB-UART", "Silicon Labs CP210x USB-UART", ... else the product string.
[[nodiscard]] std::string adapter_name(const UsbInfo& info);

struct DebugProbe {
    std::string name, vid, pid, serial, location;
};

// Map a USB VID/PID (4-digit lower-case hex) to a known debug probe name, or nullopt if not a probe.
[[nodiscard]] std::optional<std::string> probe_name(std::string_view vid, std::string_view pid);

// USB debug probes that are plugged in (ST-Link, J-Link, CMSIS-DAP, FTDI JTAG adapters).
[[nodiscard]] std::vector<DebugProbe> find_probes();

} // namespace malgrind::host
