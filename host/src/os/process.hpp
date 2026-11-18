#pragma once

// Child process spawning and PATH lookup without a shell.
// Windows refuses .bat/.cmd launchers to prevent command-line argument injection.

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace malgrind::host::os {

// stdout of the child process (stderr discarded). Empty if it cannot be started.
std::string capture(const std::vector<std::string>& argv);

// Run with inherited stdio. Returns the exit code, or -1 if the process cannot be started.
int run(const std::vector<std::string>& argv);

// Runs argv with `env_add` ("NAME=value") added to this process's environment. stdout and stderr arrive, in order,
// as lines (a lone \r also ends a line, so progress bars become lines). Returns the exit code, or -1 if it cannot
// be started.
int run_lines(const std::vector<std::string>& argv, const std::vector<std::string>& env_add,
              const std::function<void(std::string_view line)>& on_line);

// First match in PATH. Windows: if `name` has no extension, tries each PATHEXT entry, ignoring .bat/.cmd.
[[nodiscard]] std::optional<std::filesystem::path> find_in_path(const std::string& name);

// Pure, testable on every OS: MSVCRT quoting rules (backslashes before quotes doubled, args with space/tab/quote quoted).
[[nodiscard]] std::wstring windows_command_line(const std::vector<std::string>& argv_utf8);

} // namespace malgrind::host::os
