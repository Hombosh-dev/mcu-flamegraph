#pragma once

// OS abstraction for paths, environment variables, UTF-8 conversion and calendar time.
// Outside host/src/os/, paths stay std::filesystem::path internally and cross JSON, log lines or process boundaries
// strictly through to_utf8() / from_utf8().

#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace malgrind::host::os {

enum class Kind { Linux, MacOS, Windows };

[[nodiscard]] constexpr Kind kind() noexcept {
#if defined(_WIN32)
    return Kind::Windows;
#elif defined(__APPLE__)
    return Kind::MacOS;
#else
    return Kind::Linux;
#endif
}

[[nodiscard]] std::string_view kind_name() noexcept; // "linux" | "macos" | "windows"

[[nodiscard]] std::optional<std::string> get_env(std::string_view name);
void set_env(std::string_view name, std::string_view value);
void unset_env(std::string_view name);

[[nodiscard]] std::filesystem::path home_dir();
// Base directory for per-user application data, without a trailing "malgrind" component.
[[nodiscard]] std::filesystem::path user_data_dir();
// Per-user projects root avoiding spaces/non-ASCII where PlatformIO's ESP-IDF builder forbids them.
[[nodiscard]] std::filesystem::path safe_projects_root_for(Kind os_kind, const std::filesystem::path& user_data,
                                                             const std::filesystem::path& home, std::string_view system_drive);
[[nodiscard]] std::filesystem::path safe_projects_root();
[[nodiscard]] std::filesystem::path executable_path();
[[nodiscard]] std::string current_user();

inline constexpr char kPathListSep = kind() == Kind::Windows ? ';' : ':';
[[nodiscard]] std::vector<std::filesystem::path> split_path_list(std::string_view list, char sep = kPathListSep);
[[nodiscard]] std::string exe_name(std::string_view base); // appends ".exe" on Windows if missing (case-insensitive)

[[nodiscard]] std::string to_utf8(const std::filesystem::path& p);
[[nodiscard]] std::filesystem::path from_utf8(std::string_view s);

[[nodiscard]] std::tm utc_tm(std::time_t t);
[[nodiscard]] std::tm local_tm(std::time_t t);

} // namespace malgrind::host::os
