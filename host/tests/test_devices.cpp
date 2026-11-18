// Device model, stable OS-neutral id generation, and device resolution self-checks.
#include "devices.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace malgrind::host;

namespace {

void device_id_checks() {
    // USB with serial: spaces, slashes, and uppercase are sanitized to lowercase [a-z0-9.-].
    os::PortInfo with_serial;
    with_serial.path = "/dev/ttyUSB0";
    with_serial.name = "ttyUSB0";
    with_serial.usb = os::UsbInfo{"10c4", "ea60", "CP2102", "Silicon Labs", "A5/BC 01.2-X", "1-3"};
    assert(device_id(with_serial) == "usb-10c4-ea60-sn-a5-bc-01.2-x");

    // Truncation to max 96 chars.
    os::PortInfo long_serial = with_serial;
    long_serial.usb->serial = std::string(120, 'Z');
    const std::string truncated = device_id(long_serial);
    assert(truncated.size() == 96);
    assert(truncated == std::string("usb-10c4-ea60-sn-") + std::string(96 - 17, 'z'));

    // USB with location only (no serial).
    os::PortInfo loc_only;
    loc_only.path = "COM5";
    loc_only.name = "COM5";
    loc_only.usb = os::UsbInfo{"1a86", "7523", "USB-SERIAL CH340", "WCH", "", "Port_#0003.Hub_#0001"};
    assert(device_id(loc_only) == "usb-1a86-7523-at-port--0003.hub--0001");

    // No USB fallback: uses name when non-empty, else path.
    os::PortInfo no_usb_name;
    no_usb_name.path = "/dev/ttyS0";
    no_usb_name.name = "ttyS0";
    assert(device_id(no_usb_name) == "port-ttys0");

    os::PortInfo no_usb_path_only;
    no_usb_path_only.path = "/dev/ttyS1";
    assert(device_id(no_usb_path_only) == "port--dev-ttys1");
}

void probe_device_id_checks() {
    DebugProbe with_serial{"ST-Link", "0483", "374b", "ST/001 A", "1-6"};
    assert(probe_device_id(with_serial) == "probe-0483-374b-sn-st-001-a");

    DebugProbe loc_only{"CMSIS-DAP (DAPLink)", "0d28", "0204", "", "1-4.2"};
    assert(probe_device_id(loc_only) == "probe-0d28-0204-at-1-4.2");

    DebugProbe fallback{"SEGGER J-Link", "1366", "0101", "", ""};
    assert(probe_device_id(fallback) == "probe-1366-0101-segger-j-link");
}

void resolve_device_checks() {
    Device serial1;
    serial1.id = "usb-10c4-ea60-sn-0001";
    serial1.kind = DeviceKind::Serial;
    serial1.path = "/dev/ttyUSB0";
    serial1.name = "ttyUSB0";
    serial1.title = "Silicon Labs CP210x USB-UART";

    Device probe1;
    probe1.id = "probe-0483-374b-sn-st001";
    probe1.kind = DeviceKind::Probe;
    probe1.path = "";
    probe1.name = "ST-Link";
    probe1.title = "ST-Link";

    Device probe2;
    probe2.id = "probe-0483-374b-sn-st002";
    probe2.kind = DeviceKind::Probe;
    probe2.path = "";
    probe2.name = "ST-Link";
    probe2.title = "ST-Link";

    const std::vector<Device> now{serial1, probe1, probe2};

    // By id
    auto r1 = resolve_device("usb-10c4-ea60-sn-0001", now);
    assert(r1 && r1->path == "/dev/ttyUSB0");

    // By path
    auto r2 = resolve_device("/dev/ttyUSB0", now);
    assert(r2 && r2->id == "usb-10c4-ea60-sn-0001");

    // By unique name
    auto r3 = resolve_device("ttyUSB0", now);
    assert(r3 && r3->id == "usb-10c4-ea60-sn-0001");

    // Empty and missing
    assert(!resolve_device("", now));
    assert(!resolve_device("nonexistent", now));

    // Ambiguous name ("ST-Link" matches two probes), but each resolves uniquely by its own id.
    assert(!resolve_device("ST-Link", now));
    auto p1 = resolve_device("probe-0483-374b-sn-st001", now);
    auto p2 = resolve_device("probe-0483-374b-sn-st002", now);
    assert(p1 && p1->id == "probe-0483-374b-sn-st001");
    assert(p2 && p2->id == "probe-0483-374b-sn-st002");

    // Ambiguous path and ambiguous id also return nullopt.
    Device dup_path1 = serial1;
    Device dup_path2 = serial1;
    dup_path2.id = "usb-10c4-ea60-sn-0002";
    assert(!resolve_device("/dev/ttyUSB0", {dup_path1, dup_path2}));
    assert(!resolve_device(serial1.id, {serial1, serial1}));
}

void family_hint_checks() {
    assert(family_hint_for_usb("303a", "1001") == "esp32");
    assert(family_hint_for_usb("303a", "0002") == "esp32");

    // Generic USB-UART bridges can sit in front of any MCU: never guess.
    for (const auto& [vid, pid] : {std::pair{"1a86", "7523"}, std::pair{"1a86", "55d3"}, std::pair{"10c4", "ea60"},
                                   std::pair{"0403", "6001"}, std::pair{"0403", "6010"}, std::pair{"067b", "2303"},
                                   std::pair{"2e8a", "000a"}, std::pair{"", ""}}) {
        assert(family_hint_for_usb(vid, pid).empty());
    }

    os::PortInfo native_usb{"/dev/ttyACM0", "ttyACM0", "", os::UsbInfo{"303a", "1001", "USB JTAG", "Espressif", "1", "1-2"}};
    const Device esp_dev = device_from_port(native_usb);
    assert(esp_dev.family_hint == "esp32");
    assert(esp_dev.title == "Espressif USB-Serial/JTAG (built-in debug port)");

    for (const auto& [vid, pid] : {std::pair{"10c4", "ea60"}, std::pair{"1a86", "7523"}, std::pair{"0403", "6001"}, std::pair{"067b", "2303"}}) {
        os::PortInfo bridge{"/dev/ttyUSB0", "ttyUSB0", "", os::UsbInfo{vid, pid, "", "", "", "1-3"}};
        assert(device_from_port(bridge).family_hint.empty());
    }

    os::PortInfo no_usb{"/dev/ttyS0", "ttyS0", "Serial port", std::nullopt};
    assert(device_from_port(no_usb).family_hint.empty());
}

void list_devices_smoke_check() {
    const auto devs = list_devices();
    for (const auto& d : devs) {
        assert(!d.id.empty());
        assert(d.id.size() <= 96);
        for (unsigned char ch : d.id) {
            const bool valid = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-';
            assert(valid);
        }
        if (d.kind == DeviceKind::Probe) {
            assert(d.path.empty());
        } else {
            assert(!d.path.empty());
        }
    }
}

} // namespace

int main() {
    device_id_checks();
    probe_device_id_checks();
    resolve_device_checks();
    family_hint_checks();
    list_devices_smoke_check();
    std::puts("devices ok");
}
