#pragma once

// PlatformIO integration: finding pio, reading the project, and flashing plain firmware without the agent.
//
// Paths given to pio must be absolute: `-d <project>` makes pio change directory.

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace malgrind::host {

// The `pio` executable: PATH first, then PlatformIO's own installs under `home` (a GUI app often lacks the shell's PATH).
[[nodiscard]] std::optional<std::filesystem::path> find_pio(const std::filesystem::path& home);

struct ProjectInfo {
    std::filesystem::path dir;
    std::vector<std::string> envs; // [env:NAME] sections, in file order
    std::string default_env;       // first of [platformio] default_envs that exists, else the first env
    // `platform` and `framework` of each env (its own, else the shared [env] section's); an env that says nothing has no entry
    // (it may inherit through `extends`, which is not followed)
    std::map<std::string, std::string> platforms, frameworks;
};

// Reads dir/platformio.ini. On failure `why` says what is wrong (no file, no env).
[[nodiscard]] std::optional<ProjectInfo> read_project(const std::filesystem::path& dir, std::string& why);

struct Invocation {
    std::vector<std::string> argv;
    std::vector<std::string> env; // "NAME=value", added to the environment of the child
};

// The user's own firmware, built and uploaded as PlatformIO would: this is what "remove agent" runs.
[[nodiscard]] Invocation plain_flash(const std::filesystem::path& pio, const ProjectInfo& project, const std::string& env,
                                     const std::string& port);

} // namespace malgrind::host
