#include "settings.hpp"

#include "os/paths.hpp"

#include <nlohmann/json.hpp>

namespace malgrind::host {

void to_json(nlohmann::json& j, const RunSettings& s) {
    j = {{"env", s.env},
         {"extra_args", s.extra_args},
         {"build_flags", s.build_flags},
         {"rate_hz", s.rate_hz},
         {"elf", os::to_utf8(s.elf)},
         {"build_command", s.build_command},
         {"upload_command", s.upload_command}};
}

void from_json(const nlohmann::json& j, RunSettings& s) {
    s = {};
    if (!j.is_object()) return;
    auto text = [&](const char* key) { return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string{}; };
    s.env = text("env");
    s.extra_args = text("extra_args");
    s.build_flags = text("build_flags");
    s.elf = os::from_utf8(text("elf"));
    s.build_command = text("build_command");
    s.upload_command = text("upload_command");
    if (j.contains("rate_hz") && j["rate_hz"].is_number_unsigned()) s.rate_hz = j["rate_hz"].get<unsigned>();
}

} // namespace malgrind::host
