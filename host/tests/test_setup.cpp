// Installing PlatformIO and locating Python, PlatformIO core directories, virtual environments and toolchains.
#include "env.hpp"
#include "os/paths.hpp"
#include "os/tools.hpp"
#include "pio.hpp"
#include "setup.hpp"
#include "symbols.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace malgrind::host;
namespace fs = std::filesystem;

namespace {

bool contains(const std::string& s, std::string_view part) { return s.find(part) != std::string::npos; }

fs::path fresh_dir(const char* name) {
    const auto p = fs::canonical(fs::temp_directory_path()) / name;
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

struct Fake {
    ScopedEnv save_path{"PATH"};
    ScopedEnv save_core{"PLATFORMIO_CORE_DIR", std::nullopt};
    ScopedEnv save_drive{"SystemDrive", std::nullopt};
    ScopedEnv save_log{"FAKE_LOG"};
    ScopedEnv save_fail{"FAKE_FAIL", std::nullopt};
    fs::path root = fs::canonical(fs::temp_directory_path()) / "malgrind_setup";
    fs::path home = root / "home", bin = root / "bin", python = bin / os::exe_name("python3");

    Fake() {
        fs::remove_all(root);
        fs::create_directories(bin);
        fs::create_directories(home);
        // "-m venv DIR" creates venv_bin/python; "-m pip install ..." creates venv_bin/pio next to it.
        fs::copy_file(MALGRIND_FAKE_TOOL, python, fs::copy_options::overwrite_existing);
        os::set_env("PATH", os::to_utf8(bin));
        os::set_env("FAKE_LOG", os::to_utf8(root / "log"));
    }
    ~Fake() { fs::remove_all(root); }
};

void venv_bin_for_checks() {
    assert(os::venv_bin_for(os::Kind::Linux, "/a/penv") == fs::path("/a/penv/bin"));
    assert(os::venv_bin_for(os::Kind::MacOS, "/a/penv") == fs::path("/a/penv/bin"));
    assert(os::venv_bin_for(os::Kind::Windows, "C:/a/penv") == fs::path("C:/a/penv/Scripts"));
    assert(os::venv_bin("/a/penv") == os::venv_bin_for(os::kind(), "/a/penv"));
}

void install_pio_steps_for_both_layouts() {
    const auto root = fresh_dir("malgrind_setup_layouts");
    const auto posix_env = root / "posix_penv";
    const auto win_env = root / "win_penv";

    // POSIX layout: fresh -> 2 steps, half-made (no pip) -> 2 steps, complete -> 1 step
    auto steps = install_pio_steps_for(os::Kind::Linux, {"/usr/bin/python3"}, posix_env);
    assert(steps.size() == 2);
    assert((steps[0].argv == std::vector<std::string>{"/usr/bin/python3", "-m", "venv", os::to_utf8(posix_env)}));
    assert((steps[1].argv == std::vector<std::string>{os::to_utf8(posix_env / "bin/python"), "-m", "pip", "install",
                                                     "--disable-pip-version-check", "platformio"}));
    fs::create_directories(posix_env / "bin");
    std::ofstream(posix_env / "bin/python") << "";
    assert(install_pio_steps_for(os::Kind::Linux, {"/usr/bin/python3"}, posix_env).size() == 2);
    std::ofstream(posix_env / "bin/pip") << "";
    steps = install_pio_steps_for(os::Kind::Linux, {"/usr/bin/python3"}, posix_env);
    assert(steps.size() == 1);
    assert(steps[0].argv[0] == os::to_utf8(posix_env / "bin/python"));

    // Windows layout: Scripts/python.exe + Scripts/pip.exe and multi-arg `py -3` prefix
    steps = install_pio_steps_for(os::Kind::Windows, {"C:\\Windows\\py.exe", "-3"}, win_env);
    assert(steps.size() == 2);
    assert((steps[0].argv == std::vector<std::string>{"C:\\Windows\\py.exe", "-3", "-m", "venv", os::to_utf8(win_env)}));
    assert((steps[1].argv == std::vector<std::string>{os::to_utf8(win_env / "Scripts/python.exe"), "-m", "pip", "install",
                                                     "--disable-pip-version-check", "platformio"}));
    fs::create_directories(win_env / "Scripts");
    std::ofstream(win_env / "Scripts/python.exe") << "";
    assert(install_pio_steps_for(os::Kind::Windows, {"C:\\Windows\\py.exe", "-3"}, win_env).size() == 2);
    std::ofstream(win_env / "Scripts/pip.exe") << "";
    steps = install_pio_steps_for(os::Kind::Windows, {"C:\\Windows\\py.exe", "-3"}, win_env);
    assert(steps.size() == 1);
    assert(steps[0].argv[0] == os::to_utf8(win_env / "Scripts/python.exe"));

    fs::remove_all(root);
}

void platformio_core_dirs_checks() {
    ScopedEnv save_core("PLATFORMIO_CORE_DIR", std::nullopt);
    ScopedEnv save_drive("SystemDrive", std::nullopt);
    const auto root = fresh_dir("malgrind_setup_cores");
    const auto home = root / "home";
    const auto custom = root / "custom_core";
    const auto missing = root / "missing_core";
    fs::create_directories(home);

    assert(os::platformio_core_dirs(home).empty());

    fs::create_directories(home / ".platformio");
    assert((os::platformio_core_dirs(home) == std::vector<fs::path>{home / ".platformio"}));

    // Non-existent $PLATFORMIO_CORE_DIR is ignored
    os::set_env("PLATFORMIO_CORE_DIR", os::to_utf8(missing));
    assert((os::platformio_core_dirs(home) == std::vector<fs::path>{home / ".platformio"}));

    // Existing $PLATFORMIO_CORE_DIR comes before home/.platformio
    fs::create_directories(custom);
    os::set_env("PLATFORMIO_CORE_DIR", os::to_utf8(custom));
    assert((os::platformio_core_dirs(home) == std::vector<fs::path>{custom, home / ".platformio"}));
    // And install_pio_steps targets the first core dir when one exists
    const auto steps = install_pio_steps({"/usr/bin/python3"}, home);
    assert(steps.size() == 2 && steps[0].argv[3] == os::to_utf8(custom / "penv"));

    // Deduplication when $PLATFORMIO_CORE_DIR points to home/.platformio
    os::set_env("PLATFORMIO_CORE_DIR", os::to_utf8(home / "." / ".platformio"));
    assert(os::platformio_core_dirs(home).size() == 1);

    // find_addr2line searches PATH first (Host) and <core>/packages/toolchain-*/bin across platformio_core_dirs via families
    {
        const auto bin_dir = root / "bin";
        fs::create_directories(bin_dir);
        const auto host_tool = bin_dir / os::exe_name("addr2line");
        std::ofstream(host_tool) << "";
        const auto arm_tool = bin_dir / os::exe_name("arm-none-eabi-addr2line");
        std::ofstream(arm_tool) << "";
        const auto generic_riscv = bin_dir / os::exe_name("riscv-none-elf-addr2line");
        std::ofstream(generic_riscv) << "";
        ScopedEnv save_path("PATH", os::to_utf8(bin_dir));
        os::set_env("PLATFORMIO_CORE_DIR", os::to_utf8(custom));
        const auto tool_dir = custom / "packages/toolchain-xtensa-esp32/bin";
        fs::create_directories(tool_dir);
        const auto tool = tool_dir / os::exe_name("xtensa-esp32-elf-addr2line");
        std::ofstream(tool) << "";
        const auto riscv_dir = custom / "packages/toolchain-riscv32-esp/bin";
        fs::create_directories(riscv_dir);
        const auto riscv_tool = riscv_dir / os::exe_name("riscv32-esp-elf-addr2line");
        std::ofstream(riscv_tool) << "";

        assert(find_addr2line(malgrind::Arch::Host) == host_tool);
        assert(find_addr2line(malgrind::Arch::Xtensa) == tool);
        assert(find_addr2line(malgrind::Arch::RiscV32) == riscv_tool);
        // No registered family handles ArmV7M yet, so find_addr2line returns empty instead of hardcoding a tool name.
        assert(find_addr2line(malgrind::Arch::ArmV7M).empty());
    }

    fs::remove_all(root);
}

void pio_python_checks() {
    ScopedEnv save_core("PLATFORMIO_CORE_DIR", std::nullopt);
    ScopedEnv save_drive("SystemDrive", std::nullopt);
    const auto root = fresh_dir("malgrind_setup_piopy");
    const auto home = root / "home";
    const auto custom = root / "custom_core";
    const auto pio = root / "pio";
    fs::create_directories(home);
    std::ofstream(pio) << "#!/usr/bin/env python3\n";

    // Without penv python, POSIX falls back to parsing the shebang of `pio`
    if constexpr (os::kind() != os::Kind::Windows) {
        assert((os::pio_python(home, pio) == std::vector<std::string>{"/usr/bin/env", "python3"}));
        std::ofstream(root / "notscript") << "binary";
        assert(!os::pio_python(home, root / "notscript").has_value());
        std::ofstream(root / "empty_shebang") << "#!   \n";
        assert(!os::pio_python(home, root / "empty_shebang").has_value());
    }

    // With <core>/penv/<bin>/python[.exe], penv takes precedence over the shebang
    const auto home_py = os::venv_bin(home / ".platformio/penv") / os::exe_name("python");
    fs::create_directories(home_py.parent_path());
    std::ofstream(home_py) << "";
    assert((os::pio_python(home, pio) == std::vector<std::string>{os::to_utf8(home_py)}));

    // $PLATFORMIO_CORE_DIR takes precedence over home/.platformio
    const auto custom_py = os::venv_bin(custom / "penv") / os::exe_name("python");
    fs::create_directories(custom_py.parent_path());
    std::ofstream(custom_py) << "";
    os::set_env("PLATFORMIO_CORE_DIR", os::to_utf8(custom));
    assert((os::pio_python(home, pio) == std::vector<std::string>{os::to_utf8(custom_py)}));

    fs::remove_all(root);
}

void python_hint_checks() {
    const auto hint = os::python_hint();
    assert(!hint.empty());
    if constexpr (os::kind() == os::Kind::Linux) assert(contains(hint, "python3-venv"));
    else if constexpr (os::kind() == os::Kind::MacOS) assert(contains(hint, "Homebrew"));
    else assert(contains(hint, "python.org"));
}

void installs_into_the_place_find_pio_looks() {
    Fake fake;
    const auto py = os::find_python();
    assert(py && (*py == std::vector<std::string>{os::to_utf8(fake.python)}));
    assert(!find_pio(fake.home)); // nothing installed yet
    const auto venv_dir = fake.home / ".platformio/penv";
    const auto venv_bin = os::venv_bin(venv_dir);
    const auto steps = install_pio_steps(*py, fake.home);
    assert(steps.size() == 2 && steps[0].argv[2] == "venv" && steps[0].argv[3] == os::to_utf8(venv_dir));
    assert(steps[1].argv.back() == "platformio" && steps[1].argv[0] == os::to_utf8(venv_bin / os::exe_name("python")));
    std::string lines;
    install_pio(*py, fake.home, [&](std::string_view l) { lines.append(l); });
    const auto expected_pio = venv_bin / os::exe_name("pio");
    assert(fs::exists(expected_pio));
    assert(find_pio(fake.home) == expected_pio); // exactly where find_pio looks
    // a second run reuses the environment: only pip runs
    assert(install_pio_steps(*py, fake.home).size() == 1);
}

void a_missing_venv_module_says_what_to_install() {
    Fake fake;
    os::set_env("FAKE_FAIL", "venv");
    std::string lines;
    bool threw = false;
    try {
        install_pio({os::to_utf8(fake.python)}, fake.home, [&](std::string_view l) { lines.append(l); });
    } catch (const std::exception& e) {
        threw = contains(e.what(), os::python_hint());
    }
    assert(threw && contains(lines, "ensurepip")); // the raw output was shown too
    // the half-made environment is gone, so after installing venv support a second click installs (a stale one made pip fail forever)
    assert(!fs::exists(fake.home / ".platformio/penv"));

    // Multi-element python_cmd (e.g. Windows `py -3`) also detects venv failure cleanly
    const auto py_launcher = fake.bin / os::exe_name("py");
    fs::copy_file(MALGRIND_FAKE_TOOL, py_launcher, fs::copy_options::overwrite_existing);
    bool multi_threw = false;
    try {
        install_pio({os::to_utf8(py_launcher), "-3"}, fake.home, [](std::string_view) {});
    } catch (const std::exception& e) {
        multi_threw = contains(e.what(), os::python_hint());
    }
    assert(multi_threw && !fs::exists(fake.home / ".platformio/penv"));

    os::unset_env("FAKE_FAIL");
    install_pio({os::to_utf8(fake.python)}, fake.home, [](std::string_view) {});
    assert(fs::exists(os::venv_bin(fake.home / ".platformio/penv") / os::exe_name("pio")));
}

void a_broken_environment_from_an_older_try_is_rebuilt() {
    Fake fake;
    const auto venv_bin = os::venv_bin(fake.home / ".platformio/penv");
    fs::create_directories(venv_bin);
    std::ofstream(venv_bin / os::exe_name("python")) << ""; // an interpreter, but no pip
    assert(install_pio_steps({os::to_utf8(fake.python)}, fake.home).size() == 2); // so the venv step runs again
    std::ofstream(venv_bin / os::exe_name("pip")) << "";
    assert(install_pio_steps({os::to_utf8(fake.python)}, fake.home).size() == 1); // a working one is reused
}

void a_network_failure_is_named() {
    Fake fake;
    os::set_env("FAKE_FAIL", "pip");
    bool threw = false;
    try {
        install_pio({os::to_utf8(fake.python)}, fake.home, [](std::string_view) {});
    } catch (const std::exception& e) {
        threw = contains(e.what(), "online");
    }
    assert(threw);
}

void success_without_pio_is_not_success() {
    Fake fake;
    os::set_env("FAKE_FAIL", "nopio");
    bool threw = false;
    try {
        install_pio({os::to_utf8(fake.python)}, fake.home, [](std::string_view) {});
    } catch (const std::exception& e) {
        threw = contains(e.what(), "no pio");
    }
    assert(threw);
}

void no_python_means_no_python() {
    Fake fake;
    os::set_env("PATH", os::to_utf8(fake.root / "nonexistent"));
    assert(!os::find_python());

    // Falls back to `python` when `python3` is missing
    fs::remove(fake.python);
    const auto py2 = fake.bin / os::exe_name("python");
    fs::copy_file(MALGRIND_FAKE_TOOL, py2, fs::copy_options::overwrite_existing);
    os::set_env("PATH", os::to_utf8(fake.bin));
    assert((os::find_python() == std::vector<std::string>{os::to_utf8(py2)}));
}

} // namespace

int main() {
    venv_bin_for_checks();
    install_pio_steps_for_both_layouts();
    platformio_core_dirs_checks();
    pio_python_checks();
    python_hint_checks();
    installs_into_the_place_find_pio_looks();
    a_missing_venv_module_says_what_to_install();
    a_broken_environment_from_an_older_try_is_rebuilt();
    a_network_failure_is_named();
    success_without_pio_is_not_success();
    no_python_means_no_python();
    std::puts("setup ok");
}
