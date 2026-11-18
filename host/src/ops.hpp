#pragma once

// Steps that both front-ends need: the app's JSON protocol (serve.cpp) and the terminal commands (cli.cpp).

#include "devices.hpp"
#include "families/family.hpp"
#include "github.hpp"
#include "malgrind/target_info.hpp"
#include "os/serial.hpp"
#include "pio.hpp"
#include "source.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace malgrind::host {

inline constexpr const char* kNoPio =
    "PlatformIO (pio) was not found: install it with `pip install platformio` (attaching to a running board works without it)";

using SourceFactory =
    std::function<std::variant<std::unique_ptr<ByteSource>, os::OpenFailure>(const Device& dev, unsigned baud)>;

// The error for a port that could not be opened; Busy becomes "busy: ...".
[[nodiscard]] std::runtime_error open_error(const std::string& port, const os::OpenFailure& failure);

// Opens the port exclusively (or stdin when port == "-"). `baud` 0 uses the default family baud. Throws open_error.
[[nodiscard]] std::unique_ptr<ByteSource> open_source(const std::string& port, unsigned baud = 0);

// Opens a resolved serial device via `factory` (default: SerialSource::open). `baud` 0 uses the hinted/default family baud. Throws open_error.
[[nodiscard]] std::unique_ptr<ByteSource> open_device_source(const Device& dev, const SourceFactory& factory = {},
                                                             unsigned baud = 0);

// After an upload the board resets and its USB port can vanish or renumber for a moment: keeps trying for about 5 s.
[[nodiscard]] std::unique_ptr<ByteSource> open_after_upload(
    const std::string& device_ref, const std::function<std::vector<Device>()>& devices_fn = list_devices,
    const SourceFactory& factory = {}, unsigned baud = 0);

struct Prepared {
    std::filesystem::path pio;
    ProjectInfo project;
    std::string env, port;
    const Family* family = nullptr;
};
// pio, the project, its environment and matching target family, checked. `env` empty: the project's default. Throws with a reason.
[[nodiscard]] Prepared prepare_project(const std::filesystem::path& home, const std::filesystem::path& dir, std::string env,
                                       std::string port);

// Runs a pio invocation, handing every output line to on_line. Throws if it fails.
void run_pio(const Invocation& invocation, const std::function<void(std::string_view line)>& on_line);

struct ProbeResult {
    bool busy = false;  // somebody else has the port
    bool denied = false; // this user may not open the port (not in its group, Linux)
    bool agent = false; // a malgrind frame arrived
    Arch arch{};
    unsigned ncores = 0;
    double cpu_mhz = 0;
};
// Listens to an already-open source for up to 2 s for a malgrind frame. Never records.
[[nodiscard]] ProbeResult probe_source(ByteSource& source);
// Listens to the port for up to 2 s for a malgrind frame (`baud` 0 tries registered family bauds). Never records and never resets the board. Throws open_error.
[[nodiscard]] ProbeResult probe_port(const std::string& port, unsigned baud = 0);
[[nodiscard]] ProbeResult probe_device(const Device& dev, const SourceFactory& factory = {}, unsigned baud = 0);

// Shallow-clones the repository into `dest` (which must not exist; git's progress goes to on_line) and returns the PlatformIO
// project inside it: the clone itself or its only firmware/* folder. Never asks for a password (a private repository
// fails at once). On any failure `dest` is removed again and this throws.
[[nodiscard]] std::filesystem::path clone_project(const GithubRepo& repo, const std::filesystem::path& dest, const LineFn& on_line);

// Identifies the board on `port` via registered families (resets the board when the family's identify does). Throws on failure.
[[nodiscard]] BoardIdentity identify_board(const ToolContext& tools, const std::string& port, const LineFn& on_line,
                                           std::string_view family_hint = {});

// Builds the project with the agent (`build`), backs up what is on the board (see backup.hpp), then flashes (`upload`): nothing
// is flashed unless the backup succeeded. A board that already runs the agent is not backed up again (its flash holds our
// own build). The two commands come from make_plan (plan.hpp); write_plan_files must have run before. Progress and the path
// of the backup arrive through on_line. Throws.
void flash_agent(const Prepared& prepared, const std::filesystem::path& home, const std::filesystem::path& backup_dir,
                 const Invocation& build, const Invocation& upload, const LineFn& on_line);

// Writes the newest backup of the board on `port` back, byte for byte. Throws, naming the board, if there is no backup of it.
void restore_flash(const std::filesystem::path& pio, const std::filesystem::path& home, const std::string& port,
                   const std::filesystem::path& backup_dir, const LineFn& on_line);

// One way to profile the board that is plugged in, and, when it is not possible, what is missing.
struct Method {
    std::string id; // "attach", "new_run", "no_flash"
    bool available = false;
    std::string why; // empty when available
};
// `family`: target family for building the agent (nullable); `project_problem`: why the chosen project cannot be used
// with the agent (empty: fine, or none chosen yet).
[[nodiscard]] std::vector<Method> methods(const ProbeResult& probe, const Family* family, bool have_pio,
                                          const std::string& project_problem);

} // namespace malgrind::host
