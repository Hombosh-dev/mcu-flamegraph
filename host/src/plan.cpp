#include "plan.hpp"

#include "os/paths.hpp"
#include "os/process.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;

namespace malgrind::host {

namespace {

bool safe_word_char(unsigned char c) { return std::isalnum(c) || std::string_view("_@%+=:,./-").find(static_cast<char>(c)) != std::string_view::npos; }

// Replaces the command line of `inv` by the one the user typed. The environment (the agent's) is left alone.
void override_command(Invocation& inv, const std::string& line, const char* what) {
    if (line.empty()) return;
    auto words = split_command(line);
    if (!words || words->empty()) throw std::runtime_error(std::format("{}: this is not a command line (is a quote left open?)", what));
    const auto& program = words->front();
    const bool has_sep = program.find_first_of("/\\") != std::string::npos;
    const bool found = has_sep ? fs::exists(os::from_utf8(program)) : os::find_in_path(program).has_value();
    if (!found) throw std::runtime_error(std::format("{}: '{}' was not found", what, program));
    inv.argv = std::move(*words);
}

const Family* resolve_family(const Prepared& p, const std::string& env) {
    if (p.family) return p.family;
    std::string why;
    const Family* family = family_for_project(p.project, env, why);
    if (!family) throw std::runtime_error(why);
    return family;
}

} // namespace

std::optional<std::vector<std::string>> split_command(std::string_view line) {
    enum class Mode { Plain, Single, Double } mode = Mode::Plain;
    std::vector<std::string> words;
    std::string current;
    bool have_word = false; // an empty pair of quotes is a word too
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        switch (mode) {
            case Mode::Plain:
                if (std::isspace(static_cast<unsigned char>(c))) {
                    if (have_word) words.push_back(std::exchange(current, {})), have_word = false;
                } else if (c == '\'') {
                    mode = Mode::Single, have_word = true;
                } else if (c == '"') {
                    mode = Mode::Double, have_word = true;
                } else if (c == '\\') {
                    if (i + 1 >= line.size()) return std::nullopt;
                    current += line[++i], have_word = true;
                } else {
                    current += c, have_word = true;
                }
                break;
            case Mode::Single:
                if (c == '\'') mode = Mode::Plain;
                else current += c;
                break;
            case Mode::Double:
                if (c == '"') mode = Mode::Plain;
                else if (c == '\\' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\')) current += line[++i];
                else current += c;
                break;
        }
    }
    if (mode != Mode::Plain) return std::nullopt;
    if (have_word) words.push_back(std::move(current));
    return words;
}

std::string join_command(const std::vector<std::string>& words) {
    std::string out;
    for (const auto& word : words) {
        if (!out.empty()) out += ' ';
        if (!word.empty() && std::ranges::all_of(word, [](unsigned char c) { return safe_word_char(c); })) {
            out += word;
            continue;
        }
        out += '\'';
        for (const char c : word) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
        out += '\'';
    }
    return out;
}

Plan make_plan(const Prepared& p, const fs::path& agent_dir, const RunSettings& s) {
    const std::string env = s.env.empty() ? p.env : s.env;
    if (std::ranges::find(p.project.envs, env) == p.project.envs.end())
        throw std::runtime_error(std::format("platformio.ini has no environment '{}'", env));
    if (s.rate_hz != 0 && (s.rate_hz < kMinRateHz || s.rate_hz > kMaxRateHz))
        throw std::runtime_error(std::format("the sampling rate must be between {} and {} Hz", kMinRateHz, kMaxRateHz));
    if (!s.elf.empty() && !fs::exists(s.elf)) throw std::runtime_error("the firmware ELF does not exist: " + os::to_utf8(s.elf));
    const auto extra = split_command(s.extra_args);
    if (!extra) throw std::runtime_error("extra arguments: a quote is left open");

    const Family* family = resolve_family(p, env);
    auto cmds = family->agent_commands(ToolContext{{}, p.pio, agent_dir}, p.project, env, p.port, s, false);
    Plan plan{std::move(cmds.build), std::move(cmds.upload), std::move(cmds.elf), {}, {}, env, s.rate_hz};
    for (auto* inv : {&plan.build, &plan.upload}) {
        inv->argv.insert(inv->argv.end(), extra->begin(), extra->end());
        if (!s.build_flags.empty()) inv->env.push_back("PLATFORMIO_BUILD_FLAGS=" + s.build_flags);
    }
    override_command(plan.build, s.build_command, "build command");
    override_command(plan.upload, s.upload_command, "upload command");
    plan.build_text = join_command(plan.build.argv);
    plan.upload_text = join_command(plan.upload.argv);
    return plan;
}

void write_plan_files(const Prepared& p, const fs::path& agent_dir, const Plan& plan) {
    const Family* family = resolve_family(p, plan.env);
    RunSettings s;
    s.env = plan.env;
    s.rate_hz = plan.rate_hz;
    (void)family->agent_commands(ToolContext{{}, p.pio, agent_dir}, p.project, plan.env, p.port, s, true);
}

} // namespace malgrind::host
