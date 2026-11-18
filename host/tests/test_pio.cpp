// PlatformIO integration self-check: project parsing, the sdkconfig merge, the build invocation and process output.
#include "env.hpp"
#include "families/esp32.hpp"
#include "ops.hpp"
#include "os/paths.hpp"
#include "os/process.hpp"
#include "os/tools.hpp"
#include "pio.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>

using namespace malgrind::host;
namespace fs = std::filesystem;

namespace {

bool contains(const std::string& s, std::string_view part) { return s.find(part) != std::string::npos; }
bool any_contains(const std::vector<std::string>& v, std::string_view part) {
    return std::ranges::any_of(v, [&](const std::string& s) { return contains(s, part); });
}
fs::path fresh(const char* name) {
    // canonical: on macOS /tmp and /var are symlinks, and the current directory is reported without them
    const auto p = fs::canonical(fs::temp_directory_path()) / os::from_utf8(name);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}
void write(const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file) << text;
}
std::string slurp(const fs::path& file) {
    std::ifstream in(file);
    return {std::istreambuf_iterator<char>(in), {}};
}

void read_envs() {
    const auto dir = fresh("malgrind_pio_a");
    write(dir / "platformio.ini", "[platformio]\ndefault_envs = b, a\n[env:a]\nboard = x\n[env:b]\n[env]\nfoo = 1\n");
    std::string why;
    const auto p = read_project(dir, why);
    assert(p && p->dir == dir && p->envs == (std::vector<std::string>{"a", "b"}) && p->default_env == "b"); // bare [env] is no env

    write(dir / "platformio.ini", "[env:only]\n");
    assert(read_project(dir, why)->default_env == "only");

    assert(!read_project(dir / "missing", why) && contains(why, "platformio.ini"));
    write(dir / "empty/platformio.ini", "[platformio]\n");
    assert(!read_project(dir / "empty", why) && contains(why, "[env:"));

    // PlatformIO project reading itself allows spaces; chip families (such as ESP-IDF) reject them if required
    write(dir / "my proj/platformio.ini", "[env:a]\n");
    const auto with_space = read_project(dir / "my proj", why);
    assert(with_space && with_space->dir == dir / "my proj");
    fs::remove_all(dir);
}

// pio runs with `-d <project>` and so changes directory: no path it is given may depend on where we run.
// The agent is an ESP-IDF component: other frameworks and chips build and flash fine, and then show nothing.
void unsupported_projects_are_refused() {
    const auto dir = fresh("malgrind_pio_fw");
    ScopedEnv save_path("PATH", os::to_utf8(dir / "bin"));
    ScopedEnv save_core("PLATFORMIO_CORE_DIR", std::nullopt);
    ScopedEnv save_drive("SystemDrive", std::nullopt);
    fs::create_directories(dir / "bin");
    fs::copy_file(MALGRIND_FAKE_TOOL, dir / "bin" / os::exe_name("pio"), fs::copy_options::overwrite_existing);
    const auto prepare = [&](const char* ini, const char* env, const fs::path& proj_dir = {}) {
        const fs::path target = proj_dir.empty() ? dir : proj_dir;
        write(target / "platformio.ini", ini);
        try {
            (void)prepare_project(dir, target, env, "/dev/ttyUSB0");
            return std::string("ok");
        } catch (const std::exception& e) {
            return std::string(e.what());
        }
    };
    assert(prepare("[env:a]\nplatform = espressif32\nframework = espidf\n", "a") == "ok");
    assert(prepare("[env:a]\nplatform = espressif32@6.9.0\nframework = arduino, espidf\n", "a") == "ok"); // IDF with Arduino as a component
    assert(prepare("[env:a]\nboard = esp32dev\n", "a") == "ok"); // nothing said (extends, a shared section...): let the build decide
    assert(prepare("[env]\nframework = espidf\n[env:a]\nplatform = espressif32\n", "a") == "ok"); // the shared [env] section counts
    assert(contains(prepare("[env:a]\nplatform = espressif32\nframework = arduino\n", "a"), "arduino"));
    assert(contains(prepare("[env:a]\nplatform = espressif32\nframework = arduino\n[env:b]\nframework = espidf\n", "a"), "espidf")); // per env
    assert(contains(prepare("[env:a]\nplatform = ststm32\nframework = stm32cube\n", "a"), "ESP32"));
    assert(contains(prepare("[env:a]\nplatform = espressif32\n", "missing"), "no environment"));
    assert(contains(prepare("[env:a]\nplatform = espressif32\nframework = espidf\n", "a", dir / "my proj"), "space"));
    fs::remove_all(dir);
}

void relative_paths_become_absolute() {
    const auto dir = fresh("malgrind_pio_rel");
    write(dir / "proj/platformio.ini", "[env:esp32dev]\n");
    write(dir / "agent-copy/agent/esp32/inject.py", "");
    write(dir / "agent-copy/agent/esp32/sdkconfig.agent", "CONFIG_X=1\n");
    const auto saved = fs::current_path();
    fs::current_path(dir);

    std::string why;
    const auto p = read_project("proj", why);
    assert(p && p->dir == dir / "proj");

    const auto inv = agent_build("/opt/pio", *p, "esp32dev", "/dev/ttyUSB0", "agent-copy"); // a relative --agent-dir too
    for (const auto& entry : inv.env) {
        const auto value = entry.substr(entry.find('=') + 1);
        const auto path_str = value.starts_with("pre:") ? value.substr(4) : value;
        assert(os::from_utf8(path_str).is_absolute()); // every path in the environment is absolute
    }
    assert(os::from_utf8(inv.argv[3]).is_absolute());

    fs::current_path(dir / "proj");
    const auto here = read_project(".", why);
    assert(here && here->dir == dir / "proj"); // neither "/." nor a trailing "/"
    fs::current_path(saved);
    fs::remove_all(dir);
}

void merge_keeps_user_keys() {
    const auto agent = slurp(fs::path(MALGRIND_SOURCE_DIR) / "agent/esp32/sdkconfig.agent");
    assert(contains(agent, "CONFIG_ESP_CONSOLE_UART_BAUDRATE=921600"));
    const auto merged = merge_sdkconfig("CONFIG_A=1\n# CONFIG_GPTIMER_ISR_IRAM_SAFE is not set\nCONFIG_ESP_CONSOLE_UART_BAUDRATE=115200\n"
                                        "CONFIG_ESP_CONSOLE_UART_DEFAULT=y\n# CONFIG_B is not set",
                                        agent);
    assert(contains(merged, "CONFIG_A=1\n") && contains(merged, "# CONFIG_B is not set\n")); // user keys survive
    assert(contains(merged, "CONFIG_GPTIMER_ISR_IRAM_SAFE=y\n") && contains(merged, "CONFIG_ESP_CONSOLE_UART_BAUDRATE=921600\n"));
    assert(contains(merged, "# CONFIG_ESP_CONSOLE_UART_DEFAULT is not set\n") && contains(merged, "CONFIG_ESP_CONSOLE_UART_CUSTOM=y\n"));
    assert(!contains(merged, "115200") && !contains(merged, "IRAM_SAFE is not set") && !contains(merged, "UART_DEFAULT=y"));
    assert(!contains(merged, "DEFAULT and CUSTOM are one")); // the agent file's comments are not copied
    assert(merge_sdkconfig("", agent).ends_with("\n") && merge_sdkconfig("X", "CONFIG_Y=1\n") == "X\nCONFIG_Y=1\n");
}

void agent_build_leaves_the_project_alone() {
    const auto dir = fresh("malgrind_pio_\xc3\xa4/proj"); // non-ASCII, still one argv element and never a shell string
    write(dir / "sdkconfig.esp32dev", "CONFIG_A=1\nCONFIG_ESP_CONSOLE_UART_BAUDRATE=115200\n");
    write(dir / "platformio.ini", "[env:esp32dev]\n");
    const auto before = slurp(dir / "sdkconfig.esp32dev");
    const ProjectInfo project{dir, {"esp32dev"}, "esp32dev"};
    const fs::path agent = fs::absolute(MALGRIND_SOURCE_DIR).lexically_normal();
    const auto inv = agent_build("/opt/pio", project, "esp32dev", "/dev/ttyUSB0", agent);

    assert(inv.argv == (std::vector<std::string>{"/opt/pio", "run", "-d", os::to_utf8(dir), "-e", "esp32dev", "-t", "upload",
                                                 "--upload-port", "/dev/ttyUSB0"}));
    assert(std::ranges::find(inv.env, "PLATFORMIO_BUILD_DIR=" + os::to_utf8(dir / ".pio/malgrind/build")) != inv.env.end());
    assert(std::ranges::find(inv.env, "PLATFORMIO_EXTRA_SCRIPTS=pre:" + os::to_utf8(agent / "agent/esp32/inject.py")) != inv.env.end());
    assert(std::ranges::find(inv.env, "MALGRIND_AGENT_DIR=" + os::to_utf8(agent / "agent/esp32")) != inv.env.end());
    assert(std::ranges::find(inv.env, "MALGRIND_SDKCONFIG=" + os::to_utf8(dir / ".pio/malgrind/sdkconfig.esp32dev")) != inv.env.end());

    const auto merged = slurp(dir / ".pio/malgrind/sdkconfig.esp32dev");
    assert(contains(merged, "CONFIG_A=1") && contains(merged, "BAUDRATE=921600") && !contains(merged, "115200"));
    assert(slurp(dir / "sdkconfig.esp32dev") == before); // the user's own file is untouched
    assert(fs::exists(dir / ".pio/malgrind/sdkconfig.esp32dev") && !fs::exists(dir / ".pio/malgrind/build")); // the seed is not in the dir pio wipes
    assert(agent_elf(project, "esp32dev") == dir / ".pio/malgrind/build/esp32dev/firmware.elf");
    fs::remove_all(dir.parent_path());
}

void build_without_upload_and_stable_sdkconfig() {
    const auto dir = fresh("malgrind_pio_noup");
    write(dir / "platformio.ini", "[env:esp32dev]\n");
    const ProjectInfo project{dir, {"esp32dev"}, "esp32dev"};
    const auto build = agent_build("/opt/pio", project, "esp32dev", "/dev/ttyUSB0", MALGRIND_SOURCE_DIR, false);
    assert(!any_contains(build.argv, "upload") && !any_contains(build.argv, "ttyUSB0") && any_contains(build.argv, "esp32dev"));
    const auto sdkconfig = dir / ".pio/malgrind/sdkconfig.esp32dev";
    const auto stamp = fs::last_write_time(sdkconfig);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto upload = agent_build("/opt/pio", project, "esp32dev", "/dev/ttyUSB0", MALGRIND_SOURCE_DIR);
    assert(any_contains(upload.argv, "upload") && build.env == upload.env);
    assert(fs::last_write_time(sdkconfig) == stamp); // same content: not rewritten, so IDF does not rebuild everything
    fs::remove_all(dir);
}

void plain_flash_has_no_agent() {
    const ProjectInfo project{"/tmp/proj", {"esp32dev"}, "esp32dev"};
    const auto inv = plain_flash("/opt/pio", project, "esp32dev", "/dev/ttyUSB0");
    assert(inv.argv.size() == 10 && inv.argv[0] == "/opt/pio" && inv.argv[8] == "--upload-port");
    assert(inv.env.empty() && !any_contains(inv.argv, "malgrind"));
}

void find_pio_fallback() {
    ScopedEnv save_path("PATH", std::nullopt);
    ScopedEnv save_core("PLATFORMIO_CORE_DIR", std::nullopt);
    ScopedEnv save_drive("SystemDrive", std::nullopt);
    const auto home = fresh("malgrind_pio_home");
    assert(!find_pio(home));
    const auto pio = os::venv_bin(home / ".platformio/penv") / os::exe_name("pio");
    write(pio, "");
    assert(find_pio(home) == pio);
    fs::remove_all(home);
}

void run_lines_streams() {
    std::vector<std::string> lines;
    const int code = os::run_lines(
        {MALGRIND_FAKE_TOOL, "a"},
        {"FAKE_TOOL_ROLE=echo", "FAKE_OUT=1", "FAKE_ERR=b", "FAKE_RAW=p1\rp2\n", "FAKE_EXIT=3"},
        [&](std::string_view line) { lines.emplace_back(line); });
    assert(code == 3 && lines == (std::vector<std::string>{"1", "a", "b", "p1", "p2"})); // \r splits progress lines too
    assert(os::run_lines({"/no/such/program"}, {}, [](std::string_view) {}) != 0);
}

void esptool_command_line() {
    ScopedEnv save_core("PLATFORMIO_CORE_DIR", std::nullopt);
    ScopedEnv save_drive("SystemDrive", std::nullopt);
    const auto home = fresh("malgrind_pio_esptool");
    write(home / "pio", "#!/usr/bin/env python3\n");
    assert(!esptool(home / "pio", home, "/dev/ttyUSB0", {"chip_id"})); // no esptool package yet
    write(home / ".platformio/packages/tool-esptoolpy/esptool.py", "");
    const auto script = os::to_utf8(home / ".platformio/packages/tool-esptoolpy/esptool.py");
    if constexpr (os::kind() != os::Kind::Windows) {
        const auto inv = esptool(home / "pio", home, "/dev/ttyUSB0", {"read_flash", "0", "4096", "out.bin"});
        assert(inv && inv->env.empty());
        // the interpreter is the two words of the shebang line, then the script, the connection, the command
        assert((inv->argv == std::vector<std::string>{"/usr/bin/env", "python3", script, "--port", "/dev/ttyUSB0", "--baud", "921600",
                                                      "read_flash", "0", "4096", "out.bin"}));
        write(home / "notscript", "binary");
        assert(!esptool(home / "notscript", home, "p", {}));
    }
    const auto penv_py = os::venv_bin(home / ".platformio/penv") / os::exe_name("python");
    write(penv_py, "");
    const auto inv = esptool(home / "pio", home, "/dev/ttyUSB0", {"read_flash", "0", "4096", "out.bin"});
    assert(inv && inv->env.empty());
    assert((inv->argv == std::vector<std::string>{os::to_utf8(penv_py), script, "--port", "/dev/ttyUSB0", "--baud", "921600",
                                                  "read_flash", "0", "4096", "out.bin"}));
    fs::remove_all(home);
}

} // namespace

int main() {
    read_envs();
    unsupported_projects_are_refused();
    relative_paths_become_absolute();
    merge_keeps_user_keys();
    agent_build_leaves_the_project_alone();
    build_without_upload_and_stable_sdkconfig();
    plain_flash_has_no_agent();
    find_pio_fallback();
    run_lines_streams();
    esptool_command_line();
    std::puts("pio ok");
}
