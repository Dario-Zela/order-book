// Id-map microbenchmarks (DESIGN §5.2, §10 target: find hit < 30 ns).
//
// Methodology (§8): keys follow the replay-realistic pattern — dense
// sequential ITCH refs with deletes as frequent as inserts — NOT
// uniform-random keys; hash-map numbers change materially with realism.
// DoNotOptimize/ClobberMemory keep the optimiser from deleting the work.
// std::unordered_map columns quantify the §5.2 justification.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include "core/id_map.hpp"

namespace {

// Steady-state population with sequential keys and churned holes, plus a
// lookup schedule mixing hits and misses like E/X/D/U traffic does.
struct Workload {
    std::vector<std::uint64_t> live;
    std::vector<std::uint64_t> lookups;  // 90% hits, 10% dead refs
    std::uint64_t next_ref = 1;

    explicit Workload(std::size_t n_live, std::uint64_t seed) {
        std::mt19937_64 rng(seed);
        std::vector<std::uint64_t> dead;
        while (live.size() < n_live) {
            live.push_back(next_ref++);
            if (rng() % 2 == 0 && live.size() > 1) {  // churn: delete a random live
                const auto idx = rng() % live.size();
                dead.push_back(live[idx]);
                live[idx] = live.back();
                live.pop_back();
            }
        }
        for (std::size_t i = 0; i < n_live * 4; ++i) {
            const bool hit = rng() % 10 != 0;
            if (hit || dead.empty()) {
                lookups.push_back(live[rng() % live.size()]);
            } else {
                lookups.push_back(dead[rng() % dead.size()]);
            }
        }
    }
};

constexpr std::size_t kLive = 1u << 20;  // ~1M live orders, mid-day realistic

void BM_IdMap_FindMixed(benchmark::State& state) {
    const Workload w(kLive, 42);
    ob::IdMap<std::uint64_t> m(kLive * 2);
    for (auto k : w.live) m.insert(k, k);
    std::size_t i = 0;
    for (auto _ : state) {
        auto* p = m.find(w.lookups[i]);
        benchmark::DoNotOptimize(p);
        i = (i + 1) % w.lookups.size();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}
BENCHMARK(BM_IdMap_FindMixed);

void BM_StdUnorderedMap_FindMixed(benchmark::State& state) {
    const Workload w(kLive, 42);
    std::unordered_map<std::uint64_t, std::uint64_t> m;
    m.reserve(kLive * 2);
    for (auto k : w.live) m.emplace(k, k);
    std::size_t i = 0;
    for (auto _ : state) {
        auto it = m.find(w.lookups[i]);
        benchmark::DoNotOptimize(it);
        i = (i + 1) % w.lookups.size();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}
BENCHMARK(BM_StdUnorderedMap_FindMixed);

void BM_IdMap_InsertEraseChurn(benchmark::State& state) {
    ob::IdMap<std::uint64_t> m(kLive * 2);
    std::uint64_t next = 1;
    std::vector<std::uint64_t> live;
    live.reserve(kLive);
    for (std::size_t i = 0; i < kLive / 2; ++i) {
        m.insert(next, next);
        live.push_back(next++);
    }
    std::mt19937_64 rng(7);
    for (auto _ : state) {
        m.insert(next, next);
        live.push_back(next++);
        const auto idx = rng() % live.size();
        m.erase(live[idx]);
        live[idx] = live.back();
        live.pop_back();
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * 2);
}
BENCHMARK(BM_IdMap_InsertEraseChurn);

void BM_StdUnorderedMap_InsertEraseChurn(benchmark::State& state) {
    std::unordered_map<std::uint64_t, std::uint64_t> m;
    m.reserve(kLive * 2);
    std::uint64_t next = 1;
    std::vector<std::uint64_t> live;
    live.reserve(kLive);
    for (std::size_t i = 0; i < kLive / 2; ++i) {
        m.emplace(next, next);
        live.push_back(next++);
    }
    std::mt19937_64 rng(7);
    for (auto _ : state) {
        m.emplace(next, next);
        live.push_back(next++);
        const auto idx = rng() % live.size();
        m.erase(live[idx]);
        live[idx] = live.back();
        live.pop_back();
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * 2);
}
BENCHMARK(BM_StdUnorderedMap_InsertEraseChurn);

}  // namespace
