#pragma once

// OS console and session helpers: stop signals, binary stdio, display detection, and opening URLs.

#include <string>

namespace malgrind::host::os {

void install_stop_handler(void (*on_stop)());
void binary_stdio();
[[nodiscard]] bool has_display();
void open_url(const std::string& url);

} // namespace malgrind::host::os
