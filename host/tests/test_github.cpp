// GitHub link parsing and cloning, with a fake `git` on PATH.
#include "env.hpp"
#include "github.hpp"
#include "ops.hpp"
#include "os/paths.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace malgrind::host;
namespace fs = std::filesystem;

namespace {

bool contains(const std::string& s, std::string_view part) { return s.find(part) != std::string::npos; }
std::string slurp(const fs::path& file) {
    std::ifstream in(file);
    return {std::istreambuf_iterator<char>(in), {}};
}

void accepts_repository_links() {
    const auto a = parse_github_url("https://github.com/Hombosh-dev/mcu-flamegraph");
    assert(a && a->url == "https://github.com/Hombosh-dev/mcu-flamegraph.git" && a->name == "mcu-flamegraph");
    assert(parse_github_url("https://github.com/o/r.git/")->url == "https://github.com/o/r.git");
    assert(parse_github_url("  https://github.com/o/r.js  \n")->name == "r.js");
    assert(parse_github_url("https://github.com/o/my_repo-2.0")->name == "my_repo-2.0");
}

void refuses_everything_else() {
    for (const char* bad : {"", "github.com/o/r", "http://github.com/o/r", "https://gitlab.com/o/r", "https://github.com.evil.com/o/r",
                            "https://github.com/o", "https://github.com/o/", "https://github.com//r", "https://github.com/o/r/tree/main",
                            "https://github.com/o/..", "https://github.com/o/.hidden", "https://github.com/o/a..b", "https://github.com/-o/r",
                            "file:///etc/passwd", "ext::sh -c touch% /tmp/x", "ssh://git@github.com/o/r", "git@github.com:o/r.git",
                            "--upload-pack=touch /tmp/x", "https://github.com/o/r --upload-pack=x", "https://github.com/o/r\nx",
                            "https://github.com/o/r?x=1", "https://user:pw@github.com/o/r"})
        assert(!parse_github_url(bad) && bad);
}

struct FakeGit {
    fs::path root = fs::canonical(fs::temp_directory_path()) / "malgrind_github";
    ScopedEnv save_path{"PATH"};
    ScopedEnv save_log{"FAKE_LOG"};
    ScopedEnv save_git{"FAKE_GIT", std::nullopt};
    FakeGit() {
        fs::remove_all(root);
        fs::create_directories(root / "bin");
        fs::copy_file(MALGRIND_FAKE_TOOL, root / "bin" / os::exe_name("git"), fs::copy_options::overwrite_existing);
        os::set_env("PATH", path_with(root / "bin")); // the fake git first
        os::set_env("FAKE_LOG", os::to_utf8(root / "log"));
    }
    ~FakeGit() { fs::remove_all(root); }
};

void path_with_checks() {
    ScopedEnv save_path("PATH", std::nullopt);
    const fs::path bin = "/tmp/malgrind_bin";
    assert(path_with(bin) == os::to_utf8(bin));
    os::set_env("PATH", "");
    assert(path_with(bin) == os::to_utf8(bin));
    os::set_env("PATH", "/usr/bin");
    assert(path_with(bin) == os::to_utf8(bin) + os::kPathListSep + "/usr/bin");
}

void clone_finds_the_project() {
    FakeGit fake;
    std::string lines;
    const auto dest = fake.root / "clone";
    const auto project = clone_project(*parse_github_url("https://github.com/o/r"), dest, [&](std::string_view l) { lines.append(l).append("\n"); });
    assert(project == dest / "firmware/x" && fs::exists(project / "platformio.ini") && contains(lines, "Cloning into"));
    const auto log = slurp(fake.root / "log");
    // https only, shallow, never a password prompt, and the link comes after `--` so it can never be read as an option
    assert(contains(log, "-c protocol.allow=never -c protocol.https.allow=always clone --depth 1 --progress -- https://github.com/o/r.git " + os::to_utf8(dest)));
    assert(contains(log, "prompt=0"));
}

void clone_without_platformio_ini_leaves_nothing() {
    FakeGit fake;
    os::set_env("FAKE_GIT", "nopio");
    const auto dest = fake.root / "clone";
    bool threw = false;
    try {
        (void)clone_project(*parse_github_url("https://github.com/o/r"), dest, [](std::string_view) {});
    } catch (const std::exception& e) {
        threw = contains(e.what(), "no platformio.ini");
    }
    assert(threw && !fs::exists(dest));
}

void failed_clone_leaves_nothing() {
    FakeGit fake;
    os::set_env("FAKE_GIT", "fail");
    std::string lines;
    bool threw = false;
    try {
        (void)clone_project(*parse_github_url("https://github.com/o/r"), fake.root / "clone", [&](std::string_view l) { lines.append(l); });
    } catch (const std::exception& e) {
        threw = contains(e.what(), "128") && contains(e.what(), "public");
    }
    assert(threw && contains(lines, "repository not found") && !fs::exists(fake.root / "clone"));
}

void missing_git_is_explained() {
    FakeGit fake;
    os::set_env("PATH", os::to_utf8(fake.root / "nonexistent"));
    bool threw = false;
    try {
        (void)clone_project(*parse_github_url("https://github.com/o/r"), fake.root / "clone", [](std::string_view) {});
    } catch (const std::exception& e) {
        threw = contains(e.what(), "git was not found");
    }
    assert(threw);
}

} // namespace

int main() {
    accepts_repository_links();
    refuses_everything_else();
    path_with_checks();
    clone_finds_the_project();
    clone_without_platformio_ini_leaves_nothing();
    failed_clone_leaves_nothing();
    missing_git_is_explained();
    std::puts("github ok");
}
