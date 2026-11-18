#pragma once

// Whether this user may open a serial port, and how to give them access. On Linux a USB serial port belongs to a group
// ("dialout" on Debian and Ubuntu); a user who is not in it gets "Permission denied". Two fixes exist and both are applied:
// an ACL on the device (works at once, until the board is replugged) and membership of the group (permanent, after the next login).

#include "pio.hpp"

#include <optional>
#include <string>

namespace malgrind::host::os {

struct PortAccess {
    bool ok = true;            // may open now (macOS/Windows: always true; failures show up as OpenFailure instead)
    std::string group;         // Linux: owning group; else ""
    bool in_group = false;
    bool fixable = false;      // a grant_access request can fix it (Linux, not ok, pkexec present)
};

[[nodiscard]] PortAccess port_access(const std::string& path);
[[nodiscard]] bool can_grant_access();                 // Linux && pkexec found
[[nodiscard]] std::optional<Invocation> grant_access_command(const std::string& path, const std::string& user,
                                                             const std::string& group);   // Linux only, else nullopt
[[nodiscard]] std::string manual_access_commands(const std::string& path, const std::string& user, const std::string& group);
[[nodiscard]] std::string grant_access_unavailable(const std::string& path, const std::string& user, const std::string& group);

} // namespace malgrind::host::os
