// serve() self-check: the JSON-lines protocol the app talks, driven through in-memory pipes and MemorySource (plus one POSIX pty check).
#include "devices.hpp"
#include "env.hpp"
#include "ops.hpp"
#include "os/paths.hpp"
#include "os/tools.hpp"
#include "pipe_stream.hpp"
#include "plan.hpp"
#include "serve.hpp"
#include "source.hpp"
#include "store.hpp"
#include "stream.hpp"

#ifndef _WIN32
#include "pty.hpp"
#endif

#include <nlohmann/json.hpp>

#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

using namespace malgrind::host;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct Harness {
    fs::path root;
    ScopedEnv save_path;
    ScopedEnv save_core{"PLATFORMIO_CORE_DIR", std::nullopt};
    ScopedEnv save_drive{"SystemDrive", std::nullopt};
    ScopedEnv save_git{"FAKE_GIT", std::nullopt};
    InPipe in_buf;
    OutPipe out_buf;
    std::istream in{&in_buf};
    std::ostream out{&out_buf};
    std::mutex devices_mutex;
    std::vector<Device> extra_devices;
    bool override_devices = false;
    std::mutex source_mutex;
    SourceFactory source_factory;
    MemorySource* last_source = nullptr;
    std::thread thread;
    int exit_code = -1;
    int next_id = 1;
    std::vector<json> events; // everything without an id that arrived while waiting for a reply

    explicit Harness(const char* name, bool real_serial = false)
        : root(fs::temp_directory_path() / os::from_utf8(name)),
          save_path("PATH", os::to_utf8(root / "empty-path")) {
        fs::remove_all(root);
        fs::create_directories(root / "home");
        ServeOptions opts{root / "runs", MALGRIND_SOURCE_DIR, root / "home", root / "backups", root / "projects",
                         [this] { return current_devices(); }};
        if (!real_serial) {
            opts.open_source = [this](const Device& dev, unsigned baud)
                -> std::variant<std::unique_ptr<ByteSource>, os::OpenFailure> {
                SourceFactory fn;
                {
                    std::scoped_lock lock(source_mutex);
                    fn = source_factory;
                }
                if (fn) return fn(dev, baud);
                auto src = std::make_unique<MemorySource>(dev.name.empty() ? dev.path : dev.name);
                {
                    std::scoped_lock lock(source_mutex);
                    last_source = src.get();
                }
                return std::unique_ptr<ByteSource>(std::move(src));
            };
        }
        thread = std::thread([this, opts = std::move(opts)] { exit_code = serve(in, out, opts); });
    }
    ~Harness() {
        finish();
        fs::remove_all(root);
    }
    void set_source_factory(SourceFactory fn) {
        std::scoped_lock lock(source_mutex);
        source_factory = std::move(fn);
    }
    MemorySource* source() {
        std::scoped_lock lock(source_mutex);
        return last_source;
    }
    std::vector<Device> current_devices() {
        std::scoped_lock lock(devices_mutex);
        if (override_devices) return extra_devices;
        auto devs = list_devices();
        devs.insert(devs.end(), extra_devices.begin(), extra_devices.end());
        return devs;
    }
    Device plug(const std::string& path, std::string id = "", std::string name = "") {
        Device d;
        d.kind = DeviceKind::Serial;
        d.path = path;
        if (name.empty()) {
            name = os::to_utf8(os::from_utf8(path).filename());
            if (name.empty()) name = path;
        }
        d.name = std::move(name);
        if (id.empty()) {
            os::PortInfo p;
            p.path = d.path;
            p.name = d.name;
            id = device_id(p);
        }
        d.id = std::move(id);
        d.title = d.name;
        d.access = os::port_access(d.path);
        std::scoped_lock lock(devices_mutex);
        extra_devices.push_back(d);
        return d;
    }
    void finish() {
        in_buf.close();
        if (thread.joinable()) thread.join();
    }
    json call(json request) {
        const int id = next_id++;
        request["id"] = id;
        in_buf.push(request.dump());
        for (;;) {
            const auto line = out_buf.next();
            assert(line && "no reply from serve");
            auto j = json::parse(*line);
            if (j.contains("id") && j["id"] == id) return j;
            events.push_back(std::move(j));
        }
    }
    void push_raw(const std::string& line) { in_buf.push(line); }
    json next_line() {
        const auto line = out_buf.next();
        assert(line);
        return json::parse(*line);
    }
    // The run list, as an array.
    json runs() { return call({{"cmd", "runs"}})["result"]; }
    // Snapshot value of the whole run (0 until the first frame).
    unsigned samples() {
        const auto r = call({{"cmd", "snapshot"}, {"window_s", 0}});
        assert(r["ok"]);
        return r["result"]["tree"].is_null() ? 0 : r["result"]["tree"]["value"].get<unsigned>();
    }
};

bool contains(const std::string& s, std::string_view part) { return s.find(part) != std::string::npos; }

void hello_and_runs() {
    Harness h("malgrind_serve_a");
    const auto hello = h.call({{"cmd", "hello"}});
    assert(hello["ok"] && hello["result"]["version"] == MALGRIND_VERSION &&
           hello["result"]["data_dir"] == os::to_utf8(h.root / "runs"));
    assert(hello["result"].contains("pio")); // a path or null
    assert(h.runs() == json::array());
    const auto devs = h.call({{"cmd", "devices"}});
    assert(devs["ok"] && devs["result"]["devices"].is_array() && devs["result"]["can_grant_access"].is_boolean());
    assert(h.call({{"cmd", "ports"}})["ok"] == false);
    assert(h.call({{"cmd", "boards"}})["ok"] == false);
}

void bad_input() {
    Harness h("malgrind_serve_b");
    h.push_raw("not json");
    auto bad = h.next_line();
    assert(bad["ok"] == false && bad["id"].is_null() && contains(bad["error"], "JSON"));
    h.push_raw(R"({"id":7,"cmd":"nope"})");
    auto unknown = h.next_line();
    assert(unknown["id"] == 7 && unknown["ok"] == false && contains(unknown["error"], "unknown command"));
    h.push_raw(R"({"id":8})");
    assert(h.next_line()["ok"] == false); // no cmd
    assert(h.call({{"cmd", "hello"}})["ok"]); // and the loop carries on
    assert(h.call({{"cmd", "open"}, {"run", "../x"}})["ok"] == false); // a run id is never a path
}

void attach_snapshot_stop() {
    Harness h("malgrind_serve_c");
    const auto dev = h.plug("fake-port-c", "usb-10c4-ea60-sn-0001", "ttyUSB0");
    const auto attached = h.call({{"cmd", "attach"}, {"port", dev.path}, {"name", "t"}});
    assert(attached["ok"] && attached["result"]["run"]["name"] == "t");
    assert(attached["result"]["run"]["device_id"] == dev.id && attached["result"]["run"]["port"] == dev.name);
    const std::string id = attached["result"]["run"]["id"];
    assert(h.samples() == 0 && h.call({{"cmd", "functions"}, {"window_s", 0}})["result"]["functions"].empty());
    h.source()->push(make_stream(2)); // 8 samples
    assert(wait_for([&] { return h.samples() == 8; }));
    const auto fn = h.call({{"cmd", "functions"}, {"window_s", 0}, {"focus", "0x400d1fff"}})["result"];
    assert(fn["total"] == 8 && fn["callees"].size() == 2);
    assert(h.call({{"cmd", "snapshot"}, {"window_s", 0}, {"core", 1}})["result"]["tree"]["value"] == 2);

    auto runs = h.runs();
    assert(runs.size() == 1 && runs[0]["id"] == id && runs[0]["live"] == true);
    assert(h.call({{"cmd", "delete"}, {"run", id}})["ok"] == false); // the current run cannot be deleted
    std::this_thread::sleep_for(20ms);
    assert(h.call({{"cmd", "stop"}})["ok"]);
    runs = h.runs();
    assert(runs[0]["duration_ms"].get<unsigned>() > 0 && !runs[0].contains("live") && runs[0]["ncores"] == 2);

    // history: open the stored run, the same picture comes back
    const auto opened = h.call({{"cmd", "open"}, {"run", id}});
    assert(opened["ok"] && h.samples() == 8);
    assert(h.call({{"cmd", "update"}, {"run", id}, {"name", "renamed"}, {"notes", "n"}})["result"]["name"] == "renamed");
    assert(h.call({{"cmd", "open"}, {"run", "missing"}})["ok"] == false);
    assert(h.call({{"cmd", "stop"}})["ok"] && h.samples() == 0); // closing the view
    assert(h.call({{"cmd", "delete"}, {"run", id}})["ok"] && h.runs().empty());
}

void busy_port() {
    Harness h("malgrind_serve_d");
    const auto dev = h.plug("fake-busy", "usb-10c4-ea60-sn-busy", "ttyUSB0");
    h.set_source_factory([](const Device&, unsigned) {
        return os::OpenFailure{os::OpenError::Busy, false, "busy"};
    });
    const auto probe = h.call({{"cmd", "probe"}, {"port", dev.path}});
    assert(probe["ok"] && probe["result"]["busy"] == true && probe["result"]["agent"] == false);
    const auto attach = h.call({{"cmd", "attach"}, {"device", dev.id}, {"name", "x"}});
    assert(attach["ok"] == false && contains(attach["error"], "busy"));
    assert(h.runs().empty()); // a failed attach leaves no run behind
}

void probe_finds_agent() {
    Harness h("malgrind_serve_e");
    const auto dev = h.plug("fake-agent", "usb-10c4-ea60-sn-agent", "ttyUSB0");
    h.set_source_factory([](const Device&, unsigned) {
        auto src = std::make_unique<MemorySource>();
        src->push(make_stream(1));
        src->close();
        return std::variant<std::unique_ptr<ByteSource>, os::OpenFailure>(std::move(src));
    });
    const auto probe = h.call({{"cmd", "probe"}, {"device", dev.id}});
    assert(probe["ok"] && probe["result"]["busy"] == false && probe["result"]["agent"] == true);
    assert(probe["result"]["arch"] == "xtensa" && probe["result"]["ncores"] == 2);
    assert(probe["result"]["family"] == "esp32" && probe["result"]["family_name"] == "ESP32 (ESP-IDF)");
    assert(h.runs().empty()); // probing records nothing

    // Silent board reports empty family.
    h.set_source_factory([](const Device&, unsigned) {
        auto src = std::make_unique<MemorySource>();
        src->close();
        return std::variant<std::unique_ptr<ByteSource>, os::OpenFailure>(std::move(src));
    });
    const auto silent = h.call({{"cmd", "probe"}, {"device", dev.id}});
    assert(silent["ok"] && silent["result"]["agent"] == false);
    assert(silent["result"]["family"] == "" && silent["result"]["family_name"] == "");

    // Agent frame whose arch has no registered family also reports empty family.
    h.set_source_factory([](const Device&, unsigned) {
        auto src = std::make_unique<MemorySource>();
        malgrind::Profiler p({"arm", malgrind::Arch::ArmV7M, 72'000'000, 1}, 997);
        const std::uint32_t pc = 0x08001000;
        p.record(0, &pc, 1, 10);
        std::vector<std::uint8_t> bytes;
        p.flush([&](const std::uint8_t* d, std::size_t n) { bytes.insert(bytes.end(), d, d + n); }, [] {}, 100);
        src->push(bytes);
        src->close();
        return std::variant<std::unique_ptr<ByteSource>, os::OpenFailure>(std::move(src));
    });
    const auto unhandled = h.call({{"cmd", "probe"}, {"device", dev.id}});
    assert(unhandled["ok"] && unhandled["result"]["agent"] == true && unhandled["result"]["arch"] == "armv7m");
    assert(unhandled["result"]["family"] == "" && unhandled["result"]["family_name"] == "");
}

const json& method(const json& result, const char* id) {
    for (const auto& m : result["methods"])
        if (m["id"] == id) return m;
    assert(false && "method missing");
    static const json none;
    return none;
}

// A fake pio in the harness's home, so "no PlatformIO" does not hide the project's own reason.
void fake_pio(Harness& h) {
    const auto pio = os::venv_bin(h.root / "home/.platformio/penv") / os::exe_name("pio");
    fs::create_directories(pio.parent_path());
    fs::copy_file(MALGRIND_FAKE_TOOL, pio, fs::copy_options::overwrite_existing);
}

json add_project(Harness& h, json args) {
    args["cmd"] = "project_add";
    return h.call(std::move(args));
}

void probe_lists_methods() {
    Harness h("malgrind_serve_m");
    const auto dev = h.plug("fake-agent-m", "usb-10c4-ea60-sn-m1", "ttyUSB0");
    h.set_source_factory([](const Device&, unsigned) {
        auto src = std::make_unique<MemorySource>();
        src->push(make_stream(1));
        src->close();
        return std::variant<std::unique_ptr<ByteSource>, os::OpenFailure>(std::move(src));
    });
    const auto with_agent = h.call({{"cmd", "probe"}, {"port", dev.path}});
    assert(method(with_agent["result"], "attach")["available"] == true);
    assert(method(with_agent["result"], "no_flash")["available"] == false);
    assert(contains(method(with_agent["result"], "no_flash")["why"], "debug probe"));

    // a project that cannot take the agent is named as the reason new_run is unavailable (an Arduino project here)
    fake_pio(h);
    const auto proj = h.root / "arduino";
    fs::create_directories(proj);
    std::ofstream(proj / "platformio.ini") << "[env:a]\nplatform = espressif32\nframework = arduino\n";
    h.set_source_factory([](const Device&, unsigned) {
        auto src = std::make_unique<MemorySource>();
        src->close();
        return std::variant<std::unique_ptr<ByteSource>, os::OpenFailure>(std::move(src));
    });
    const auto quiet_dev = h.plug("fake-quiet-m", "usb-10c4-ea60-sn-m2", "ttyUSB1");
    const auto silent = h.call({{"cmd", "probe"}, {"device", quiet_dev.id}, {"project", os::to_utf8(proj)}});
    assert(silent["ok"] && silent["result"]["agent"] == false);
    assert(method(silent["result"], "attach")["available"] == false);
    assert(method(silent["result"], "new_run")["available"] == false && contains(method(silent["result"], "new_run")["why"], "arduino"));

    // No project and no family_hint / agent -> no matching target family for new_run.
    const auto no_proj = h.call({{"cmd", "probe"}, {"device", quiet_dev.id}});
    assert(no_proj["ok"] && method(no_proj["result"], "new_run")["available"] == false);
    assert(contains(method(no_proj["result"], "new_run")["why"], "family"));

    // A non-ESP32 project reports the family mismatch from family_for_project.
    const auto stm_proj = h.root / "stm32";
    fs::create_directories(stm_proj);
    std::ofstream(stm_proj / "platformio.ini") << "[env:a]\nplatform = ststm32\nframework = stm32cube\n";
    const auto added_stm = add_project(h, {{"source", "folder"}, {"dir", os::to_utf8(stm_proj)}});
    assert(added_stm["ok"]);
    const std::string stm_id = added_stm["result"]["id"];
    const auto stm_probe = h.call({{"cmd", "probe"}, {"device", quiet_dev.id}, {"project", stm_id}});
    assert(stm_probe["ok"] && method(stm_probe["result"], "new_run")["available"] == false);
    assert(contains(method(stm_probe["result"], "new_run")["why"], "ststm32"));
}

void remove_agent_restores_or_says_why_not() {
    Harness h("malgrind_serve_n");
    const auto dev = h.plug("fake-port-n", "usb-10c4-ea60-sn-n1", "ttyUSB0");
    const auto bad = h.call({{"cmd", "remove_agent"}, {"device", dev.id}, {"how", "burn"}});
    assert(bad["ok"] == false && contains(bad["error"], "restore"));
    // the default is a restore: without PlatformIO (and so without esptool) the reason is given, not a rebuild
    const auto none = h.call({{"cmd", "remove_agent"}, {"device", dev.id}});
    assert(none["ok"] == false && contains(none["error"], "PlatformIO"));
    assert(h.call({{"cmd", "remove_agent"}, {"device", dev.id}, {"how", "rebuild"}})["ok"] == false); // needs dir, env
}

void projects_of_every_kind() {
    Harness h("malgrind_serve_p");
    fake_pio(h);
    const auto mine = h.root / "my-firmware";
    fs::create_directories(mine);
    std::ofstream(mine / "platformio.ini") << "[env:a]\n[env:b]\n";

    // a folder of the user's own: trusted at once, named after the folder, its environments listed
    const auto folder = add_project(h, {{"source", "folder"}, {"dir", os::to_utf8(mine)}});
    assert(folder["ok"] && folder["result"]["source"] == "folder" && folder["result"]["trusted"] == true);
    assert(folder["result"]["name"] == "my-firmware" && folder["result"]["envs"] == json::array({"a", "b"}) && folder["result"]["problem"] == "");
    assert(add_project(h, {{"source", "folder"}, {"dir", os::to_utf8(h.root / "nothing")}})["ok"] == false);

    // no code: only a name; a file that is not an ELF is refused
    const auto none = add_project(h, {{"source", "none"}, {"name", "Water simulator"}});
    assert(none["ok"] && none["result"]["has_source"] == false && none["result"]["trusted"] == false && none["result"]["envs"].empty());
    std::ofstream(h.root / "not.elf") << "text";
    const auto bad_elf = add_project(h, {{"source", "none"}, {"elf", os::to_utf8(h.root / "not.elf")}});
    assert(bad_elf["ok"] == false && contains(bad_elf["error"], "ELF"));

    // links: only github https, nothing that git would run
    for (const char* url : {"file:///etc", "ext::sh -c id", "https://example.com/o/r", "-oProxyCommand=x"})
        assert(add_project(h, {{"source", "git"}, {"url", url}})["ok"] == false);

    // nothing can be built without source code, or before a cloned project is trusted
    const auto dev = h.plug("not-a-serial", "port-not-serial", "not-serial");
    h.set_source_factory([](const Device&, unsigned) {
        return os::OpenFailure{os::OpenError::Other, false, "not a serial port"};
    });
    const std::string none_id = none["result"]["id"];
    const auto no_source = h.call({{"cmd", "new_run"}, {"project", none_id}, {"device", dev.id}, {"name", "x"}});
    assert(no_source["ok"] == false && contains(no_source["error"], "no source code"));
    const auto probe = h.call({{"cmd", "probe"}, {"device", dev.id}, {"project", none_id}});
    assert(probe["ok"] == false); // not a serial port: the probe says so before it says anything else
    assert(h.call({{"cmd", "new_run"}, {"project", "nope"}, {"device", dev.id}, {"name", "x"}})["ok"] == false);

    // rename, environment, trust; a wrong environment is refused
    const std::string id = folder["result"]["id"];
    assert(h.call({{"cmd", "project_update"}, {"project", id}, {"name", "renamed"}, {"env", "b"}})["result"]["env"] == "b");
    assert(h.call({{"cmd", "project_update"}, {"project", id}, {"env", "zzz"}})["ok"] == false);

    // removing a project never deletes the user's folder
    assert(h.call({{"cmd", "project_remove"}, {"project", id}})["ok"] && fs::exists(mine / "platformio.ini"));
    assert(h.call({{"cmd", "project_remove"}, {"project", id}})["ok"] == false);
    assert(h.call({{"cmd", "projects"}})["result"].size() == 1);
}

void a_cloned_project_must_be_trusted() {
    Harness h("malgrind_serve_q");
    fake_pio(h);
    const auto bin = h.root / "bin";
    fs::create_directories(bin);
    fs::copy_file(MALGRIND_FAKE_TOOL, bin / os::exe_name("git"), fs::copy_options::overwrite_existing);
    os::set_env("PATH", path_with(bin));

    const auto added = add_project(h, {{"source", "git"}, {"url", "https://github.com/o/repo"}});
    assert(added["ok"] && added["result"]["name"] == "repo" && added["result"]["trusted"] == false && added["result"]["envs"] == json::array({"a"}));
    const fs::path cloned_dir = os::from_utf8(added["result"]["dir"].get<std::string>());
    assert(cloned_dir.parent_path().filename() == "firmware" && cloned_dir.filename() == "x");
    assert(added["result"]["url"] == "https://github.com/o/repo.git");
    const std::string id = added["result"]["id"];

    // untrusted: it is not built, and the probe says why
    h.set_source_factory([](const Device&, unsigned) {
        auto src = std::make_unique<MemorySource>();
        src->close();
        return std::variant<std::unique_ptr<ByteSource>, os::OpenFailure>(std::move(src));
    });
    const auto dev = h.plug("fake-port-q", "usb-10c4-ea60-sn-q1", "ttyUSB0");
    const auto refused = h.call({{"cmd", "new_run"}, {"project", id}, {"device", dev.id}, {"name", "x"}});
    assert(refused["ok"] == false && contains(refused["error"], "not been trusted"));
    const auto probe = h.call({{"cmd", "probe"}, {"device", dev.id}, {"project", id}});
    assert(probe["ok"] && method(probe["result"], "new_run")["available"] == false && contains(method(probe["result"], "new_run")["why"], "trusted"));

    // after the user's yes the same project is buildable as far as the checks go
    assert(h.call({{"cmd", "project_update"}, {"project", id}, {"trusted", true}})["result"]["trusted"] == true);
    const auto after = h.call({{"cmd", "probe"}, {"device", dev.id}, {"project", id}});
    assert(method(after["result"], "new_run")["available"] == true);

    // a failed clone leaves no project behind
    os::set_env("FAKE_GIT", "fail");
    const auto failed = add_project(h, {{"source", "git"}, {"url", "https://github.com/o/other"}});
    assert(failed["ok"] == false && h.call({{"cmd", "projects"}})["result"].size() == 1);
}

void runs_belong_to_projects() {
    Harness h("malgrind_serve_r");
    const auto dev = h.plug("fake-port-r1", "usb-10c4-ea60-sn-r1", "ttyUSB0");
    const auto other = h.plug("fake-port-r2", "usb-10c4-ea60-sn-r2", "ttyUSB1");
    const auto none = add_project(h, {{"source", "none"}, {"name", "Water simulator"}});
    const std::string id = none["result"]["id"];
    assert(h.call({{"cmd", "projects"}})["result"][0]["run_count"] == 0 && h.call({{"cmd", "projects"}})["result"][0]["last_run"].is_null());
    const auto run = h.call({{"cmd", "attach"}, {"device", dev.id}, {"name", "a"}, {"project", id}});
    assert(run["ok"] && run["result"]["run"]["project_id"] == id);
    assert(h.call({{"cmd", "attach"}, {"device", other.id}, {"name", "b"}, {"project", "missing"}})["ok"] == false);
    h.call({{"cmd", "stop"}});
    const auto list = h.call({{"cmd", "projects"}})["result"];
    assert(list[0]["run_count"] == 1 && list[0]["last_run"].is_string() && h.runs()[0]["project_id"] == id);
}

void setup_reports_the_machine() {
    Harness h("malgrind_serve_t");
    const auto setup = h.call({{"cmd", "setup"}});
    assert(setup["ok"] && setup["result"]["pio"].is_null() && setup["result"]["can_grant_access"].is_boolean() &&
           setup["result"]["os"].is_string() && !setup["result"]["user"].get<std::string>().empty());
    assert(setup["result"].contains("python") && setup["result"].contains("git"));
    assert(setup["result"]["python_hint"].is_string() && !setup["result"]["python_hint"].get<std::string>().empty());
    fake_pio(h);
    assert(h.call({{"cmd", "setup"}})["result"]["pio"].is_string()); // found in the home's PlatformIO folder now
}

void install_pio_refuses_what_it_cannot_do() {
    Harness h("malgrind_serve_u");
    fake_pio(h); // there is a pio already
    const auto again = h.call({{"cmd", "install_pio"}});
    assert(again["ok"] == false && contains(again["error"], "already installed"));
    fs::remove(os::venv_bin(h.root / "home/.platformio/penv") / os::exe_name("pio"));
    os::set_env("PATH", os::to_utf8(h.root / "nonexistent")); // and no Python to install it with
    const auto nopython = h.call({{"cmd", "install_pio"}});
    assert(nopython["ok"] == false && contains(nopython["error"], "Python 3 was not found"));
}

void devices_tell_access_and_reject_unknown() {
    Harness h("malgrind_serve_v");
    const auto devs = h.call({{"cmd", "devices"}});
    assert(devs["ok"] && devs["result"]["devices"].is_array() && devs["result"]["can_grant_access"].is_boolean());
    for (const auto& d : devs["result"]["devices"]) {
        assert(d.contains("id") && d.contains("kind") && d.contains("path") && d.contains("name") && d.contains("title"));
        assert(d.contains("usb") && d.contains("family_hint") && d.contains("family_hint_name"));
        assert(d["access"]["ok"].is_boolean() && d["access"].contains("group") && d["access"]["in_group"].is_boolean() &&
               d["access"]["fixable"].is_boolean());
    }
    const auto unknown = h.call({{"cmd", "grant_access"}, {"device", "/dev/ttyUSB99"}});
    assert(unknown["ok"] == false && contains(unknown["error"], "is not plugged in"));
    assert(h.call({{"cmd", "grant_access"}, {"port", "/etc/passwd"}})["ok"] == false); // only ports the engine found itself
}

void a_port_without_permission_is_reported_not_opened() {
    Harness h("malgrind_serve_w");
    fake_pio(h);
    const auto dev = h.plug("fake-denied", "usb-10c4-ea60-sn-denied", "ttyUSB0");
    h.set_source_factory([](const Device&, unsigned) {
        return os::OpenFailure{os::OpenError::Denied, false, "permission denied"};
    });
    const auto probe = h.call({{"cmd", "probe"}, {"device", dev.id}});
    assert(probe["ok"] && probe["result"]["denied"] == true && probe["result"]["agent"] == false);
    assert(method(probe["result"], "attach")["available"] == false && contains(method(probe["result"], "attach")["why"], "Grant access"));
    assert(method(probe["result"], "new_run")["available"] == false && contains(method(probe["result"], "new_run")["why"], "Grant access"));
    // opening it for real names the fix too
    const auto attach = h.call({{"cmd", "attach"}, {"port", dev.path}, {"name", "x"}});
    assert(attach["ok"] == false && contains(attach["error"], "no permission") && contains(attach["error"], "Grant access"));
}

// A buildable project for the settings tests: a folder with two environments (the harness's agent files are the repository's).
std::string settings_project(Harness& h) {
    fake_pio(h);
    const auto dir = h.root / "settings-proj";
    fs::create_directories(dir);
    std::ofstream(dir / "platformio.ini") << "[env:a]\n[env:b]\n";
    const auto added = add_project(h, {{"source", "folder"}, {"dir", os::to_utf8(dir)}});
    assert(added["ok"]);
    return added["result"]["id"];
}

void devices_shape_and_id_resolution() {
    Harness h("malgrind_serve_dev");
    const auto proj_id = settings_project(h);

    Device serial;
    serial.id = "usb-10c4-ea60-sn-0001";
    serial.kind = DeviceKind::Serial;
    serial.path = "/dev/ttyUSB0";
    serial.name = "ttyUSB0";
    serial.title = "Silicon Labs CP210x USB-UART";
    serial.usb = os::UsbInfo{"10c4", "ea60", "CP2102", "Silicon Labs", "0001", "1-3"};
    serial.access = os::PortAccess{false, "dialout", false, true};
    serial.family_hint = "esp32";

    Device probe;
    probe.id = "probe-0483-374b-sn-st01";
    probe.kind = DeviceKind::Probe;
    probe.path = "";
    probe.name = "ST-Link";
    probe.title = "ST-Link";
    probe.usb = std::nullopt;
    probe.access = os::PortAccess{};
    probe.family_hint = "";

    {
        std::scoped_lock lock(h.devices_mutex);
        h.override_devices = true;
        h.extra_devices = {serial, probe};
    }

    const auto res = h.call({{"cmd", "devices"}});
    assert(res["ok"]);
    assert(res["result"]["can_grant_access"] == os::can_grant_access());
    const auto& list = res["result"]["devices"];
    assert(list.size() == 2);

    const auto& s = list[0];
    assert(s["id"] == "usb-10c4-ea60-sn-0001");
    assert(s["kind"] == "serial");
    assert(s["path"] == "/dev/ttyUSB0");
    assert(s["name"] == "ttyUSB0");
    assert(s["title"] == "Silicon Labs CP210x USB-UART");
    assert(s["usb"]["vid"] == "10c4" && s["usb"]["pid"] == "ea60");
    assert(s["usb"]["product"] == "CP2102" && s["usb"]["manufacturer"] == "Silicon Labs");
    assert(s["usb"]["serial"] == "0001" && s["usb"]["location"] == "1-3");
    assert(s["access"]["ok"] == false && s["access"]["group"] == "dialout" &&
           s["access"]["in_group"] == false && s["access"]["fixable"] == true);
    assert(s["family_hint"] == "esp32" && s["family_hint_name"] == "ESP32 (ESP-IDF)");

    const auto& p = list[1];
    assert(p["id"] == "probe-0483-374b-sn-st01");
    assert(p["kind"] == "probe");
    assert(p["path"] == "");
    assert(p["name"] == "ST-Link");
    assert(p["title"] == "ST-Link");
    assert(p["usb"].is_null());
    assert(p["access"]["ok"] == true && p["access"]["group"] == "" &&
           p["access"]["in_group"] == false && p["access"]["fixable"] == false);
    assert(p["family_hint"] == "" && p["family_hint_name"] == "");

    // Unknown device returns exact message.
    const auto missing = h.call({{"cmd", "attach"}, {"device", "usb-missing"}, {"name", "x"}});
    assert(missing["ok"] == false && missing["error"] == "device usb-missing is not plugged in");

    // Missing device/port argument.
    const auto no_arg = h.call({{"cmd", "attach"}, {"name", "x"}});
    assert(no_arg["ok"] == false && contains(no_arg["error"], "missing argument: device"));

    // Probe device is rejected by commands requiring a serial port.
    for (const json& req : {json{{"cmd", "attach"}, {"device", probe.id}, {"name", "x"}},
                            json{{"cmd", "probe"}, {"device", probe.id}},
                            json{{"cmd", "grant_access"}, {"device", probe.id}},
                            json{{"cmd", "chip_id"}, {"device", probe.id}},
                            json{{"cmd", "remove_agent"}, {"device", probe.id}},
                            json{{"cmd", "run_plan"}, {"project", proj_id}, {"device", probe.id}}}) {
        const auto err = h.call(req);
        assert(err["ok"] == false && contains(err["error"], "not a serial port"));
    }

    // attach {device: "<id>"} resolves device id to MemorySource and saves device_id + port on RunMeta.
    const auto attached = h.call({{"cmd", "attach"}, {"device", serial.id}, {"name", "by-id"}});
    assert(attached["ok"]);
    assert(attached["result"]["run"]["device_id"] == serial.id);
    assert(attached["result"]["run"]["port"] == "ttyUSB0");
    h.source()->push(make_stream(1));
    assert(wait_for([&] { return h.samples() == 4; }));
    assert(h.call({{"cmd", "stop"}})["ok"]);
    const auto runs = h.runs();
    assert(runs.size() == 1 && runs[0]["device_id"] == serial.id && runs[0]["port"] == "ttyUSB0");
    RunStore store(h.root / "runs");
    const auto stored = store.list().at(0);
    assert(stored.device_id == serial.id && stored.port == "ttyUSB0");
}

void run_plan_shows_what_was_detected_and_what_would_run() {
    Harness h("malgrind_serve_x");
    const auto dev = h.plug("fake-port-x", "usb-10c4-ea60-sn-x1", "ttyUSB0");
    const auto id = settings_project(h);
    const auto plan = h.call({{"cmd", "run_plan"}, {"project", id}, {"device", dev.id}, {"mode", "new_run"}})["result"];
    assert(plan["detected"]["env"] == "a" && plan["detected"]["rate_hz"] == 997 && plan["envs"] == json::array({"a", "b"}));
    assert(plan["last"].is_null() && plan["error"] == "");
    assert(plan["elf_hint"] == os::to_utf8(fs::absolute(h.root / "settings-proj/.pio/malgrind/build/a/firmware.elf").lexically_normal()));
    const std::string build = plan["commands"]["build"], upload = plan["commands"]["upload"];
    assert(json(build) == plan["detected_commands"]["build"] && json(upload) == plan["detected_commands"]["upload"]); // no settings: the detected commands
    assert(contains(build, " run -d ") && !contains(build, "upload") && contains(upload, "-t upload --upload-port " + dev.path));
    assert(plan["commands"]["env"].is_array() && !plan["commands"]["env"].empty()); // the agent's environment is shown too

    // the panel's current fields change the preview, from the same function the run uses
    const auto edited = h.call({{"cmd", "run_plan"}, {"project", id}, {"device", dev.id},
                                {"settings", {{"env", "b"}, {"extra_args", "--verbose"}, {"rate_hz", 500}}}})["result"];
    assert(contains(edited["commands"]["build"], " -e b --verbose") && contains(edited["commands"]["upload"], "--verbose"));

    // a field that does not hold up is reported next to the commands, so the panel can show it while it is being edited
    const auto bad = h.call({{"cmd", "run_plan"}, {"project", id}, {"port", dev.path}, {"settings", {{"rate_hz", 9}}}});
    assert(bad["ok"] && contains(bad["result"]["error"], "between 10 and 5000") && bad["result"]["commands"].is_null());
    assert(!fs::exists(h.root / "settings-proj/.pio")); // a preview writes nothing into the project

    // attach only needs the ELF
    const auto attach = h.call({{"cmd", "run_plan"}, {"project", id}, {"device", dev.id}, {"mode", "attach"}})["result"];
    assert(attach["commands"].is_null() && attach["detected"].contains("elf"));
}

void new_run_checks_every_setting_before_the_board_is_touched() {
    Harness h("malgrind_serve_y");
    const auto dev = h.plug("fake-port-y", "usb-10c4-ea60-sn-y1", "ttyUSB0");
    const auto id = settings_project(h);
    for (const json& bad : {json{{"rate_hz", 9}}, json{{"env", "zzz"}}, json{{"build_command", "no-such-program-xyz"}},
                            json{{"elf", os::to_utf8(h.root / "no-such.elf")}}}) {
        const auto r = h.call({{"cmd", "new_run"}, {"project", id}, {"device", dev.id}, {"name", "x"}, {"settings", bad}});
        assert(r["ok"] == false);
    }
    assert(h.runs().empty() && !fs::exists(h.root / "settings-proj/.pio")); // nothing started, nothing written
}

void a_run_keeps_its_settings_and_the_project_offers_them_again() {
    Harness h("malgrind_serve_z");
    const auto dev = h.plug("fake-port-z", "usb-10c4-ea60-sn-z1", "ttyUSB0");
    const auto usb9 = h.plug("fake-port-9", "usb-10c4-ea60-sn-usb9", "ttyUSB9");
    const auto id = settings_project(h);
    const auto missing = h.call({{"cmd", "attach"}, {"device", dev.id}, {"name", "a"}, {"project", id},
                                 {"settings", {{"elf", os::to_utf8(h.root / "no-such.elf")}}}});
    assert(missing["ok"] == false && contains(missing["error"], "ELF does not exist") && h.runs().empty());

    const json used = {
        {"rate_hz", 700},
        {"extra_args", "-v"},
        {"upload_command", join_command({MALGRIND_FAKE_TOOL, "upload", "--port", "fake-port-7"})},
        {"build_command", join_command({MALGRIND_FAKE_TOOL, "build", "-e", "zzz"})},
        {"elf", os::to_utf8(os::from_utf8(MALGRIND_FAKE_TOOL))},
    };
    const auto run = h.call({{"cmd", "attach"}, {"device", dev.id}, {"name", "a"}, {"project", id}, {"settings", used}});
    assert(run["ok"] && run["result"]["run"]["settings"]["rate_hz"] == 700);
    h.call({{"cmd", "stop"}});
    assert(h.runs()[0]["settings"]["extra_args"] == "-v" && h.runs()[0]["settings"]["upload_command"] == used["upload_command"]); // the run keeps ALL of it
    // ...but only the board-independent part is offered again: a remembered upload line would flash the board it was typed for
    // (its --upload-port), a remembered build line its environment, a remembered ELF would label a new build with the old functions
    const auto plan = h.call({{"cmd", "run_plan"}, {"project", id}, {"device", usb9.id}})["result"];
    assert(plan["last"]["rate_hz"] == 700 && plan["last"]["extra_args"] == "-v");
    assert(plan["last"]["upload_command"] == "" && plan["last"]["build_command"] == "" && plan["last"]["elf"] == "");
    assert(contains(plan["commands"]["upload"], "--upload-port " + usb9.path)); // on another board the line names that board
    assert(h.call({{"cmd", "projects"}})["result"][0]["last_settings"]["rate_hz"] == 700);

    // a typed upload line that names no port is allowed, with a warning; one that names the chosen port is not warned about
    const auto typed = [&](const std::string& line) {
        return h.call({{"cmd", "run_plan"}, {"project", id}, {"device", usb9.id}, {"settings", {{"upload_command", line}}}})["result"];
    };
    const std::string no_port_cmd = join_command({MALGRIND_FAKE_TOOL, "upload"});
    const std::string with_port_cmd = join_command({MALGRIND_FAKE_TOOL, "upload", "--port", usb9.path});
    assert(contains(typed(no_port_cmd)["warning"], usb9.path) && typed(no_port_cmd)["error"] == "");
    assert(typed(with_port_cmd)["warning"] == "" && plan["warning"] == "");
}

void open_while_live_refused() {
    Harness h("malgrind_serve_f");
    const auto dev = h.plug("fake-port-f", "usb-10c4-ea60-sn-f1", "ttyUSB0");
    const auto id = h.call({{"cmd", "attach"}, {"device", dev.id}, {"name", "a"}})["result"]["run"]["id"];
    const auto second = h.call({{"cmd", "attach"}, {"device", dev.id}, {"name", "b"}});
    assert(second["ok"] == false);
    const auto open = h.call({{"cmd", "open"}, {"run", id}});
    assert(open["ok"] == false && contains(open["error"], "stop the live run first"));
}

void eof_finishes_run() {
    Harness h("malgrind_serve_g");
    const auto dev = h.plug("fake-port-g", "usb-10c4-ea60-sn-g1", "ttyUSB0");
    h.call({{"cmd", "attach"}, {"device", dev.id}, {"name", "a"}});
    h.source()->push(make_stream(1));
    assert(wait_for([&] { return h.samples() == 4; }));
    std::this_thread::sleep_for(20ms);
    h.in_buf.close(); // the app window closed
    h.thread.join();
    assert(h.exit_code == 0);
    RunStore store(h.root / "runs");
    assert(store.list().size() == 1 && store.list()[0].duration_ms > 0);
}

void open_after_upload_reenumerates() {
    int calls = 0;
    int open_calls = 0;
    MemorySource* raw = nullptr;
    auto source = open_after_upload(
        "usb-1a86-7523-sn-99",
        [&]() -> std::vector<Device> {
            if (++calls == 1) return {}; // USB device still re-enumerating after reset
            Device d;
            d.id = "usb-1a86-7523-sn-99";
            d.kind = DeviceKind::Serial;
            d.path = "fake-acm1";
            d.name = "ttyACM1";
            return {d};
        },
        [&](const Device& d, unsigned) -> std::variant<std::unique_ptr<ByteSource>, os::OpenFailure> {
            if (++open_calls == 1) return os::OpenFailure{os::OpenError::NotFound, true, "re-enumerating"};
            auto mem = std::make_unique<MemorySource>(d.name);
            raw = mem.get();
            return std::unique_ptr<ByteSource>(std::move(mem));
        });
    assert(calls == 3 && open_calls == 2 && raw != nullptr);
    raw->push(make_stream(1));
    std::array<std::uint8_t, 64> buf{};
    const auto n = source->read(buf, 500);
    assert(n && *n > 0);
}

#ifndef _WIN32
void pty_serial_attach_and_busy() {
    Harness h("malgrind_serve_pty", /*real_serial=*/true);
    Pty pty;
    const auto dev = h.plug(pty.slave, "usb-10c4-ea60-sn-pty", "ttyPty0");
    const auto attached = h.call({{"cmd", "attach"}, {"device", dev.id}, {"name", "pty"}});
    assert(attached["ok"] && attached["result"]["run"]["device_id"] == dev.id);
    pty.send(make_stream(1)); // 4 samples
    assert(wait_for([&] { return h.samples() == 4; }));
    assert(h.call({{"cmd", "stop"}})["ok"]);

    if (!exclusive_open_is_enforced()) {
        std::puts("pty_serial_attach_and_busy: busy check skipped, this platform does not enforce exclusive open on a pty");
        return;
    }
    Pty held;
    const auto held_dev = h.plug(held.slave);
    auto other_program = open_source(held.slave);
    assert(other_program);
    const auto probe = h.call({{"cmd", "probe"}, {"device", held_dev.id}});
    assert(probe["ok"] && probe["result"]["busy"] == true && probe["result"]["agent"] == false);
    const auto busy_attach = h.call({{"cmd", "attach"}, {"device", held_dev.id}, {"name", "busy"}});
    assert(busy_attach["ok"] == false && contains(busy_attach["error"], "busy"));
}
#endif

} // namespace

int main() {
    hello_and_runs();
    bad_input();
    attach_snapshot_stop();
    busy_port();
    probe_finds_agent();
    probe_lists_methods();
    projects_of_every_kind();
    a_cloned_project_must_be_trusted();
    runs_belong_to_projects();
    devices_shape_and_id_resolution();
    setup_reports_the_machine();
    install_pio_refuses_what_it_cannot_do();
    run_plan_shows_what_was_detected_and_what_would_run();
    new_run_checks_every_setting_before_the_board_is_touched();
    a_run_keeps_its_settings_and_the_project_offers_them_again();
    devices_tell_access_and_reject_unknown();
    a_port_without_permission_is_reported_not_opened();
    remove_agent_restores_or_says_why_not();
    open_while_live_refused();
    eof_finishes_run();
    open_after_upload_reenumerates();
#ifndef _WIN32
    pty_serial_attach_and_busy();
#endif
    std::puts("serve ok");
}
