#include "devices.hpp"

#include "families/family.hpp"

#include <cstddef>
#include <utility>

namespace malgrind::host {

namespace {

constexpr std::size_t kMaxDeviceIdLen = 96;

std::string sanitize_id(std::string_view raw) {
    std::string out;
    out.reserve(raw.size() < kMaxDeviceIdLen ? raw.size() : kMaxDeviceIdLen);
    for (unsigned char ch : raw) {
        if (out.size() >= kMaxDeviceIdLen) break;
        if (ch >= 'A' && ch <= 'Z') {
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('-');
        }
    }
    return out;
}

template <typename Pred>
std::optional<std::optional<Device>> match_unique(const std::vector<Device>& now, Pred&& pred) {
    const Device* found = nullptr;
    for (const auto& d : now) {
        if (!pred(d)) continue;
        if (found != nullptr) return std::optional<Device>{std::nullopt};
        found = &d;
    }
    if (found != nullptr) return std::optional<Device>{*found};
    return std::nullopt;
}

} // namespace

std::string device_id(const os::PortInfo& port) {
    if (port.usb && !port.usb->serial.empty()) {
        return sanitize_id("usb-" + port.usb->vid + "-" + port.usb->pid + "-sn-" + port.usb->serial);
    }
    if (port.usb && !port.usb->location.empty()) {
        return sanitize_id("usb-" + port.usb->vid + "-" + port.usb->pid + "-at-" + port.usb->location);
    }
    return sanitize_id("port-" + (port.name.empty() ? port.path : port.name));
}

std::string probe_device_id(const DebugProbe& probe) {
    if (!probe.serial.empty()) {
        return sanitize_id("probe-" + probe.vid + "-" + probe.pid + "-sn-" + probe.serial);
    }
    if (!probe.location.empty()) {
        return sanitize_id("probe-" + probe.vid + "-" + probe.pid + "-at-" + probe.location);
    }
    return sanitize_id("probe-" + probe.vid + "-" + probe.pid + "-" + probe.name);
}

std::string family_hint_for_usb(std::string_view vid, std::string_view pid) {
    for (const Family* f : all_families()) {
        if (auto hint = f->family_hint_for_usb(vid, pid); !hint.empty()) return hint;
    }
    return {};
}

Device device_from_port(os::PortInfo port) {
    Device d;
    d.id = device_id(port);
    d.kind = DeviceKind::Serial;
    d.path = std::move(port.path);
    d.name = std::move(port.name);
    d.title = port.usb ? adapter_name(*port.usb) : (!port.description.empty() ? port.description : d.name);
    if (port.usb) d.family_hint = family_hint_for_usb(port.usb->vid, port.usb->pid);
    d.usb = std::move(port.usb);
    d.access = os::port_access(d.path);
    return d;
}

std::vector<Device> list_devices() {
    std::vector<Device> out;
    for (auto& p : os::list_serial_ports()) {
        out.push_back(device_from_port(std::move(p)));
    }
    for (auto& probe : find_probes()) {
        Device d;
        d.id = probe_device_id(probe);
        d.kind = DeviceKind::Probe;
        d.path = "";
        d.name = probe.name;
        d.title = probe.name;
        d.usb = os::UsbInfo{probe.vid, probe.pid, "", "", probe.serial, probe.location};
        d.access = {};
        out.push_back(std::move(d));
    }
    return out;
}

std::optional<Device> resolve_device(std::string_view ref, const std::vector<Device>& now) {
    if (ref.empty()) return std::nullopt;
    if (auto res = match_unique(now, [&](const Device& d) { return d.id == ref; })) return *res;
    if (auto res = match_unique(now, [&](const Device& d) { return !d.path.empty() && d.path == ref; })) return *res;
    if (auto res = match_unique(now, [&](const Device& d) { return !d.name.empty() && d.name == ref; })) return *res;
    return std::nullopt;
}

} // namespace malgrind::host
