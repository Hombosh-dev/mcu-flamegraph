#include "families/family.hpp"

#include "families/esp32.hpp"

#include <stdexcept>

namespace malgrind::host {

BoardIdentity Family::identify(const ToolContext&, const std::string&, const LineFn&) const {
    throw std::runtime_error(std::string(name()) + ": board identification is not supported");
}

void Family::read_flash(const ToolContext&, const std::string&, const BoardIdentity&, const std::filesystem::path&,
                        const LineFn&) const {
    throw std::runtime_error(std::string(name()) + ": reading flash is not supported");
}

void Family::write_flash(const ToolContext&, const std::string&, const std::filesystem::path&, const LineFn&) const {
    throw std::runtime_error(std::string(name()) + ": writing flash is not supported");
}

bool Family::is_agent_image(const std::filesystem::path&) const { return false; }

const std::vector<const Family*>& all_families() {
    static const std::vector<const Family*> families{&esp32_family()};
    return families;
}

const Family* family_by_id(std::string_view id) {
    for (const Family* f : all_families()) {
        if (f->id() == id) return f;
    }
    return nullptr;
}

const Family* family_for_arch(Arch arch) {
    for (const Family* f : all_families()) {
        if (f->handles_arch(arch)) return f;
    }
    return nullptr;
}

const Family* family_for_project(const ProjectInfo& project, const std::string& env, std::string& why) {
    why.clear();
    for (const Family* f : all_families()) {
        const std::string problem = f->project_problem(project, env);
        if (problem.empty()) {
            why.clear();
            return f;
        }
        if (!why.empty()) why += '\n';
        why += problem;
    }
    if (why.empty()) why = "no target families are registered";
    return nullptr;
}

} // namespace malgrind::host
