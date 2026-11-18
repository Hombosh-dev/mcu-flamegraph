#include "session.hpp"

#include "discover.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <format>

namespace fs = std::filesystem;

namespace malgrind::host {

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
} // namespace

Session::Session(Emit emit, fs::path elf, fs::path project, bool search_elf)
    : emit_(std::move(emit)), elf_(std::move(elf)), project_(std::move(project)), search_elf_(search_elf) {}

std::unique_ptr<Session> Session::live(std::unique_ptr<ByteSource> source, RunWriter writer, fs::path elf,
                                       fs::path project, Emit emit) {
    std::unique_ptr<Session> s(new Session(std::move(emit), std::move(elf), std::move(project), true));
    s->run_id_ = writer.meta().id;
    s->writer_.emplace(std::move(writer));
    s->live_ = true;
    s->thread_ = std::thread([raw = s.get(), src = std::move(source)]() mutable { raw->read_loop(std::move(src)); });
    return s;
}

std::unique_ptr<Session> Session::replay(const RunStore& store, std::string_view id, Emit emit) {
    const auto meta = store.get(id);
    if (!meta) return nullptr;
    std::unique_ptr<Session> s(new Session(std::move(emit), store.elf(id), {}, false));
    s->run_id_ = meta->id;
    for (const auto& chunk : store.chunks(id)) s->on_bytes(chunk.bytes, Aggregator::Ms{chunk.t_ms});
    return s;
}

Session::~Session() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void Session::notice(std::string text) const { emit_({{"event", "notice"}, {"text", std::move(text)}}); }

void Session::record(const std::function<void(RunWriter&)>& fn) {
    if (!writer_ || !recording_) return;
    try {
        fn(*writer_);
    } catch (const std::exception& e) {
        recording_ = false;
        notice(std::format("cannot record this run any more: {}", e.what()));
    }
}

void Session::read_loop(std::unique_ptr<ByteSource> source) {
    const auto start = Clock::now();
    auto elapsed = [&] { return std::chrono::duration_cast<Aggregator::Ms>(Clock::now() - start); };
    std::array<std::uint8_t, 4096> buf;
    bool hinted = false;
    bool ended = false;
    while (!stop_) {
        const auto n = source ? source->read(buf) : std::nullopt;
        if (!n) {
            ended = true;
            break;
        }
        if (*n == 0) {
            if (!ready_ && !hinted && elapsed() > 5s) {
                hinted = true;
                notice("no malgrind frames yet: is the agent running on the board, and is the baud rate right?");
            }
            continue;
        }
        const auto t = elapsed();
        const std::span bytes(buf.data(), *n);
        record([&](RunWriter& w) { w.append(static_cast<std::uint32_t>(t.count()), bytes); }); // on disk before it is decoded
        on_bytes(bytes, t);
    }
    record([&](RunWriter& w) { w.finish(static_cast<std::uint64_t>(elapsed().count())); });
    live_ = false;
    if (ended) emit_({{"event", "ended"}, {"run", run_id_}, {"reason", "eof"}});
}

void Session::on_bytes(std::span<const std::uint8_t> bytes, Aggregator::Ms t) {
    auto [frames, text] = reader_.feed(bytes);
    if (!text.empty()) emit_({{"event", "log"}, {"text", std::move(text)}});
    for (auto& frame : frames) {
        if (!ready_) setup(frame);
        ready_.load()->add(std::move(frame), t);
    }
}

void Session::setup(const Frame& first) {
    fs::path elf = elf_;
    // Only inside a project the caller named: the working directory of a GUI app is arbitrary, and an ELF of another
    // firmware would label every address with a wrong function.
    if (elf.empty() && search_elf_ && !project_.empty()) {
        if (const auto found = find_elf(first.arch, project_, project_)) elf = *found;
    }
    if (elf.empty()) {
        notice(std::format("no {} firmware ELF found: functions show as raw addresses", arch_name(first.arch)));
    }
    const auto tool = find_addr2line(first.arch);
    if (!elf.empty() && tool.empty())
        notice(std::format("no addr2line found for {}: functions show as raw addresses", arch_name(first.arch)));
    symbols_.emplace(elf, tool);
    aggregator_.emplace(*symbols_);
    record([&](RunWriter& w) {
        w.set_target(first.arch, first.ncores, first.cpu_hz / 1e6);
        if (!elf.empty() && search_elf_) w.set_elf(elf);
    });
    emit_({{"event", "target"},
           {"arch", std::string(arch_name(first.arch))},
           {"ncores", first.ncores},
           {"cpu_mhz", first.cpu_hz / 1e6},
           {"elf", elf.empty() ? nlohmann::json(nullptr) : nlohmann::json(elf.string())}});
    ready_ = &*aggregator_; // last: queries from other threads only see a complete setup
}

std::optional<std::string> Session::snapshot(std::optional<unsigned> core, Aggregator::Ms window,
                                             std::optional<Aggregator::Ms> at) const {
    const auto* agg = ready_.load();
    return agg ? std::optional(agg->snapshot_json(core, window, at)) : std::nullopt;
}

std::optional<std::string> Session::functions(std::optional<unsigned> core, Aggregator::Ms window,
                                              std::optional<Aggregator::Ms> at, std::string_view focus) const {
    const auto* agg = ready_.load();
    return agg ? std::optional(agg->functions_json(core, window, at, focus)) : std::nullopt;
}

std::optional<std::string> Session::folded() const {
    const auto* agg = ready_.load();
    return agg ? std::optional(agg->folded()) : std::nullopt;
}

} // namespace malgrind::host
