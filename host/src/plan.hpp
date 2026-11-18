#pragma once

// From RunSettings to the commands a run executes. One function makes both the preview the app shows and the real run, so
// what the user reads is what runs. No shell is involved anywhere: a command line is split into words by split_command and
// run as an argument list.

#include "ops.hpp"
#include "settings.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace malgrind::host {

// Splits like a shell would, without being one: words by white space, 'single quotes' literal, "double quotes" with \" and \\,
// a backslash outside quotes escapes the next character. nullopt for an unbalanced quote or a trailing backslash.
[[nodiscard]] std::optional<std::vector<std::string>> split_command(std::string_view line);

// The inverse: quotes what needs it, so split_command(join_command(x)) == x.
[[nodiscard]] std::string join_command(const std::vector<std::string>& words);

struct Plan {
    Invocation build;                  // compiles the project with the agent
    Invocation upload;                 // flashes it (after the backup)
    std::filesystem::path elf;         // firmware ELF produced by the agent build
    std::string build_text, upload_text; // the two command lines as text, for the app to show
    std::string env;                   // the PlatformIO environment the plan builds (the setting, else the detected one)
    unsigned rate_hz = 0;              // 0: the agent's default; written into the build config by write_plan_files
};

// The detected commands, changed by `settings`: extra arguments appended to both, PLATFORMIO_BUILD_FLAGS in their environment,
// the rate carried to write_plan_files, a command line replaced when an override is given (its environment variables stay: they
// carry the agent).
// `prepared.env` is the detected environment. Writes nothing. Throws std::runtime_error with a message for the field at fault:
// unknown environment, rate outside kMinRateHz..kMaxRateHz, a command that does not split or whose program is not found, an
// ELF that does not exist.
[[nodiscard]] Plan make_plan(const Prepared& prepared, const std::filesystem::path& agent_dir, const RunSettings& settings);

// What a plan needs on disk before it runs (generated agent build configuration under <project>/.pio/malgrind/).
void write_plan_files(const Prepared& prepared, const std::filesystem::path& agent_dir, const Plan& plan);

} // namespace malgrind::host
