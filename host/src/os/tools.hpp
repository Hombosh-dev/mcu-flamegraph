#pragma once

// Locating Python, PlatformIO core directories, virtual-environment binaries and OS-appropriate install hints.

#include "os/paths.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace malgrind::host::os {

// Existing PlatformIO core directories, deduplicated and in priority order:
//   1. $PLATFORMIO_CORE_DIR (when set and an existing directory)
//   2. home / ".platformio" (when home is non-empty and the directory exists)
//   3. Windows only: <SystemDrive>\.platformio (when SystemDrive is set and the directory exists)
[[nodiscard]] std::vector<std::filesystem::path> platformio_core_dirs(const std::filesystem::path& home);

// Binary directory inside a Python virtual environment: venv/bin on POSIX, venv\Scripts on Windows.
[[nodiscard]] std::filesystem::path venv_bin(const std::filesystem::path& venv);
[[nodiscard]] std::filesystem::path venv_bin_for(Kind os_kind, const std::filesystem::path& venv);

// Command prefix that runs Python 3:
//   POSIX:   {"<path-to-python3>"} or {"<path-to-python>"}
//   Windows: {"<path-to-python3.exe|python.exe>"} skipping WindowsApps Store stubs, else {"<path-to-py.exe>", "-3"}
[[nodiscard]] std::optional<std::vector<std::string>> find_python();

// Python command that runs inside PlatformIO's environment (which has pyserial):
//   1. <core>/penv/<venv_bin>/python[.exe] across platformio_core_dirs(home)
//   2. POSIX fallback: words from the #! shebang line of `pio`
[[nodiscard]] std::optional<std::vector<std::string>> pio_python(const std::filesystem::path& home,
                                                                    const std::filesystem::path& pio);

// One-phrase OS-specific instructions for installing Python 3 with venv support.
[[nodiscard]] std::string python_hint();

} // namespace malgrind::host::os
