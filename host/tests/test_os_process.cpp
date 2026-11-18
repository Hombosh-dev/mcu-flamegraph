// OS process spawning, MSVCRT quoting, PATH lookup and fd non-leak self-check.
#include "os/paths.hpp"
#include "os/process.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace malgrind::host;
namespace fs = std::filesystem;

namespace {

class ScopedEnv {
public:
    explicit ScopedEnv(std::string name) : name_(std::move(name)), saved_(os::get_env(name_)) {}
    ~ScopedEnv() {
        if (saved_) os::set_env(name_, *saved_);
        else os::unset_env(name_);
    }

private:
    std::string name_;
    std::optional<std::string> saved_;
};

fs::path fresh_dir(const char* name) {
    const auto p = fs::canonical(fs::temp_directory_path()) / name;
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void windows_command_line_checks() {
    assert(os::windows_command_line({}).empty());
    assert(os::windows_command_line({""}) == L"\"\"");
    assert(os::windows_command_line({"plain", "abc/def"}) == L"plain abc/def");
    assert(os::windows_command_line({"a b", "c\"d", "e\\\\"}) == L"\"a b\" \"c\\\"d\" e\\\\");
    assert(os::windows_command_line({"a b\\", "x\\\"y", "tab\there"}) == L"\"a b\\\\\" \"x\\\\\\\"y\" \"tab\there\"");
    assert(os::windows_command_line({"проєкт ü"}) == L"\"проєкт ü\"");
}

void run_lines_checks() {
    std::vector<std::string> lines;
    const int code = os::run_lines(
        {MALGRIND_HELPER_ECHO, "a b", "c\"d", "e\\\\", "проєкт ü"},
        {"HELPER_VAR=hello world"},
        [&](std::string_view line) { lines.emplace_back(line); });
    assert(code == 0);
    assert((lines == std::vector<std::string>{"a b", "c\"d", "e\\\\", "проєкт ü", "HELPER_VAR=hello world"}));

    // Also verify stderr merging, \r line splitting, and non-zero exit code without a shell.
    std::vector<std::string> streamed;
    const int exit_code = os::run_lines(
        {MALGRIND_HELPER_ECHO, "first"},
        {"HELPER_VAR=v2", "HELPER_RAW=p1\rp2\n", "HELPER_ERR=err line", "HELPER_EXIT=7"},
        [&](std::string_view line) { streamed.emplace_back(line); });
    assert(exit_code == 7);
    assert((streamed == std::vector<std::string>{"first", "HELPER_VAR=v2", "p1", "p2", "err line"}));

    assert(os::run_lines({}, {}, [](std::string_view) {}) == -1);
    assert(os::run_lines({"/no/such/malgrind_missing_program"}, {}, [](std::string_view) {}) != 0);
}

void capture_and_run_checks() {
    ScopedEnv save_err("HELPER_ERR");
    ScopedEnv save_exit("HELPER_EXIT");
    ScopedEnv save_var("HELPER_VAR");
    os::unset_env("HELPER_VAR");
    os::unset_env("HELPER_EXIT");
    os::set_env("HELPER_ERR", "discarded stderr");

    const std::string out = os::capture({MALGRIND_HELPER_ECHO, "", "a b", "c\"d", "e\\\\", "проєкт ü"});
    assert(out == "\na b\nc\"d\ne\\\\\nпроєкт ü\nHELPER_VAR=\n");

    os::unset_env("HELPER_ERR");
    assert(os::capture({}).empty());
    assert(os::capture({"/no/such/malgrind_missing_program"}).empty());

    assert(os::run({MALGRIND_HELPER_ECHO, "run-ok"}) == 0);
    os::set_env("HELPER_EXIT", "7");
    assert(os::run({MALGRIND_HELPER_ECHO, "run-exit"}) == 7);
    os::unset_env("HELPER_EXIT");

    assert(os::run({}) == -1);
    assert(os::run({"/no/such/malgrind_missing_program"}) != 0);
}

void find_in_path_and_batch_checks() {
    ScopedEnv save_path("PATH");
    const auto dir = fresh_dir("malgrind_os_process_path");

    // dir1/mytool is a directory (must be rejected), dir2/mytool is non-executable on POSIX, dir3/mytool[.exe] is valid.
    fs::create_directories(dir / "dir1/mytool");
    fs::create_directories(dir / "dir2");
    fs::create_directories(dir / "dir3");
    {
        std::ofstream(dir / "dir2/mytool") << "not executable\n";
        std::ofstream(dir / "dir3" / os::exe_name("mytool")) << "#!/bin/sh\n";
    }
    if constexpr (os::kind() != os::Kind::Windows) {
        fs::permissions(dir / "dir2/mytool", fs::perms::owner_read | fs::perms::owner_write);
        fs::permissions(dir / "dir3/mytool", fs::perms::owner_all);
    }

    const std::string sep(1, os::kPathListSep);
    os::set_env("PATH", os::to_utf8(dir / "dir1") + sep + os::to_utf8(dir / "dir2") + sep + os::to_utf8(dir / "dir3"));

    assert(os::find_in_path("mytool") == dir / "dir3" / os::exe_name("mytool"));
    assert(!os::find_in_path("malgrind_missing_tool").has_value());
    assert(!os::find_in_path("").has_value());

    if constexpr (os::kind() == os::Kind::Windows) {
        {
            std::ofstream(dir / "dir1/onlybat.bat") << "@echo off\n";
            std::ofstream(dir / "dir1/onlycmd.CMD") << "@echo off\n";
        }
        assert(!os::find_in_path("onlybat").has_value());
        assert(!os::find_in_path("onlybat.bat").has_value());
        assert(!os::find_in_path("onlycmd").has_value());
        assert(!os::find_in_path("onlycmd.cmd").has_value());
        assert(os::run({os::to_utf8(dir / "dir1/onlybat.bat")}) == -1);
        assert(os::run_lines({os::to_utf8(dir / "dir1/onlycmd.CMD")}, {}, [](std::string_view) {}) == -1);
        assert(os::capture({os::to_utf8(dir / "dir1/onlybat.bat")}).empty());
    }

    fs::remove_all(dir);
}

void fd_non_leak_checks() {
#ifndef _WIN32
    const int before = ::open("/dev/null", O_RDONLY);
    assert(before >= 0);
    ::close(before);

    for (int i = 0; i < 10; ++i) {
        assert(!os::capture({MALGRIND_HELPER_ECHO, "fd"}).empty());
        assert(os::capture({"/no/such/malgrind_missing_program"}).empty());
        assert(os::run_lines({MALGRIND_HELPER_ECHO, "fd"}, {"HELPER_VAR=ok"}, [](std::string_view) {}) == 0);
        assert(os::run_lines({"/no/such/malgrind_missing_program"}, {}, [](std::string_view) {}) != 0);
    }

    const int after = ::open("/dev/null", O_RDONLY);
    assert(after >= 0);
    ::close(after);
    assert(after == before);
#endif
}

} // namespace

int main() {
    windows_command_line_checks();
    run_lines_checks();
    capture_and_run_checks();
    find_in_path_and_batch_checks();
    fd_non_leak_checks();
    std::puts("os_process ok");
}
