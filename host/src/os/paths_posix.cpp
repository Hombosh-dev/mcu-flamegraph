#include "os/paths.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <pwd.h>
#include <unistd.h>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

namespace malgrind::host::os {

std::optional<std::string> get_env(std::string_view name) {
    const std::string key(name);
    if (const char* val = std::getenv(key.c_str())) return std::string(val);
    return std::nullopt;
}

void set_env(std::string_view name, std::string_view value) {
    const std::string key(name);
    const std::string val(value);
    ::setenv(key.c_str(), val.c_str(), 1);
}

void unset_env(std::string_view name) {
    const std::string key(name);
    ::unsetenv(key.c_str());
}

fs::path home_dir() {
    if (const auto home = get_env("HOME"); home && !home->empty()) return from_utf8(*home);
    std::vector<char> buf(1024);
    passwd pw{};
    passwd* found = nullptr;
    for (;;) {
        const int rc = getpwuid_r(geteuid(), &pw, buf.data(), buf.size(), &found);
        if (rc == 0) return (found && pw.pw_dir) ? from_utf8(pw.pw_dir) : fs::path{};
        if (rc != ERANGE || buf.size() >= 65536) return {};
        buf.resize(buf.size() * 2);
    }
}

fs::path user_data_dir() {
    if constexpr (kind() == Kind::MacOS) {
        return home_dir() / "Library/Application Support";
    } else {
        if (const auto xdg = get_env("XDG_DATA_HOME"); xdg && !xdg->empty()) return from_utf8(*xdg);
        return home_dir() / ".local/share";
    }
}

fs::path executable_path() {
#ifdef __APPLE__
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size == 0) return {};
    std::vector<char> buf(size);
    if (_NSGetExecutablePath(buf.data(), &size) == 0) return fs::path(buf.data());
    return {};
#else
    std::error_code ec;
    const auto exe = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path{} : exe;
#endif
}

std::string current_user() {
    std::vector<char> buf(1024);
    passwd pw{};
    passwd* found = nullptr;
    for (;;) {
        const int rc = getpwuid_r(geteuid(), &pw, buf.data(), buf.size(), &found);
        if (rc == 0) return (found && pw.pw_name) ? std::string(pw.pw_name) : std::string{};
        if (rc != ERANGE || buf.size() >= 65536) return {};
        buf.resize(buf.size() * 2);
    }
}

std::string to_utf8(const fs::path& p) { return p.string(); }

fs::path from_utf8(std::string_view s) { return fs::path(s); }

std::tm utc_tm(std::time_t t) {
    std::tm tm{};
    gmtime_r(&t, &tm);
    return tm;
}

std::tm local_tm(std::time_t t) {
    std::tm tm{};
    localtime_r(&t, &tm);
    return tm;
}

} // namespace malgrind::host::os
