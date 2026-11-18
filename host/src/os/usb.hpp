#pragma once

#include "os/ports.hpp"

#include <filesystem>
#include <vector>

namespace malgrind::host::os {

[[nodiscard]] std::vector<UsbInfo> list_usb_devices();
[[nodiscard]] std::vector<UsbInfo> list_usb_devices_linux(const std::filesystem::path& sys_root); // tests

} // namespace malgrind::host::os
