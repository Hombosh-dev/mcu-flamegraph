#include "backup.hpp"

#include <algorithm>

namespace fs = std::filesystem;

namespace malgrind::host {

namespace {

// Board identity keys: lowercase hex and '-' only (e.g. "24-0a-c4-aa-bb-0c"), never empty or path-like.
bool valid_board_key(std::string_view key) {
    if (key.empty() || key.size() > 64 || key.front() == '-' || key.back() == '-') return false;
    return std::ranges::all_of(key, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-';
    });
}

} // namespace

std::optional<fs::path> backup_path(const fs::path& dir, std::string_view key, std::string_view stamp) {
    if (!valid_board_key(key)) return std::nullopt;
    return dir / std::string(key) / (std::string(stamp) + ".bin.part");
}

std::optional<fs::path> latest_backup(const fs::path& dir, std::string_view key) {
    if (!valid_board_key(key)) return std::nullopt;
    std::error_code ec;
    std::optional<fs::path> best;
    for (const auto& entry : fs::directory_iterator(dir / std::string(key), ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".bin") continue;
        if (!best || entry.path().filename() > best->filename()) best = entry.path(); // names are timestamps
    }
    return best;
}

} // namespace malgrind::host
