#pragma once

// Portable RAII environment helpers for host tests.

#include "os/paths.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace malgrind::host {

class ScopedEnv {
public:
    explicit ScopedEnv(std::string name) : name_(std::move(name)), saved_(os::get_env(name_)) {}

    ScopedEnv(std::string name, std::optional<std::string_view> value)
        : name_(std::move(name)), saved_(os::get_env(name_)) {
        if (value) os::set_env(name_, *value);
        else os::unset_env(name_);
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

    ~ScopedEnv() {
        if (saved_) os::set_env(name_, *saved_);
        else os::unset_env(name_);
    }

private:
    std::string name_;
    std::optional<std::string> saved_;
};

// Prepends `dir` to the current PATH using os::kPathListSep without leaving a trailing separator.
[[nodiscard]] inline std::string path_with(const std::filesystem::path& dir) {
    std::string out = os::to_utf8(dir);
    if (const auto current = os::get_env("PATH"); current && !current->empty()) {
        if (!out.empty()) out.push_back(os::kPathListSep);
        out.append(*current);
    }
    return out;
}

} // namespace malgrind::host
