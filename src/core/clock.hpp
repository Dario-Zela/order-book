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
#include <cstdio>
#include <cstring>
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

// --- x86 TSC fast path (§8) -------------------------------------------
// rdtsc + measured frequency: the invariant TSC ticks uniformly across
// cores on anything modern (constant_tsc/nonstop_tsc — verified from
// /proc/cpuinfo on Linux before trusting it). Unserialised rdtsc is the
// right call for inter-thread interval stamps: ordering against
// neighbouring instructions is irrelevant when both endpoints only need a
// consistent global timeline. Calibrated against the OS clock at startup.

#if defined(__x86_64__)
inline std::uint64_t tsc_now() noexcept { return __builtin_ia32_rdtsc(); }
#endif

class TscClock {
public:
    // Calibrates ns-per-tick over `calib_ms`. supported() is false on
    // non-x86 or when the invariant-TSC flags are absent; now_ns() then
    // falls back to the OS clock, so callers never branch.
    explicit TscClock(unsigned calib_ms = 20) {
#if defined(__x86_64__)
        supported_ = has_invariant_tsc();
        if (supported_) {
            const std::uint64_t w0 = now_ns();
            const std::uint64_t t0 = tsc_now();
            const std::uint64_t spin_until = w0 + calib_ms * 1'000'000ull;
            while (now_ns() < spin_until) {
            }
            const std::uint64_t w1 = now_ns();
            const std::uint64_t t1 = tsc_now();
            ns_per_tick_ = static_cast<double>(w1 - w0) / static_cast<double>(t1 - t0);
            base_tsc_ = t1;
            base_ns_ = w1;
        }
#else
        (void)calib_ms;
#endif
    }

    [[nodiscard]] bool supported() const noexcept { return supported_; }
    [[nodiscard]] double ns_per_tick() const noexcept { return ns_per_tick_; }

    [[nodiscard]] std::uint64_t clock_now_ns() const noexcept {
#if defined(__x86_64__)
        if (supported_) {
            const std::uint64_t dt = tsc_now() - base_tsc_;
            return base_ns_ + static_cast<std::uint64_t>(static_cast<double>(dt) *
                                                         ns_per_tick_);
        }
#endif
        return now_ns();
    }

private:
#if defined(__x86_64__) && defined(__linux__)
    static bool has_invariant_tsc() {
        std::FILE* f = std::fopen("/proc/cpuinfo", "r");
        if (f == nullptr) return false;
        char line[512];
        bool constant = false;
        bool nonstop = false;
        while (std::fgets(line, sizeof(line), f) != nullptr) {
            if (std::strstr(line, "constant_tsc") != nullptr) constant = true;
            if (std::strstr(line, "nonstop_tsc") != nullptr) nonstop = true;
            if (constant && nonstop) break;
        }
        std::fclose(f);
        return constant && nonstop;
    }
#elif defined(__x86_64__)
    static bool has_invariant_tsc() { return true; }  // macOS x86: Nehalem+
#endif

    bool supported_ = false;
    double ns_per_tick_ = 0.0;
#if defined(__x86_64__)
    std::uint64_t base_tsc_ = 0;  // used only on the TSC path
    std::uint64_t base_ns_ = 0;
#endif
};

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
