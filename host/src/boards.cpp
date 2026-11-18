#include "boards.hpp"
#include "families/family.hpp"
#include "os/usb.hpp"

#include <algorithm>
#include <tuple>

namespace malgrind::host {

std::string adapter_name(const UsbInfo& info) {
    if (info.vid == "1a86") return "WCH CH340/CH343 USB-UART";
    if (info.vid == "10c4") return "Silicon Labs CP210x USB-UART";
    if (info.vid == "0403") return "FTDI USB-UART";
    if (info.vid == "067b") return "Prolific PL2303 USB-UART";
    if (info.vid == "2e8a") return "Raspberry Pi USB serial";
    for (const Family* f : all_families()) {
        if (auto name = f->usb_adapter_name(info.vid, info.pid); !name.empty()) return name;
    }
    return info.product.empty() ? "USB serial adapter" : info.product;
}

std::optional<std::string> probe_name(std::string_view vid, std::string_view pid) {
    if (vid == "0483" && (pid == "3744" || pid == "3748" || pid == "374b" || pid == "374d" || pid == "374e" || pid == "374f" || pid == "3752" || pid == "3753"))
        return "ST-Link";
    if (vid == "1366") return "SEGGER J-Link";
    if (vid == "0d28" && pid == "0204") return "CMSIS-DAP (DAPLink)";
    if (vid == "2e8a" && pid == "000c") return "Raspberry Pi Debug Probe";
    if (vid == "15ba") return "Olimex debugger";
    if (vid == "0403" && (pid == "6010" || pid == "6014")) return "FTDI FT2232/FT232H (can be a JTAG adapter, e.g. ESP-Prog)";
    return std::nullopt;
}

std::vector<DebugProbe> find_probes() {
    std::vector<DebugProbe> probes;
    for (auto& info : os::list_usb_devices()) {
        auto name = probe_name(info.vid, info.pid);
        if (!name) continue;
        probes.push_back(DebugProbe{
            std::move(*name),
            std::move(info.vid),
            std::move(info.pid),
            std::move(info.serial),
            std::move(info.location),
        });
    }
    std::ranges::sort(probes, [](const DebugProbe& a, const DebugProbe& b) {
        return std::tie(a.name, a.serial, a.location) < std::tie(b.name, b.serial, b.location);
    });
    return probes;
}

} // namespace malgrind::host
