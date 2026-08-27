// CLOCK_MONOTONIC_RAW timestamp source for benchmark timing.
//
// Invariants:
// - All timestamps in this benchmark are CLOCK_MONOTONIC_RAW nanoseconds.
// - Two reads on the same host are directly comparable; two reads on
//   different hosts are not.
#pragma once

#include <cstdint>
#include <ctime>

namespace pulsarbench {

inline std::uint64_t monotonic_nanos() noexcept {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

}  // namespace pulsarbench
