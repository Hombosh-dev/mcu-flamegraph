#include "serve.hpp"

#include "devices.hpp"
#include "discover.hpp"
#include "github.hpp"
#include "ops.hpp"
#include "os/access.hpp"
#include "os/paths.hpp"
#include "os/process.hpp"
#include "os/tools.hpp"
#include "plan.hpp"
#include "pio.hpp"
#include "projects.hpp"
#include "setup.hpp"
#include "session.hpp"
#include "source.hpp"
#include "store.hpp"
#include "wire.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;

namespace malgrind::host {

namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;

constexpr const char* kVersion = MALGRIND_VERSION; // from the VERSION file, see host/CMakeLists.txt
std::string str_arg(const json& req, const char* key) {
    if (!req.contains(key) || !req[key].is_string()) throw std::runtime_error(std::string("missing argument: ") + key);
    return req[key].get<std::string>();
}

std::optional<std::string> opt_str_arg(const json& req, const char* key) {
    if (!req.contains(key) || !req[key].is_string()) return std::nullopt;
    return req[key].get<std::string>();
}

std::string device_arg(const json& req) {
    if (const auto dev = opt_str_arg(req, "device")) return *dev;
    if (const auto port = opt_str_arg(req, "port")) return *port;
    throw std::runtime_error("missing argument: device");
}

class Server {
public:
    Server(std::ostream& out, ServeOptions options)
        : out_(out), options_(std::move(options)), store_(options_.data_dir), projects_(options_.projects_dir) {}

    int run(std::istream& in) {
        for (std::string line; std::getline(in, line);)
            if (!line.empty()) handle_line(line);
        // The app is gone. A build that is still running is waited for first: it may start a run when it finishes,
        // and that run must be closed too. ponytail: kill pio's process group if waiting proves too slow.
        if (worker_.joinable()) worker_.join();
        set_session(nullptr); // closes the live run so it keeps its length
        return 0;
    }

private:
    // ---- output: one line per object, never interleaved ---------------------------------------------------------
    void send(const json& j) {
        std::scoped_lock lock(out_mutex_);
        // the board's console text is not always UTF-8 (boot ROM at another baud rate): replace, never throw
        out_ << j.dump(-1, ' ', false, json::error_handler_t::replace) << '\n' << std::flush;
    }
    void reply(const json& id, json result) { send({{"id", id}, {"ok", true}, {"result", std::move(result)}}); }
    void reply_raw(const json& id, const std::string& result) { // result is JSON text already: no parse and dump of a big tree
        std::scoped_lock lock(out_mutex_);
        out_ << R"({"id":)" << id.dump() << R"(,"ok":true,"result":)" << result << "}\n" << std::flush;
    }
    void fail(const json& id, const std::string& message) { send({{"id", id}, {"ok", false}, {"error", message}}); }
    Emit emitter() { return [this](const json& event) { send(event); }; }

    // ---- the session being shown (live, ended, or opened from history) ------------------------------------------
    std::shared_ptr<Session> current() {
        std::scoped_lock lock(session_mutex_);
        return session_;
    }
    void set_session(std::shared_ptr<Session> next) {
        {
            std::scoped_lock lock(session_mutex_);
            std::swap(session_, next);
        }
        // `next` (the old session) is released here, outside the lock: closing a live run waits for its reader thread
    }
    bool live() {
        const auto s = current();
        return s && s->live();
    }

    // ---- requests ----------------------------------------------------------------------------------------------------
    void handle_line(const std::string& line) {
        const auto req = json::parse(line, nullptr, false);
        if (!req.is_object()) {
            send({{"id", nullptr}, {"ok", false}, {"error", "malformed JSON: expected one JSON object per line"}});
            return;
        }
        const json id = req.contains("id") ? req["id"] : json(nullptr);
        try {
            const auto cmd = str_arg(req, "cmd");
            if (cmd == "snapshot" || cmd == "functions") return reply_raw(id, query(cmd, req));
            if (cmd == "new_run" || cmd == "remove_agent" || cmd == "chip_id" || cmd == "project_add" || cmd == "install_pio" || cmd == "grant_access") return run_long(id, cmd, req);
            reply(id, command(cmd, req));
        } catch (const std::exception& e) {
            fail(id, e.what());
        }
    }

    json command(const std::string& cmd, const json& req) {
        if (cmd == "hello") {
            const auto pio = find_pio(options_.home);
            return {{"version", kVersion}, {"data_dir", os::to_utf8(options_.data_dir)}, {"pio", pio ? json(os::to_utf8(*pio)) : json(nullptr)}};
        }
        if (cmd == "devices") return devices_command();
        if (cmd == "probe") return probe(req);
        if (cmd == "project") {
            std::string why;
            const auto project = read_project(os::from_utf8(str_arg(req, "dir")), why);
            if (!project) throw std::runtime_error(why);
            return {{"dir", os::to_utf8(project->dir)}, {"envs", project->envs}, {"default_env", project->default_env}};
        }
        if (cmd == "run_plan") return run_plan(req);
        if (cmd == "setup") return setup_info();
        if (cmd == "projects") return list_projects();
        if (cmd == "project_update") return update_project(req);
        if (cmd == "project_remove") {
            const auto id = str_arg(req, "project");
            if (!projects_.remove(id)) throw std::runtime_error("no such project: " + id);
            return json::object();
        }
        if (cmd == "attach") return attach(req);
        if (cmd == "stop") {
            set_session(nullptr);
            return json::object();
        }
        if (cmd == "runs") {
            const auto cur = current();
            auto runs = json::array();
            for (const auto& meta : store_.list()) {
                json run = meta;
                if (cur && cur->live() && cur->run_id() == meta.id) run["live"] = true;
                runs.push_back(std::move(run));
            }
            return runs;
        }
        if (cmd == "open") {
            if (live()) throw std::runtime_error("stop the live run first");
            const auto id = str_arg(req, "run");
            auto session = Session::replay(store_, id, emitter());
            if (!session) throw std::runtime_error("no such run: " + id);
            set_session(std::move(session));
            return {{"run", *store_.get(id)}};
        }
        if (cmd == "update") {
            const auto id = str_arg(req, "run");
            const auto meta = store_.update(id, opt_str_arg(req, "name"), opt_str_arg(req, "notes"));
            if (!meta) throw std::runtime_error("no such run: " + id);
            return *meta;
        }
        if (cmd == "delete") {
            const auto id = str_arg(req, "run");
            if (const auto cur = current(); cur && cur->run_id() == id) throw std::runtime_error("this run is open: close it first");
            if (!store_.remove(id)) throw std::runtime_error("no such run: " + id);
            return json::object();
        }
        throw std::runtime_error("unknown command: " + cmd);
    }

    // snapshot and functions answer with the aggregator's JSON text as it is
    std::string query(const std::string& cmd, const json& req) {
        std::optional<unsigned> core;
        if (req.contains("core") && req["core"].is_number_unsigned()) core = req["core"].get<unsigned>();
        const Aggregator::Ms window = std::chrono::seconds(req.value("window_s", 10));
        std::optional<Aggregator::Ms> at;
        if (req.contains("at_ms") && req["at_ms"].is_number()) at = Aggregator::Ms(req["at_ms"].get<long long>());
        const auto session = current();
        if (cmd == "snapshot") {
            auto result = session ? session->snapshot(core, window, at) : std::nullopt;
            return result ? *result : R"({"tree":null})";
        }
        auto result = session ? session->functions(core, window, at, req.value("focus", "")) : std::nullopt;
        return result ? *result : R"({"functions":[]})";
    }

    Device resolve_serial(const std::string& ref) const {
        const auto now = options_.devices ? options_.devices() : list_devices();
        const auto dev = resolve_device(ref, now);
        if (!dev) throw std::runtime_error(std::format("device {} is not plugged in", ref));
        if (dev->kind != DeviceKind::Serial) throw std::runtime_error(std::format("device {} is not a serial port", ref));
        return *dev;
    }

    json probe(const json& req) {
        const auto dev = resolve_serial(device_arg(req));
        const auto r = probe_device(dev, options_.open_source);
        const Family* probed_family = r.agent ? family_for_arch(r.arch) : nullptr;
        json result = {{"busy", r.busy},
                       {"agent", r.agent},
                       {"denied", r.denied},
                       {"family", probed_family ? std::string(probed_family->id()) : ""},
                       {"family_name", probed_family ? std::string(probed_family->name()) : ""}};
        if (r.agent) {
            result["arch"] = std::string(arch_name(r.arch));
            result["ncores"] = r.ncores;
            result["cpu_mhz"] = r.cpu_mhz;
        }
        const Family* method_family =
            probed_family ? probed_family : (!dev.family_hint.empty() ? family_by_id(dev.family_hint) : nullptr);
        std::string problem; // why the chosen project cannot be built with the agent; none chosen: no problem yet
        if (const auto id = opt_str_arg(req, "project"); id && !id->empty()) {
            try {
                const auto project = project_of(*id);
                require_buildable(project);
                method_family = prepare_project(options_.home, project.dir, project.env, dev.path).family;
            } catch (const std::exception& e) {
                method_family = nullptr;
                problem = e.what();
            }
        }
        json list = json::array();
        for (const auto& m : methods(r, method_family, find_pio(options_.home).has_value(), problem))
            list.push_back({{"id", m.id}, {"available", m.available}, {"why", m.why}});
        result["methods"] = std::move(list);
        return result;
    }

    json start_live(std::unique_ptr<ByteSource> source, RunMeta meta, fs::path elf, fs::path project) {
        auto writer = store_.create(std::move(meta));
        json run = writer.meta();
        set_session(Session::live(std::move(source), std::move(writer), std::move(elf), std::move(project), emitter()));
        return {{"run", std::move(run)}};
    }

    json attach(const json& req) {
        if (busy_) throw std::runtime_error("busy: a build or flash is running");
        if (live()) throw std::runtime_error("a run is live: stop it first");
        const auto dev = resolve_serial(device_arg(req));
        std::string project_id;
        const auto settings = settings_arg(req);
        fs::path project_dir, elf = !settings.elf.empty() ? settings.elf : os::from_utf8(opt_str_arg(req, "elf").value_or(""));
        if (!elf.empty() && !fs::exists(elf)) throw std::runtime_error("the firmware ELF does not exist: " + os::to_utf8(elf));
        if (const auto id = opt_str_arg(req, "project"); id && !id->empty()) {
            const auto project = project_of(*id);
            project_id = project.id;
            project_dir = project.dir;
            if (elf.empty()) elf = project.elf; // a project without code may still have been given its ELF
        }
        auto source = open_device_source(dev, options_.open_source);
        remember_settings(project_id, settings);
        return start_live(std::move(source),
                          {.name = str_arg(req, "name"),
                           .project = os::to_utf8(project_dir),
                           .port = dev.name.empty() ? dev.path : dev.name,
                           .device_id = dev.id,
                           .project_id = project_id,
                           .settings = settings},
                          std::move(elf), std::move(project_dir));
    }

    // ---- projects -----------------------------------------------------------------------------------------------------
    ProjectMeta project_of(const std::string& id) {
        auto project = projects_.get(id);
        if (!project) throw std::runtime_error("no such project: " + id);
        return std::move(*project);
    }

    // Building runs the project's own scripts (platformio.ini extra_scripts): a project from the internet needs the user's yes first.
    static void require_buildable(const ProjectMeta& project) {
        if (project.origin == Origin::None)
            throw std::runtime_error("this project has no source code: add a folder or a GitHub link to build with the agent");
        if (!project.trusted)
            throw std::runtime_error("this project came from GitHub and has not been trusted yet: open it and press Trust before building");
    }

    // The record plus what the folder says now: its environments, and why it cannot be used (moved, deleted).
    json project_json(const ProjectMeta& project) {
        json j = project;
        j["envs"] = json::array();
        j["problem"] = "";
        if (project.origin != Origin::None) {
            std::string why;
            if (const auto info = read_project(project.dir, why)) j["envs"] = info->envs;
            else j["problem"] = why;
        }
        return j;
    }

    json list_projects() {
        std::map<std::string, std::pair<unsigned, std::string>> runs; // project id -> run count, start of the newest run
        for (const auto& run : store_.list()) { // newest first
            auto& entry = runs[run.project_id];
            if (entry.first++ == 0) entry.second = run.started;
        }
        auto list = json::array();
        for (const auto& project : projects_.list()) {
            json j = project_json(project);
            const auto it = runs.find(project.id);
            j["run_count"] = it == runs.end() ? 0u : it->second.first;
            j["last_run"] = it == runs.end() ? json(nullptr) : json(it->second.second);
            list.push_back(std::move(j));
        }
        return list;
    }

    json update_project(const json& req) {
        const auto id = str_arg(req, "project");
        const auto current = project_of(id);
        ProjectUpdate change;
        change.name = opt_str_arg(req, "name");
        change.env = opt_str_arg(req, "env");
        if (req.contains("trusted") && req["trusted"].is_boolean()) change.trusted = req["trusted"].get<bool>();
        if (change.env && !change.env->empty()) {
            std::string why;
            const auto info = current.origin == Origin::None ? std::nullopt : read_project(current.dir, why);
            if (!info || std::ranges::find(info->envs, *change.env) == info->envs.end())
                throw std::runtime_error(std::format("platformio.ini has no environment '{}'", *change.env));
        }
        return project_json(*projects_.update(id, change));
    }

    // folder: a PlatformIO project (or a repository with one under firmware/*) the user chose; git: a clone of a GitHub repository;
    // none: just a board, optionally with the firmware's ELF.
    json add_project(const json& req) {
        const auto source = str_arg(req, "source");
        auto name = opt_str_arg(req, "name").value_or("");
        if (source == "folder") {
            const fs::path chosen = os::from_utf8(str_arg(req, "dir"));
            std::string why;
            const auto found = find_project(chosen, why);
            if (!found) throw std::runtime_error(why);
            const auto info = read_project(*found, why);
            if (!info) throw std::runtime_error(why);
            if (name.empty()) name = fs::absolute(chosen).lexically_normal().parent_path().empty() ? "project" : os::to_utf8(info->dir.filename());
            // the user picked their own folder, so it is trusted; they are not asked again
            return project_json(projects_.create({.name = name, .origin = Origin::Folder, .dir = info->dir, .env = info->default_env, .trusted = true}));
        }
        if (source == "git") {
            const auto repo = parse_github_url(str_arg(req, "url"));
            if (!repo) throw std::runtime_error("paste the link of a repository, like https://github.com/owner/repo");
            auto project = projects_.create({.name = name.empty() ? repo->name : name, .origin = Origin::Git, .url = repo->url});
            try {
                const auto dir = clone_project(*repo, projects_.clone_dir(project.id), build_line());
                std::string why;
                const auto info = read_project(dir, why);
                if (!info) throw std::runtime_error(why);
                project = *projects_.update(project.id, {.env = info->default_env, .dir = info->dir});
            } catch (...) {
                projects_.remove(project.id); // no half-made project is left behind
                throw;
            }
            return project_json(project); // not trusted yet
        }
        if (source == "none") {
            const auto elf = os::from_utf8(opt_str_arg(req, "elf").value_or(""));
            if (!elf.empty() && !elf_machine(elf)) throw std::runtime_error("that file is not an ELF (a firmware .elf)");
            if (name.empty()) name = elf.empty() ? "Board" : os::to_utf8(elf.stem());
            return project_json(projects_.create({.name = name, .origin = Origin::None}, elf));
        }
        throw std::runtime_error("source must be \"folder\", \"git\" or \"none\"");
    }

    // What the machine has for the jobs the app needs done: PlatformIO to build, git for GitHub links, and port access helpers.
    json setup_info() {
        auto path_or_null = [](const std::optional<fs::path>& p) { return p ? json(os::to_utf8(*p)) : json(nullptr); };
        const auto py = os::find_python();
        return {{"pio", path_or_null(find_pio(options_.home))},
                {"python", py && !py->empty() ? json((*py)[0]) : json(nullptr)},
                {"python_hint", os::python_hint()},
                {"git", path_or_null(os::find_in_path("git"))},
                {"can_grant_access", os::can_grant_access()},
                {"os", std::string(os::kind_name())},
                {"user", os::current_user()}};
    }

    json install_pio_command() {
        if (find_pio(options_.home)) throw std::runtime_error("PlatformIO is already installed");
        const auto python = os::find_python();
        if (!python) throw std::runtime_error("Python 3 was not found: " + os::python_hint() + ", then try again");
        install_pio(*python, options_.home, build_line());
        return {{"pio", os::to_utf8(*find_pio(options_.home))}};
    }

    // Gives this user access to a serial port that belongs to a group they are not in.
    json grant_access(const json& req) {
        const auto dev = resolve_serial(device_arg(req));
        const auto before = os::port_access(dev.path);
        if (before.ok) return {{"ok_now", true}, {"relogin", false}}; // nothing to do
        const auto user = os::current_user();
        const auto command = os::grant_access_command(dev.path, user, before.group);
        if (!command) {
            if (!os::can_grant_access())
                throw std::runtime_error(os::grant_access_unavailable(dev.path, user, before.group));
            throw std::runtime_error("cannot build the command for this user or port");
        }
        const int code = os::run_lines(command->argv, command->env, build_line());
        if (code == 126 || code == 127) throw std::runtime_error("the password prompt was cancelled, or the password was wrong");
        if (code != 0) throw std::runtime_error(std::format("granting access failed (exit code {})", code));
        const auto after = os::port_access(dev.path);
        return {{"ok_now", after.ok}, {"relogin", !after.in_group}}; // relogin: the group applies from the next login, the ACL until the board is replugged
    }

    json devices_command() const {
        const auto now = options_.devices ? options_.devices() : list_devices();
        auto list = json::array();
        for (const auto& d : now) {
            json usb = nullptr;
            if (d.usb) {
                usb = {{"vid", d.usb->vid},
                       {"pid", d.usb->pid},
                       {"serial", d.usb->serial},
                       {"location", d.usb->location},
                       {"product", d.usb->product},
                       {"manufacturer", d.usb->manufacturer}};
            }
            const Family* hinted = d.family_hint.empty() ? nullptr : family_by_id(d.family_hint);
            list.push_back({{"id", d.id},
                            {"kind", d.kind == DeviceKind::Probe ? "probe" : "serial"},
                            {"path", d.path},
                            {"name", d.name},
                            {"title", d.title},
                            {"usb", std::move(usb)},
                            {"access",
                             {{"ok", d.access.ok},
                              {"group", d.access.group},
                              {"in_group", d.access.in_group},
                              {"fixable", d.access.fixable}}},
                            {"family_hint", d.family_hint},
                            {"family_hint_name", hinted ? std::string(hinted->name()) : ""}});
        }
        return {{"devices", std::move(list)}, {"can_grant_access", os::can_grant_access()}};
    }

    // ---- slow commands, one at a time, on a worker thread ------------------------------------------------------------
    void run_long(const json& id, const std::string& cmd, const json& req) {
        if (busy_.exchange(true)) throw std::runtime_error("busy: another build or flash is running");
        if (worker_.joinable()) worker_.join(); // the previous one has finished (it cleared busy_)
        worker_ = std::thread([this, id, cmd, req] {
            try {
                reply(id, cmd == "new_run" ? new_run(req) : cmd == "remove_agent" ? remove_agent(req) : cmd == "project_add" ? add_project(req)
                                : cmd == "install_pio" ? install_pio_command() : cmd == "grant_access" ? grant_access(req) : chip_id_command(req));
            } catch (const std::exception& e) {
                fail(id, e.what());
            }
            busy_ = false;
        });
    }

    struct PreparedDevice {
        Prepared prepared;
        Device device;
    };

    // The project the request names, checked and ready to build (trusted, with source).
    PreparedDevice prepare(const json& req) {
        const auto project = project_of(str_arg(req, "project"));
        require_buildable(project);
        auto dev = resolve_serial(device_arg(req));
        auto p = prepare_project(options_.home, project.dir, opt_str_arg(req, "env").value_or(project.env), dev.path);
        return {std::move(p), std::move(dev)};
    }
    LineFn build_line() {
        return [this](std::string_view line) { send({{"event", "build"}, {"line", std::string(line)}}); };
    }

    json new_run(const json& req) {
        const auto pd = prepare(req);
        const auto& p = pd.prepared;
        const auto& dev = pd.device;
        if (live()) throw std::runtime_error("stop the live run first");
        const auto settings = settings_arg(req);
        // every setting is checked before the board is touched; a typo in a field costs nothing
        const auto plan = make_plan(p, options_.agent_dir, settings);
        write_plan_files(p, options_.agent_dir, plan);
        flash_agent(p, options_.home, options_.backup_dir, plan.build, plan.upload, build_line());
        auto source = open_after_upload(dev.id, options_.devices ? options_.devices : list_devices, options_.open_source,
                                        p.family->agent_baud());
        remember_settings(str_arg(req, "project"), settings);
        return start_live(std::move(source),
                          {.name = str_arg(req, "name"),
                           .project = os::to_utf8(p.project.dir),
                           .env = plan.env,
                           .port = dev.name.empty() ? dev.path : dev.name,
                           .device_id = dev.id,
                           .project_id = str_arg(req, "project"),
                           .settings = settings},
                          settings.elf.empty() ? plan.elf : settings.elf, p.project.dir);
    }

    // ---- run settings -------------------------------------------------------------------------------------------------
    static RunSettings settings_arg(const json& req) {
        return req.contains("settings") && req["settings"].is_object() ? req["settings"].get<RunSettings>() : RunSettings{};
    }

    // What a run used is offered again before the next one (the panel is shown every time, pre-filled with it). The two typed
    // command lines and the ELF are not: they belong to that board and that build. A remembered upload line carries the old
    // --upload-port, so on another board the header would name one board while the line flashed the other; a remembered ELF
    // would label a fresh build with the old one's functions. The run itself keeps all of its settings.
    void remember_settings(const std::string& project_id, RunSettings settings) {
        settings.build_command.clear();
        settings.upload_command.clear();
        settings.elf.clear();
        if (!project_id.empty()) projects_.update(project_id, {.last_settings = std::move(settings)});
    }

    // What the settings panel needs: the detected values, what the project used last, and the exact commands for the settings
    // the panel holds right now (so the preview and the real run come from the same make_plan). A field that does not hold up is
    // reported as `error` next to the commands, not as a failed request: the panel is being edited.
    json run_plan(const json& req) {
        const auto project = project_of(str_arg(req, "project"));
        const auto mode = opt_str_arg(req, "mode").value_or("new_run");
        json result = {{"last", project.last_settings ? json(*project.last_settings) : json(nullptr)}, {"commands", nullptr}, {"error", ""}};
        RunSettings detected;
        detected.elf = project.elf; // a project without code may have been given its ELF
        if (mode == "attach") {
            result["detected"] = detected;
            return result;
        }
        require_buildable(project);
        const auto dev = resolve_serial(device_arg(req));
        const auto p = prepare_project(options_.home, project.dir, project.env, dev.path);
        detected.env = p.env;
        detected.rate_hz = kDefaultRateHz;
        detected.elf.clear(); // a new run finds its ELF after the build: nothing to offer here
        result["detected"] = detected;
        result["envs"] = p.project.envs;
        const auto detected_plan = make_plan(p, options_.agent_dir, {});
        result["elf_hint"] = os::to_utf8(detected_plan.elf); // where the build puts it; used when no ELF is chosen
        result["detected_commands"] = {{"build", detected_plan.build_text}, {"upload", detected_plan.upload_text}};
        result["warning"] = "";
        try {
            const auto settings = settings_arg(req);
            const auto plan = make_plan(p, options_.agent_dir, settings);
            result["commands"] = {{"build", plan.build_text}, {"upload", plan.upload_text}, {"env", plan.build.env}};
            // a typed upload line replaces the one that names the chosen board: say so when it names no board at all
            if (!settings.upload_command.empty() && plan.upload_text.find(p.port) == std::string::npos)
                result["warning"] = "the upload command does not mention " + p.port + ", the board you chose: make sure it flashes that one";
        } catch (const std::exception& e) {
            result["error"] = e.what();
        }
        return result;
    }

    json remove_agent(const json& req) {
        const auto how = opt_str_arg(req, "how").value_or("restore");
        if (how != "restore" && how != "rebuild") throw std::runtime_error("how must be \"restore\" or \"rebuild\"");
        if (how == "rebuild") {
            const auto pd = prepare(req);
            set_session(nullptr); // the port must be free for the upload
            run_pio(plain_flash(pd.prepared.pio, pd.prepared.project, pd.prepared.env, pd.prepared.port), build_line());
            return json::object();
        }
        const auto pio = find_pio(options_.home);
        if (!pio) throw std::runtime_error(kNoPio);
        const auto dev = resolve_serial(device_arg(req));
        set_session(nullptr);
        restore_flash(*pio, options_.home, dev.path, options_.backup_dir, build_line());
        return json::object();
    }

    json chip_id_command(const json& req) {
        if (live()) throw std::runtime_error("stop the live run first");
        const auto pio = find_pio(options_.home);
        if (!pio) throw std::runtime_error(kNoPio);
        const auto dev = resolve_serial(device_arg(req));
        const auto id = identify_board({options_.home, *pio, options_.agent_dir}, dev.path, {}, dev.family_hint);
        if (id.chip.empty()) throw std::runtime_error("could not identify the board on " + dev.path);
        return {{"chip", id.chip}};
    }

    std::ostream& out_;
    std::mutex out_mutex_;
    ServeOptions options_;
    RunStore store_;
    ProjectStore projects_;
    std::mutex session_mutex_;
    std::shared_ptr<Session> session_;
    std::atomic<bool> busy_{false};
    std::thread worker_; // last: joined before the rest goes away
};

} // namespace

int serve(std::istream& in, std::ostream& out, const ServeOptions& options) { return Server(out, options).run(in); }

} // namespace malgrind::host
