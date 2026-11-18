// Board adapter naming, probe identification table, and Linux USB device enumeration self-checks.
#include "boards.hpp"
#include "os/usb.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace malgrind::host;
namespace fs = std::filesystem;

namespace {

void adapter_name_checks() {
    assert(adapter_name(os::UsbInfo{"1a86", "55d3", "USB Single Serial", "WCH", "0001", "1-1"}) == "WCH CH340/CH343 USB-UART");
    assert(adapter_name(os::UsbInfo{"10c4", "ea60", "CP2102", "Silicon Labs", "", "1-2"}) == "Silicon Labs CP210x USB-UART");
    assert(adapter_name(os::UsbInfo{"0403", "6001", "FT232R", "FTDI", "A50285BIA", "1-3"}) == "FTDI USB-UART");
    assert(adapter_name(os::UsbInfo{"067b", "2303", "PL2303", "Prolific", "", "1-4"}) == "Prolific PL2303 USB-UART");
    assert(adapter_name(os::UsbInfo{"303a", "1001", "USB JTAG/serial debug unit", "", "", "1-5"})
           == "Espressif USB-Serial/JTAG (built-in debug port)");
    assert(adapter_name(os::UsbInfo{"2e8a", "000a", "Pico", "Raspberry Pi", "", "1-6"}) == "Raspberry Pi USB serial");
    assert(adapter_name(os::UsbInfo{"abcd", "0001", "My Gadget", "", "", ""}) == "My Gadget");
    assert(adapter_name(os::UsbInfo{"abcd", "0001", "", "", "", ""}) == "USB serial adapter");
}

void probe_name_checks() {
    for (const char* pid : {"3744", "3748", "374b", "374d", "374e", "374f", "3752", "3753"}) {
        assert(probe_name("0483", pid) == "ST-Link");
    }
    assert(!probe_name("0483", "9999"));

    assert(probe_name("1366", "0101") == "SEGGER J-Link");
    assert(probe_name("1366", "1051") == "SEGGER J-Link");

    assert(probe_name("0d28", "0204") == "CMSIS-DAP (DAPLink)");
    assert(!probe_name("0d28", "0205"));

    assert(probe_name("2e8a", "000c") == "Raspberry Pi Debug Probe");
    assert(!probe_name("2e8a", "000a"));

    assert(probe_name("15ba", "002a") == "Olimex debugger");

    assert(probe_name("0403", "6010") == "FTDI FT2232/FT232H (can be a JTAG adapter, e.g. ESP-Prog)");
    assert(probe_name("0403", "6014") == "FTDI FT2232/FT232H (can be a JTAG adapter, e.g. ESP-Prog)");
    assert(!probe_name("0403", "6001"));

    assert(!probe_name("1a86", "55d3"));
    assert(!probe_name("", ""));
}

#if !defined(_WIN32) && !defined(__APPLE__)
void write_file(const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file) << text << "\n";
}

// A USB device under devices/pci/usb1/<name>, symlinked from bus/usb/devices/<name>, plus an interface dir without idVendor.
void usb_device(const fs::path& sys, const std::string& name, std::string_view vid, std::string_view pid,
                std::string_view product, std::string_view manufacturer, std::string_view serial = {}) {
    const auto dir = sys / "devices/pci/usb1" / name;
    write_file(dir / "idVendor", vid);
    write_file(dir / "idProduct", pid);
    write_file(dir / "product", product);
    write_file(dir / "manufacturer", manufacturer);
    if (!serial.empty()) write_file(dir / "serial", serial);
    const auto iface = dir / (name + ":1.0");
    fs::create_directories(iface);
    fs::create_directories(sys / "bus/usb/devices");
    fs::create_directory_symlink(dir, sys / "bus/usb/devices" / name);
    // Real Linux sysfs also places interface symlinks (no idVendor) in bus/usb/devices/.
    fs::create_directory_symlink(iface, sys / "bus/usb/devices" / (name + ":1.0"));
}

std::vector<DebugProbe> filter_and_sort_probes(std::vector<os::UsbInfo> devices) {
    std::vector<DebugProbe> probes;
    for (auto& info : devices) {
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

void list_usb_devices_linux_checks() {
    const auto sys = fs::canonical(fs::temp_directory_path()) / "malgrind_boards_usb";
    fs::remove_all(sys);

    usb_device(sys, "1-1", "1A86", "55D3", "USB Single Serial", "WCH", "0001"); // serial adapter, not a probe
    usb_device(sys, "1-3", "0483", "374B", "STM32 STLink", "STMicroelectronics", "ST002");
    usb_device(sys, "1-6", "0483", "374b", "STM32 STLink", "STMicroelectronics", "ST001");
    usb_device(sys, "1-4", "0D28", "0204", "DAPLink CMSIS-DAP", "Arm");
    usb_device(sys, "1-5", "1d6b", "0002", "xHCI Host Controller", "Linux");

    const auto devices = os::list_usb_devices_linux(sys);
    assert(devices.size() == 5);
    assert(devices[0].location == "1-1" && devices[0].vid == "1a86" && devices[0].pid == "55d3");
    assert(devices[0].product == "USB Single Serial" && devices[0].manufacturer == "WCH" && devices[0].serial == "0001");
    assert(devices[2].location == "1-4" && devices[2].serial.empty());

    const auto probes = filter_and_sort_probes(devices);
    assert(probes.size() == 3);
    assert(probes[0].name == "CMSIS-DAP (DAPLink)" && probes[0].vid == "0d28" && probes[0].pid == "0204"
           && probes[0].serial.empty() && probes[0].location == "1-4");
    assert(probes[1].name == "ST-Link" && probes[1].vid == "0483" && probes[1].pid == "374b"
           && probes[1].serial == "ST001" && probes[1].location == "1-6");
    assert(probes[2].name == "ST-Link" && probes[2].vid == "0483" && probes[2].pid == "374b"
           && probes[2].serial == "ST002" && probes[2].location == "1-3");

    // Empty bus/usb/devices and missing sysfs both return empty cleanly.
    const auto empty_sys = fs::canonical(fs::temp_directory_path()) / "malgrind_boards_empty";
    fs::remove_all(empty_sys);
    fs::create_directories(empty_sys / "bus/usb/devices");
    assert(os::list_usb_devices_linux(empty_sys).empty());
    fs::remove_all(empty_sys);

    const auto missing_sys = fs::temp_directory_path() / "malgrind_boards_missing";
    fs::remove_all(missing_sys);
    assert(os::list_usb_devices_linux(missing_sys).empty());

    fs::remove_all(sys);
}
#endif

void find_probes_smoke_check() {
    const auto probes = find_probes();
    assert(std::ranges::is_sorted(probes, [](const DebugProbe& a, const DebugProbe& b) {
        return std::tie(a.name, a.serial, a.location) < std::tie(b.name, b.serial, b.location);
    }));
}

} // namespace

int main() {
    adapter_name_checks();
    probe_name_checks();
#if !defined(_WIN32) && !defined(__APPLE__)
    list_usb_devices_linux_checks();
#endif
    find_probes_smoke_check();
    std::puts("boards ok");
}
