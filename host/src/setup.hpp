#pragma once

// Installing PlatformIO for the user. It goes into <core>/penv (a Python virtual environment of its own, no elevated
// privileges needed), which is where find_pio() looks after PATH.

#include "ops.hpp"
#include "os/paths.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace malgrind::host {

// Pure layout helper: builds the install commands for `os_kind` targeting `env_dir` (so tests can verify both layouts).
[[nodiscard]] std::vector<Invocation> install_pio_steps_for(os::Kind os_kind, const std::vector<std::string>& python_cmd,
                                                            const std::filesystem::path& env_dir);

// The two commands: create the environment (skipped when it exists), then pip install platformio into it.
[[nodiscard]] std::vector<Invocation> install_pio_steps(const std::vector<std::string>& python_cmd,
                                                        const std::filesystem::path& home);

// Runs the steps, every output line to on_line, and checks that pio exists afterwards. Throws with what to do about the usual
// failures: missing venv support, no network.
void install_pio(const std::vector<std::string>& python_cmd, const std::filesystem::path& home, const LineFn& on_line);

} // namespace malgrind::host
