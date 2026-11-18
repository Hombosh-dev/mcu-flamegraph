#include "projects.hpp"

#include "os/paths.hpp"
#include "store.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <tuple>

namespace fs = std::filesystem;

namespace malgrind::host {

namespace {

bool valid_id(std::string_view id) {
    return !id.empty() && id.size() <= 40 &&
           std::ranges::all_of(id, [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; });
}

// project.json is replaced atomically: a crash leaves the old file or the new one, never half of one.
void write_project(const fs::path& dir, const ProjectMeta& p) {
    const auto tmp = dir / "project.json.tmp";
    std::ofstream(tmp, std::ios::trunc) << nlohmann::json(p).dump(2);
    fs::rename(tmp, dir / "project.json");
}

std::optional<ProjectMeta> read_project_file(const fs::path& dir) {
    std::ifstream in(dir / "project.json");
    const auto j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_object()) return std::nullopt;
    ProjectMeta p;
    p.id = j.value("id", os::to_utf8(dir.filename()));
    p.name = j.value("name", "");
    const auto source = j.value("source", "none");
    p.origin = source == "folder" ? Origin::Folder : source == "git" ? Origin::Git : Origin::None;
    p.dir = os::from_utf8(j.value("dir", ""));
    p.url = j.value("url", "");
    p.env = j.value("env", "");
    p.created = j.value("created", "");
    p.elf = os::from_utf8(j.value("elf", ""));
    p.trusted = j.value("trusted", false);
    if (j.contains("last_settings") && j["last_settings"].is_object()) p.last_settings = j["last_settings"].get<RunSettings>();
    return p;
}

} // namespace

std::string_view origin_name(Origin origin) noexcept {
    return origin == Origin::Folder ? "folder" : origin == Origin::Git ? "git" : "none";
}

void to_json(nlohmann::json& j, const ProjectMeta& p) {
    j = {{"id", p.id},           {"name", p.name},         {"source", origin_name(p.origin)}, {"dir", os::to_utf8(p.dir)},
         {"url", p.url},         {"env", p.env},           {"created", p.created},            {"elf", os::to_utf8(p.elf)},
         {"trusted", p.trusted}, {"has_source", p.origin != Origin::None}};
    j["last_settings"] = p.last_settings ? nlohmann::json(*p.last_settings) : nlohmann::json(nullptr);
}

ProjectStore::ProjectStore(fs::path root) : root_(std::move(root)) { fs::create_directories(root_); }

fs::path ProjectStore::default_root() {
    return os::safe_projects_root();
}

std::optional<fs::path> ProjectStore::dir_of(std::string_view id) const {
    if (!valid_id(id)) return std::nullopt;
    return root_ / os::from_utf8(id);
}

fs::path ProjectStore::clone_dir(std::string_view id) const {
    const auto dir = dir_of(id);
    return dir ? *dir / "src" : fs::path{};
}

ProjectMeta ProjectStore::create(ProjectMeta meta, const fs::path& elf) {
    const auto base = "p-" + utc_now("%Y%m%d-%H%M%S");
    fs::path dir;
    for (int n = 1;; ++n) { // create_directory is the atomic claim of an id
        meta.id = n == 1 ? base : std::format("{}-{}", base, n);
        dir = root_ / os::from_utf8(meta.id);
        if (fs::create_directory(dir)) break;
    }
    meta.created = utc_now("%Y-%m-%dT%H:%M:%SZ");
    if (!elf.empty()) {
        std::error_code ec;
        fs::copy_file(elf, dir / "firmware.elf", fs::copy_options::overwrite_existing, ec);
        if (!ec) meta.elf = dir / "firmware.elf";
    }
    write_project(dir, meta);
    return meta;
}

std::vector<ProjectMeta> ProjectStore::list() const {
    std::vector<ProjectMeta> projects;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root_, ec))
        if (entry.is_directory())
            if (auto p = read_project_file(entry.path())) projects.push_back(std::move(*p));
    std::ranges::sort(projects, [](const ProjectMeta& a, const ProjectMeta& b) { return std::tie(b.created, b.id) < std::tie(a.created, a.id); });
    return projects;
}

std::optional<ProjectMeta> ProjectStore::get(std::string_view id) const {
    const auto dir = dir_of(id);
    return dir ? read_project_file(*dir) : std::nullopt;
}

std::optional<ProjectMeta> ProjectStore::update(std::string_view id, const ProjectUpdate& change) {
    const auto dir = dir_of(id);
    auto meta = dir ? read_project_file(*dir) : std::nullopt;
    if (!meta) return std::nullopt;
    if (change.name) meta->name = *change.name;
    if (change.env) meta->env = *change.env;
    if (change.trusted) meta->trusted = *change.trusted;
    if (change.dir) meta->dir = *change.dir;
    if (change.last_settings) meta->last_settings = *change.last_settings;
    write_project(*dir, *meta);
    return meta;
}

bool ProjectStore::remove(std::string_view id) {
    const auto dir = dir_of(id);
    std::error_code ec;
    return dir && fs::exists(*dir, ec) && fs::remove_all(*dir, ec) > 0;
}

} // namespace malgrind::host
