// Match-mode throughput (§6): submits/sec against a populated book. Each
// iteration is a rest + a crossing IOC that consumes it, so the book returns
// to its baseline — no unbounded growth, steady-state numbers.

#include <benchmark/benchmark.h>

#include <cstdint>

#include "book/flat_book.hpp"
#include "engine/match.hpp"

namespace {

using ob::OrderId;
using ob::Price;
using ob::Qty;
using ob::Side;

void BM_Match_RestThenIocFill(benchmark::State& state) {
    ob::book::BookResources res(1 << 16);
    ob::book::FlatBook book(res, 1);
    // Baseline ladder: 20 levels a side, 4 orders each, around 10000/10010.
    OrderId ref = 1;
    for (int lvl = 0; lvl < 20; ++lvl) {
        for (int k = 0; k < 4; ++k) {
            book.add(ref++, Side::Bid, static_cast<Price>(9990 - lvl * 10), 100);
            book.add(ref++, Side::Ask, static_cast<Price>(10020 + lvl * 10), 100);
        }
    }
    ob::engine::NullMatchHooks hooks;
    for (auto _ : state) {
        // Rest a bid inside the spread, then lift it with a crossing IOC ask.
        auto r1 = ob::engine::submit(book, ref, Side::Bid, 10000, 100,
                                           ob::engine::Tif::limit, hooks);
        auto r2 = ob::engine::submit(book, ref + 1, Side::Ask, 10000, 100,
                                           ob::engine::Tif::ioc, hooks);
        benchmark::DoNotOptimize(r1);
        benchmark::DoNotOptimize(r2);
        ref += 2;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * 2);
}
BENCHMARK(BM_Match_RestThenIocFill);

void BM_Match_MultiLevelSweep(benchmark::State& state) {
    // A taker that walks 5 levels per submit; book rebuilt outside timing.
    ob::book::BookResources res(1 << 20);
    ob::book::FlatBook book(res, 1);
    ob::engine::NullMatchHooks hooks;
    OrderId ref = 1;
    for (auto _ : state) {
        state.PauseTiming();
        for (int lvl = 0; lvl < 5; ++lvl) {
            book.add(ref++, Side::Ask, static_cast<Price>(10010 + lvl), 100);
        }
        state.ResumeTiming();
        auto r = ob::engine::submit(book, ref++, Side::Bid, 10014, 500,
                                          ob::engine::Tif::ioc, hooks);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}
BENCHMARK(BM_Match_MultiLevelSweep);

}  // namespace
