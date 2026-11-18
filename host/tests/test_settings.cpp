// Run settings self-check: the command-line splitter, and how settings turn into the commands of a run.
#include "families/family.hpp"
#include "plan.hpp"
#include "settings.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace malgrind::host;
namespace fs = std::filesystem;

namespace {

using Words = std::vector<std::string>;

bool contains(const std::string& s, std::string_view part) { return s.find(part) != std::string::npos; }
bool has_env(const Invocation& inv, const std::string& entry) { return std::ranges::find(inv.env, entry) != inv.env.end(); }
bool has_env_starting(const Invocation& inv, std::string_view prefix) {
    return std::ranges::any_of(inv.env, [&](const std::string& e) { return e.starts_with(prefix); });
}

void split_and_join_agree() {
    for (const Words& words : {Words{"pio", "run"}, Words{"a b", "c"}, Words{"it's"}, Words{"say \"hi\""}, Words{"", "x"}, Words{"back\\slash"},
                               Words{"$(id)", "`x`", "a;b"}, Words{"ünï"}, Words{"line\nbreak"}, Words{"-DX=\"a b\""}}) {
        const auto text = join_command(words);
        assert(split_command(text) == words && !text.empty());
    }
    assert((split_command(R"(a "b c" 'd e' f\ g "" "q\"r")") == Words{"a", "b c", "d e", "f g", "", "q\"r"}));
    assert((split_command("  one   two  ") == Words{"one", "two"}) && split_command("")->empty() && split_command("   ")->empty());
    assert(!split_command("a \"b") && !split_command("a 'b") && !split_command("trailing\\"));
    assert(join_command({"pio", "run", "-d", "/p", "-e", "esp32dev"}) == "pio run -d /p -e esp32dev"); // nothing to quote, nothing added
}

// A project with two environments, and the repository as the place the agent files live.
struct World {
    fs::path root = fs::canonical(fs::temp_directory_path()) / "malgrind_settings";
    fs::path project = root / "my proj"; // a space: it must be quoted in the text, and stay one argument
    World() {
        fs::remove_all(root);
        fs::create_directories(project);
        std::ofstream(project / "platformio.ini") << "[env:a]\n[env:b]\n";
    }
    ~World() { fs::remove_all(root); }
    [[nodiscard]] Prepared prepared() const {
        return {"/opt/pio", ProjectInfo{project, {"a", "b"}, "a"}, "a", "/dev/ttyUSB0", family_by_id("esp32")};
    }
};
const fs::path kAgent = MALGRIND_SOURCE_DIR;

void defaults_are_the_detected_commands() {
    World w;
    const auto p = w.prepared();
    const auto plan = make_plan(p, kAgent, {});
    const auto cmds = p.family->agent_commands(ToolContext{{}, p.pio, kAgent}, p.project, "a", p.port, {}, false);
    assert(plan.build.argv == cmds.build.argv && plan.build.env == cmds.build.env &&
           plan.upload.argv == cmds.upload.argv && plan.upload.env == cmds.upload.env && plan.elf == cmds.elf);
    assert(!has_env_starting(plan.build, "MALGRIND_RATE_HZ") && !has_env_starting(plan.build, "PLATFORMIO_BUILD_FLAGS")); // nothing is added
    assert(contains(plan.build_text, "pio run -d '" + w.project.string() + "' -e a") && !contains(plan.build_text, "upload"));
    assert(contains(plan.upload_text, "-t upload --upload-port /dev/ttyUSB0"));
}

void settings_change_both_commands() {
    World w;
    RunSettings s;
    s.env = "b";
    s.extra_args = "--verbose 'a b'";
    s.rate_hz = 500;
    s.build_flags = "-DFOO=1 -DBAR";
    const auto plan = make_plan(w.prepared(), kAgent, s);
    for (const auto* inv : {&plan.build, &plan.upload}) {
        assert(inv->argv.size() > 3 && *(inv->argv.end() - 2) == "--verbose" && inv->argv.back() == "a b"); // appended, one word each
        assert(std::ranges::find(inv->argv, "b") != inv->argv.end() && std::ranges::find(inv->argv, "a") == inv->argv.end()); // -e b
        assert(has_env(*inv, "PLATFORMIO_BUILD_FLAGS=-DFOO=1 -DBAR") && !has_env_starting(*inv, "MALGRIND_RATE_HZ")); // the rate is not an env var
        assert(has_env_starting(*inv, "PLATFORMIO_EXTRA_SCRIPTS=") && has_env_starting(*inv, "MALGRIND_SDKCONFIG=")); // the agent injection stays
        assert(contains(inv->env.back(), "=") && contains(plan.build_text, "'a b'"));
    }
    assert(contains(plan.upload_text, "-t upload") && plan.rate_hz == 500 && plan.env == "b");
}

std::string slurp(const fs::path& file) {
    std::ifstream in(file);
    return {std::istreambuf_iterator<char>(in), {}};
}

// The rate reaches the agent as an sdkconfig key (a changed sdkconfig makes IDF rebuild the agent; a changed environment variable
// would not), and only when the user set one.
void the_rate_is_written_into_the_sdkconfig() {
    World w;
    std::ofstream(w.project / "sdkconfig.a") << "CONFIG_USER=1\n";
    auto p = w.prepared();
    write_plan_files(p, kAgent, make_plan(p, kAgent, {}));
    const auto sdkconfig = w.project / ".pio/malgrind/sdkconfig.a";
    assert(contains(slurp(sdkconfig), "CONFIG_USER=1") && !contains(slurp(sdkconfig), "CONFIG_MALGRIND_RATE_HZ")); // default: the Kconfig's 997
    write_plan_files(p, kAgent, make_plan(p, kAgent, {.rate_hz = 500}));
    assert(contains(slurp(sdkconfig), "CONFIG_MALGRIND_RATE_HZ=500\n") && contains(slurp(sdkconfig), "CONFIG_USER=1")); // and the user's keys stay
    assert(contains(slurp(sdkconfig), "CONFIG_ESP_CONSOLE_UART_BAUDRATE=921600")); // as do the agent's own
    write_plan_files(p, kAgent, make_plan(p, kAgent, {.rate_hz = 800}));
    assert(contains(slurp(sdkconfig), "CONFIG_MALGRIND_RATE_HZ=800") && !contains(slurp(sdkconfig), "RATE_HZ=500")); // a change replaces the line
    write_plan_files(p, kAgent, make_plan(p, kAgent, {}));
    assert(!contains(slurp(sdkconfig), "CONFIG_MALGRIND_RATE_HZ")); // and back to the default
    // the Kconfig option exists in the agent, with the default the app shows
    assert(contains(slurp(fs::path(MALGRIND_SOURCE_DIR) / "agent/esp32/malgrind/Kconfig.projbuild"), "default 997") && kDefaultRateHz == 997);
}

// PlatformIO configures CMake once and then keeps it: a build directory made with other agent files would silently ignore a
// Kconfig option that a newer malgrind added. So the directory is deleted when the agent's files differ from the ones it was made with.
void a_build_made_with_other_agent_files_is_started_clean() {
    World w;
    const auto agent = w.root / "agent-copy";
    fs::create_directories(agent / "agent");
    fs::copy(fs::path(MALGRIND_SOURCE_DIR) / "agent/esp32", agent / "agent/esp32", fs::copy_options::recursive);
    const auto p = w.prepared();
    const auto marker = w.project / ".pio/malgrind/build/esp32dev/old-configuration";
    fs::create_directories(marker.parent_path());
    std::ofstream(marker) << "x";

    write_plan_files(p, agent, make_plan(p, agent, {}));
    assert(!fs::exists(marker)); // made before the stamp existed (a malgrind 0.3 build): configured without today's Kconfig
    fs::create_directories(marker.parent_path()); // the clean start removed the directory; a new build makes it again
    std::ofstream(marker) << "x";
    write_plan_files(p, agent, make_plan(p, agent, {.rate_hz = 600}));
    assert(fs::exists(marker)); // the same agent files, only the rate changed: the build is kept (the sdkconfig change is enough)
    std::ofstream(agent / "agent/esp32/malgrind/Kconfig.projbuild", std::ios::app) << "\n# changed\n";
    write_plan_files(p, agent, make_plan(p, agent, {.rate_hz = 600}));
    assert(!fs::exists(marker)); // other agent files than the build was made with
}

void an_override_replaces_the_line_but_not_the_agent() {
    World w;
    RunSettings s;
    s.extra_args = "--ignored";
    s.build_command = "/bin/echo build 'with space'";
    const auto plan = make_plan(w.prepared(), kAgent, s);
    assert((plan.build.argv == Words{"/bin/echo", "build", "with space"})); // exactly what was typed; the extra arguments are not added to it
    assert(has_env_starting(plan.build, "PLATFORMIO_EXTRA_SCRIPTS=") && has_env_starting(plan.build, "MALGRIND_SDKCONFIG="));
    assert(plan.build_text == "/bin/echo build 'with space'");
    assert(std::ranges::find(plan.upload.argv, "--ignored") != plan.upload.argv.end()); // the upload line is still the detected one
    s.build_command = "sh -c 'exit 0'"; // a program found on PATH is accepted too
    assert((make_plan(w.prepared(), kAgent, s).build.argv == Words{"sh", "-c", "exit 0"}));
}

void bad_settings_name_the_field() {
    World w;
    const auto message = [&](RunSettings s) -> std::string {
        try {
            (void)make_plan(w.prepared(), kAgent, s);
        } catch (const std::exception& e) {
            return e.what();
        }
        return "";
    };
    assert(contains(message({.env = "zzz"}), "no environment 'zzz'"));
    assert(contains(message({.rate_hz = 9}), "between 10 and 5000") && contains(message({.rate_hz = 5001}), "between 10 and 5000"));
    assert(message({.rate_hz = 10}).empty() && message({.rate_hz = 5000}).empty());
    assert(contains(message({.elf = w.root / "missing.elf"}), "ELF does not exist"));
    assert(contains(message({.extra_args = "a \"b"}), "extra arguments"));
    assert(contains(message({.build_command = "no-such-program-xyz run"}), "build command") && contains(message({.build_command = "no-such-program-xyz run"}), "not found"));
    assert(contains(message({.upload_command = "/no/such/tool"}), "upload command"));
    assert(contains(message({.build_command = "echo \"open"}), "not a command line"));
}

void a_plan_writes_nothing() {
    World w;
    (void)make_plan(w.prepared(), kAgent, {.extra_args = "-v", .rate_hz = 600});
    assert(!fs::exists(w.project / ".pio") && std::distance(fs::directory_iterator(w.project), {}) == 1); // only platformio.ini
}

void settings_survive_json() {
    RunSettings s{.env = "b", .extra_args = "-v", .build_flags = "-DX", .rate_hz = 800, .elf = "/x/y.elf", .build_command = "pio run", .upload_command = ""};
    const nlohmann::json j = s;
    assert(j["rate_hz"] == 800 && j["elf"] == "/x/y.elf" && j.get<RunSettings>() == s);
    assert(nlohmann::json::parse(R"({"rate_hz": 500})").get<RunSettings>() == RunSettings{.rate_hz = 500}); // missing keys: defaults
    assert(nlohmann::json::parse(R"({"rate_hz": "fast", "env": 3})").get<RunSettings>() == RunSettings{}); // wrong types are ignored, never thrown
    assert(nlohmann::json(nullptr).get<RunSettings>() == RunSettings{});
}

} // namespace

int main() {
    split_and_join_agree();
    defaults_are_the_detected_commands();
    settings_change_both_commands();
    the_rate_is_written_into_the_sdkconfig();
    a_build_made_with_other_agent_files_is_started_clean();
    an_override_replaces_the_line_but_not_the_agent();
    bad_settings_name_the_field();
    a_plan_writes_nothing();
    settings_survive_json();
    std::puts("settings ok");
}
