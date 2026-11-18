// Safe flash self-check: the backup taken before the agent goes on, the restore, and the list of methods.
// pio and esptool are shell scripts that log their command line, so no board is needed.
#include "backup.hpp"
#include "families/esp32.hpp"
#include "families/family.hpp"
#include "ops.hpp"
#include "plan.hpp"
#include "pty.hpp"
#include "stream.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

using namespace malgrind::host;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

constexpr const char* kMac = "24-0a-c4-aa-bb-0c";

bool contains(const std::string& s, std::string_view part) { return s.find(part) != std::string::npos; }
std::string slurp(const fs::path& file) {
    std::ifstream in(file);
    return {std::istreambuf_iterator<char>(in), {}};
}
void write(const fs::path& file, std::string_view text, bool executable = false) {
    fs::create_directories(file.parent_path());
    std::ofstream(file) << text;
    if (executable) fs::permissions(file, fs::perms::owner_exec, fs::perm_options::add);
}

struct Lines {
    std::string text;
    LineFn fn() { return [this](std::string_view l) { text.append(l).append("\n"); }; }
};

// A home with a fake pio and a fake esptool.py (a shell script: pio's shebang names /bin/sh, so it runs as one).
struct Fake {
    fs::path root = fs::canonical(fs::temp_directory_path()) / "malgrind_flash";
    fs::path home = root / "home", backups = root / "backups", project = root / "proj", log = root / "log";

    // `esptool_installed` false: like a fresh machine, where PlatformIO downloads esptool only when it first builds
    explicit Fake(bool esptool_installed = true) {
        fs::remove_all(root);
        write(home / ".platformio/penv/bin/pio", R"(#!/bin/sh
echo "pio $* build=$PLATFORMIO_BUILD_DIR rate=$MALGRIND_RATE_HZ flags=$PLATFORMIO_BUILD_FLAGS" >> "$FAKE_LOG"
mkdir -p "$FAKE_ESPTOOL_DIR" && cp "$FAKE_ESPTOOL_SRC" "$FAKE_ESPTOOL_DIR/esptool.py"
)", true);
        write(root / "esptool.src", R"(echo "esptool $*" >> "$FAKE_LOG"
shift 4   # --port P --baud B
echo "Chip is ESP32-D0WD-V3"
echo "MAC: 24:0a:c4:AA:bb:0c"
case "$1" in
flash_id) echo "Detected flash size: 4MB" ;;
read_flash)
  [ "$FAKE_FAIL" = read_flash ] && { echo "A fatal error occurred: timeout" >&2; exit 2; }
  head -c 4194304 /dev/zero > "$4"
  [ -n "$FAKE_AGENT_IMAGE" ] && printf 'malgrind-agent-v1' | dd of="$4" bs=1 seek=1000 conv=notrunc 2>/dev/null ;;
esac
exit 0
)");
        setenv("FAKE_ESPTOOL_SRC", (root / "esptool.src").c_str(), 1);
        setenv("FAKE_ESPTOOL_DIR", (home / ".platformio/packages/tool-esptoolpy").c_str(), 1);
        if (esptool_installed) {
            fs::create_directories(home / ".platformio/packages/tool-esptoolpy");
            fs::copy_file(root / "esptool.src", home / ".platformio/packages/tool-esptoolpy/esptool.py");
        }
        write(project / "platformio.ini", "[env:esp32dev]\nplatform = espressif32\nframework = espidf\n");
        setenv("FAKE_LOG", log.c_str(), 1);
        setenv("PATH", "/usr/bin:/bin", 1); // not the real pio
        unsetenv("FAKE_FAIL");
        unsetenv("FAKE_AGENT_IMAGE");
    }
    ~Fake() { fs::remove_all(root); }
    [[nodiscard]] std::string calls() const { return slurp(log); }
    [[nodiscard]] Prepared prepared(const std::string& port) const { return prepare_project(home, project, "", port); }
    // what the engine does for a run: the plan for these settings, its files, then the flash
    void flash(const std::string& port, Lines& lines, const RunSettings& settings = {}) const {
        const auto p = prepared(port);
        const auto plan = make_plan(p, MALGRIND_SOURCE_DIR, settings);
        write_plan_files(p, MALGRIND_SOURCE_DIR, plan);
        flash_agent(p, home, backups, plan.build, plan.upload, lines.fn());
    }
    // backups of the board, finished ones only
    [[nodiscard]] std::size_t backup_count() const {
        std::size_t n = 0;
        for (const auto& e : fs::directory_iterator(backups / kMac, std::filesystem::directory_options::none))
            n += e.path().extension() == ".bin";
        return n;
    }
};

void backs_up_then_flashes() {
    Fake fake;
    Pty board; // a board that is not running the agent: silent
    Lines lines;
    fake.flash(board.slave, lines);
    const auto calls = fake.calls();
    assert(contains(calls, "flash_id") && contains(calls, "read_flash 0 4194304"));
    // build, then backup, then upload: a compile error costs the board nothing
    const auto build = calls.find("pio run"), backup = calls.find("read_flash"), upload = calls.find("-t upload");
    assert(build < backup && backup < upload && calls.substr(build, backup - build).find("-t upload") == std::string::npos);
    assert(contains(calls, "build=" + (fake.project / ".pio/malgrind/build").string())); // the agent build, not the user's
    assert(fake.backup_count() == 1 && contains(lines.text, "backup saved"));
    for (const auto& e : fs::directory_iterator(fake.backups / kMac)) assert(e.path().extension() == ".bin" && fs::file_size(e.path()) == 4194304);
}

void no_backup_no_flash() {
    Fake fake;
    Pty board;
    setenv("FAKE_FAIL", "read_flash", 1);
    Lines lines;
    bool threw = false;
    try {
        fake.flash(board.slave, lines);
    } catch (const std::exception& e) {
        threw = contains(e.what(), "esptool failed") && contains(e.what(), "timeout");
    }
    assert(threw);
    assert(contains(fake.calls(), "pio run") && !contains(fake.calls(), "-t upload")); // built, but nothing was flashed
    assert(fake.backup_count() == 0 && fs::is_empty(fake.backups / kMac)); // and no half-written file stays
}

void board_with_agent_is_not_backed_up_again() {
    Fake fake;
    Pty board;
    std::thread agent([&] {
        std::this_thread::sleep_for(300ms);
        board.send(make_stream(1));
    });
    Lines lines;
    fake.flash(board.slave, lines);
    agent.join();
    assert(!contains(fake.calls(), "esptool") && contains(fake.calls(), "-t upload") && !fs::exists(fake.backups));
    assert(contains(lines.text, "already runs malgrind"));
}

void our_own_image_is_not_kept() {
    Fake fake; // frames not arriving (the agent is installed but silent, or the port is wrong) but the flash holds our build
    Pty board;
    setenv("FAKE_AGENT_IMAGE", "1", 1);
    Lines lines;
    fake.flash(board.slave, lines);
    assert(fake.backup_count() == 0 && contains(fake.calls(), "-t upload") && contains(lines.text, "no backup kept"));
}

void fresh_machine_has_no_esptool_until_the_first_build() {
    Fake fake(false);
    Pty board;
    Lines lines;
    assert(!fs::exists(fake.home / ".platformio/packages/tool-esptoolpy"));
    fake.flash(board.slave, lines);
    assert(fake.backup_count() == 1 && contains(fake.calls(), "-t upload")); // the build brought esptool, the backup could run
}

void a_compile_error_stops_before_the_board_is_touched() {
    Fake fake;
    Pty board;
    write(fake.home / ".platformio/penv/bin/pio", "#!/bin/sh\necho \"pio $*\" >> \"$FAKE_LOG\"\necho 'error: boom' >&2\nexit 1\n", true);
    Lines lines;
    bool threw = false;
    try {
        fake.flash(board.slave, lines);
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw && !contains(fake.calls(), "esptool") && !fs::exists(fake.backups) && contains(lines.text, "boom"));
}

void the_settings_shape_the_commands_around_the_backup() {
    Fake fake;
    Pty board;
    Lines lines;
    RunSettings s;
    s.extra_args = "--verbose";
    s.rate_hz = 500;
    s.build_flags = "-DX=1";
    s.upload_command = (fake.home / ".platformio/penv/bin/pio").string() + " run -t upload --my-upload";
    fake.flash(board.slave, lines, s);
    const auto calls = fake.calls();
    const auto build = calls.find("--verbose"), backup = calls.find("read_flash"), upload = calls.find("--my-upload");
    // the build line carries the extra argument, the environment carries the rate and flags, then the backup, then the typed upload line
    assert(build != std::string::npos && build < backup && backup < upload);
    assert(contains(calls.substr(0, backup), "flags=-DX=1"));
    assert(contains(calls.substr(upload), "flags=-DX=1")); // an override keeps the environment of the run
    assert(contains(slurp(fake.project / ".pio/malgrind/sdkconfig.esp32dev"), "CONFIG_MALGRIND_RATE_HZ=500")); // the rate is in the build's sdkconfig
    assert(!contains(calls, "--upload-port")); // the detected upload line was replaced, not extended
    assert(fake.backup_count() == 1);
}

void restore_writes_the_backup_back() {
    Fake fake;
    Pty board;
    Lines lines;
    // no backup yet: refused, and the message names the board
    bool threw = false;
    try {
        restore_flash(*find_pio(fake.home), fake.home, board.slave, fake.backups, lines.fn());
    } catch (const std::exception& e) {
        threw = contains(e.what(), "no backup") && contains(e.what(), kMac) && contains(e.what(), "Rebuild my project");
    }
    assert(threw && !contains(fake.calls(), "write_flash"));

    fake.flash(board.slave, lines);
    const auto saved = *latest_backup(fake.backups, kMac);
    restore_flash(*find_pio(fake.home), fake.home, board.slave, fake.backups, lines.fn());
    const auto calls = fake.calls();
    // the whole image goes back to address 0, header bytes untouched
    assert(contains(calls, "write_flash --flash_mode keep --flash_freq keep --flash_size keep 0 " + saved.string()));
}

class ProbeOnlyFamily final : public Family {
public:
    [[nodiscard]] std::string_view id() const override { return "probe-only"; }
    [[nodiscard]] std::string_view name() const override { return "Probe-only MCU"; }
    [[nodiscard]] FamilyCaps caps() const override { return {.agent = false}; }
    [[nodiscard]] bool handles_arch(malgrind::Arch) const override { return false; }
    [[nodiscard]] std::string project_problem(const ProjectInfo&, const std::string&) const override { return {}; }
    [[nodiscard]] unsigned agent_baud() const override { return 0; }
    [[nodiscard]] AgentCommands agent_commands(const ToolContext&, const ProjectInfo&, const std::string&,
                                               const std::string&, const RunSettings&, bool) const override {
        return {};
    }
    [[nodiscard]] std::vector<std::string> addr2line_names(malgrind::Arch) const override { return {}; }
};

void methods_say_what_is_missing() {
    const auto find = [](const std::vector<Method>& list, std::string_view id) {
        return *std::ranges::find_if(list, [&](const Method& m) { return m.id == id; });
    };
    const Family& esp = esp32_family();
    auto m = methods({.agent = true}, &esp, true, "");
    assert(find(m, "attach").available && find(m, "attach").why.empty() && find(m, "new_run").available);
    assert(!find(m, "no_flash").available && contains(find(m, "no_flash").why, "debug probe"));

    m = methods({}, &esp, false, ""); // nothing running, no PlatformIO
    assert(!find(m, "attach").available && contains(find(m, "attach").why, "no malgrind agent"));
    assert(!find(m, "new_run").available && contains(find(m, "new_run").why, "PlatformIO"));

    m = methods({}, nullptr, true, ""); // no matching family
    assert(!find(m, "new_run").available && contains(find(m, "new_run").why, "family") && !contains(find(m, "new_run").why, "ESP32"));

    ProbeOnlyFamily no_agent;
    m = methods({}, &no_agent, true, ""); // family does not support agent builds
    assert(!find(m, "new_run").available && contains(find(m, "new_run").why, "Probe-only MCU") && !contains(find(m, "new_run").why, "ESP32"));

    m = methods({}, nullptr, true, "environment 'x' uses framework 'arduino'");
    assert(!find(m, "new_run").available && contains(find(m, "new_run").why, "arduino"));

    m = methods({.busy = true}, &esp, true, ""); // somebody else has the port
    assert(!find(m, "attach").available && contains(find(m, "attach").why, "busy"));
    assert(!find(m, "new_run").available && contains(find(m, "new_run").why, "busy"));

    m = methods({.denied = true}, &esp, true, "");
    assert(!find(m, "attach").available && contains(find(m, "attach").why, "Grant access"));
    assert(!find(m, "new_run").available && contains(find(m, "new_run").why, "Grant access"));
}

} // namespace

int main() {
    backs_up_then_flashes();
    no_backup_no_flash();
    board_with_agent_is_not_backed_up_again();
    our_own_image_is_not_kept();
    fresh_machine_has_no_esptool_until_the_first_build();
    a_compile_error_stops_before_the_board_is_touched();
    the_settings_shape_the_commands_around_the_backup();
    restore_writes_the_backup_back();
    methods_say_what_is_missing();
    std::puts("flash ok");
}
