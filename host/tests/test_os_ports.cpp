// OS serial port enumeration and pure hardware-id/instance-id parser self-checks.
#include "os/ports.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace malgrind::host;
namespace fs = std::filesystem;

namespace {

void parse_vid_pid_checks() {
    {
        const auto vp = os::parse_vid_pid("USB\\VID_1A86&PID_7523&REV_0264");
        assert(vp && vp->vid == "1a86" && vp->pid == "7523");
    }
    {
        const auto vp = os::parse_vid_pid("USB\\VID_303A&PID_1001&MI_00");
        assert(vp && vp->vid == "303a" && vp->pid == "1001");
    }
    {
        const auto vp = os::parse_vid_pid("FTDIBUS\\COMPORT&VID_0403&PID_6001");
        assert(vp && vp->vid == "0403" && vp->pid == "6001");
    }
    {
        const auto vp = os::parse_vid_pid("usb\\pid_Ea60&vid_10C4");
        assert(vp && vp->vid == "10c4" && vp->pid == "ea60");
    }

    assert(!os::parse_vid_pid(""));
    assert(!os::parse_vid_pid("ACPI\\PNP0501"));
    assert(!os::parse_vid_pid("USB\\VID_1A86"));
    assert(!os::parse_vid_pid("USB\\PID_7523"));
    assert(!os::parse_vid_pid("USB\\VID_1A8&PID_7523"));
    assert(!os::parse_vid_pid("USB\\VID_1A860&PID_7523"));
    assert(!os::parse_vid_pid("USB\\VID_1A8G&PID_7523"));
    assert(!os::parse_vid_pid("USB\\XVID_1A86&PID_7523"));
    assert(!os::parse_vid_pid("BTHENUM\\{00001101-0000-1000-8000-00805F9B34FB}_VID&0001000A_PID&0001"));
}

void parse_windows_serial_checks() {
    assert(os::parse_windows_serial("USB\\VID_10C4&PID_EA60\\0001") == "0001");
    assert(os::parse_windows_serial("USB\\VID_1A86&PID_7523\\6&1234ABCD&0&2").empty());
    assert(os::parse_windows_serial("FTDIBUS\\VID_0403+PID_6001+A50285BIA\\0000") == "A50285BIA");
    assert(os::parse_windows_serial("ftdibus\\vid_0403+pid_6001+MySerial99\\0000") == "MySerial99");

    assert(os::parse_windows_serial("FTDIBUS\\VID_0403+PID_6001+6&1234ABCD&0&2\\0000").empty());
    assert(os::parse_windows_serial("FTDIBUS\\VID_0403+PID_6001\\0000").empty());
    assert(os::parse_windows_serial("FTDIBUS\\VID_0403+PID_6001+\\0000").empty());
    assert(os::parse_windows_serial("FTDIBUS\\COMPORT&VID_0403&PID_6001\\0000").empty());

    assert(os::parse_windows_serial("").empty());
    assert(os::parse_windows_serial("0001").empty());
    assert(os::parse_windows_serial("USB\\VID_10C4&PID_EA60").empty());
    assert(os::parse_windows_serial("USB\\VID_10C4&PID_EA60\\").empty());
    assert(os::parse_windows_serial("USB\\\\0001").empty());
    assert(os::parse_windows_serial("\\\\0001").empty());
    assert(os::parse_windows_serial("USB\\VID_10C4&PID_EA60\\0001\\extra").empty());
}

#if !defined(_WIN32) && !defined(__APPLE__)
void write_file(const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file) << text << "\n";
}

void touch(const fs::path& file) {
    fs::create_directories(file.parent_path());
    std::ofstream{file};
}

void make_usb_device(const fs::path& sys, const std::string& bus_name, std::string_view vid, std::string_view pid,
                     std::string_view product, std::string_view manufacturer, std::string_view serial = {}) {
    const auto dir = sys / "devices/pci/usb1" / bus_name;
    write_file(dir / "idVendor", vid);
    write_file(dir / "idProduct", pid);
    write_file(dir / "product", product);
    write_file(dir / "manufacturer", manufacturer);
    if (!serial.empty()) write_file(dir / "serial", serial);
    fs::create_directories(dir / (bus_name + ":1.0"));
}

void link_tty(const fs::path& sys, const std::string& tty, const fs::path& target) {
    fs::create_directories(sys / "class/tty" / tty);
    fs::create_directory_symlink(target, sys / "class/tty" / tty / "device");
}

void list_serial_ports_linux_checks() {
    const auto root = fs::canonical(fs::temp_directory_path()) / "malgrind_test_os_ports";
    fs::remove_all(root);
    const auto dev = root / "dev";
    const auto sys = root / "sys";

    // Create devices out of alphabetical order plus non-USB ttys that must be ignored.
    for (const char* name : {"ttyUSB1", "ttyS0", "tty0", "null", "cu.usbserial-0001", "ttyUSB0", "ttyACM0"}) {
        touch(dev / name);
    }

    // ttyACM0 -> interface (1 level below USB device), uppercase VID/PID normalized to lowercase.
    make_usb_device(sys, "1-1", "1A86", "55D3", "USB Single Serial", "WCH", "0001");
    link_tty(sys, "ttyACM0", sys / "devices/pci/usb1/1-1/1-1:1.0");

    // ttyUSB0 -> deeper usb-serial child (2 levels below USB device), no serial file.
    make_usb_device(sys, "1-2", "10c4", "ea60", "CP2102 USB to UART Bridge Controller", "Silicon Labs");
    fs::create_directories(sys / "devices/pci/usb1/1-2/1-2:1.0/ttyUSB0");
    link_tty(sys, "ttyUSB0", sys / "devices/pci/usb1/1-2/1-2:1.0/ttyUSB0");

    // ttyUSB1 has no sysfs entry -> still included, with usb == nullopt.

    const auto ports = os::list_serial_ports_linux(dev, sys);
    assert(ports.size() == 3);

    // Sorted by name: ttyACM0, ttyUSB0, ttyUSB1.
    assert(ports[0].name == "ttyACM0");
    assert(ports[0].path == (dev / "ttyACM0").string());
    assert(ports[0].usb.has_value());
    assert(ports[0].usb->vid == "1a86" && ports[0].usb->pid == "55d3");
    assert(ports[0].usb->product == "USB Single Serial" && ports[0].usb->manufacturer == "WCH");
    assert(ports[0].usb->serial == "0001" && ports[0].usb->location == "1-1");

    assert(ports[1].name == "ttyUSB0");
    assert(ports[1].path == (dev / "ttyUSB0").string());
    assert(ports[1].usb.has_value());
    assert(ports[1].usb->vid == "10c4" && ports[1].usb->pid == "ea60");
    assert(ports[1].usb->product == "CP2102 USB to UART Bridge Controller");
    assert(ports[1].usb->manufacturer == "Silicon Labs");
    assert(ports[1].usb->serial.empty() && ports[1].usb->location == "1-2");

    assert(ports[2].name == "ttyUSB1");
    assert(ports[2].path == (dev / "ttyUSB1").string());
    assert(!ports[2].usb.has_value());

    // Missing dev/sys roots return empty cleanly.
    assert(os::list_serial_ports_linux(root / "missing_dev", sys).empty());

    // Missing sysfs root still returns ttyUSB*/ttyACM* with empty usb.
    const auto no_sys = os::list_serial_ports_linux(dev, root / "missing_sys");
    assert(no_sys.size() == 3);
    assert(!no_sys[0].usb.has_value() && !no_sys[1].usb.has_value() && !no_sys[2].usb.has_value());

    fs::remove_all(root);
}
#endif

void live_list_ports_smoke_check() {
    const auto ports = os::list_serial_ports();
    assert(std::ranges::is_sorted(ports, {}, &os::PortInfo::name));
}

} // namespace

int main() {
    parse_vid_pid_checks();
    parse_windows_serial_checks();
#if !defined(_WIN32) && !defined(__APPLE__)
    list_serial_ports_linux_checks();
#endif
    live_list_ports_smoke_check();
    std::puts("os_ports ok");
}
