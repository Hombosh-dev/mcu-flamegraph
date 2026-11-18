#include "ops.hpp"

#include "backup.hpp"
#include "discover.hpp"
#include "os/process.hpp"
#include "store.hpp"
#include "wire.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>

namespace fs = std::filesystem;

namespace malgrind::host {

using namespace std::chrono_literals;

namespace {

std::vector<unsigned> unique_bauds(std::string_view family_hint = {}) {
    std::vector<unsigned> bauds;
    if (!family_hint.empty()) {
        if (const Family* f = family_by_id(family_hint); f && f->agent_baud() != 0) {
            bauds.push_back(f->agent_baud());
        }
    }
    for (const Family* f : all_families()) {
        if (const unsigned b = f->agent_baud(); b != 0 && std::ranges::find(bauds, b) == bauds.end()) {
            bauds.push_back(b);
        }
    }
    return bauds;
}

unsigned resolve_baud(unsigned baud, std::string_view family_hint = {}) {
    if (baud != 0) return baud;
    const auto bauds = unique_bauds(family_hint);
    if (bauds.empty()) throw std::runtime_error("no target family baud rate is configured");
    return bauds.front();
}

std::vector<const Family*> identify_candidates(std::string_view family_hint, bool require_backup) {
    std::vector<const Family*> list;
    auto push_if = [&](const Family* f) {
        if (!f || !f->caps().identify || (require_backup && !f->caps().backup)) return;
        if (std::ranges::find(list, f) == list.end()) list.push_back(f);
    };
    if (!family_hint.empty()) push_if(family_by_id(family_hint));
    for (const Family* f : all_families()) push_if(f);
    return list;
}

std::variant<std::unique_ptr<ByteSource>, os::OpenFailure> open_device_result(const Device& dev, unsigned baud,
                                                                              const SourceFactory& factory) {
    if (factory) {
        auto res = factory(dev, baud);
        if (auto* source = std::get_if<std::unique_ptr<ByteSource>>(&res); source && !*source) {
            return os::OpenFailure{os::OpenError::Other, false, "source factory returned null"};
        }
        return res;
    }
    auto res = SerialSource::open(dev, baud);
    if (auto* source = std::get_if<std::unique_ptr<SerialSource>>(&res)) {
        return std::unique_ptr<ByteSource>(std::move(*source));
    }
    return std::get<os::OpenFailure>(std::move(res));
}

ProbeResult probe_failure(const std::string& port, const os::OpenFailure& failure) {
    if (failure.kind == os::OpenError::Busy) return {.busy = true};
    if (failure.kind == os::OpenError::Denied) return {.denied = true};
    throw open_error(port, failure);
}

} // namespace

std::runtime_error open_error(const std::string& port, const os::OpenFailure& failure) {
    if (failure.kind == os::OpenError::Busy)
        return std::runtime_error("busy: another program (maybe another malgrind, or a serial monitor) is using " + port);
    if (failure.kind == os::OpenError::Denied)
        return std::runtime_error(std::format("no permission to use {}: choose Grant access on the board's tile (this user is not allowed to open the port)", port));
    return std::runtime_error(std::format("cannot open {}: {}", port, failure.detail));
}

std::unique_ptr<ByteSource> open_source(const std::string& port, unsigned baud) {
    if (port == "-") return std::make_unique<StdinSource>();
    auto res = SerialSource::open_path(port, resolve_baud(baud));
    if (auto* source = std::get_if<std::unique_ptr<SerialSource>>(&res)) return std::move(*source);
    throw open_error(port, std::get<os::OpenFailure>(res));
}

std::unique_ptr<ByteSource> open_device_source(const Device& dev, const SourceFactory& factory, unsigned baud) {
    auto res = open_device_result(dev, resolve_baud(baud, dev.family_hint), factory);
    if (auto* source = std::get_if<std::unique_ptr<ByteSource>>(&res)) return std::move(*source);
    throw open_error(dev.path, std::get<os::OpenFailure>(res));
}

std::unique_ptr<ByteSource> open_after_upload(const std::string& device_ref,
                                              const std::function<std::vector<Device>()>& devices_fn,
                                              const SourceFactory& factory, unsigned baud) {
    for (int attempt = 0;; ++attempt) {
        const auto now = devices_fn ? devices_fn() : list_devices();
        const auto dev = resolve_device(device_ref, now);
        if (!dev || dev->kind != DeviceKind::Serial) {
            if (attempt >= 20) throw std::runtime_error("device " + device_ref + " is not plugged in");
            // The USB device may still be re-enumerating after reset.
            std::this_thread::sleep_for(250ms);
            continue;
        }
        auto res = open_device_result(*dev, resolve_baud(baud, dev->family_hint), factory);
        if (auto* source = std::get_if<std::unique_ptr<ByteSource>>(&res)) return std::move(*source);
        const auto& failure = std::get<os::OpenFailure>(res);
        if (!failure.transient || attempt >= 20) throw open_error(dev->path, failure);
        std::this_thread::sleep_for(250ms);
    }
}

Prepared prepare_project(const fs::path& home, const fs::path& dir, std::string env, std::string port) {
    const auto pio = find_pio(home);
    if (!pio) throw std::runtime_error(kNoPio);
    std::string why;
    auto project = read_project(dir, why);
    if (!project) throw std::runtime_error(why);
    if (env.empty()) env = project->default_env;
    if (std::ranges::find(project->envs, env) == project->envs.end())
        throw std::runtime_error(std::format("platformio.ini has no environment '{}'", env));
    std::string family_why;
    const Family* family = family_for_project(*project, env, family_why);
    if (!family) throw std::runtime_error(family_why);
    return {*pio, std::move(*project), std::move(env), std::move(port), family};
}

void run_pio(const Invocation& invocation, const std::function<void(std::string_view line)>& on_line) {
    const int code = os::run_lines(invocation.argv, invocation.env, on_line);
    if (code != 0) throw std::runtime_error(code < 0 ? "cannot start pio" : std::format("build or upload failed (pio exit code {})", code));
}

ProbeResult probe_source(ByteSource& source) {
    FrameReader reader;
    std::array<std::uint8_t, 4096> buf;
    for (const auto end = std::chrono::steady_clock::now() + 2s; std::chrono::steady_clock::now() < end;) {
        const auto n = source.read(buf, 100);
        if (!n) break;
        if (*n == 0) continue;
        if (const auto frames = reader.feed(std::span(buf).first(*n)).frames; !frames.empty())
            return {.agent = true, .arch = frames[0].arch, .ncores = frames[0].ncores, .cpu_mhz = frames[0].cpu_hz / 1e6};
    }
    return {};
}

ProbeResult probe_port(const std::string& port, unsigned baud) {
    if (port == "-") {
        StdinSource stdin_source;
        return probe_source(stdin_source);
    }
    const auto bauds = baud != 0 ? std::vector<unsigned>{baud} : unique_bauds();
    for (const unsigned b : bauds) {
        auto res = SerialSource::open_path(port, b);
        if (auto* source = std::get_if<std::unique_ptr<SerialSource>>(&res)) {
            const auto result = probe_source(**source);
            if (result.agent) return result;
            continue;
        }
        return probe_failure(port, std::get<os::OpenFailure>(res));
    }
    return {};
}

ProbeResult probe_device(const Device& dev, const SourceFactory& factory, unsigned baud) {
    const auto bauds = baud != 0 ? std::vector<unsigned>{baud} : unique_bauds(dev.family_hint);
    for (const unsigned b : bauds) {
        auto res = open_device_result(dev, b, factory);
        if (auto* source = std::get_if<std::unique_ptr<ByteSource>>(&res)) {
            const auto result = probe_source(**source);
            if (result.agent) return result;
            continue;
        }
        return probe_failure(dev.path, std::get<os::OpenFailure>(res));
    }
    return {};
}

BoardIdentity identify_board(const ToolContext& tools, const std::string& port, const LineFn& on_line,
                             std::string_view family_hint) {
    std::string last_error;
    for (const Family* f : identify_candidates(family_hint, false)) {
        try {
            return f->identify(tools, port, on_line);
        } catch (const std::exception& e) {
            last_error = e.what();
        }
    }
    throw std::runtime_error(last_error.empty() ? "could not identify the board on " + port : last_error);
}

void flash_agent(const Prepared& p, const fs::path& home, const fs::path& backup_dir, const Invocation& build, const Invocation& upload,
                 const LineFn& on_line) {
    if (!p.family) throw std::runtime_error("no target family selected");
    const auto probe = probe_port(p.port, p.family->agent_baud());
    if (probe.busy) throw open_error(p.port, {os::OpenError::Busy, false, {}});
    if (probe.denied) throw open_error(p.port, {os::OpenError::Denied, false, {}});
    // Build first: a compile error then costs the board neither a reset nor a backup, and PlatformIO has downloaded the flash
    // tool by the time the backup needs it (on a fresh machine it is not there before the first build).
    run_pio(build, on_line);
    if (probe.agent) {
        on_line("the board already runs malgrind: no backup taken now");
    } else if (p.family->caps().backup) {
        on_line("backing up the board's flash first (this takes a minute)...");
        const ToolContext tools{home, p.pio, {}};
        const auto board = p.family->identify(tools, p.port, on_line);
        const auto part = backup_path(backup_dir, board.key, utc_now("%Y%m%d-%H%M%S"));
        if (!part) throw std::runtime_error("invalid board identity key: " + board.key);
        std::error_code ec;
        fs::create_directories(part->parent_path(), ec);
        if (ec) throw std::runtime_error(std::format("cannot create {}: {}", part->parent_path().string(), ec.message()));
        try {
            p.family->read_flash(tools, p.port, board, *part, on_line);
            if (fs::file_size(*part, ec) != board.flash_bytes || ec) throw std::runtime_error("the backup file is incomplete");
        } catch (...) {
            fs::remove(*part, ec);
            throw;
        }
        if (p.family->is_agent_image(*part)) { // a board someone else set up with malgrind: the "original" is not worth keeping
            fs::remove(*part, ec);
            on_line("this board already holds a malgrind build: no backup kept");
        } else {
            auto done = *part;
            done.replace_extension(); // "x.bin.part" -> "x.bin"
            fs::rename(*part, done);
            on_line("backup saved: " + done.string());
        }
    }
    run_pio(upload, on_line); // nothing left to compile
}

void restore_flash(const fs::path& pio, const fs::path& home, const std::string& port, const fs::path& backup_dir, const LineFn& on_line) {
    const ToolContext tools{home, pio, {}};
    std::optional<std::pair<const Family*, BoardIdentity>> found;
    std::string last_error;
    for (const Family* f : identify_candidates({}, true)) {
        try {
            found.emplace(f, f->identify(tools, port, on_line));
            break;
        } catch (const std::exception& e) {
            last_error = e.what();
        }
    }
    if (!found) throw std::runtime_error(last_error.empty() ? "flash restore is not supported for this board" : last_error);
    const auto& [family, board] = *found;
    const auto file = latest_backup(backup_dir, board.key);
    if (!file)
        throw std::runtime_error(std::format(
            "no backup of this board ({}): malgrind takes one when it first flashes the agent onto a board. If you have this firmware's project, use \"Rebuild my project instead\"",
            board.key));
    on_line("restoring " + file->string());
    family->write_flash(tools, port, *file, on_line);
}

fs::path clone_project(const GithubRepo& repo, const fs::path& dest, const LineFn& on_line) {
    const auto git = os::find_in_path("git");
    if (!git) throw std::runtime_error("git was not found: install git to open a project from GitHub");
    // protocol.allow=never then https only: whatever the link says, git speaks nothing but https here
    const int code = os::run_lines({git->string(), "-c", "protocol.allow=never", "-c", "protocol.https.allow=always", "clone", "--depth", "1",
                                "--progress", "--", repo.url, dest.string()},
                               {"GIT_TERMINAL_PROMPT=0"}, on_line);
    std::error_code ec;
    if (code != 0) {
        fs::remove_all(dest, ec);
        throw std::runtime_error(std::format("git clone failed (exit code {}): is the repository public and the link right?", code));
    }
    std::string why;
    const auto project = find_project(dest, why);
    if (!project) {
        fs::remove_all(dest, ec);
        throw std::runtime_error("no platformio.ini found in this repository (looked in its root and in firmware/*)");
    }
    return *project;
}

std::vector<Method> methods(const ProbeResult& probe, const Family* family, bool have_pio,
                            const std::string& project_problem) {
    const std::string busy = "the port is busy: another program is using it";
    const std::string denied = "no permission to use this port: choose Grant access on the board's tile";
    const std::string blocked = probe.denied ? denied : probe.busy ? busy : "";
    std::vector<Method> list;
    list.push_back({"attach", probe.agent, probe.agent ? "" : !blocked.empty() ? blocked : "no malgrind agent is running on this board"});
    std::string new_run_why;
    if (!blocked.empty()) {
        new_run_why = blocked;
    } else if (!project_problem.empty()) {
        new_run_why = project_problem;
    } else if (!family) {
        new_run_why = "no matching target family";
    } else if (!family->caps().agent) {
        new_run_why = std::string(family->name()) + ": building with the agent is not supported";
    } else if (!have_pio) {
        new_run_why = kNoPio;
    }
    list.push_back({"new_run", new_run_why.empty(), new_run_why});
    list.push_back({"no_flash", false, "needs a debug probe (JTAG/SWD); not supported yet"});
    return list;
}

} // namespace malgrind::host
