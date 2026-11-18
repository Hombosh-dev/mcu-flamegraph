#include "os/paths.hpp"

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

#include <cstdlib>
#include <memory>

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

std::string wide_to_utf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::optional<fs::path> known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(id, 0, nullptr, &raw);
    if (FAILED(hr) || !raw) {
        if (raw) CoTaskMemFree(raw);
        return std::nullopt;
    }
    fs::path p(raw);
    CoTaskMemFree(raw);
    return p;
}

} // namespace

std::optional<std::string> get_env(std::string_view name) {
    const std::wstring wname = utf8_to_wide(name);
    wchar_t* raw = nullptr;
    std::size_t len = 0;
    if (_wdupenv_s(&raw, &len, wname.c_str()) != 0 || !raw) return std::nullopt;
    const std::unique_ptr<wchar_t, decltype(&std::free)> guard(raw, &std::free);
    return wide_to_utf8(raw);
}

void set_env(std::string_view name, std::string_view value) {
    _wputenv_s(utf8_to_wide(name).c_str(), utf8_to_wide(value).c_str());
}

void unset_env(std::string_view name) {
    _wputenv_s(utf8_to_wide(name).c_str(), L"");
}

fs::path home_dir() {
    if (auto p = known_folder(FOLDERID_Profile)) return *p;
    if (const auto env = get_env("USERPROFILE"); env && !env->empty()) return from_utf8(*env);
    return {};
}

fs::path user_data_dir() {
    if (auto p = known_folder(FOLDERID_LocalAppData)) return *p;
    if (const auto env = get_env("LOCALAPPDATA"); env && !env->empty()) return from_utf8(*env);
    return {};
}

fs::path executable_path() {
    std::wstring buf(1024, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            return fs::path(buf);
        }
        if (buf.size() >= 32768) return {};
        buf.resize(buf.size() * 2);
    }
}

std::string current_user() {
    DWORD len = 0;
    GetUserNameW(nullptr, &len);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0) return {};
    std::wstring buf(len, L'\0');
    if (!GetUserNameW(buf.data(), &len) || len == 0) return {};
    buf.resize(len - 1);
    return wide_to_utf8(buf);
}

std::string to_utf8(const fs::path& p) { return wide_to_utf8(p.native()); }

fs::path from_utf8(std::string_view s) { return fs::path(utf8_to_wide(s)); }

std::tm utc_tm(std::time_t t) {
    std::tm tm{};
    gmtime_s(&tm, &t);
    return tm;
}

std::tm local_tm(std::time_t t) {
    std::tm tm{};
    localtime_s(&tm, &t);
    return tm;
}

} // namespace malgrind::host::os
