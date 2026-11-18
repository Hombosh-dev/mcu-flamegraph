#pragma once

// Backups of a board's whole flash, taken before the agent is flashed onto it, so "remove agent" can put the original
// firmware back byte for byte (even when nobody has its source). Layout:
//   <dir>/<key>/<yyyymmdd-hhmmss>.bin      key as [0-9a-f-] (e.g. "24-0a-c4-aa-bb-0c"); a file still being written ends in ".part"
// The helpers here are pure (path validation and lookup); reading and writing flash is each Family's job.

#include <filesystem>
#include <optional>
#include <string_view>

namespace malgrind::host {

// Where to write the backup of `key` taken at `stamp`: <dir>/<key>/<stamp>.bin.part (the directory is created by the
// caller). nullopt if `key` is malformed, so it can never name a path outside `dir`.
[[nodiscard]] std::optional<std::filesystem::path> backup_path(const std::filesystem::path& dir, std::string_view key,
                                                               std::string_view stamp);

// The newest finished backup (*.bin) of that board, nullopt if there is none or `key` is malformed.
[[nodiscard]] std::optional<std::filesystem::path> latest_backup(const std::filesystem::path& dir, std::string_view key);

} // namespace malgrind::host
