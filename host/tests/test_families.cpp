// Chip family interface, registry, and ESP32 family implementation self-checks.
#include "families/esp32.hpp"
#include "families/family.hpp"
#include "os/paths.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace malgrind;
using namespace malgrind::host;
namespace fs = std::filesystem;

namespace {

bool contains(std::string_view s, std::string_view part) { return s.find(part) != std::string_view::npos; }

fs::path fresh(const char* name) {
    const auto p = fs::canonical(fs::temp_directory_path()) / os::from_utf8(name);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void write(const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

std::string slurp(const fs::path& file) {
    std::ifstream in(file);
    return {std::istreambuf_iterator<char>(in), {}};
}

class StubFamily final : public Family {
public:
    [[nodiscard]] std::string_view id() const override { return "stub"; }
    [[nodiscard]] std::string_view name() const override { return "Stub"; }
    [[nodiscard]] FamilyCaps caps() const override { return {}; }
    [[nodiscard]] bool handles_arch(Arch) const override { return false; }
    [[nodiscard]] std::string project_problem(const ProjectInfo&, const std::string&) const override { return {}; }
    [[nodiscard]] unsigned agent_baud() const override { return 115200; }
    [[nodiscard]] AgentCommands agent_commands(const ToolContext&, const ProjectInfo&, const std::string&,
                                               const std::string&, const RunSettings&, bool) const override {
        return {};
    }
    [[nodiscard]] std::vector<std::string> addr2line_names(Arch) const override { return {}; }
};

void registry_lookups() {
    const auto& families = all_families();
    assert(!families.empty());
    const Family* esp = family_by_id("esp32");
    assert(esp == &esp32_family());
    assert(std::ranges::find(families, esp) != families.end());
    assert(family_by_id("unknown") == nullptr && family_by_id("") == nullptr);

    assert(family_for_arch(Arch::Xtensa) == esp);
    assert(family_for_arch(Arch::RiscV32) == esp);
    assert(family_for_arch(Arch::Host) == nullptr);
    assert(family_for_arch(Arch::ArmV7M) == nullptr);

    for (const Family* f : families) {
        const auto manifest = fs::path(MALGRIND_SOURCE_DIR) / "agent" / f->id() / "family.json";
        assert(fs::exists(manifest));
        const auto parsed = nlohmann::json::parse(slurp(manifest));
        assert(parsed.at("id").get<std::string>() == f->id());
    }
}

void base_family_defaults() {
    StubFamily stub;
    assert(!stub.is_agent_image("/no/such/image.bin"));
    assert(stub.family_hint_for_usb("303a", "1001").empty());
    assert(stub.usb_adapter_name("303a", "1001").empty());

    auto threw = [](auto&& fn) {
        try {
            fn();
            return false;
        } catch (const std::runtime_error&) {
            return true;
        }
    };
    assert(threw([&] { (void)stub.identify({}, "port", {}); }));
    assert(threw([&] { stub.read_flash({}, "port", {}, "/tmp/out.bin", {}); }));
    assert(threw([&] { stub.write_flash({}, "port", "/tmp/in.bin", {}); }));
}

void esp32_metadata_and_usb() {
    const Family& esp = esp32_family();
    assert(esp.id() == "esp32" && esp.name() == "ESP32 (ESP-IDF)");
    const auto caps = esp.caps();
    assert(caps.agent && caps.backup && caps.identify && caps.identify_resets_board);
    assert(esp.handles_arch(Arch::Xtensa) && esp.handles_arch(Arch::RiscV32));
    assert(!esp.handles_arch(Arch::ArmV7M) && !esp.handles_arch(Arch::Host));
    assert(esp.agent_baud() == 921600);

    const auto xtensa_tools = esp.addr2line_names(Arch::Xtensa);
    assert(xtensa_tools == (std::vector<std::string>{"xtensa-esp32-elf-addr2line", "xtensa-esp32s3-elf-addr2line"}));
    assert(esp.addr2line_names(Arch::RiscV32) == (std::vector<std::string>{"riscv32-esp-elf-addr2line"}));
    assert(esp.addr2line_names(Arch::ArmV7M).empty());

    assert(esp.usb_adapter_name("303a", "1001") == "Espressif USB-Serial/JTAG (built-in debug port)");
    assert(esp.usb_adapter_name("10c4", "ea60").empty());
    assert(esp.family_hint_for_usb("303a", "1001") == "esp32");
    assert(esp.family_hint_for_usb("10c4", "ea60").empty());
}

void project_matching_and_problems() {
    const auto dir = fresh("malgrind_families_proj");
    std::string why;

    // Valid ESP-IDF ini
    write(dir / "platformio.ini", "[env:a]\nplatform = espressif32@6.9.0\nframework = arduino, espidf\n");
    const auto idf_proj = read_project(dir, why);
    assert(idf_proj);
    const Family* matched = family_for_project(*idf_proj, "a", why);
    assert(matched == &esp32_family() && why.empty());
    assert(matched->project_problem(*idf_proj, "a").empty());

    // Arduino-ESP32 ini -> problem text names framework & espidf
    write(dir / "platformio.ini", "[env:a]\nplatform = espressif32\nframework = arduino\n");
    const auto arduino_proj = read_project(dir, why);
    assert(arduino_proj);
    assert(family_for_project(*arduino_proj, "a", why) == nullptr);
    assert(contains(why, "arduino") && contains(why, "espidf"));

    // STM32 ini -> no family matches, combined why names ESP32 / ststm32
    write(dir / "platformio.ini", "[env:a]\nplatform = ststm32\nframework = stm32cube\n");
    const auto stm_proj = read_project(dir, why);
    assert(stm_proj);
    assert(family_for_project(*stm_proj, "a", why) == nullptr);
    assert(contains(why, "ststm32") && contains(why, "ESP32"));

    // Space in project path -> rejected by ESP32 project_problem
    write(dir / "with space/platformio.ini", "[env:a]\nplatform = espressif32\nframework = espidf\n");
    const auto space_proj = read_project(dir / "with space", why);
    assert(space_proj);
    assert(family_for_project(*space_proj, "a", why) == nullptr);
    assert(contains(why, "space"));

    fs::remove_all(dir);
}

void agent_commands_preview_and_write() {
    const auto dir = fresh("malgrind_families_cmds");
    const auto proj_dir = dir / "proj";
    write(proj_dir / "platformio.ini", "[env:esp32dev]\nplatform = espressif32\nframework = espidf\n");
    write(proj_dir / "sdkconfig.esp32dev", "CONFIG_USER=1\nCONFIG_ESP_CONSOLE_UART_BAUDRATE=115200\n");

    std::string why;
    const auto project = read_project(proj_dir, why);
    assert(project);

    const ToolContext tools{{}, "/opt/pio", MALGRIND_SOURCE_DIR};
    const Family& esp = esp32_family();

    // write_files = false: preview creates nothing inside .pio/malgrind
    const auto preview = esp.agent_commands(tools, *project, "esp32dev", "/dev/ttyUSB0", {.rate_hz = 500}, false);
    assert(!fs::exists(proj_dir / ".pio"));
    assert(preview.elf == proj_dir / ".pio/malgrind/build/esp32dev/firmware.elf");
    assert(std::ranges::find(preview.build.argv, "upload") == preview.build.argv.end());
    assert(std::ranges::find(preview.upload.argv, "upload") != preview.upload.argv.end());
    assert(std::ranges::find(preview.upload.argv, "/dev/ttyUSB0") != preview.upload.argv.end());

    // write_files = true: writes sdkconfig.<env> and agent.stamp
    const auto real = esp.agent_commands(tools, *project, "esp32dev", "/dev/ttyUSB0", {.rate_hz = 500}, true);
    assert(real.build.argv == preview.build.argv && real.upload.argv == preview.upload.argv && real.elf == preview.elf);
    const auto sdkconfig = slurp(proj_dir / ".pio/malgrind/sdkconfig.esp32dev");
    assert(contains(sdkconfig, "CONFIG_USER=1\n") && contains(sdkconfig, "CONFIG_MALGRIND_RATE_HZ=500\n"));
    assert(contains(sdkconfig, "CONFIG_ESP_CONSOLE_UART_BAUDRATE=921600\n") && !contains(sdkconfig, "115200"));
    assert(fs::exists(proj_dir / ".pio/malgrind/agent.stamp"));

    // Missing agent files throw cleanly
    bool missing_threw = false;
    try {
        (void)esp.agent_commands({{}, "/opt/pio", dir / "missing-agent"}, *project, "esp32dev", "/dev/ttyUSB0", {}, false);
    } catch (const std::runtime_error&) {
        missing_threw = true;
    }
    assert(missing_threw);

    fs::remove_all(dir);
}

void esptool_parsing_and_agent_image() {
    assert(parse_mac("MAC: 24:0a:c4:AA:bb:0C") == "24-0a-c4-aa-bb-0c");
    assert(parse_mac("Chip is ESP32\nMAC: 24:0a:c4:aa:bb:0c\nUploading stub...\n") == "24-0a-c4-aa-bb-0c");
    assert(!parse_mac("no mac here") && !parse_mac("MAC: 24:0a:c4") && !parse_mac("MAC: zz:0a:c4:aa:bb:0c"));

    assert(parse_flash_size("Detected flash size: 4MB") == 4u * 1024 * 1024);
    assert(parse_flash_size("Device: 4016\nDetected flash size: 16MB\n") == 16u * 1024 * 1024);
    assert(parse_flash_size("Detected flash size: 512KB") == 512u * 1024);
    assert(!parse_flash_size("Detected flash size: Unknown") && !parse_flash_size("") && !parse_flash_size("Detected flash size: 4GB"));

    assert(parse_chip("Chip is ESP32-D0WD-V3 (revision v3.1)\r\nMAC: 24:0a:c4:aa:bb:0c\r\n") == "ESP32-D0WD-V3 (revision v3.1)");
    assert(!parse_chip("no chip here") && !parse_chip("Chip is   \n"));

    const auto root = fresh("malgrind_families_img");
    write(root / "agent.bin", std::string("\xe9\x03junk") + std::string(kEsp32AgentMarker) + "\0more");
    write(root / "plain.bin", "\xe9\x03 only the user's firmware here");
    std::string big(70000, 'a');
    big.insert(65536 - 5, kEsp32AgentMarker); // straddles a 64 KiB read boundary
    write(root / "big.bin", big);

    const Family& esp = esp32_family();
    assert(esp.is_agent_image(root / "agent.bin") && !esp.is_agent_image(root / "plain.bin") && esp.is_agent_image(root / "big.bin"));
    assert(!esp.is_agent_image(root / "missing.bin"));
    fs::remove_all(root);
}

void sdkconfig_merge_checks() {
    const auto agent = slurp(fs::path(MALGRIND_SOURCE_DIR) / "agent/esp32/sdkconfig.agent");
    assert(contains(agent, "CONFIG_ESP_CONSOLE_UART_BAUDRATE=921600"));
    const auto merged = merge_sdkconfig("CONFIG_A=1\n# CONFIG_GPTIMER_ISR_IRAM_SAFE is not set\nCONFIG_ESP_CONSOLE_UART_BAUDRATE=115200\n"
                                        "CONFIG_ESP_CONSOLE_UART_DEFAULT=y\n# CONFIG_B is not set",
                                        agent);
    assert(contains(merged, "CONFIG_A=1\n") && contains(merged, "# CONFIG_B is not set\n"));
    assert(contains(merged, "CONFIG_GPTIMER_ISR_IRAM_SAFE=y\n") && contains(merged, "CONFIG_ESP_CONSOLE_UART_BAUDRATE=921600\n"));
    assert(contains(merged, "# CONFIG_ESP_CONSOLE_UART_DEFAULT is not set\n") && contains(merged, "CONFIG_ESP_CONSOLE_UART_CUSTOM=y\n"));
    assert(!contains(merged, "115200") && !contains(merged, "IRAM_SAFE is not set") && !contains(merged, "UART_DEFAULT=y"));
    assert(!contains(merged, "DEFAULT and CUSTOM are one"));
}

} // namespace

int main() {
    registry_lookups();
    base_family_defaults();
    esp32_metadata_and_usb();
    project_matching_and_problems();
    agent_commands_preview_and_write();
    esptool_parsing_and_agent_image();
    sdkconfig_merge_checks();
    std::puts("families ok");
}
