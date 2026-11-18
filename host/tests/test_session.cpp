// Session self-check: a live run over MemorySource (plus one POSIX pty check), its recording and its replay.
#include "os/paths.hpp"
#include "session.hpp"
#include "source.hpp"
#include "store.hpp"
#include "stream.hpp"

#ifndef _WIN32
#include "pty.hpp"
#endif

#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

using namespace malgrind;
using namespace malgrind::host;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct Events {
    std::mutex mutex;
    std::vector<json> list;
    Emit sink() { return [this](const json& e) { std::scoped_lock lock(mutex); list.push_back(e); }; }
    [[nodiscard]] std::vector<json> named(const char* name) {
        std::scoped_lock lock(mutex);
        std::vector<json> out;
        for (const auto& e : list) if (e["event"] == name) out.push_back(e);
        return out;
    }
    [[nodiscard]] std::string logs() {
        std::string all;
        for (const auto& e : named("log")) all += e["text"].get<std::string>();
        return all;
    }
};

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

fs::path fresh(const char* name) {
    const auto p = fs::temp_directory_path() / os::from_utf8(name);
    fs::remove_all(p);
    return p;
}

constexpr auto kAll = Aggregator::Ms{0};

struct LiveMemory {
    MemorySource* source = nullptr;
    std::unique_ptr<Session> session;
};

// 2 rounds = 4 frames: core 0 sends 3 samples per round, core 1 one, so 8 samples in all.
LiveMemory start_memory(RunStore& store, Events& ev, const fs::path& project) {
    auto source = std::make_unique<MemorySource>();
    auto* raw = source.get();
    auto session = Session::live(std::move(source), store.create({.name = "t"}), {}, project, ev.sink());
    return {raw, std::move(session)};
}

void live_records_and_aggregates() {
    const auto root = fresh("malgrind_session_a");
    RunStore store(root / "runs");
    Events ev;
    auto [src, session] = start_memory(store, ev, root / "project");
    assert(session->live() && !session->snapshot(std::nullopt, kAll, std::nullopt)); // nothing yet
    src->push(make_stream(2));
    assert(wait_for([&] { auto s = session->snapshot(std::nullopt, kAll, std::nullopt); return s && contains(*s, R"("name":"all","value":8)"); }));
    const auto targets = ev.named("target");
    assert(targets.size() == 1 && targets[0]["ncores"] == 2 && targets[0]["arch"] == "xtensa");
    assert(session->snapshot(1u, kAll, std::nullopt)->find(R"("name":"all","value":2)") != std::string::npos);
    session.reset();
    fs::remove_all(root);
}

void ended_on_eof() {
    const auto root = fresh("malgrind_session_b");
    RunStore store(root / "runs");
    Events ev;
    auto [src, session] = start_memory(store, ev, root / "project");
    src->push(make_stream(1));
    assert(wait_for([&] { return session->snapshot(std::nullopt, kAll, std::nullopt).has_value(); }));
    std::this_thread::sleep_for(20ms);
    src->close(); // the board is unplugged; Session's reader thread destroys source on EOF
    src = nullptr;
    assert(wait_for([&] { return !ev.named("ended").empty(); }, 1000ms));
    assert(!session->live() && ev.named("ended")[0]["run"] == session->run_id());
    session.reset();
    assert(store.list().size() == 1 && store.list()[0].duration_ms > 0 && store.list()[0].ncores == 2);
    fs::remove_all(root);
}

void no_elf_raw_addresses() {
    const auto root = fresh("malgrind_session_c");
    fs::create_directories(root / "project");
    RunStore store(root / "runs");
    Events ev;
    auto [src, session] = start_memory(store, ev, root / "project");
    src->push(make_stream(1));
    assert(wait_for([&] { return session->snapshot(std::nullopt, kAll, std::nullopt).has_value(); }));
    assert(contains(*session->snapshot(std::nullopt, kAll, std::nullopt), R"("name":"0x400d2fff")"));
    const auto notices = ev.named("notice");
    assert(!notices.empty() && contains(notices[0]["text"], "ELF"));
    assert(!store.list()[0].has_elf);
    session.reset();
    fs::remove_all(root);
}

void replay_matches_live() {
    const auto root = fresh("malgrind_session_d");
    RunStore store(root / "runs");
    std::string live_view, id;
    {
        Events ev;
        auto [src, session] = start_memory(store, ev, root / "project");
        src->push(make_stream(3));
        assert(wait_for([&] { auto s = session->snapshot(std::nullopt, kAll, std::nullopt); return s && contains(*s, R"("name":"all","value":12)"); }));
        live_view = *session->snapshot(std::nullopt, kAll, std::nullopt);
        id = session->run_id();
    }
    Events ev;
    const auto replay = Session::replay(store, id, ev.sink());
    assert(replay && !replay->live());
    assert(replay->snapshot(std::nullopt, kAll, std::nullopt) == live_view);
    assert(!Session::replay(store, "missing", ev.sink()));
    fs::remove_all(root);
}

void log_passthrough() {
    const auto root = fresh("malgrind_session_e");
    RunStore store(root / "runs");
    Events ev;
    auto [src, session] = start_memory(store, ev, root / "project");
    src->push(std::string_view("I (1) app: hi\n"));
    assert(wait_for([&] { return contains(ev.logs(), "I (1) app: hi\n"); }));
    session.reset();
    fs::remove_all(root);
}

void survives_a_broken_disk() {
    const auto root = fresh("malgrind_session_f");
    RunStore store(root / "runs");
    Events ev;
    auto [src, session] = start_memory(store, ev, root / "project");
    // Replace meta.json with a non-empty directory so RunWriter::save()'s rename fails even on Windows (where frames.mlg is open).
    const auto run_dir = root / "runs" / os::from_utf8(session->run_id());
    fs::remove(run_dir / "meta.json");
    fs::create_directories(run_dir / "meta.json/block");
    src->push(make_stream(1));
    assert(wait_for([&] { return session->snapshot(std::nullopt, kAll, std::nullopt).has_value(); })); // still shown live
    assert(wait_for([&] {
        for (const auto& n : ev.named("notice"))
            if (contains(n["text"], "cannot record")) return true;
        return false;
    }));
    session.reset(); // and closing does not throw either
    fs::remove_all(root);
}

#ifndef _WIN32
void pty_serial_records_live() {
    const auto root = fresh("malgrind_session_pty");
    RunStore store(root / "runs");
    Pty pty;
    Events ev;
    auto source = SerialSource::open_path(pty.slave, 921600);
    assert(std::holds_alternative<std::unique_ptr<SerialSource>>(source));
    auto session = Session::live(std::get<std::unique_ptr<SerialSource>>(std::move(source)),
                                 store.create({.name = "pty"}), {}, root / "project", ev.sink());
    pty.send(make_stream(2));
    assert(wait_for([&] { auto s = session->snapshot(std::nullopt, kAll, std::nullopt); return s && contains(*s, R"("name":"all","value":8)"); }));
    std::this_thread::sleep_for(20ms);
    pty.hang_up();
    assert(wait_for([&] { return !ev.named("ended").empty(); }, 1000ms));
    assert(!session->live());
    session.reset();
    assert(store.list().size() == 1 && store.list()[0].duration_ms > 0 && store.list()[0].ncores == 2);
    fs::remove_all(root);
}
#endif

} // namespace

int main() {
    live_records_and_aggregates();
    ended_on_eof();
    no_elf_raw_addresses();
    replay_matches_live();
    log_passthrough();
    survives_a_broken_disk();
#ifndef _WIN32
    pty_serial_records_live();
#endif
    std::puts("session ok");
}
