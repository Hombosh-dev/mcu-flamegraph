#include "os/paths.hpp"

#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

namespace malgrind::host::os {

std::string_view kind_name() noexcept {
    switch (kind()) {
        case Kind::Linux: return "linux";
        case Kind::MacOS: return "macos";
        case Kind::Windows: return "windows";
    }
    return "linux";
}

std::vector<fs::path> split_path_list(std::string_view list, char sep) {
    std::vector<fs::path> out;
    std::size_t pos = 0;
    while (pos <= list.size()) {
        const auto next = list.find(sep, pos);
        const auto part = next == std::string_view::npos ? list.substr(pos) : list.substr(pos, next - pos);
        if (!part.empty()) out.push_back(from_utf8(part));
        if (next == std::string_view::npos) break;
        pos = next + 1;
    }
    return out;
}

std::string exe_name(std::string_view base) {
    if constexpr (kind() == Kind::Windows) {
        if (base.size() >= 4) {
            const auto tail = base.substr(base.size() - 4);
            if (tail[0] == '.' &&
                std::tolower(static_cast<unsigned char>(tail[1])) == 'e' &&
                std::tolower(static_cast<unsigned char>(tail[2])) == 'x' &&
                std::tolower(static_cast<unsigned char>(tail[3])) == 'e') {
                return std::string(base);
            }
        }
        return std::string(base) + ".exe";
    }
    return std::string(base);
}

fs::path safe_projects_root_for(Kind os_kind, const fs::path& user_data, const fs::path& home, std::string_view system_drive) {
    if (os_kind == Kind::MacOS) return home / ".malgrind/projects";
    const fs::path candidate = user_data / "malgrind/projects";
    if (os_kind == Kind::Windows) {
        const std::string utf8 = to_utf8(candidate);
        const bool safe = !user_data.empty() && !utf8.empty() &&
                          std::ranges::all_of(utf8, [](unsigned char c) { return c < 0x80 && c != ' '; });
        if (safe) return candidate;
        std::string drive = system_drive.empty() ? "C:" : std::string(system_drive);
        if (drive.back() != '/' && drive.back() != '\\') drive.push_back('/');
        return from_utf8(drive + ".malgrind/projects");
    }
    return candidate;
}

fs::path safe_projects_root() {
    return safe_projects_root_for(kind(), user_data_dir(), home_dir(), get_env("SystemDrive").value_or("C:"));
}

} // namespace malgrind::host::os
