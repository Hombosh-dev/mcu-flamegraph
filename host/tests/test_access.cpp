// Port access self-check: reading permissions, pkexec discovery, and that the privileged command can only carry plain names.
#include "os/access.hpp"
#include "os/paths.hpp"
#include "os/process.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <grp.h>
#include <iterator>
#include <unistd.h>

using namespace malgrind::host::os;
namespace fs = std::filesystem;

namespace {

class ScopedEnv {
public:
    explicit ScopedEnv(std::string name) : name_(std::move(name)), saved_(get_env(name_)) {}
    ~ScopedEnv() {
        if (saved_) set_env(name_, *saved_);
        else unset_env(name_);
    }

private:
    std::string name_;
    std::optional<std::string> saved_;
};

// The script is what runs as root, so it is run here: a fake pkexec just executes its arguments, and fake setfacl and usermod
// log what they were given and fail on demand (FAKE_FAIL=setfacl,usermod).
struct Tools {
    ScopedEnv save_path{"PATH"};
    ScopedEnv save_log{"FAKE_LOG"};
    ScopedEnv save_fail{"FAKE_FAIL"};
    fs::path root = fs::canonical(fs::temp_directory_path()) / "malgrind_access_tools";
    fs::path bin = root / "bin", log = root / "log";
    explicit Tools(bool with_pkexec = true) {
        fs::remove_all(root);
        fs::create_directories(bin);
        if (with_pkexec) write("pkexec", "exec \"$@\"\n");
        for (const char* tool : {"setfacl", "usermod"})
            write(tool, std::string("echo \"") + tool + " $*\" >> \"$FAKE_LOG\"\ncase \",$FAKE_FAIL,\" in *," + tool + ",*) exit 1;; esac\nexit 0\n");
        set_env("PATH", with_pkexec ? (to_utf8(bin) + ":/usr/bin:/bin") : to_utf8(bin));
        set_env("FAKE_LOG", to_utf8(log));
    }
    ~Tools() { fs::remove_all(root); }
    void write(const char* name, const std::string& body) const {
        std::ofstream(bin / name) << "#!/bin/sh\n" << body;
        fs::permissions(bin / name, fs::perms::owner_all);
    }
    // runs the real command for these values; returns its exit code
    int run(const char* group, const char* fail = "") const {
        set_env("FAKE_FAIL", fail);
        fs::remove(log);
        const auto inv = grant_access_command("/dev/ttyUSB0", "partum", group);
        assert(inv);
        return run_lines(inv->argv, inv->env, [](std::string_view) {});
    }
    [[nodiscard]] std::string calls() const {
        std::ifstream in(log);
        return {std::istreambuf_iterator<char>(in), {}};
    }
};

void reads_permissions() {
    const auto dir = fs::temp_directory_path() / "malgrind_access";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const auto port = dir / "ttyFAKE0";
    std::ofstream(port) << "x";
    fs::permissions(port, fs::perms::owner_read | fs::perms::owner_write);
    {
        Tools with_pkexec(true);
        assert(can_grant_access());
        const auto mine = port_access(to_utf8(port));
        assert(mine.ok && mine.in_group && !mine.fixable);
        assert(mine.group == getgrgid(getegid())->gr_name); // a new file belongs to the user's own group
        if (geteuid() != 0) { // root may open anything
            fs::permissions(port, fs::perms::none);
            const auto denied = port_access(to_utf8(port));
            assert(!denied.ok && denied.fixable);
        }
        const auto missing = port_access(to_utf8(dir / "missing"));
        assert(!missing.ok && missing.group.empty() && !missing.fixable);
    }
    if (geteuid() != 0) {
        Tools without_pkexec(false);
        assert(!can_grant_access());
        fs::permissions(port, fs::perms::none);
        const auto denied = port_access(to_utf8(port));
        assert(!denied.ok && !denied.fixable);
    }
    assert(!current_user().empty());
    fs::remove_all(dir);
}

void the_privileged_command_carries_names_only() {
    Tools t(true);
    const auto inv = grant_access_command("/dev/ttyUSB0", "partum", "dialout");
    assert(inv && inv->env.empty() && inv->argv.size() == 8);
    assert(inv->argv[0] == to_utf8(t.bin / "pkexec") && inv->argv[1] == "/bin/sh" && inv->argv[2] == "-c" && inv->argv[4] == "sh");
    // user, port and group are separate arguments after the script, in the order the script reads them ($1 $2 $3)
    assert(inv->argv[5] == "partum" && inv->argv[6] == "/dev/ttyUSB0" && inv->argv[7] == "dialout");
    const auto& script = inv->argv[3];
    for (const char* value : {"partum", "ttyUSB0", "dialout"}) assert(script.find(value) == std::string::npos);
    assert(script.find("setfacl") != std::string::npos && script.find("usermod") != std::string::npos);
    assert(grant_access_command("/dev/cu.usbserial-1410", "a.b_c-d", "uucp")); // other platforms' names are fine
}

void returns_nullopt_without_pkexec() {
    Tools t(false);
    assert(!can_grant_access());
    assert(!grant_access_command("/dev/ttyUSB0", "partum", "dialout").has_value());
}

void refuses_anything_that_is_not_a_plain_name() {
    Tools t(true);
    for (const char* bad : {"", "a b", "a;b", "$(id)", "-rf", "a\nb", "a'b", "a`b`", "a/b"}) {
        assert(!grant_access_command("/dev/ttyUSB0", bad, "dialout") && bad);
        assert(!grant_access_command("/dev/ttyUSB0", "me", bad) && bad);
    }
    for (const char* bad : {"", "ttyUSB0", "/etc/passwd", "/dev/../etc/passwd", "/dev/tty USB", "/dev/tty;id", "/dev/tty$HOME", "-x"})
        assert(!grant_access_command(bad, "me", "dialout") && bad);
}

bool has(const std::string& s, std::string_view part) { return s.find(part) != std::string::npos; }

void the_script_does_what_it_says() {
    Tools t(true);
    // both fixes, with user, port and group in the places the script reads them
    assert(t.run("dialout") == 0);
    assert(has(t.calls(), "setfacl -m u:partum:rw /dev/ttyUSB0") && has(t.calls(), "usermod -aG dialout partum"));
    // a device that belongs to root: no group helps, only the ACL is set
    assert(t.run("root") == 0 && has(t.calls(), "setfacl") && !has(t.calls(), "usermod"));
    // either fix is enough: one failing does not fail the call, both failing does
    assert(t.run("dialout", "setfacl") == 0 && has(t.calls(), "usermod -aG dialout partum"));
    assert(t.run("dialout", "usermod") == 0 && has(t.calls(), "setfacl"));
    assert(t.run("dialout", "setfacl,usermod") != 0);
    assert(t.run("root", "setfacl") != 0); // root's device and no ACL: nothing was done
}

void manual_commands_name_the_fix() {
    const auto text = manual_access_commands("/dev/ttyUSB0", "partum", "dialout");
    assert(text.find("sudo setfacl -m u:partum:rw /dev/ttyUSB0") != std::string::npos);
    assert(text.find("sudo usermod -aG dialout partum") != std::string::npos);
    const auto unavail = grant_access_unavailable("/dev/ttyUSB0", "partum", "dialout");
    assert(unavail.find("pkexec") != std::string::npos && unavail.find("setfacl") != std::string::npos);
}

} // namespace

int main() {
    reads_permissions();
    the_privileged_command_carries_names_only();
    returns_nullopt_without_pkexec();
    refuses_anything_that_is_not_a_plain_name();
    the_script_does_what_it_says();
    manual_commands_name_the_fix();
    std::puts("access ok");
}
