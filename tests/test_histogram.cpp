#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "core/clock.hpp"
#include "core/histogram.hpp"

using ob::LogHistogram;

TEST_CASE("histogram: values below 32 are exact") {
    LogHistogram h;
    for (std::uint64_t v = 0; v < 32; ++v) h.record(v);
    CHECK(h.total() == 32);
    CHECK(h.min() == 0);
    CHECK(h.max() == 31);
    CHECK(h.quantile(0.0) == 0);
    CHECK(h.quantile(1.0) == 31);
    // The k-th smallest of 0..31 is k: exact buckets must return it exactly.
    for (int k = 0; k < 32; ++k) {
        CHECK(h.quantile((static_cast<double>(k) + 0.5) / 32.0) == static_cast<std::uint64_t>(k));
    }
}

TEST_CASE("histogram: quantiles track a sorted oracle within 3.2% + top clamp") {
    LogHistogram h;
    std::mt19937_64 rng(42);
    std::vector<std::uint64_t> vals;
    // Log-uniform latencies from ~30ns to ~100ms — nine-ish decades, like
    // a real latency distribution's dynamic range.
    for (int i = 0; i < 200'000; ++i) {
        const double e = 1.5 + 6.5 * std::uniform_real_distribution<>(0, 1)(rng);
        const auto v = static_cast<std::uint64_t>(std::pow(10.0, e));
        vals.push_back(v);
        h.record(v);
    }
    std::sort(vals.begin(), vals.end());
    for (double q : {0.5, 0.9, 0.99, 0.999}) {
        const auto exact = vals[static_cast<std::size_t>(q * static_cast<double>(vals.size()))];
        const auto est = h.quantile(q);
        CHECK(est >= exact);  // conservative: upper edge, never understates
        CHECK(static_cast<double>(est) <= static_cast<double>(exact) * 1.033);
    }
    CHECK(h.max() == vals.back());
    CHECK(h.min() == vals.front());
}

TEST_CASE("histogram: quantile is monotone in q") {
    LogHistogram h;
    std::mt19937_64 rng(7);
    for (int i = 0; i < 50'000; ++i) h.record(rng() % 1'000'000);
    std::uint64_t prev = 0;
    for (double q = 0.0; q <= 1.0; q += 0.01) {
        const auto v = h.quantile(q);
        CHECK(v >= prev);
        prev = v;
    }
}

TEST_CASE("histogram: merge equals recording into one") {
    LogHistogram a;
    LogHistogram b;
    LogHistogram both;
    std::mt19937_64 rng(3);
    for (int i = 0; i < 10'000; ++i) {
        const auto v = rng() % 500'000;
        (i % 2 == 0 ? a : b).record(v);
        both.record(v);
    }
    a.merge(b);
    CHECK(a.total() == both.total());
    CHECK(a.max() == both.max());
    CHECK(a.min() == both.min());
    for (double q : {0.1, 0.5, 0.9, 0.99}) CHECK(a.quantile(q) == both.quantile(q));
}

TEST_CASE("histogram: empty and huge values behave") {
    LogHistogram h;
    CHECK(h.total() == 0);
    CHECK(h.quantile(0.99) == 0);
    h.record(~std::uint64_t{0});
    CHECK(h.quantile(0.5) == ~std::uint64_t{0});
    CHECK(h.max() == ~std::uint64_t{0});
}

TEST_CASE("clock: monotone and cheap") {
    const auto a = ob::now_ns();
    const auto b = ob::now_ns();
    CHECK(b >= a);
    const auto cost = ob::clock_overhead_ns();
    CHECK(cost < 1'000);  // a clock costing >1us couldn't support ns claims
}

TEST_CASE("tsc clock: agrees with the OS clock where supported") {
    const ob::TscClock tsc(10);
#if defined(__x86_64__)
    if (tsc.supported()) {  // exercised for real on x86 CI runners
        const auto w0 = ob::now_ns();
        const auto t0 = tsc.clock_now_ns();
        ob::clock_calibration_sink += ob::clock_overhead_ns();  // ~1ms of work
        const auto w1 = ob::now_ns();
        const auto t1 = tsc.clock_now_ns();
        const double os_dt = static_cast<double>(w1 - w0);
        const double tsc_dt = static_cast<double>(t1 - t0);
        CHECK(tsc_dt > 0);
        CHECK(tsc_dt > os_dt * 0.90);  // within 10% over ~1ms
        CHECK(tsc_dt < os_dt * 1.10);
        CHECK(tsc.ns_per_tick() > 0.05);  // 20GHz upper bound sanity
        CHECK(tsc.ns_per_tick() < 10.0);  // 100MHz lower bound sanity
    }
#endif
    // The fallback must always be a working clock, everywhere.
    const auto a = tsc.clock_now_ns();
    const auto b = tsc.clock_now_ns();
    CHECK(b >= a);
}
