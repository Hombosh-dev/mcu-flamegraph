#include "os/ports.hpp"

#include <windows.h>
#include <initguid.h>
#include <devguid.h>
#include <setupapi.h>
#include <cfgmgr32.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace malgrind::host::os {

namespace {

struct DevInfoSet {
    HDEVINFO handle = INVALID_HANDLE_VALUE;
    explicit DevInfoSet(HDEVINFO h) noexcept : handle(h) {}
    ~DevInfoSet() {
        if (handle != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(handle);
    }
    DevInfoSet(const DevInfoSet&) = delete;
    DevInfoSet& operator=(const DevInfoSet&) = delete;
};

struct RegKeyGuard {
    HKEY key = nullptr;
    explicit RegKeyGuard(HKEY k) noexcept : key(k) {}
    ~RegKeyGuard() {
        if (key != nullptr && key != INVALID_HANDLE_VALUE) RegCloseKey(key);
    }
    RegKeyGuard(const RegKeyGuard&) = delete;
    RegKeyGuard& operator=(const RegKeyGuard&) = delete;
};

std::string wide_to_utf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::optional<std::string> read_port_name(HDEVINFO devs, SP_DEVINFO_DATA* dev_data) {
    const RegKeyGuard key(SetupDiOpenDevRegKey(devs, dev_data, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ));
    if (key.key == nullptr || key.key == INVALID_HANDLE_VALUE) return std::nullopt;

    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key.key, L"PortName", nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS) return std::nullopt;
    if ((type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) return std::nullopt;

    std::wstring buf(bytes / sizeof(wchar_t) + 1, L'\0');
    DWORD read_bytes = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
    if (RegQueryValueExW(key.key, L"PortName", nullptr, &type, reinterpret_cast<LPBYTE>(buf.data()), &read_bytes) != ERROR_SUCCESS) {
        return std::nullopt;
    }

    const auto max_chars = static_cast<std::ptrdiff_t>(read_bytes / sizeof(wchar_t));
    const auto nul = std::find(buf.begin(), buf.begin() + max_chars, L'\0');
    return wide_to_utf8(std::wstring_view(buf.data(), static_cast<std::size_t>(nul - buf.begin())));
}

std::vector<std::string> read_dev_reg_strings(HDEVINFO devs, SP_DEVINFO_DATA* dev_data, DWORD prop) {
    DWORD type = 0;
    DWORD bytes = 0;
    SetupDiGetDeviceRegistryPropertyW(devs, dev_data, prop, &type, nullptr, 0, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t)) return {};

    std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 2, L'\0');
    DWORD actual = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(devs, dev_data, prop, &type,
                                           reinterpret_cast<PBYTE>(buf.data()),
                                           static_cast<DWORD>(buf.size() * sizeof(wchar_t)), &actual)) {
        return {};
    }

    const std::size_t nchars = actual / sizeof(wchar_t);
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < nchars && buf[pos] != L'\0') {
        const auto start = buf.begin() + static_cast<std::ptrdiff_t>(pos);
        const auto end = buf.begin() + static_cast<std::ptrdiff_t>(nchars);
        const auto nul = std::find(start, end, L'\0');
        const auto len = static_cast<std::size_t>(nul - start);
        out.push_back(wide_to_utf8(std::wstring_view(buf.data() + pos, len)));
        pos += len + 1;
    }
    return out;
}

std::string read_dev_reg_string(HDEVINFO devs, SP_DEVINFO_DATA* dev_data, DWORD prop) {
    auto list = read_dev_reg_strings(devs, dev_data, prop);
    return list.empty() ? std::string{} : std::move(list.front());
}

std::string read_instance_id(HDEVINFO devs, SP_DEVINFO_DATA* dev_data) {
    DWORD needed = 0;
    SetupDiGetDeviceInstanceIdW(devs, dev_data, nullptr, 0, &needed);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed == 0) return {};

    std::wstring buf(needed, L'\0');
    if (!SetupDiGetDeviceInstanceIdW(devs, dev_data, buf.data(), needed, &needed)) return {};
    const auto nul = std::find(buf.begin(), buf.end(), L'\0');
    return wide_to_utf8(std::wstring_view(buf.data(), static_cast<std::size_t>(nul - buf.begin())));
}

std::string read_parent_instance_id(DEVINST dev_inst) {
    DEVINST parent = 0;
    if (CM_Get_Parent(&parent, dev_inst, 0) != CR_SUCCESS) return {};

    ULONG len = 0;
    if (CM_Get_Device_ID_Size(&len, parent, 0) != CR_SUCCESS || len == 0) return {};

    std::wstring buf(static_cast<std::size_t>(len) + 1, L'\0');
    if (CM_Get_Device_IDW(parent, buf.data(), static_cast<ULONG>(buf.size()), 0) != CR_SUCCESS) return {};
    const auto nul = std::find(buf.begin(), buf.end(), L'\0');
    return wide_to_utf8(std::wstring_view(buf.data(), static_cast<std::size_t>(nul - buf.begin())));
}

bool contains_mi_marker(std::string_view s) {
    constexpr std::string_view kMarker = "&mi_";
    if (s.size() < kMarker.size()) return false;
    for (std::size_t i = 0; i + kMarker.size() <= s.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < kMarker.size(); ++j) {
            if (std::tolower(static_cast<unsigned char>(s[i + j])) != kMarker[j]) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

} // namespace

std::vector<PortInfo> list_serial_ports() {
    const DevInfoSet devs(SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT));
    if (devs.handle == INVALID_HANDLE_VALUE) return {};

    std::vector<PortInfo> ports;
    SP_DEVINFO_DATA dev_data{};
    dev_data.cbSize = sizeof(dev_data);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(devs.handle, index, &dev_data); ++index) {
        const auto name = read_port_name(devs.handle, &dev_data);
        if (!name || !name->starts_with("COM")) continue;

        const auto hw_ids = read_dev_reg_strings(devs.handle, &dev_data, SPDRP_HARDWAREID);
        std::optional<VidPid> vp;
        bool is_composite_iface = false;
        for (const auto& hw : hw_ids) {
            if (!vp) vp = parse_vid_pid(hw);
            if (contains_mi_marker(hw)) is_composite_iface = true;
        }
        if (!vp) continue; // skip non-USB ports (ACPI, Bluetooth, virtual)

        std::string serial;
        if (is_composite_iface) {
            // Composite USB interfaces (e.g. CDC-ACM) get generated instance IDs; the USB serial lives on the parent.
            serial = parse_windows_serial(read_parent_instance_id(dev_data.DevInst));
        }
        if (serial.empty()) {
            serial = parse_windows_serial(read_instance_id(devs.handle, &dev_data));
        }

        UsbInfo usb;
        usb.vid = std::move(vp->vid);
        usb.pid = std::move(vp->pid);
        usb.product = read_dev_reg_string(devs.handle, &dev_data, SPDRP_DEVICEDESC);
        usb.manufacturer = read_dev_reg_string(devs.handle, &dev_data, SPDRP_MFG);
        usb.serial = std::move(serial);
        usb.location = read_dev_reg_string(devs.handle, &dev_data, SPDRP_LOCATION_INFORMATION);

        ports.push_back(PortInfo{
            *name,
            *name,
            read_dev_reg_string(devs.handle, &dev_data, SPDRP_FRIENDLYNAME),
            std::move(usb),
        });
    }

    std::ranges::sort(ports, {}, &PortInfo::name);
    return ports;
}

} // namespace malgrind::host::os
