// Host tool self-check: frame decoding/resync and the aggregation tree.
#include "aggregate.hpp"
#include "discover.hpp"
#include "source.hpp"
#include "stream.hpp"
#include "wire.hpp"

#include "malgrind/profiler.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace malgrind;
using namespace malgrind::host;
using namespace std::chrono_literals;

int main() {
    const auto stream = make_stream(3); // 6 frames: core 0 and core 1 per flush

    // whole stream, and the same stream cut into odd-sized pieces
    auto whole = FrameReader().feed(stream);
    assert(whole.frames.size() == 6 && whole.text.empty());
    assert(whole.frames[0].arch == Arch::Xtensa && whole.frames[0].ncores == 2 && whole.frames[1].core == 1);
    assert(whole.frames[0].stacks.size() == 1 && whole.frames[0].stacks[0].count == 3);
    assert(whole.frames[0].stacks[0].pcs == (std::vector<std::uint32_t>{0x400d1000, 0x400d2000, 0x400d3000}));
    assert(whole.frames[0].flush_cyc == 5000 && whole.frames[0].isr_avg == 100);
    FrameReader chunked;
    std::size_t frames = 0;
    for (std::size_t i = 0; i < stream.size(); i += 7)
        frames += chunked.feed(std::span(stream).subspan(i, std::min<std::size_t>(7, stream.size() - i))).frames.size();
    assert(frames == 6);

    // log text passes through; one corrupted frame is lost, the rest still decode
    const std::string log = "I (123) app: hello\n";
    std::vector<std::uint8_t> damaged(log.begin(), log.end());
    damaged.insert(damaged.end(), stream.begin(), stream.end());
    damaged[log.size() + 20] ^= 0xFF;
    auto resynced = FrameReader().feed(damaged);
    assert(resynced.frames.size() == 5 && resynced.text.starts_with(log));

    // aggregation: no ELF, so names are the raw (call-adjusted) addresses
    Symbolizer symbols({}, {});
    Aggregator agg(symbols);
    agg.add(whole.frames[0], 0ms);      // core 0, early
    agg.add(whole.frames[1], 0ms);      // core 1, early
    agg.add(whole.frames[2], 29000ms);  // core 0, late
    auto contains = [](const std::string& s, const char* part) { return s.find(part) != std::string::npos; };

    assert(agg.duration() == 29000ms);
    assert(contains(agg.snapshot_json(std::nullopt, 10s), R"("name":"all","value":3)"));        // window ends at the last frame
    assert(contains(agg.snapshot_json(std::nullopt, 0s), R"("name":"all","value":7)"));        // 3 + 1 + 3
    assert(contains(agg.snapshot_json(std::nullopt, 5s, 1000ms), R"("name":"all","value":4)")); // scrubbed back: 3 + 1
    assert(contains(agg.snapshot_json(1u, 0s), R"("name":"all","value":1)"));
    assert(contains(agg.snapshot_json(std::nullopt, 0s), R"("duration_ms":29000)"));
    // main (0x400d3000-1) is the root of every stack and carries all of them
    assert(contains(agg.snapshot_json(std::nullopt, 0s), R"("name":"0x400d2fff","value":7)"));

    const auto folded = agg.folded();
    assert(contains(folded, "core0;0x400d2fff;0x400d1fff;0x400d1000 6\n"));
    assert(contains(folded, "core1;0x400d2fff;0x400d1fff;0x400d17ff") == false); // a core-1 leaf is not call-adjusted twice

    // function table: b = 0x400d1fff is called by main and calls a (6 samples) and c (1)
    const auto fn = nlohmann::json::parse(agg.functions_json(std::nullopt, 0s, std::nullopt, "0x400d1fff"));
    assert(fn["total"] == 7);
    assert(fn["functions"][0]["name"] == "0x400d1000" && fn["functions"][0]["self"] == 6 && fn["functions"][0]["total"] == 6);
    assert(fn["callers"].size() == 1 && fn["callers"][0]["name"] == "0x400d2fff" && fn["callers"][0]["count"] == 7);
    assert(fn["callees"].size() == 2);
    assert(nlohmann::json::parse(agg.functions_json(std::nullopt, 0s, std::nullopt, ""))["callers"].empty());
    assert(nlohmann::json::parse(agg.functions_json(1u, 0s, std::nullopt, ""))["total"] == 1); // core filter

    // recursion: names {main, r, r, leaf}: r counts once in `total`, and calls itself
    Frame rec;
    rec.arch = Arch::Xtensa, rec.ncores = 1, rec.core = 0, rec.cpu_hz = 1, rec.stacks = {{5, {0x400d5000, 0x400d1000, 0x400d1000, 0x400d3000}}};
    Symbolizer rec_symbols({}, {});
    Aggregator rec_agg(rec_symbols);
    rec_agg.add(rec, 0ms);
    const auto rfn = nlohmann::json::parse(rec_agg.functions_json(std::nullopt, 0s, std::nullopt, "0x400d0fff"));
    assert(rfn["total"] == 5);
    for (const auto& f : rfn["functions"])
        if (f["name"] == "0x400d0fff") assert(f["total"] == 5 && f["self"] == 0);   // 5, not 10
    assert(rfn["callees"].size() == 2); // itself and the leaf

    // project and ELF discovery in a throw-away tree
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "malgrind_test_tree";
    fs::remove_all(root);
    auto fake_elf = [](const fs::path& file, unsigned char machine) {
        fs::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary);
        const unsigned char header[20] = {0x7f, 'E', 'L', 'F', 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, machine, 0};
        out.write(reinterpret_cast<const char*>(header), sizeof header);
    };
    std::string why;
    fs::create_directories(root / "firmware/a");
    assert(!find_project(root, why) && why.find("no PlatformIO project") != std::string::npos);
    std::ofstream(root / "firmware/a/platformio.ini");
    assert(find_project(root, why) == root / "firmware/a");
    fs::create_directories(root / "firmware/b");
    std::ofstream(root / "firmware/b/platformio.ini");
    assert(!find_project(root, why) && why.find("several") != std::string::npos);

    fake_elf(root / "firmware/a/.pio/build/esp32dev/firmware.elf", 94);   // Xtensa
    fake_elf(root / "firmware/b/.pio/build/stm/firmware.elf", 40);        // ARM, newer
    fake_elf(root / "build/app.elf", 94);                                  // Xtensa, newest
    fs::last_write_time(root / "build/app.elf", fs::file_time_type::clock::now() + 10s);
    assert(elf_machine(root / "firmware/a/.pio/build/esp32dev/firmware.elf") == 94);
    assert(find_elf(Arch::Xtensa, root) == root / "build/app.elf");         // newest of the matching ones
    assert(find_elf(Arch::ArmV7M, root) == root / "firmware/b/.pio/build/stm/firmware.elf");
    assert(!find_elf(Arch::RiscV32, root));                                  // nothing built for it
    assert(!elf_machine(root / "firmware/a/platformio.ini"));                // not an ELF
    // an agent build (.pio/malgrind) wins over newer ordinary builds: it is the firmware that sends frames
    const auto agent_elf = root / "firmware/a/.pio/malgrind/build/esp32dev/firmware.elf";
    fake_elf(agent_elf, 94);
    fs::last_write_time(agent_elf, fs::file_time_type::clock::now() - 100s);
    assert(find_elf(Arch::Xtensa, root) == agent_elf);
    assert(find_elf(Arch::ArmV7M, root) == root / "firmware/b/.pio/build/stm/firmware.elf");
    fs::remove_all(root);

    std::puts("host ok");
}
