#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace malgrind::host::os::detail {

// Emits complete non-empty lines separated by \r or \n, or flushes trailing buffered text when `all` is true.
void flush_lines(std::string& pending, const std::function<void(std::string_view)>& on_line, bool all);

} // namespace malgrind::host::os::detail
