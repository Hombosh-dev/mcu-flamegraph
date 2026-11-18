#include "symbols.hpp"

#include "families/family.hpp"
#include "os/paths.hpp"
#include "os/process.hpp"
#include "os/tools.hpp"

#include <algorithm>
#include <format>
#include <sstream>

namespace fs = std::filesystem;

namespace malgrind::host {

namespace {

// Return addresses point after the call; step back one byte so the call stays inside its function.
std::uint32_t lookup_addr(std::size_t index, std::uint32_t pc) { return index == 0 ? pc : pc - 1; }

std::vector<std::string> tool_names(Arch arch) {
    if (arch == Arch::Host) return {"addr2line"};
    if (const Family* family = family_for_arch(arch)) return family->addr2line_names(arch);
    return {};
}

// "(anonymous namespace)::ns::f(int, char*)" -> "ns::f". addr2line names the same function with and without
// its parameter list (symbol table vs. DWARF), and parameters only make flamegraph labels long.
std::string short_name(std::string name) {
    constexpr std::string_view kAnon = "(anonymous namespace)::";
    for (auto at = name.find(kAnon); at != std::string::npos; at = name.find(kAnon)) name.erase(at, kAnon.size());
    int depth = 0; // inside <...> or {...}: template arguments, lambdas
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c == '<' || c == '{') ++depth;
        else if (c == '>' || c == '}') --depth;
        else if (c == '(' && depth == 0 && !name.substr(0, i).ends_with("operator")) return name.substr(0, i);
    }
    return name;
}

} // namespace

fs::path find_addr2line(Arch arch) {
    std::vector<fs::path> dirs;
    if (const auto path = os::get_env("PATH")) dirs = os::split_path_list(*path);
    for (const auto& core : os::platformio_core_dirs(os::home_dir())) {
        std::error_code ec;
        for (const auto& pkg : fs::directory_iterator(core / "packages", ec))
            if (os::to_utf8(pkg.path().filename()).starts_with("toolchain-")) dirs.push_back(pkg.path() / "bin");
    }
    for (const auto& name : tool_names(arch)) {
        const auto bin = os::exe_name(name);
        for (const auto& dir : dirs)
            if (std::error_code ec; fs::exists(dir / bin, ec)) return dir / bin;
    }
    return {};
}

void Symbolizer::prepare(const Frame& frame) {
    std::vector<std::uint32_t> todo;
    {
        std::scoped_lock lock(mutex_);
        for (const auto& stack : frame.stacks)
            for (std::size_t i = 0; i < stack.pcs.size(); ++i)
                if (const auto a = lookup_addr(i, stack.pcs[i]); !cache_.contains(a)) todo.push_back(a);
    }
    std::ranges::sort(todo);
    todo.erase(std::ranges::unique(todo).begin(), todo.end());
    if (todo.empty()) return;

    std::vector<std::vector<std::string>> chains(todo.size());
    if (!elf_.empty() && !tool_.empty()) {
        // -i prints every inlined frame of an address with nothing between addresses, so each address is
        // followed by an invalid one: its "??" answer marks where the previous chain ends.
        constexpr std::size_t kChunk = 128; // keep the command line short
        constexpr const char* kSentinel = "0xffffffff";
        for (std::size_t first = 0; first < todo.size(); first += kChunk) {
            const auto last = std::min(first + kChunk, todo.size());
            std::vector<std::string> argv{os::to_utf8(tool_), "-i", "-f", "-C", "-e", os::to_utf8(elf_)};
            for (auto i = first; i < last; ++i) {
                argv.push_back(std::format("0x{:x}", todo[i]));
                argv.push_back(kSentinel);
            }
            std::istringstream out(os::capture(argv));
            std::string func, location; // addr2line prints "function" then "file:line" per frame
            for (auto i = first; i < last; ++i) {
                bool first_pair = true;
                while (std::getline(out, func) && std::getline(out, location)) {
                    if (func == "??" && !first_pair) break; // the sentinel's answer
                    first_pair = false;
                    if (func != "??") chains[i].push_back(short_name(func));
                }
            }
        }
    }
    std::scoped_lock lock(mutex_);
    for (std::size_t i = 0; i < todo.size(); ++i)
        cache_[todo[i]] = chains[i].empty() ? std::vector<std::string>{std::format("0x{:x}", todo[i])} : chains[i];
}

std::vector<std::string> Symbolizer::frames(std::size_t index, std::uint32_t pc) const {
    std::scoped_lock lock(mutex_);
    const auto it = cache_.find(lookup_addr(index, pc));
    return it != cache_.end() ? it->second : std::vector<std::string>{std::format("0x{:x}", pc)};
}

} // namespace malgrind::host
