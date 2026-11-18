#include "setup.hpp"

#include "os/paths.hpp"
#include "os/process.hpp"
#include "os/tools.hpp"

#include <format>
#include <stdexcept>

namespace fs = std::filesystem;

namespace malgrind::host {

namespace {

fs::path env_dir(const fs::path& home) {
    if (const auto dirs = os::platformio_core_dirs(home); !dirs.empty()) return dirs[0] / "penv";
    return home / ".platformio/penv";
}

} // namespace

std::vector<Invocation> install_pio_steps_for(os::Kind os_kind, const std::vector<std::string>& python_cmd,
                                              const fs::path& env) {
    const fs::path bin = os::venv_bin_for(os_kind, env);
    const fs::path venv_py = bin / (os_kind == os::Kind::Windows ? "python.exe" : "python");
    const fs::path venv_pip = bin / (os_kind == os::Kind::Windows ? "pip.exe" : "pip");
    std::vector<Invocation> steps;
    std::error_code ec;
    // "has a python" is not enough: where the venv package is missing, `venv` fails after it made python, and that
    // half-made environment has no pip. Only an environment that can install packages is reused.
    if (!fs::exists(venv_py, ec) || !fs::exists(venv_pip, ec)) {
        auto argv = python_cmd;
        argv.insert(argv.end(), {"-m", "venv", os::to_utf8(env)});
        steps.push_back({std::move(argv), {}});
    }
    steps.push_back({{os::to_utf8(venv_py), "-m", "pip", "install", "--disable-pip-version-check", "platformio"}, {}});
    return steps;
}

std::vector<Invocation> install_pio_steps(const std::vector<std::string>& python_cmd, const fs::path& home) {
    return install_pio_steps_for(os::kind(), python_cmd, env_dir(home));
}

void install_pio(const std::vector<std::string>& python_cmd, const fs::path& home, const LineFn& on_line) {
    const fs::path env = env_dir(home);
    std::error_code ec;
    const bool env_existed = fs::exists(env, ec);
    const auto steps = install_pio_steps(python_cmd, home);
    for (const auto& step : steps) {
        std::string output;
        const int code = os::run_lines(step.argv, step.env, [&](std::string_view line) {
            output.append(line).append("\n");
            on_line(line);
        });
        if (code == 0) continue;
        const bool venv_step = step.argv.size() >= python_cmd.size() + 2 &&
                               step.argv[python_cmd.size()] == "-m" &&
                               step.argv[python_cmd.size() + 1] == "venv";
        if (venv_step && !env_existed) { // what we half-made would make the next try fail the same way
            std::error_code rm_ec;
            fs::remove_all(env, rm_ec);
        }
        if (venv_step && (output.find("ensurepip") != std::string::npos || output.find("python3-venv") != std::string::npos))
            throw std::runtime_error("Python cannot create virtual environments here: " + os::python_hint() + ", then try again");
        if (venv_step) throw std::runtime_error(std::format("could not create {} (python exit code {})", os::to_utf8(env), code));
        throw std::runtime_error(std::format("pip could not install PlatformIO (exit code {}): is the computer online?", code));
    }
    if (!fs::exists(os::venv_bin(env) / os::exe_name("pio"), ec))
        throw std::runtime_error("the install finished but there is no pio in " + os::to_utf8(env));
}

} // namespace malgrind::host
