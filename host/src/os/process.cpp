#include "os/process.hpp"

#include "os/process_internal.hpp"

#include <cstdint>

namespace malgrind::host::os {

namespace {

// Locale-independent UTF-8 -> wide conversion so windows_command_line() behaves identically on every OS.
std::wstring utf8_to_wstring(std::string_view s) {
    std::wstring out;
    out.reserve(s.size());
    auto append_cp = [&](std::uint32_t cp) {
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp <= 0xFFFF) {
                out.push_back(static_cast<wchar_t>(cp));
            } else {
                cp -= 0x10000;
                out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
                out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
            }
        } else {
            out.push_back(static_cast<wchar_t>(cp));
        }
    };
    auto is_cont = [](unsigned char b) { return (b & 0xC0) == 0x80; };
    for (std::size_t i = 0; i < s.size();) {
        const auto b0 = static_cast<unsigned char>(s[i]);
        if (b0 < 0x80) {
            append_cp(b0);
            ++i;
        } else if ((b0 & 0xE0) == 0xC0 && i + 1 < s.size() && is_cont(static_cast<unsigned char>(s[i + 1]))) {
            const std::uint32_t cp = ((b0 & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
            if (cp >= 0x80) {
                append_cp(cp);
                i += 2;
            } else {
                append_cp(0xFFFD);
                ++i;
            }
        } else if ((b0 & 0xF0) == 0xE0 && i + 2 < s.size() && is_cont(static_cast<unsigned char>(s[i + 1])) &&
                   is_cont(static_cast<unsigned char>(s[i + 2]))) {
            const std::uint32_t cp = ((b0 & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
                                     (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
            if (cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF)) {
                append_cp(cp);
                i += 3;
            } else {
                append_cp(0xFFFD);
                ++i;
            }
        } else if ((b0 & 0xF8) == 0xF0 && i + 3 < s.size() && is_cont(static_cast<unsigned char>(s[i + 1])) &&
                   is_cont(static_cast<unsigned char>(s[i + 2])) && is_cont(static_cast<unsigned char>(s[i + 3]))) {
            const std::uint32_t cp = ((b0 & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
                                     ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) |
                                     (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
            if (cp >= 0x10000 && cp <= 0x10FFFF) {
                append_cp(cp);
                i += 4;
            } else {
                append_cp(0xFFFD);
                ++i;
            }
        } else {
            append_cp(0xFFFD);
            ++i;
        }
    }
    return out;
}

} // namespace

std::wstring windows_command_line(const std::vector<std::string>& argv_utf8) {
    std::wstring cmd;
    for (std::size_t idx = 0; idx < argv_utf8.size(); ++idx) {
        if (idx > 0) cmd.push_back(L' ');
        const std::wstring arg = utf8_to_wstring(argv_utf8[idx]);
        if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
            cmd.append(arg);
            continue;
        }
        cmd.push_back(L'"');
        for (auto it = arg.begin(); it != arg.end();) {
            std::size_t backslashes = 0;
            while (it != arg.end() && *it == L'\\') {
                ++it;
                ++backslashes;
            }
            if (it == arg.end()) {
                // Closing quote follows: double trailing backslashes so they stay literal.
                cmd.append(backslashes * 2, L'\\');
                break;
            }
            if (*it == L'"') {
                cmd.append(backslashes * 2 + 1, L'\\');
                cmd.push_back(*it);
            } else {
                cmd.append(backslashes, L'\\');
                cmd.push_back(*it);
            }
            ++it;
        }
        cmd.push_back(L'"');
    }
    return cmd;
}

namespace detail {

void flush_lines(std::string& pending, const std::function<void(std::string_view)>& on_line, bool all) {
    for (std::size_t at; (at = pending.find_first_of("\r\n")) != std::string::npos;) {
        if (at > 0) on_line(std::string_view(pending).substr(0, at));
        pending.erase(0, at + 1);
    }
    if (all && !pending.empty()) {
        on_line(pending);
        pending.clear();
    }
}

} // namespace detail

} // namespace malgrind::host::os
