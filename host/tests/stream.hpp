#pragma once

// Test data shared by the host tests: a byte stream made by the real encoder in core/.

#include "malgrind/profiler.hpp"

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

// `flushes` rounds of a 2-core Xtensa target = 2 * flushes frames. Per round: core 0 reports a <- b <- main three
// times (stack 0x400d1000, 0x400d2000, 0x400d3000), core 1 reports c <- b <- main once (0x400d1800 as the leaf).
inline std::vector<std::uint8_t> make_stream(unsigned flushes) {
    using namespace malgrind;
    Profiler p({"test", Arch::Xtensa, 240'000'000, 2}, 997);
    const std::uint32_t leaf_a[] = {0x400d1000, 0x400d2000, 0x400d3000};
    const std::uint32_t leaf_c[] = {0x400d1800, 0x400d2000, 0x400d3000};
    std::vector<std::uint8_t> stream;
    for (unsigned i = 0; i < flushes; ++i) {
        for (int k = 0; k < 3; ++k) p.record(0, leaf_a, 3, 100);
        p.record(1, leaf_c, 3, 100);
        p.flush([&](const std::uint8_t* d, std::size_t n) { stream.insert(stream.end(), d, d + n); }, [] {}, 5000);
    }
    return stream;
}

template <class Pred>
bool wait_for(Pred pred, std::chrono::milliseconds limit = std::chrono::milliseconds(3000)) {
    using namespace std::chrono_literals;
    for (const auto end = std::chrono::steady_clock::now() + limit; std::chrono::steady_clock::now() < end;
         std::this_thread::sleep_for(10ms))
        if (pred()) return true;
    return false;
}
