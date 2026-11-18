#pragma once

// Projects: what the user profiles. A project is a container (a name, and optionally the source code of the firmware), so a
// board whose code nobody has can still be profiled (attach now, a debug probe later). On disk:
//   <root>/<id>/project.json     ProjectMeta, rewritten atomically
//   <root>/<id>/src/             the clone of a GitHub project
//   <root>/<id>/firmware.elf     the ELF of a project without source code, if the user gave one
// A project of Origin::Folder only points at the user's own folder, which is never modified or deleted.

#include "settings.hpp"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace malgrind::host {

enum class Origin { Folder, Git, None };
[[nodiscard]] std::string_view origin_name(Origin origin) noexcept; // "folder", "git", "none"

struct ProjectMeta {
    std::string id{}, name{};
    Origin origin = Origin::None;
    std::filesystem::path dir{}; // the PlatformIO project (platformio.ini inside); empty for Origin::None
    std::string url{};           // Origin::Git: the clone URL
    std::string env{};           // chosen PlatformIO environment; empty: the project's default
    std::string created{};       // ISO-8601 UTC
    std::filesystem::path elf{}; // Origin::None: firmware.elf inside the project directory, or empty
    bool trusted = false;        // building runs the project's scripts: a Git project must be trusted first
    std::optional<RunSettings> last_settings{}; // what the last run used: offered again, before the next one
};

void to_json(nlohmann::json& j, const ProjectMeta& p); // adds "has_source"

struct ProjectUpdate {
    std::optional<std::string> name, env;
    std::optional<bool> trusted;
    std::optional<std::filesystem::path> dir;
    std::optional<RunSettings> last_settings;
};

class ProjectStore {
public:
    explicit ProjectStore(std::filesystem::path root); // created if missing
    // os::user_data_dir() / "malgrind/projects" (macOS: ~/.malgrind/projects, a path with no space)
    [[nodiscard]] static std::filesystem::path default_root();

    // Assigns id ("p-yyyymmdd-hhmmss", "-2"... on a clash) and `created`. `elf`: copied to <id>/firmware.elf (Origin::None).
    // A Git project's `dir` is set by the caller (after the clone) with update().
    [[nodiscard]] ProjectMeta create(ProjectMeta meta, const std::filesystem::path& elf = {});
    [[nodiscard]] std::vector<ProjectMeta> list() const; // newest first
    [[nodiscard]] std::optional<ProjectMeta> get(std::string_view id) const;
    std::optional<ProjectMeta> update(std::string_view id, const ProjectUpdate& change);
    // Deletes the record and a clone. false if there is no such project. Never touches a Folder project's own folder.
    bool remove(std::string_view id);
    // Where the clone of project `id` goes. Empty for a malformed id.
    [[nodiscard]] std::filesystem::path clone_dir(std::string_view id) const;

private:
    [[nodiscard]] std::optional<std::filesystem::path> dir_of(std::string_view id) const;
    std::filesystem::path root_;
};

} // namespace malgrind::host
