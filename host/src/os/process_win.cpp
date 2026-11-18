#include "os/process.hpp"

#include "os/paths.hpp"
#include "os/process_internal.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cwchar>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace malgrind::host::os {

namespace {

std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

bool is_batch_ext(std::string_view ext) { return iequals(ext, ".bat") || iequals(ext, ".cmd"); }

bool has_batch_ext(std::string_view path) {
    return path.size() >= 4 && is_batch_ext(path.substr(path.size() - 4));
}

std::vector<fs::path> pathext_candidates(const std::string& name) {
    const fs::path base = from_utf8(name);
    if (base.has_extension()) {
        if (is_batch_ext(to_utf8(base.extension()))) return {};
        return {base};
    }
    std::vector<fs::path> out;
    const std::string pathext = get_env("PATHEXT").value_or(".COM;.EXE");
    const std::string effective = pathext.empty() ? ".COM;.EXE" : pathext;
    std::size_t pos = 0;
    while (pos <= effective.size()) {
        const auto next = effective.find(';', pos);
        const auto raw = next == std::string::npos ? std::string_view(effective).substr(pos)
                                                   : std::string_view(effective).substr(pos, next - pos);
        if (!raw.empty()) {
            std::string ext = raw.front() == '.' ? std::string(raw) : "." + std::string(raw);
            if (!is_batch_ext(ext)) out.push_back(from_utf8(name + ext));
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return out;
}

std::optional<fs::path> resolve_program(const std::string& arg0) {
    if (arg0.empty() || has_batch_ext(arg0)) return std::nullopt;
    const bool has_sep = arg0.find('/') != std::string::npos || arg0.find('\\') != std::string::npos;
    std::optional<fs::path> found;
    if (!has_sep) {
        found = find_in_path(arg0);
    } else {
        const fs::path p = from_utf8(arg0);
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) {
            found = p;
        } else if (!p.has_extension()) {
            const fs::path with_exe = from_utf8(arg0 + ".exe");
            if (fs::is_regular_file(with_exe, ec)) found = with_exe;
        }
    }
    if (!found || has_batch_ext(to_utf8(*found))) return std::nullopt;
    std::error_code ec;
    const fs::path abs = fs::absolute(*found, ec);
    return ec ? *found : abs;
}

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE h) noexcept : h_(h) {}
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : h_(std::exchange(other.h_, INVALID_HANDLE_VALUE)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset();
            h_ = std::exchange(other.h_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    ~UniqueHandle() { reset(); }

    void reset(HANDLE h = INVALID_HANDLE_VALUE) noexcept {
        if (valid()) CloseHandle(h_);
        h_ = h;
    }
    [[nodiscard]] bool valid() const noexcept { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }
    [[nodiscard]] HANDLE get() const noexcept { return h_; }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

class ProcThreadAttrList {
public:
    ~ProcThreadAttrList() {
        if (list_) DeleteProcThreadAttributeList(list_);
    }

    bool init(HANDLE* handles, std::size_t count) {
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        if (bytes == 0) return false;
        buf_.resize(bytes);
        auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buf_.data());
        if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes)) return false;
        list_ = list;
        return UpdateProcThreadAttribute(list_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, count * sizeof(HANDLE), nullptr, nullptr) != 0;
    }

    [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept { return list_; }

private:
    std::vector<std::byte> buf_;
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
};

struct EnvStringsGuard {
    LPWCH ptr = nullptr;
    ~EnvStringsGuard() {
        if (ptr) FreeEnvironmentStringsW(ptr);
    }
};

std::wstring env_key(std::wstring_view entry) {
    const std::size_t start = (!entry.empty() && entry[0] == L'=') ? 1 : 0;
    const std::size_t eq = entry.find(L'=', start);
    return std::wstring(eq == std::wstring_view::npos ? entry : entry.substr(0, eq));
}

std::vector<wchar_t> build_env_block(const std::vector<std::string>& env_add) {
    struct Added {
        std::wstring key;
        std::wstring entry;
    };
    std::vector<Added> added;
    for (const auto& item : env_add) {
        const std::wstring wide = utf8_to_wide(item);
        const std::size_t eq = wide.find(L'=');
        if (eq == std::wstring::npos || eq == 0) continue;
        std::wstring key = wide.substr(0, eq);
        const auto existing = std::ranges::find_if(added, [&](const Added& a) { return _wcsicmp(a.key.c_str(), key.c_str()) == 0; });
        if (existing != added.end()) existing->entry = wide;
        else added.push_back({std::move(key), wide});
    }

    std::vector<std::wstring> entries;
    EnvStringsGuard raw{GetEnvironmentStringsW()};
    if (raw.ptr) {
        for (const wchar_t* p = raw.ptr; *p; p += std::wcslen(p) + 1) {
            const std::wstring_view entry(p);
            const std::wstring key = env_key(entry);
            const bool replaced = !key.empty() && entry[0] != L'=' &&
                                  std::ranges::any_of(added, [&](const Added& a) { return _wcsicmp(a.key.c_str(), key.c_str()) == 0; });
            if (!replaced) entries.emplace_back(entry);
        }
    }
    for (auto& a : added) entries.push_back(std::move(a.entry));

    std::ranges::sort(entries, [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(env_key(a).c_str(), env_key(b).c_str()) < 0;
    });

    std::vector<wchar_t> block;
    for (const auto& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    if (block.empty()) block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

UniqueHandle open_nul(DWORD access) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;
    return UniqueHandle(CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
}

std::optional<int> spawn_captured(const std::vector<std::string>& argv, const std::vector<std::string>& env_add, bool merge_stderr,
                                  const std::function<void(std::string_view)>& on_chunk) {
    if (argv.empty()) return std::nullopt;
    const auto exe = resolve_program(argv[0]);
    if (!exe) return std::nullopt;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;

    HANDLE read_raw = nullptr;
    HANDLE write_raw = nullptr;
    if (!CreatePipe(&read_raw, &write_raw, &sa, 0)) return std::nullopt;
    UniqueHandle pipe_read(read_raw);
    UniqueHandle pipe_write(write_raw);
    if (!SetHandleInformation(pipe_read.get(), HANDLE_FLAG_INHERIT, 0)) return std::nullopt;

    UniqueHandle nul_in = open_nul(GENERIC_READ);
    if (!nul_in.valid()) return std::nullopt;

    UniqueHandle nul_err;
    if (!merge_stderr) {
        nul_err = open_nul(GENERIC_WRITE);
        if (!nul_err.valid()) return std::nullopt;
    }

    // Inherit only the NUL and pipe write handles so open serial ports or other descriptors never leak to children.
    std::vector<HANDLE> inherit_handles{nul_in.get(), pipe_write.get()};
    if (!merge_stderr) inherit_handles.push_back(nul_err.get());

    ProcThreadAttrList attrs;
    if (!attrs.init(inherit_handles.data(), inherit_handles.size())) return std::nullopt;

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul_in.get();
    si.StartupInfo.hStdOutput = pipe_write.get();
    si.StartupInfo.hStdError = merge_stderr ? pipe_write.get() : nul_err.get();
    si.lpAttributeList = attrs.get();

    std::wstring cmd = windows_command_line(argv);
    std::vector<wchar_t> env_block = build_env_block(env_add);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(exe->native().c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                                   env_block.data(), nullptr, &si.StartupInfo, &pi);

    // Close write ends in the parent right away so ReadFile sees EOF once the child finishes.
    pipe_write.reset();
    nul_in.reset();
    nul_err.reset();

    if (!ok) return std::nullopt;

    UniqueHandle process(pi.hProcess);
    CloseHandle(pi.hThread);

    std::array<char, 4096> buf;
    for (;;) {
        DWORD bytes_read = 0;
        const BOOL got = ReadFile(pipe_read.get(), buf.data(), static_cast<DWORD>(buf.size()), &bytes_read, nullptr);
        if (got && bytes_read > 0) {
            on_chunk(std::string_view(buf.data(), static_cast<std::size_t>(bytes_read)));
        } else {
            break;
        }
    }
    pipe_read.reset();

    WaitForSingleObject(process.get(), INFINITE);
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process.get(), &exit_code)) return -1;
    return static_cast<int>(exit_code);
}

} // namespace

std::optional<fs::path> find_in_path(const std::string& name) {
    if (name.empty()) return std::nullopt;
    const auto candidates = pathext_candidates(name);
    if (candidates.empty()) return std::nullopt;
    const auto path = get_env("PATH");
    if (!path) return std::nullopt;
    for (const auto& dir : split_path_list(*path)) {
        for (const auto& cand : candidates) {
            const fs::path full = dir / cand;
            std::error_code ec;
            if (fs::is_regular_file(full, ec)) return full;
        }
    }
    return std::nullopt;
}

std::string capture(const std::vector<std::string>& argv) {
    std::string out;
    if (!spawn_captured(argv, {}, false, [&](std::string_view chunk) { out.append(chunk); })) return {};
    return out;
}

int run(const std::vector<std::string>& argv) {
    if (argv.empty()) return -1;
    const auto exe = resolve_program(argv[0]);
    if (!exe) return -1;
    STARTUPINFOW si{};
    si.cb = sizeof(STARTUPINFOW);
    std::wstring cmd = windows_command_line(argv);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe->native().c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) return -1;
    UniqueHandle process(pi.hProcess);
    CloseHandle(pi.hThread);
    WaitForSingleObject(process.get(), INFINITE);
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process.get(), &exit_code)) return -1;
    return static_cast<int>(exit_code);
}

int run_lines(const std::vector<std::string>& argv, const std::vector<std::string>& env_add,
              const std::function<void(std::string_view line)>& on_line) {
    std::string pending;
    const auto code = spawn_captured(argv, env_add, true, [&](std::string_view chunk) {
        pending.append(chunk);
        detail::flush_lines(pending, on_line, false);
    });
    if (!code) return -1;
    detail::flush_lines(pending, on_line, true);
    return *code;
}

} // namespace malgrind::host::os
