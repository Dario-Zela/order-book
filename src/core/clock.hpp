#pragma once

// Monotonic nanosecond clock shim (DESIGN §8).
//
// Per-platform choices, documented honestly:
// - Apple: clock_gettime_nsec_np(CLOCK_UPTIME_RAW) — reads cntvct_el0 plus a
//   timebase multiply in userspace (commpage), no syscall; ~20-40 ns per
//   call on M-series. mach_absolute_time() is the same counter without the
//   ns conversion.
// - Linux/x86: clock_gettime(CLOCK_MONOTONIC_RAW) via vDSO — rdtsc plus
//   scaling, no syscall on any sane config. A raw rdtsc fast path (measured
//   frequency, constant_tsc/nonstop_tsc checked in cpuinfo) is a planned
//   refinement for the canonical Linux x86 run; for inter-thread interval
//   stamps, unserialised rdtsc would be acceptable because we do not need
//   ordering against surrounding instructions, only a consistent timeline
//   across cores (guaranteed by invariant TSC).
//
// The cost of now_ns() itself bounds what latency claims can be made —
// measure it with clock_overhead_ns() and report it next to any histogram.

#include <cstdint>
#include <time.h>

namespace ob {

// External linkage: the optimiser must assume another TU reads it, so the
// timing loop below cannot be deleted.
inline std::uint64_t clock_calibration_sink = 0;

inline std::uint64_t now_ns() noexcept {
#if defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}

// Median-of-batches estimate of one now_ns() call, in ns. Cheap (~1 ms).
inline std::uint64_t clock_overhead_ns() noexcept {
    constexpr int kBatches = 9;
    constexpr std::uint64_t kCalls = 10'000;
    std::uint64_t samples[kBatches];
    for (int b = 0; b < kBatches; ++b) {
        const std::uint64_t t0 = now_ns();
        std::uint64_t sink = 0;
        for (std::uint64_t i = 0; i < kCalls; ++i) sink += now_ns();
        const std::uint64_t t1 = now_ns();
        samples[b] = (t1 - t0) / kCalls;
        clock_calibration_sink += sink;
    }
    // insertion sort, take the median
    for (int i = 1; i < kBatches; ++i) {
        for (int j = i; j > 0 && samples[j] < samples[j - 1]; --j) {
            const auto t = samples[j];
            samples[j] = samples[j - 1];
            samples[j - 1] = t;
        }
    }
    return samples[kBatches / 2];
}

}  // namespace ob
