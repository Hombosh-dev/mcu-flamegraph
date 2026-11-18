#include "os/access.hpp"

#include "os/paths.hpp"
#include "os/process.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <grp.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace malgrind::host::os {

namespace {

bool plain_name(const std::string& s) {
    return !s.empty() && s.size() <= 64 && s[0] != '-' &&
           std::ranges::all_of(s, [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '_' || c == '-'; });
}

// setfacl: access at once. usermod: access after the next login (skipped when the device belongs to root: no group helps there).
// Either one is enough, so one failing does not fail the call.
constexpr const char* kScript = R"(ok=1
if setfacl -m "u:$1:rw" "$2"; then ok=0; fi
if [ "$3" != root ] && usermod -aG "$3" "$1"; then ok=0; fi
exit $ok)";

} // namespace

bool can_grant_access() { return find_in_path("pkexec").has_value(); }

PortAccess port_access(const std::string& path) {
    PortAccess result;
    result.ok = ::access(path.c_str(), R_OK | W_OK) == 0;
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return result;
    std::array<char, 1024> buf{};
    group grp{};
    group* found = nullptr;
    if (getgrgid_r(st.st_gid, &grp, buf.data(), buf.size(), &found) == 0 && found) result.group = grp.gr_name;
    std::vector<gid_t> groups(static_cast<std::size_t>(std::max(getgroups(0, nullptr), 0)));
    const int n = getgroups(static_cast<int>(groups.size()), groups.data());
    result.in_group = st.st_gid == getegid() || (n > 0 && std::find(groups.begin(), groups.begin() + n, st.st_gid) != groups.begin() + n);
    result.fixable = !result.ok && can_grant_access();
    return result;
}

std::optional<Invocation> grant_access_command(const std::string& path, const std::string& user, const std::string& group) {
    const auto pkexec = find_in_path("pkexec");
    if (!pkexec) return std::nullopt;
    if (!plain_name(user) || !plain_name(group) || !path.starts_with("/dev/") || path.find("..") != std::string::npos) return std::nullopt;
    if (!std::ranges::all_of(path, [](unsigned char c) { return std::isalnum(c) || c == '/' || c == '.' || c == '_' || c == '-'; })) return std::nullopt;
    return Invocation{{to_utf8(*pkexec), "/bin/sh", "-c", kScript, "sh", user, path, group}, {}};
}

std::string manual_access_commands(const std::string& path, const std::string& user, const std::string& group) {
    return std::format("sudo setfacl -m u:{0}:rw {1}\nsudo usermod -aG {2} {0}   # then log out and in again", user, path, group);
}

std::string grant_access_unavailable(const std::string& path, const std::string& user, const std::string& group) {
    return "pkexec (polkit) was not found. Run these two lines in a terminal, then press Rescan:\n" +
           manual_access_commands(path, user, group);
}

} // namespace malgrind::host::os
