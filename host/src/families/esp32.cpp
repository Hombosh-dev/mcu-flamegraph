#include "families/esp32.hpp"

#include "os/paths.hpp"
#include "os/process.hpp"
#include "os/tools.hpp"

#include <cctype>
#include <format>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace malgrind::host {

namespace {

// pio runs with `-d <project>`, which changes its directory: every path handed to it must be absolute.
fs::path absolute_path(const fs::path& path) {
    auto abs = fs::absolute(path).lexically_normal();
    if (abs.filename().empty() && abs != abs.root_path()) abs = abs.parent_path(); // "/a/b/" -> "/a/b"
    return abs;
}

std::string slurp(const fs::path& file) {
    std::ifstream in(file);
    return {std::istreambuf_iterator<char>(in), {}};
}

std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> lines;
    std::istringstream in{std::string(text)};
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    return lines;
}

// "CONFIG_X=1" and "# CONFIG_X is not set" both set CONFIG_X; anything else is no setting.
std::optional<std::string> sdkconfig_key(const std::string& line) {
    constexpr std::string_view kNotSet = " is not set";
    if (line.starts_with("CONFIG_")) return line.substr(0, line.find('='));
    if (line.starts_with("# CONFIG_") && line.ends_with(kNotSet)) return line.substr(2, line.size() - 2 - kNotSet.size());
    return std::nullopt;
}

struct AgentPaths {
    fs::path files, out, sdkconfig;
};

AgentPaths agent_paths(const ProjectInfo& project, const std::string& env, const fs::path& agent_dir) {
    AgentPaths p;
    p.files = absolute_path(agent_dir) / "agent" / esp32_family().id();
    if (!fs::exists(p.files / "sdkconfig.agent") || !fs::exists(p.files / "inject.py"))
        throw std::runtime_error("the agent files are missing from " + os::to_utf8(p.files));
    p.out = project.dir / ".pio/malgrind";
    p.sdkconfig = p.out / ("sdkconfig." + env);
    return p;
}

// The part of the agent that CMake reads once, when it configures. PlatformIO does not configure again when a component's
// CMakeLists or Kconfig changes, so a build directory made with other files would keep the old configuration (a Kconfig option
// that a newer malgrind added would simply not exist). The stamp says which files the directory was made with.
std::string agent_stamp(const fs::path& files) {
    std::uint64_t hash = 1469598103934665603ull; // FNV-1a
    for (const char* name : {"malgrind/CMakeLists.txt", "malgrind/Kconfig.projbuild", "inject.py", "sdkconfig.agent"})
        for (const unsigned char c : slurp(files / name)) hash = (hash ^ c) * 1099511628211ull;
    return std::format("{:016x}", hash);
}

bool is_hex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

// An esptool command, its output forwarded line by line and also returned as one text. Throws if it fails.
std::string run_esptool(const fs::path& pio, const fs::path& home, const std::string& port, std::vector<std::string> args,
                        const LineFn& on_line) {
    const auto inv = esptool(pio, home, port, std::move(args));
    if (!inv) throw std::runtime_error("esptool was not found: PlatformIO has not downloaded it yet (build any ESP32 project once)");
    std::string all;
    const int code = os::run_lines(inv->argv, inv->env, [&](std::string_view line) {
        all.append(line).append("\n");
        if (on_line) on_line(line);
    });
    if (code != 0) throw std::runtime_error(std::format("esptool failed (exit code {}): is the board connected and not in use?\n{}", code, all));
    return all;
}

class Esp32Family final : public Family {
public:
    [[nodiscard]] std::string_view id() const override { return "esp32"; }
    [[nodiscard]] std::string_view name() const override { return "ESP32 (ESP-IDF)"; }
    [[nodiscard]] FamilyCaps caps() const override {
        return {.agent = true, .backup = true, .identify = true, .identify_resets_board = true};
    }
    [[nodiscard]] bool handles_arch(Arch arch) const override {
        return arch == Arch::Xtensa || arch == Arch::RiscV32;
    }
    [[nodiscard]] std::string project_problem(const ProjectInfo& project, const std::string& env) const override {
        // espidf.py of PlatformIO: "Detected a whitespace character in project paths" -> stop early with a clear reason
        if (os::to_utf8(project.dir).find(' ') != std::string::npos) {
            return "PlatformIO cannot build an ESP-IDF project in a folder whose path contains a space: " + os::to_utf8(project.dir);
        }
        // The agent is an ESP-IDF component for ESP32. Anything else would build and flash, and then simply show nothing.
        if (const auto platform = project.platforms.find(env);
            platform != project.platforms.end() && !platform->second.starts_with("espressif32")) {
            return std::format("environment '{}' uses platform '{}': malgrind supports ESP32 (platform espressif32) only",
                               env, platform->second);
        }
        if (const auto framework = project.frameworks.find(env);
            framework != project.frameworks.end() && framework->second.find("espidf") == std::string::npos) {
            return std::format("environment '{}' uses framework '{}': the malgrind agent needs the ESP-IDF framework (framework = espidf)",
                               env, framework->second);
        }
        return {};
    }
    [[nodiscard]] unsigned agent_baud() const override { return kEsp32AgentBaud; }
    [[nodiscard]] AgentCommands agent_commands(const ToolContext& tools, const ProjectInfo& project,
                                               const std::string& env, const std::string& port,
                                               const RunSettings& settings, bool write_files) const override {
        if (write_files) write_agent_sdkconfig(project, env, tools.agent_root, settings.rate_hz);
        return {agent_command(tools.pio, project, env, port, tools.agent_root, false),
                agent_command(tools.pio, project, env, port, tools.agent_root, true),
                agent_elf(project, env)};
    }
    BoardIdentity identify(const ToolContext& tools, const std::string& port, const LineFn& on_line) const override {
        const auto out = run_esptool(tools.pio, tools.home, port, {"flash_id"}, on_line);
        const auto mac = parse_mac(out);
        const auto size = parse_flash_size(out);
        if (!mac || !size) {
            throw std::runtime_error("cannot tell which board this is (esptool did not report its MAC address and flash size)");
        }
        return {*mac, *size, parse_chip(out).value_or("")};
    }
    void read_flash(const ToolContext& tools, const std::string& port, const BoardIdentity& board,
                    const fs::path& out, const LineFn& on_line) const override {
        run_esptool(tools.pio, tools.home, port,
                    {"read_flash", "0", std::to_string(board.flash_bytes), os::to_utf8(out), "--no-progress"}, on_line);
    }
    void write_flash(const ToolContext& tools, const std::string& port, const fs::path& image,
                     const LineFn& on_line) const override {
        run_esptool(tools.pio, tools.home, port,
                    {"write_flash", "--flash_mode", "keep", "--flash_freq", "keep", "--flash_size", "keep", "0",
                     os::to_utf8(image)},
                    on_line);
    }
    [[nodiscard]] bool is_agent_image(const fs::path& file) const override {
        std::ifstream in(file, std::ios::binary);
        if (!in) return false;
        // search in blocks; the last bytes of a block are kept so a marker split between two blocks is still found
        std::vector<char> buf(1 << 16);
        std::string window;
        while (in.read(buf.data(), static_cast<std::streamsize>(buf.size())) || in.gcount() > 0) {
            window.append(buf.data(), static_cast<std::size_t>(in.gcount()));
            if (window.find(kEsp32AgentMarker) != std::string::npos) return true;
            if (window.size() >= kEsp32AgentMarker.size()) window.erase(0, window.size() - (kEsp32AgentMarker.size() - 1));
        }
        return false;
    }
    [[nodiscard]] std::vector<std::string> addr2line_names(Arch arch) const override {
        switch (arch) {
            case Arch::Xtensa: return {"xtensa-esp32-elf-addr2line", "xtensa-esp32s3-elf-addr2line"};
            case Arch::RiscV32: return {"riscv32-esp-elf-addr2line"};
            default: return {};
        }
    }
    [[nodiscard]] std::string family_hint_for_usb(std::string_view vid, std::string_view /*pid*/) const override {
        if (vid == "303a") return "esp32";
        return {};
    }
    [[nodiscard]] std::string usb_adapter_name(std::string_view vid, std::string_view /*pid*/) const override {
        if (vid == "303a") return "Espressif USB-Serial/JTAG (built-in debug port)";
        return {};
    }
};

} // namespace

const Family& esp32_family() {
    static const Esp32Family instance;
    return instance;
}

std::string merge_sdkconfig(std::string_view user, std::string_view agent) {
    std::vector<std::string> agent_lines;
    std::set<std::string> agent_keys;
    for (auto& line : lines_of(agent)) {
        if (const auto key = sdkconfig_key(line)) {
            agent_keys.insert(*key);
            agent_lines.push_back(std::move(line));
        }
    }
    std::string out;
    for (const auto& line : lines_of(user)) {
        const auto key = sdkconfig_key(line);
        if (!key || !agent_keys.contains(*key)) out += line + '\n';
    }
    for (const auto& line : agent_lines) out += line + '\n';
    return out;
}

void write_agent_sdkconfig(const ProjectInfo& project, const std::string& env, const fs::path& agent_dir, unsigned rate_hz) {
    const auto p = agent_paths(project, env, agent_dir);
    auto agent_keys = slurp(p.files / "sdkconfig.agent");
    if (agent_keys.empty()) throw std::runtime_error("the agent files are missing from " + os::to_utf8(p.files));
    fs::create_directories(p.out);
    if (const auto stamp = agent_stamp(p.files); slurp(p.out / "agent.stamp") != stamp) {
        std::error_code ec;
        fs::remove_all(p.out / "build", ec); // configured with other agent files: start clean (a few minutes, once per malgrind update)
        std::ofstream(p.out / "agent.stamp", std::ios::trunc) << stamp;
    }
    // The rate travels as an sdkconfig key, not as an environment variable: IDF reconfigures and recompiles the agent when
    // the sdkconfig changes, but would keep a stale object when only an environment variable does.
    if (rate_hz != 0) agent_keys += "\nCONFIG_MALGRIND_RATE_HZ=" + std::to_string(rate_hz) + "\n";
    fs::create_directories(p.out);
    // an unchanged file keeps its time stamp: a newer sdkconfig makes ESP-IDF reconfigure and rebuild everything
    const auto merged = merge_sdkconfig(slurp(project.dir / ("sdkconfig." + env)), agent_keys);
    if (slurp(p.sdkconfig) != merged) std::ofstream(p.sdkconfig, std::ios::trunc) << merged;
    if (!fs::exists(p.sdkconfig)) throw std::runtime_error("cannot write " + os::to_utf8(p.sdkconfig));
}

Invocation agent_command(const fs::path& pio, const ProjectInfo& project, const std::string& env, const std::string& port,
                         const fs::path& agent_dir, bool upload) {
    const auto p = agent_paths(project, env, agent_dir);
    std::vector<std::string> argv{os::to_utf8(pio), "run", "-d", os::to_utf8(project.dir), "-e", env};
    if (upload) argv.insert(argv.end(), {"-t", "upload", "--upload-port", port});
    return {std::move(argv),
            {"PLATFORMIO_BUILD_DIR=" + os::to_utf8(p.out / "build"),
             "PLATFORMIO_EXTRA_SCRIPTS=pre:" + os::to_utf8(p.files / "inject.py"),
             "MALGRIND_AGENT_DIR=" + os::to_utf8(p.files),
             "MALGRIND_SDKCONFIG=" + os::to_utf8(p.sdkconfig)}};
}

Invocation agent_build(const fs::path& pio, const ProjectInfo& project, const std::string& env, const std::string& port,
                       const fs::path& agent_dir, bool upload) {
    write_agent_sdkconfig(project, env, agent_dir);
    return agent_command(pio, project, env, port, agent_dir, upload);
}

fs::path agent_elf(const ProjectInfo& project, const std::string& env) {
    return project.dir / ".pio/malgrind/build" / env / "firmware.elf";
}

std::optional<Invocation> esptool(const fs::path& pio, const fs::path& home, const std::string& port, std::vector<std::string> args) {
    fs::path script_path;
    for (const auto& core : os::platformio_core_dirs(home)) {
        const fs::path candidate = core / "packages/tool-esptoolpy/esptool.py";
        if (std::error_code ec; fs::exists(candidate, ec)) {
            script_path = candidate;
            break;
        }
    }
    if (script_path.empty() && !home.empty()) {
        const fs::path fallback = home / ".platformio/packages/tool-esptoolpy/esptool.py";
        if (std::error_code ec; fs::exists(fallback, ec)) script_path = fallback;
    }
    if (script_path.empty()) return std::nullopt;
    auto python_cmd = os::pio_python(home, pio);
    if (!python_cmd) return std::nullopt;
    Invocation inv;
    inv.argv = std::move(*python_cmd);
    for (auto& a : std::vector<std::string>{os::to_utf8(script_path), "--port", port, "--baud", std::to_string(kEsp32AgentBaud)})
        inv.argv.push_back(std::move(a));
    for (auto& a : args) inv.argv.push_back(std::move(a));
    return inv;
}

std::optional<std::string> parse_chip(std::string_view out) {
    constexpr std::string_view key = "Chip is ";
    const auto at = out.find(key);
    if (at == std::string_view::npos) return std::nullopt;
    const auto start = at + key.size();
    auto end = out.find_first_of("\r\n", start);
    if (end == std::string_view::npos) end = out.size();
    while (end > start && std::isspace(static_cast<unsigned char>(out[end - 1]))) --end;
    if (end == start) return std::nullopt;
    return std::string(out.substr(start, end - start));
}

std::optional<std::string> parse_mac(std::string_view out) {
    constexpr std::string_view key = "MAC: ";
    const auto at = out.find(key);
    if (at == std::string_view::npos) return std::nullopt;
    const auto text = out.substr(at + key.size(), 17);
    if (text.size() != 17) return std::nullopt;
    std::string mac;
    for (std::size_t i = 0; i < 17; ++i) {
        if (i % 3 == 2) {
            if (text[i] != ':') return std::nullopt;
            mac += '-';
        } else {
            if (!is_hex(text[i])) return std::nullopt;
            mac += static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
        }
    }
    return mac;
}

std::optional<std::uint32_t> parse_flash_size(std::string_view out) {
    constexpr std::string_view key = "Detected flash size: ";
    const auto at = out.find(key);
    if (at == std::string_view::npos) return std::nullopt;
    auto rest = out.substr(at + key.size());
    std::uint32_t n = 0;
    std::size_t digits = 0;
    while (digits < rest.size() && std::isdigit(static_cast<unsigned char>(rest[digits])) && digits < 5)
        n = n * 10 + static_cast<std::uint32_t>(rest[digits++] - '0');
    if (digits == 0) return std::nullopt;
    rest.remove_prefix(digits);
    if (rest.starts_with("MB")) return n * 1024u * 1024u;
    if (rest.starts_with("KB")) return n * 1024u;
    return std::nullopt;
}

} // namespace malgrind::host
