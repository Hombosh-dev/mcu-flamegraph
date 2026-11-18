#pragma once

// What the user may change before a run. Empty or zero means "what malgrind detected". The same record is stored with the run
// (what it was made with) and with the project (what to offer next time). See plan.hpp for how it becomes commands.

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <string>

namespace malgrind::host {

inline constexpr unsigned kDefaultRateHz = 997; // prime: no aliasing with the RTOS tick (matches the target agent default)
inline constexpr unsigned kMinRateHz = 10, kMaxRateHz = 5000;

struct RunSettings {
    std::string env{};          // PlatformIO environment; empty: the project's
    std::string extra_args{};   // words appended to both pio commands, split like a command line (no shell)
    std::string build_flags{};  // PLATFORMIO_BUILD_FLAGS: -D... flags PlatformIO adds to the project's own
    unsigned rate_hz = 0;       // samples per second per core; 0: the agent's default, kDefaultRateHz
    std::filesystem::path elf{}; // firmware ELF for function names; empty: found automatically
    std::string build_command{}, upload_command{}; // replace the detected command line (the agent's environment variables stay)

    bool operator==(const RunSettings&) const = default;
};

void to_json(nlohmann::json& j, const RunSettings& s);
void from_json(const nlohmann::json& j, RunSettings& s); // tolerant: missing keys keep their defaults

} // namespace malgrind::host
