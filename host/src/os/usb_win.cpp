#include "os/usb.hpp"

#include <windows.h>
#include <setupapi.h>

#include <algorithm>
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

std::string wide_to_utf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), needed, nullptr, nullptr);
    return out;
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

} // namespace

std::vector<UsbInfo> list_usb_devices() {
    const DevInfoSet devs(SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES));
    if (devs.handle == INVALID_HANDLE_VALUE) return {};

    std::vector<UsbInfo> devices;
    SP_DEVINFO_DATA dev_data{};
    dev_data.cbSize = sizeof(dev_data);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(devs.handle, index, &dev_data); ++index) {
        const auto hw_ids = read_dev_reg_strings(devs.handle, &dev_data, SPDRP_HARDWAREID);
        std::optional<VidPid> vp;
        for (const auto& hw : hw_ids) {
            vp = parse_vid_pid(hw);
            if (vp) break;
        }
        if (!vp) continue;

        UsbInfo usb;
        usb.vid = std::move(vp->vid);
        usb.pid = std::move(vp->pid);
        usb.product = read_dev_reg_string(devs.handle, &dev_data, SPDRP_DEVICEDESC);
        usb.manufacturer = read_dev_reg_string(devs.handle, &dev_data, SPDRP_MFG);
        usb.serial = parse_windows_serial(read_instance_id(devs.handle, &dev_data));
        usb.location = read_dev_reg_string(devs.handle, &dev_data, SPDRP_LOCATION_INFORMATION);
        devices.push_back(std::move(usb));
    }

    std::ranges::sort(devices, {}, &UsbInfo::location);
    return devices;
}

} // namespace malgrind::host::os
