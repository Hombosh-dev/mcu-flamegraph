// OS paths, environment, UTF-8 conversion and time helpers self-check.
#include "os/paths.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <vector>

using namespace malgrind::host;
namespace fs = std::filesystem;

namespace {

void split_path_list_checks() {
    assert((os::split_path_list("a::b", ':') == std::vector<fs::path>{"a", "b"}));
    assert((os::split_path_list(":a:", ':') == std::vector<fs::path>{"a"}));
    assert((os::split_path_list("", ':').empty()));
    assert((os::split_path_list("C:\\a;;D:\\b\\", ';') == std::vector<fs::path>{"C:\\a", "D:\\b\\"}));
}

void exe_name_checks() {
    if constexpr (os::kind() == os::Kind::Windows) {
        assert(os::kind_name() == "windows");
        assert(os::kPathListSep == ';');
        assert(os::exe_name("pio") == "pio.exe");
        assert(os::exe_name("pio.exe") == "pio.exe");
        assert(os::exe_name("PIO.EXE") == "PIO.EXE");
    } else if constexpr (os::kind() == os::Kind::MacOS) {
        assert(os::kind_name() == "macos");
        assert(os::kPathListSep == ':');
        assert(os::exe_name("pio") == "pio");
    } else {
        assert(os::kind_name() == "linux");
        assert(os::kPathListSep == ':');
        assert(os::exe_name("pio") == "pio");
    }
}

void utf8_roundtrip_checks() {
    const std::string sample = "проєкт/ü";
    assert(os::to_utf8(os::from_utf8(sample)) == sample);
    assert(os::to_utf8(os::from_utf8("")).empty());
}

void env_and_user_data_dir_checks() {
    constexpr const char* kTestVar = "MALGRIND_TEST_ENV_PATH";
    os::unset_env(kTestVar);
    assert(!os::get_env(kTestVar).has_value());
    os::set_env(kTestVar, "проєкт/value");
    assert(os::get_env(kTestVar) == std::optional<std::string>{"проєкт/value"});
    os::unset_env(kTestVar);
    assert(!os::get_env(kTestVar).has_value());

    if constexpr (os::kind() == os::Kind::Linux) {
        const auto saved = os::get_env("XDG_DATA_HOME");
        os::set_env("XDG_DATA_HOME", "/tmp/malgrind-xdg-test");
        assert(os::user_data_dir() == fs::path("/tmp/malgrind-xdg-test"));
        os::unset_env("XDG_DATA_HOME");
        assert(os::user_data_dir() == os::home_dir() / ".local/share");
        if (saved) os::set_env("XDG_DATA_HOME", *saved);
        else os::unset_env("XDG_DATA_HOME");
    } else {
        assert(!os::user_data_dir().empty());
    }
}

void safe_projects_root_checks() {
    assert(os::safe_projects_root_for(os::Kind::Linux, "/home/u/.local/share", "/home/u", "C:") ==
           fs::path("/home/u/.local/share/malgrind/projects"));
    assert(os::safe_projects_root_for(os::Kind::MacOS, "/Users/u/Library/Application Support", "/Users/u", "C:") ==
           fs::path("/Users/u/.malgrind/projects"));

    const auto win_ascii = os::from_utf8("C:\\Users\\Alice\\AppData\\Local");
    assert(os::safe_projects_root_for(os::Kind::Windows, win_ascii, {}, "C:") == win_ascii / "malgrind/projects");

    const auto win_space = os::from_utf8("C:\\Users\\Ivan Petrenko\\AppData\\Local");
    assert(os::safe_projects_root_for(os::Kind::Windows, win_space, {}, "C:") == os::from_utf8("C:/.malgrind/projects"));
    assert(os::safe_projects_root_for(os::Kind::Windows, win_space, {}, "D:") == os::from_utf8("D:/.malgrind/projects"));
    assert(os::safe_projects_root_for(os::Kind::Windows, win_space, {}, "") == os::from_utf8("C:/.malgrind/projects"));

    const auto win_nonascii = os::from_utf8("C:\\Users\\Іван\\AppData\\Local");
    assert(os::safe_projects_root_for(os::Kind::Windows, win_nonascii, {}, "C:") == os::from_utf8("C:/.malgrind/projects"));

    assert(!os::safe_projects_root().empty());
}

void executable_and_time_checks() {
    const auto exe = os::executable_path();
    assert(!exe.empty() && fs::exists(exe));
    assert(!os::home_dir().empty());

    const std::tm utc0 = os::utc_tm(0);
    assert(utc0.tm_year == 70 && utc0.tm_mon == 0 && utc0.tm_mday == 1);
    assert(utc0.tm_hour == 0 && utc0.tm_min == 0 && utc0.tm_sec == 0);

    const std::tm loc0 = os::local_tm(0);
    assert(loc0.tm_year >= 69 && loc0.tm_year <= 70);
}

} // namespace

int main() {
    split_path_list_checks();
    exe_name_checks();
    utf8_roundtrip_checks();
    env_and_user_data_dir_checks();
    safe_projects_root_checks();
    executable_and_time_checks();
    std::puts("os_paths ok");
}
