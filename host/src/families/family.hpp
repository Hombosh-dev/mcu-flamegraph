#pragma once

// Chip-family abstraction: each target family describes its capabilities, validates PlatformIO projects, builds the
// agent commands, identifies and flashes boards, and supplies USB/toolchain metadata.

#include "malgrind/target_info.hpp"
#include "pio.hpp"
#include "settings.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace malgrind::host {

using LineFn = std::function<void(std::string_view line)>;

struct ToolContext {
    std::filesystem::path home, pio, agent_root; // agent_root contains agent/<family>/
};

struct AgentCommands {
    Invocation build, upload;
    std::filesystem::path elf;
};

struct BoardIdentity {
    std::string key; // MAC etc., [0-9a-f-]
    std::uint32_t flash_bytes = 0;
    std::string chip;
};

struct FamilyCaps {
    bool agent = false;                 // can build the user's project with the agent
    bool backup = false;                // can read and write back the whole flash
    bool identify = false;              // can name the chip (identify_resets_board says whether that resets it)
    bool identify_resets_board = false;
};

class Family {
public:
    virtual ~Family() = default;
    [[nodiscard]] virtual std::string_view id() const = 0;   // "esp32"
    [[nodiscard]] virtual std::string_view name() const = 0; // "ESP32 (ESP-IDF)"
    [[nodiscard]] virtual FamilyCaps caps() const = 0;
    [[nodiscard]] virtual bool handles_arch(Arch arch) const = 0;
    // "" when this family's agent can be built into env; else why not, in words for the methods report.
    [[nodiscard]] virtual std::string project_problem(const ProjectInfo& project, const std::string& env) const = 0;
    [[nodiscard]] virtual unsigned agent_baud() const = 0;
    // write_files=false: a preview (make_plan); true: also writes <project>/.pio/malgrind/* the build needs.
    [[nodiscard]] virtual AgentCommands agent_commands(const ToolContext& tools, const ProjectInfo& project,
                                                       const std::string& env, const std::string& port,
                                                       const RunSettings& settings, bool write_files) const = 0;
    virtual BoardIdentity identify(const ToolContext& tools, const std::string& port, const LineFn& on_line) const;
    virtual void read_flash(const ToolContext& tools, const std::string& port, const BoardIdentity& board,
                            const std::filesystem::path& out, const LineFn& on_line) const;
    virtual void write_flash(const ToolContext& tools, const std::string& port, const std::filesystem::path& image,
                             const LineFn& on_line) const;
    [[nodiscard]] virtual bool is_agent_image(const std::filesystem::path& image) const;
    [[nodiscard]] virtual std::vector<std::string> addr2line_names(Arch arch) const = 0;
    [[nodiscard]] virtual std::string family_hint_for_usb(std::string_view vid, std::string_view pid) const {
        (void)vid;
        (void)pid;
        return {};
    }
    [[nodiscard]] virtual std::string usb_adapter_name(std::string_view vid, std::string_view pid) const {
        (void)vid;
        (void)pid;
        return {};
    }
};

[[nodiscard]] const std::vector<const Family*>& all_families();
[[nodiscard]] const Family* family_by_id(std::string_view id);
[[nodiscard]] const Family* family_for_arch(Arch arch);
// First family with an empty project_problem; otherwise nullptr and `why` = the problems of all families, one per line.
[[nodiscard]] const Family* family_for_project(const ProjectInfo& project, const std::string& env, std::string& why);

} // namespace malgrind::host
