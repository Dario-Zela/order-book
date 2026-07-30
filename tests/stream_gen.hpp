#pragma once

// Shared deterministic hostile-stream generator: multi-symbol, crossed
// pre-open phase, replaces, clamps, unknown refs, wrong-locate ops, far
// out-of-band prices. Same seed -> same bytes, always.

#include <cstdint>
#include <vector>

#include "core/types.hpp"
#include "wire.hpp"

#include <random>

namespace obtest {

inline Wire make_stream(std::uint64_t seed, int n_msgs, std::vector<ob::StockLocate>& locates) {
    using ob::OrderId;
    using ob::Price;
    using ob::StockLocate;

    struct LiveOrder {
        OrderId ref;
        StockLocate locate;
    };

    std::mt19937_64 rng(seed);
    Wire w;
    locates = {1, 2, 3, 4};
    const std::vector<Price> mids = {10'000, 500'000, 2'000'000, 80};
    std::vector<LiveOrder> live;
    OrderId next_ref = 1;

    for (StockLocate loc : locates) {
        w.halt('T', loc);
    }
    for (int i = 0; i < n_msgs; ++i) {
        if (i == n_msgs / 4) w.sys_event('Q');  // open mid-stream: crossed pre-open books
        const auto choice = rng() % 100;
        const auto li = rng() % locates.size();
        const StockLocate loc = locates[li];
        if (choice < 45 || live.empty()) {
            // Add around the symbol's mid; occasional far-out price.
            const Price mid = mids[li];
            Price px;
            const auto shape = rng() % 100;
            if (shape < 90) {
                px = mid + static_cast<Price>(rng() % 200);  // near touch
                px = px > 100 ? px - 100 : 1;
            } else if (shape < 97) {
                px = mid + static_cast<Price>(rng() % 30'000);  // forces band growth
            } else {
                px = mid * 4 + static_cast<Price>(rng() % 1000) + 300'000;  // overflow
            }
            const char side = (rng() % 2 == 0) ? 'B' : 'S';
            const auto qty = static_cast<std::uint32_t>(1 + rng() % 900);
            w.add(next_ref, side, qty, px, loc);
            live.push_back({next_ref, loc});
            ++next_ref;
        } else {
            const auto pick = rng() % live.size();
            LiveOrder o = live[pick];
            const auto op = rng() % 100;
            if (op < 30) {  // execute (sometimes clamping over-size)
                w.exec(o.ref, static_cast<std::uint32_t>(1 + rng() % 400), 1, o.locate);
            } else if (op < 45) {  // execute with price, mixed printable
                w.exec_price(o.ref, static_cast<std::uint32_t>(1 + rng() % 400),
                             (rng() % 3 == 0) ? 'N' : 'Y',
                             mids[li] + static_cast<Price>(rng() % 50), o.locate);
            } else if (op < 60) {  // partial cancel
                w.cancel(o.ref, static_cast<std::uint32_t>(1 + rng() % 300), o.locate);
            } else if (op < 75) {  // delete
                w.del(o.ref, o.locate);
                live[pick] = live.back();
                live.pop_back();
            } else if (op < 90) {  // replace: new ref, maybe new price
                const Price npx = mids[o.locate == 4 ? 3 : o.locate - 1] +
                                  static_cast<Price>(rng() % 250);
                w.replace(o.ref, next_ref, static_cast<std::uint32_t>(1 + rng() % 700),
                          npx, o.locate);
                live[pick] = {next_ref, o.locate};
                ++next_ref;
            } else if (op < 95) {  // hostile: unknown ref
                w.exec(next_ref + 1'000'000 + rng() % 1000,
                       static_cast<std::uint32_t>(1 + rng() % 100), 1, loc);
            } else {  // hostile: op through the WRONG locate
                w.del(o.ref, static_cast<StockLocate>((o.locate % 4) + 1));
            }
        }
    }
    return w;
}

}  // namespace obtest
