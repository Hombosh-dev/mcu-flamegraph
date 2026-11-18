// Flash backup path & lookup self-check: choosing the newest backup and keeping board keys safe as path components.
#include "backup.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>

using namespace malgrind::host;
namespace fs = std::filesystem;

namespace {

fs::path fresh_root(const char* name) {
    const auto root = fs::temp_directory_path() / name;
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

void touch(const fs::path& file, std::string_view content = "x") {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << content;
}

void picks_the_newest_backup() {
    const auto root = fresh_root("malgrind_backup_a");
    assert(!latest_backup(root, "24-0a-c4-aa-bb-0c")); // nothing yet
    touch(root / "24-0a-c4-aa-bb-0c/20261116-100000.bin");
    touch(root / "24-0a-c4-aa-bb-0c/20261116-120000.bin");
    touch(root / "24-0a-c4-aa-bb-0c/20261116-110000.bin.part"); // unfinished: ignored
    touch(root / "11-22-33-44-55-66/20261117-000000.bin");      // another board
    assert(latest_backup(root, "24-0a-c4-aa-bb-0c") == root / "24-0a-c4-aa-bb-0c/20261116-120000.bin");
    assert(!latest_backup(root, "aa-aa-aa-aa-aa-aa"));
    fs::remove_all(root);
}

void key_is_never_a_path() {
    const auto root = fresh_root("malgrind_backup_b");
    touch(root / "secret/20261116-100000.bin");
    assert(!latest_backup(root, "../b/secret") && !latest_backup(root, "secret") && !latest_backup(root, ""));
    assert(!backup_path(root, "../x", "20261116-100000"));
    assert(!backup_path(root, "-bad", "20261116-100000") && !backup_path(root, "bad-", "20261116-100000"));
    assert(backup_path(root, "24-0a-c4-aa-bb-0c", "20261116-100000") ==
           root / "24-0a-c4-aa-bb-0c/20261116-100000.bin.part");
    assert(backup_path(root, "0032001a-3130510e", "20261116-100000") ==
           root / "0032001a-3130510e/20261116-100000.bin.part");
    fs::remove_all(root);
}

} // namespace

int main() {
    picks_the_newest_backup();
    key_is_never_a_path();
    std::puts("backup ok");
}
