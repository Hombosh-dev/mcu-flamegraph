#include "store.hpp"

#include "os/paths.hpp"
#include "wire.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <format>
#include <iterator>
#include <tuple>

namespace fs = std::filesystem;

namespace malgrind::host {

namespace {

constexpr std::array kArchs{Arch::Host, Arch::Xtensa, Arch::ArmV7M, Arch::RiscV32};

bool valid_id(std::string_view id) {
    return !id.empty() && id.size() <= 40 &&
           std::ranges::all_of(id, [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; });
}

// meta.json is replaced atomically: a crash leaves the old file or the new one, never half of one.
void write_meta(const fs::path& dir, const RunMeta& meta) {
    const auto tmp = dir / "meta.json.tmp";
    std::ofstream(tmp, std::ios::trunc) << nlohmann::json(meta).dump(2);
    fs::rename(tmp, dir / "meta.json");
}

std::optional<RunMeta> read_meta(const fs::path& dir) {
    std::ifstream in(dir / "meta.json");
    const auto j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_object()) return std::nullopt;
    RunMeta m;
    m.id = j.value("id", os::to_utf8(dir.filename()));
    m.name = j.value("name", "");
    m.notes = j.value("notes", "");
    m.project = j.value("project", "");
    m.env = j.value("env", "");
    m.project_id = j.value("project_id", "");
    m.port = j.value("port", "");
    m.device_id = j.value("device_id", "");
    m.started = j.value("started", "");
    if (j.contains("arch") && j["arch"].is_string())
        for (const auto a : kArchs)
            if (arch_name(a) == j["arch"].get<std::string>()) m.arch = a;
    m.ncores = j.value("ncores", 0u);
    m.cpu_mhz = j.value("cpu_mhz", 0.0);
    m.duration_ms = j.value("duration_ms", std::uint64_t{0});
    m.has_elf = fs::exists(dir / "firmware.elf");
    if (j.contains("settings") && j["settings"].is_object()) m.settings = j["settings"].get<RunSettings>();
    return m;
}

void put_le(std::ofstream& out, std::uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) out.put(static_cast<char>((v >> (8 * i)) & 0xff));
}

} // namespace

std::string utc_now(const char* fmt) {
    const std::tm tm = os::utc_tm(std::time(nullptr));
    std::array<char, 32> buf{};
    std::strftime(buf.data(), buf.size(), fmt, &tm);
    return buf.data();
}

void to_json(nlohmann::json& j, const RunMeta& m) {
    j = {{"id", m.id},           {"name", m.name},       {"notes", m.notes},
         {"project", m.project}, {"env", m.env},         {"port", m.port},
         {"device_id", m.device_id}, {"project_id", m.project_id},
         {"started", m.started}, {"ncores", m.ncores},   {"cpu_mhz", m.cpu_mhz},
         {"duration_ms", m.duration_ms}, {"has_elf", m.has_elf}};
    j["arch"] = m.arch ? nlohmann::json(std::string(arch_name(*m.arch))) : nlohmann::json(nullptr);
    j["settings"] = m.settings ? nlohmann::json(*m.settings) : nlohmann::json(nullptr);
}

RunWriter::RunWriter(fs::path dir, RunMeta meta)
    : dir_(std::move(dir)), meta_(std::move(meta)), frames_(dir_ / "frames.mlg", std::ios::binary | std::ios::app) {
    save();
}

void RunWriter::save() const { write_meta(dir_, meta_); }

void RunWriter::append(std::uint32_t t_ms, std::span<const std::uint8_t> bytes) {
    while (!bytes.empty()) { // one record holds at most 65535 bytes
        const auto n = std::min<std::size_t>(bytes.size(), 0xffff);
        put_le(frames_, t_ms, 4);
        put_le(frames_, n, 2);
        frames_.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(n));
        bytes = bytes.subspan(n);
    }
    frames_.flush();
    meta_.duration_ms = t_ms;
    if (t_ms - last_saved_ms_ >= 1000) {
        last_saved_ms_ = t_ms;
        save();
    }
}

void RunWriter::set_target(Arch arch, unsigned ncores, double cpu_mhz) {
    meta_.arch = arch, meta_.ncores = ncores, meta_.cpu_mhz = cpu_mhz;
    save();
}

void RunWriter::set_elf(const fs::path& elf) {
    std::error_code ec;
    fs::copy_file(elf, dir_ / "firmware.elf", fs::copy_options::overwrite_existing, ec);
    meta_.has_elf = !ec;
    save();
}

void RunWriter::finish(std::uint64_t duration_ms) {
    meta_.duration_ms = duration_ms;
    save();
}

RunStore::RunStore(fs::path root) : root_(std::move(root)) { fs::create_directories(root_); }

fs::path RunStore::default_root() { return os::user_data_dir() / "malgrind/runs"; }

std::optional<fs::path> RunStore::dir_of(std::string_view id) const {
    if (!valid_id(id)) return std::nullopt;
    return root_ / os::from_utf8(id);
}

RunWriter RunStore::create(RunMeta meta) {
    const auto base = utc_now("%Y%m%d-%H%M%S");
    fs::path dir;
    for (int n = 1;; ++n) { // create_directory is the atomic claim of an id
        meta.id = n == 1 ? base : std::format("{}-{}", base, n);
        dir = root_ / os::from_utf8(meta.id);
        if (fs::create_directory(dir)) break;
    }
    meta.started = utc_now("%Y-%m-%dT%H:%M:%SZ");
    return RunWriter(std::move(dir), std::move(meta));
}

std::vector<RunMeta> RunStore::list() const {
    std::vector<RunMeta> runs;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root_, ec))
        if (entry.is_directory())
            if (auto m = read_meta(entry.path())) runs.push_back(std::move(*m));
    std::ranges::sort(runs, [](const RunMeta& a, const RunMeta& b) { return std::tie(b.started, b.id) < std::tie(a.started, a.id); });
    return runs;
}

std::optional<RunMeta> RunStore::get(std::string_view id) const {
    const auto dir = dir_of(id);
    return dir ? read_meta(*dir) : std::nullopt;
}

std::vector<Chunk> RunStore::chunks(std::string_view id) const {
    std::vector<Chunk> out;
    const auto dir = dir_of(id);
    if (!dir) return out;
    std::ifstream in(*dir / "frames.mlg", std::ios::binary);
    for (;;) {
        std::array<std::uint8_t, 6> head{};
        if (!in.read(reinterpret_cast<char*>(head.data()), head.size())) break;
        Chunk c{static_cast<std::uint32_t>(head[0] | head[1] << 8 | head[2] << 16 | std::uint32_t{head[3]} << 24), {}};
        c.bytes.resize(static_cast<std::size_t>(head[4] | head[5] << 8));
        if (!in.read(reinterpret_cast<char*>(c.bytes.data()), static_cast<std::streamsize>(c.bytes.size()))) break; // torn tail
        out.push_back(std::move(c));
    }
    return out;
}

fs::path RunStore::elf(std::string_view id) const {
    const auto dir = dir_of(id);
    return dir && fs::exists(*dir / "firmware.elf") ? *dir / "firmware.elf" : fs::path{};
}

std::optional<RunMeta> RunStore::update(std::string_view id, std::optional<std::string> name, std::optional<std::string> notes) {
    const auto dir = dir_of(id);
    auto meta = dir ? read_meta(*dir) : std::nullopt;
    if (!meta) return std::nullopt;
    if (name) meta->name = std::move(*name);
    if (notes) meta->notes = std::move(*notes);
    write_meta(*dir, *meta);
    return meta;
}

bool RunStore::remove(std::string_view id) {
    const auto dir = dir_of(id);
    std::error_code ec;
    return dir && fs::exists(*dir, ec) && fs::remove_all(*dir, ec) > 0;
}

} // namespace malgrind::host
