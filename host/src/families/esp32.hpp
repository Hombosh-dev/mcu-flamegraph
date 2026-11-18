#pragma once

// ESP32 (ESP-IDF / PlatformIO espressif32) family adapter and helpers.
//
// Agent build, found working on espressif32@6.9.0 / ESP-IDF 5.3.1. The user's project is not modified; all output goes
// to <project>/.pio/malgrind/. Environment of the `pio run` call:
//   PLATFORMIO_BUILD_DIR     <project>/.pio/malgrind/build
//   PLATFORMIO_EXTRA_SCRIPTS pre:<agent-dir>/agent/esp32/inject.py   (adds the `malgrind` IDF component and points the
//                                                                      build at MALGRIND_SDKCONFIG)
//   MALGRIND_AGENT_DIR       <agent-dir>/agent/esp32
//   MALGRIND_SDKCONFIG       <project>/.pio/malgrind/sdkconfig.<env>   (NOT inside the build dir; the user's sdkconfig.<env> with the keys of
//                                                                      agent/esp32/sdkconfig.agent replaced, see below)
// Command: pio run -d <project> -e <env> -t upload --upload-port <port>
// (`pio run` has no -O option; options reach the IDF builder only through the pre-script.)
//
// Findings that shape the code:
//  * sdkconfig merge: an agent key replaces BOTH forms of the user's line (`CONFIG_X=..` and `# CONFIG_X is not set`);
//    sdkconfig.agent lists `# CONFIG_ESP_CONSOLE_UART_DEFAULT is not set` too, because DEFAULT/CUSTOM are one Kconfig
//    choice and a user's DEFAULT=y would otherwise win. Without it the console stayed at 115200.
//  * PlatformIO deletes its whole build directory when it has no project.checksum (every first build). The seeded
//    sdkconfig therefore lives next to the build directory, not in it. (A spike that reused an old directory hid this.)
//  * Paths given to pio must be absolute: `-d <project>` makes pio change directory. A relative project path once
//    created <project>/<project>/.pio/malgrind and flashed a firmware built from default settings.
//  * PlatformIO's IDF builder refuses a project path containing a space (espidf.py checks BUILD_DIR).
//  * IDF's WHOLE_ARCHIVE is unusable under PlatformIO (it sorts link flags, so --whole-archive lands around everything);
//    the agent object is forced into the link with `-u malgrind_autostart_link` instead.

#include "families/family.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace malgrind::host {

inline constexpr unsigned kEsp32AgentBaud = 921600;

// Stored in the agent's rodata (agent/esp32/malgrind/autostart.cpp). A flash image that contains it is our own build,
// and backing that up would only save the agent as if it were the user's firmware.
inline constexpr std::string_view kEsp32AgentMarker = "malgrind-agent-v1";

[[nodiscard]] const Family& esp32_family();

// The user's sdkconfig with the agent's keys applied: every `CONFIG_X=..` or `# CONFIG_X is not set` line whose key the
// agent sets is dropped, then the agent's key lines are appended. Other user lines stay, in order.
[[nodiscard]] std::string merge_sdkconfig(std::string_view user, std::string_view agent);

// Writes <project>/.pio/malgrind/sdkconfig.<env> (the user's sdkconfig with the agent's keys); nothing else of the project is
// touched, and an unchanged file is not rewritten. `agent_dir` holds agent/esp32/ (the app's resources, or the repo).
// Throws std::runtime_error if the agent files are missing.
// `rate_hz` (0: the agent's default) becomes CONFIG_MALGRIND_RATE_HZ in it.
void write_agent_sdkconfig(const ProjectInfo& project, const std::string& env, const std::filesystem::path& agent_dir,
                           unsigned rate_hz = 0);

// The command and environment that build the project with the agent (`upload` true: and flash it). Writes nothing: a
// preview of a run can be made with it. Throws if the agent files are missing.
[[nodiscard]] Invocation agent_command(const std::filesystem::path& pio, const ProjectInfo& project, const std::string& env,
                                       const std::string& port, const std::filesystem::path& agent_dir, bool upload = true);

// write_agent_sdkconfig + agent_command (`upload` false: only builds; a second call with true then uploads without
// compiling again).
[[nodiscard]] Invocation agent_build(const std::filesystem::path& pio, const ProjectInfo& project, const std::string& env,
                                     const std::string& port, const std::filesystem::path& agent_dir, bool upload = true);

// The ELF agent_build produces (it symbolizes the frames the flashed board sends).
[[nodiscard]] std::filesystem::path agent_elf(const ProjectInfo& project, const std::string& env);

// esptool.py (PlatformIO's copy, run with PlatformIO's Python) as `esptool.py --port P --baud 921600 <args>`.
// nullopt if the launcher's interpreter or the esptool package cannot be found. esptool resets the board, and every
// command prints "Chip is ..." and "MAC: ..." when it connects.
[[nodiscard]] std::optional<Invocation> esptool(const std::filesystem::path& pio, const std::filesystem::path& home,
                                                const std::string& port, std::vector<std::string> args);

// "Chip is ESP32-D0WD-V3 (revision v3.1)" anywhere in esptool's output -> "ESP32-D0WD-V3 (revision v3.1)".
[[nodiscard]] std::optional<std::string> parse_chip(std::string_view esptool_output);

// "MAC: 24:0a:c4:AA:bb:0c" anywhere in esptool's output -> "24-0a-c4-aa-bb-0c".
[[nodiscard]] std::optional<std::string> parse_mac(std::string_view esptool_output);

// "Detected flash size: 4MB" (or KB) anywhere in the output -> bytes. nullopt for "Unknown" and other units.
[[nodiscard]] std::optional<std::uint32_t> parse_flash_size(std::string_view esptool_output);

} // namespace malgrind::host
