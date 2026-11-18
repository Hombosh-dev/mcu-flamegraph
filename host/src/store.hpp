#pragma once

// Run history on disk. A run is a directory <root>/<id>/ holding
//   meta.json     RunMeta, rewritten atomically
//   frames.mlg    the raw serial reads, as records  t_ms u32 LE | n u16 LE | bytes[n]  (a torn last record is ignored)
//   firmware.elf  a copy of the ELF the run was symbolized with, so old runs stay readable after a rebuild
// Replaying a run means feeding frames.mlg to the same FrameReader and Aggregator a live run uses.

#include "malgrind/target_info.hpp"
#include "settings.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace malgrind::host {

// The current UTC time through strftime, e.g. utc_now("%Y%m%d-%H%M%S"). Also names the flash backups.
[[nodiscard]] std::string utc_now(const char* fmt);

struct RunMeta {
    std::string id{}, name{}, notes{}, project{}, env{}, port{}, device_id{};
    std::string project_id{}; // the malgrind project (projects.hpp) this run belongs to; empty for runs made without one
    std::string started{}; // ISO-8601 UTC, "2026-11-15T20:01:02Z"
    std::optional<Arch> arch{};
    unsigned ncores = 0;
    double cpu_mhz = 0;
    std::uint64_t duration_ms = 0;
    bool has_elf = false;
    std::optional<RunSettings> settings{}; // what the run was made with; absent for runs made before settings existed
};

void to_json(nlohmann::json& j, const RunMeta& m); // arch as its name, or null

struct Chunk {
    std::uint32_t t_ms;
    std::vector<std::uint8_t> bytes;
};

// Writes one run while it is live. Every call is on disk when it returns; the duration in meta.json is refreshed
// at least once a second, so a killed process leaves a run with (almost) the right length.
class RunWriter {
public:
    void append(std::uint32_t t_ms, std::span<const std::uint8_t> bytes);
    void set_target(Arch arch, unsigned ncores, double cpu_mhz);
    void set_elf(const std::filesystem::path& elf); // copies it into the run directory
    void finish(std::uint64_t duration_ms);
    [[nodiscard]] const RunMeta& meta() const { return meta_; }

private:
    friend class RunStore;
    RunWriter(std::filesystem::path dir, RunMeta meta);
    void save() const;

    std::filesystem::path dir_;
    RunMeta meta_;
    std::ofstream frames_;
    std::uint32_t last_saved_ms_ = 0;
};

class RunStore {
public:
    explicit RunStore(std::filesystem::path root); // created if missing
    // Per-user data directory (os::user_data_dir()) + "/malgrind/runs".
    [[nodiscard]] static std::filesystem::path default_root();

    [[nodiscard]] RunWriter create(RunMeta meta); // assigns id ("yyyymmdd-hhmmss", "-2"... on a clash) and `started`
    [[nodiscard]] std::vector<RunMeta> list() const; // newest first
    [[nodiscard]] std::optional<RunMeta> get(std::string_view id) const;
    [[nodiscard]] std::vector<Chunk> chunks(std::string_view id) const;
    [[nodiscard]] std::filesystem::path elf(std::string_view id) const; // empty if the run has none
    std::optional<RunMeta> update(std::string_view id, std::optional<std::string> name, std::optional<std::string> notes);
    bool remove(std::string_view id); // false if there is no such run

private:
    [[nodiscard]] std::optional<std::filesystem::path> dir_of(std::string_view id) const; // nullopt for a malformed id

    std::filesystem::path root_;
};

} // namespace malgrind::host
