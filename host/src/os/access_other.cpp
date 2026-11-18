#include "os/access.hpp"

namespace malgrind::host::os {

PortAccess port_access(const std::string& /*path*/) {
    return PortAccess{.ok = true, .group = {}, .in_group = false, .fixable = false};
}

bool can_grant_access() { return false; }

std::optional<Invocation> grant_access_command(const std::string& /*path*/, const std::string& /*user*/,
                                                 const std::string& /*group*/) {
    return std::nullopt;
}

std::string manual_access_commands(const std::string& /*path*/, const std::string& /*user*/, const std::string& /*group*/) {
    return {};
}

std::string grant_access_unavailable(const std::string& /*path*/, const std::string& /*user*/, const std::string& /*group*/) {
    return "granting serial port access automatically is not supported on this OS";
}

} // namespace malgrind::host::os
