#pragma once

// One run as the rest of the program sees it: a stream of bytes from the board (or from a stored run) that is
// decoded, symbolized and aggregated. A live session also records every byte into the run store.

#include "aggregate.hpp"
#include "source.hpp"
#include "store.hpp"
#include "symbols.hpp"
#include "wire.hpp"

#include <nlohmann/json_fwd.hpp>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace malgrind::host {

// Receives {"event": ...} objects: log (text that is not a frame), target, notice, ended. May be called from
// the session's reader thread, so it must be thread-safe.
using Emit = std::function<void(const nlohmann::json& event)>;

class Session {
public:
    // Reads `source` on a thread of its own until it ends or the session is destroyed. `elf` empty: look for the
    // firmware ELF under `project` when the first frame names the architecture; with no project, names stay raw addresses.
    [[nodiscard]] static std::unique_ptr<Session> live(std::unique_ptr<ByteSource> source, RunWriter writer,
                                                       std::filesystem::path elf, std::filesystem::path project,
                                                       Emit emit);
    // A stored run, fully loaded before this returns. nullptr if there is no such run.
    [[nodiscard]] static std::unique_ptr<Session> replay(const RunStore& store, std::string_view id, Emit emit);

    ~Session(); // live: stops the thread and closes the run (writer.finish)
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    [[nodiscard]] bool live() const { return live_; } // false once the board stopped sending, and for replays
    [[nodiscard]] std::string run_id() const { return run_id_; }

    // nullopt until the first frame has arrived.
    [[nodiscard]] std::optional<std::string> snapshot(std::optional<unsigned> core, Aggregator::Ms window,
                                                      std::optional<Aggregator::Ms> at) const;
    [[nodiscard]] std::optional<std::string> functions(std::optional<unsigned> core, Aggregator::Ms window,
                                                       std::optional<Aggregator::Ms> at, std::string_view focus) const;

    // Folded stacks ("core0;main;f;g 12" per line), the input of flamegraph.pl and speedscope. nullopt before the first frame.
    [[nodiscard]] std::optional<std::string> folded() const;

private:
    Session(Emit emit, std::filesystem::path elf, std::filesystem::path project, bool search_elf);
    void on_bytes(std::span<const std::uint8_t> bytes, Aggregator::Ms t); // decode, then aggregate
    void setup(const Frame& first); // symbols and aggregator, once the first frame names the target
    void notice(std::string text) const;
    // Runs fn on the writer. A failing disk (full, run deleted) ends recording with a notice; the live view goes on.
    void record(const std::function<void(RunWriter&)>& fn);
    void read_loop(std::unique_ptr<ByteSource> source);

    Emit emit_;
    std::filesystem::path elf_, project_;
    bool search_elf_;
    std::string run_id_;
    std::optional<RunWriter> writer_; // only touched by the reader thread (and by replay before it returns)
    bool recording_ = true;
    FrameReader reader_;
    std::optional<Symbolizer> symbols_;
    std::optional<Aggregator> aggregator_; // after symbols_: destroyed first
    std::atomic<Aggregator*> ready_{nullptr}; // set once setup() is complete; queries from other threads use it
    std::atomic<bool> live_{false}, stop_{false};
    std::thread thread_; // last: joined before anything it uses goes away
};

} // namespace malgrind::host
