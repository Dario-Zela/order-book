// SPSC ring A/B (DESIGN §7): the cached-index optimisation, measured. Each
// iteration transfers a fixed batch between two real threads; items/sec is
// the comparable number. Run variants interleaved (--benchmark_filter) on a
// quiet machine for the README table.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <thread>

#include "spsc/spsc_ring.hpp"

namespace {

struct Slot {
    std::uint64_t a;
    std::uint64_t b;
};

template <bool kCached>
void run_transfer(benchmark::State& state) {
    constexpr std::uint64_t kCount = 2'000'000;
    for (auto _ : state) {
        ob::spsc::SpscRing<Slot, 4096, kCached> ring;
        std::thread producer([&] {
            for (std::uint64_t i = 0; i < kCount; ++i) {
                ring.push({i, i ^ 0x9E3779B97F4A7C15ull});
            }
        });
        Slot out{};
        std::uint64_t got = 0;
        std::uint64_t sink = 0;
        while (got < kCount) {
            if (ring.try_pop(out)) {
                sink ^= out.b;
                ++got;
            } else {
                ob::spsc::cpu_relax();
            }
        }
        producer.join();
        benchmark::DoNotOptimize(sink);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(kCount));
}

void BM_Spsc_CachedIndices(benchmark::State& state) { run_transfer<true>(state); }
void BM_Spsc_UncachedIndices(benchmark::State& state) { run_transfer<false>(state); }
BENCHMARK(BM_Spsc_CachedIndices)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK(BM_Spsc_UncachedIndices)->Unit(benchmark::kMillisecond)->UseRealTime();

}  // namespace
