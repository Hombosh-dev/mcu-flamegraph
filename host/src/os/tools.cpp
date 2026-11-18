#include "os/tools.hpp"

#include "os/process.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace malgrind::host::os {

namespace {

bool contains_icase(std::string_view haystack, std::string_view needle) {
    return !std::ranges::search(haystack, needle, [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            }).empty();
}

} // namespace

std::vector<fs::path> platformio_core_dirs(const fs::path& home) {
    std::vector<fs::path> dirs;
    auto add_if_dir = [&](const fs::path& dir) {
        std::error_code ec;
        if (dir.empty() || !fs::is_directory(dir, ec)) return;
        for (const auto& existing : dirs) {
            std::error_code eq_ec;
            if (fs::equivalent(existing, dir, eq_ec) ||
                (eq_ec && existing.lexically_normal() == dir.lexically_normal())) {
                return;
            }
        }
        dirs.push_back(dir);
    };

    if (const auto env_dir = get_env("PLATFORMIO_CORE_DIR"); env_dir && !env_dir->empty()) {
        add_if_dir(from_utf8(*env_dir));
    }
    if (!home.empty()) {
        add_if_dir(home / ".platformio");
    }
    if constexpr (kind() == Kind::Windows) {
        if (const auto drive = get_env("SystemDrive"); drive && !drive->empty()) {
            std::string root = *drive;
            if (root.back() != '\\' && root.back() != '/') root.push_back('\\');
            add_if_dir(from_utf8(root) / ".platformio");
        }
    }
    return dirs;
}

fs::path venv_bin_for(Kind os_kind, const fs::path& venv) {
    return venv / (os_kind == Kind::Windows ? "Scripts" : "bin");
}

fs::path venv_bin(const fs::path& venv) {
    return venv_bin_for(kind(), venv);
}

std::optional<std::vector<std::string>> find_python() {
    if constexpr (kind() == Kind::Windows) {
        if (const auto path = get_env("PATH")) {
            for (const auto& dir : split_path_list(*path)) {
                for (const char* exe : {"python3.exe", "python.exe"}) {
                    const fs::path cand = dir / exe;
                    // WindowsApps holds zero-byte Microsoft Store aliases that open the Store instead of Python.
                    if (contains_icase(to_utf8(cand), "windowsapps")) continue;
                    std::error_code ec;
                    if (fs::is_regular_file(cand, ec)) return std::vector<std::string>{to_utf8(cand)};
                }
            }
        }
        if (const auto py = find_in_path("py")) return std::vector<std::string>{to_utf8(*py), "-3"};
        return std::nullopt;
    } else {
        if (const auto py3 = find_in_path("python3")) return std::vector<std::string>{to_utf8(*py3)};
        if (const auto py = find_in_path("python")) return std::vector<std::string>{to_utf8(*py)};
        return std::nullopt;
    }
}

std::optional<std::vector<std::string>> pio_python(const fs::path& home, const fs::path& pio) {
    const auto py_name = exe_name("python");
    for (const auto& core : platformio_core_dirs(home)) {
        const fs::path py = venv_bin(core / "penv") / py_name;
        std::error_code ec;
        if (fs::exists(py, ec)) return std::vector<std::string>{to_utf8(py)};
    }
    if constexpr (kind() != Kind::Windows) {
        if (pio.empty()) return std::nullopt;
        std::ifstream script(pio);
        std::string shebang;
        if (!std::getline(script, shebang) || !shebang.starts_with("#!")) return std::nullopt;
        std::vector<std::string> cmd;
        std::istringstream words(shebang.substr(2));
        for (std::string w; words >> w;) cmd.push_back(std::move(w));
        if (!cmd.empty()) return cmd;
    }
    return std::nullopt;
}

std::string python_hint() {
    if constexpr (kind() == Kind::Windows) return "install Python 3 from python.org";
    if constexpr (kind() == Kind::MacOS) return "install Python 3 (for example with Homebrew or from python.org)";
    return "install Python 3 with the venv module (for example: sudo apt install python3 python3-venv)";
}

} // namespace malgrind::host::os
