#include "pio.hpp"

#include "os/paths.hpp"
#include "os/process.hpp"
#include "os/tools.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <string_view>

namespace fs = std::filesystem;

namespace malgrind::host {

namespace {

// pio runs with `-d <project>`, which changes its directory: every path handed to it must be absolute.
fs::path absolute_path(const fs::path& path) {
    auto abs = fs::absolute(path).lexically_normal();
    if (abs.filename().empty() && abs != abs.root_path()) abs = abs.parent_path(); // "/a/b/" -> "/a/b"
    return abs;
}

std::string trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

} // namespace

std::optional<fs::path> find_pio(const fs::path& home) {
    if (auto found = os::find_in_path("pio")) return found;
    const auto bin = os::exe_name("pio");
    for (const auto& core : os::platformio_core_dirs(home)) {
        const fs::path candidate = os::venv_bin(core / "penv") / bin;
        if (std::error_code ec; fs::exists(candidate, ec)) return candidate;
    }
    if constexpr (os::kind() != os::Kind::Windows) {
        if (!home.empty()) {
            const fs::path local = home / ".local/bin" / bin;
            if (std::error_code ec; fs::exists(local, ec)) return local;
        }
    }
    return std::nullopt;
}

std::optional<ProjectInfo> read_project(const fs::path& given, std::string& why) {
    const fs::path dir = absolute_path(given);
    std::ifstream in(dir / "platformio.ini");
    if (!in) {
        why = "no platformio.ini in " + os::to_utf8(dir);
        return std::nullopt;
    }
    ProjectInfo info{dir, {}, {}, {}, {}};
    std::vector<std::string> default_envs;
    std::string section;
    std::map<std::string, std::map<std::string, std::string>> settings; // section -> key -> value
    bool in_default_envs = false;
    for (std::string raw; std::getline(in, raw);) {
        const bool indented = !raw.empty() && std::isspace(static_cast<unsigned char>(raw.front()));
        const auto line = trim(raw);
        auto add_names = [&](std::string_view names) {
            std::istringstream words{std::string(names)};
            for (std::string w; std::getline(words, w, ',');)
                for (std::istringstream parts(w); parts >> w;) default_envs.push_back(w);
        };
        if (line.starts_with("[")) {
            section = line.substr(1, line.find(']') - 1);
            in_default_envs = false;
            if (section.starts_with("env:")) info.envs.push_back(trim(section.substr(4)));
        } else if (section == "platformio" && line.starts_with("default_envs")) {
            const auto eq = line.find('=');
            if (eq != std::string::npos) add_names(line.substr(eq + 1));
            in_default_envs = true;
        } else if (in_default_envs && indented && !line.empty() && line.find('=') == std::string::npos) {
            add_names(line); // a multi-line value
        } else {
            in_default_envs = false;
            if (const auto eq = line.find('='); eq != std::string::npos && !indented) settings[section][trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
        }
    }
    for (const auto& env : info.envs) {
        for (const char* key : {"platform", "framework"}) {
            auto& target = std::string_view(key) == "platform" ? info.platforms : info.frameworks;
            for (const auto& sec : {"env:" + env, std::string("env")}) {
                const auto s = settings.find(sec);
                if (s == settings.end() || !s->second.contains(key)) continue;
                target[env] = s->second.at(key);
                break;
            }
        }
    }
    if (info.envs.empty()) {
        why = "platformio.ini in " + os::to_utf8(dir) + " has no [env:...] section";
        return std::nullopt;
    }
    info.default_env = info.envs.front();
    for (const auto& name : default_envs) {
        if (std::ranges::find(info.envs, name) != info.envs.end()) {
            info.default_env = name;
            break;
        }
    }
    return info;
}

Invocation plain_flash(const fs::path& pio, const ProjectInfo& project, const std::string& env, const std::string& port) {
    return {{os::to_utf8(pio), "run", "-d", os::to_utf8(project.dir), "-e", env, "-t", "upload", "--upload-port", port}, {}};
}

} // namespace malgrind::host
