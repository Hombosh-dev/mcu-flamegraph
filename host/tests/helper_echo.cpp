#include "os/paths.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

std::vector<std::string> utf8_args(int argc, char** argv) {
    std::vector<std::string> out;
#ifdef _WIN32
    int wargc = 0;
    if (LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc)) {
        out.reserve(static_cast<std::size_t>(wargc));
        for (int i = 0; i < wargc; ++i) out.push_back(malgrind::host::os::to_utf8(std::filesystem::path(wargv[i])));
        LocalFree(wargv);
        return out;
    }
#endif
    out.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
    return out;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    const auto args = utf8_args(argc, argv);
    for (std::size_t i = 1; i < args.size(); ++i) std::printf("%s\n", args[i].c_str());

    const auto var = malgrind::host::os::get_env("HELPER_VAR").value_or("");
    std::printf("HELPER_VAR=%s\n", var.c_str());

    if (const auto raw = malgrind::host::os::get_env("HELPER_RAW"); raw && !raw->empty()) {
        std::fwrite(raw->data(), 1, raw->size(), stdout);
    }
    std::fflush(stdout);
    if (const auto err = malgrind::host::os::get_env("HELPER_ERR"); err && !err->empty()) {
        std::fprintf(stderr, "%s\n", err->c_str());
    }
    std::fflush(nullptr);

    if (const auto code = malgrind::host::os::get_env("HELPER_EXIT"); code && !code->empty()) {
        return std::stoi(*code);
    }
    return 0;
}
