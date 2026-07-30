// Differential test (DESIGN §9.1): the same synthetic ITCH stream drives
// Engine<RefBook> and Engine<FlatBook>; the two books must agree exactly —
// L2 shape, per-level FIFO order, live-order counts, and engine stats.
// The stream generator is seeded and deterministic, and deliberately nasty:
// multi-symbol, crossed pre-open phase, replaces, clamps, unknown refs,
// far-out-of-band prices to force band growth and overflow.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include "book/flat_book.hpp"
#include "book/ref_book.hpp"
#include "engine/engine.hpp"
#include "itch/parser.hpp"
#include "wire.hpp"

using ob::OrderId;
using ob::Price;
using ob::Qty;
using ob::Side;
using ob::StockLocate;
using ob::book::BandConfig;
using ob::book::BookResources;
using ob::book::FlatBook;
using ob::book::RefBook;
using obtest::Wire;

namespace {

struct FlatFactory {
    BookResources* res;
    BandConfig cfg;
    std::unique_ptr<FlatBook> operator()(StockLocate loc) const {
        return std::make_unique<FlatBook>(*res, loc, cfg);
    }
};

struct LiveOrder {
    OrderId ref;
    StockLocate locate;
};

// Deterministic hostile stream. Returns the wire plus the locates it used.
Wire make_stream(std::uint64_t seed, int n_msgs, std::vector<StockLocate>& locates) {
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
            // Add around a drifting mid; occasional far-out price.
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

template <typename Eng>
void replay_into(Eng& eng, const Wire& w) {
    ob::itch::Parser p(w.bytes());
    p.run(eng);
}

}  // namespace

TEST_CASE("differential: flat book matches reference book over a hostile stream") {
    std::vector<StockLocate> locates;
    // Small band forces many growths and overflow paths; the oracle has no
    // bands at all — agreement means the banding is invisible, as it must be.
    const Wire w = make_stream(20260730, 60'000, locates);

    ob::engine::Engine<RefBook> ref_eng;
    BookResources res(1 << 16);
    const BandConfig cfg{.initial_half_width = 64, .max_width = 1u << 15};
    ob::engine::Engine<FlatBook, ob::book::NullListener, FlatFactory> flat_eng(
        {}, FlatFactory{&res, cfg});

    replay_into(ref_eng, w);
    replay_into(flat_eng, w);

    // Engine-level counters must agree exactly.
    CHECK(ref_eng.stats().unknown_ref == flat_eng.stats().unknown_ref);
    CHECK(ref_eng.stats().duplicate_ref == flat_eng.stats().duplicate_ref);
    CHECK(ref_eng.stats().clamped == flat_eng.stats().clamped);
    CHECK(ref_eng.stats().volume_lit == flat_eng.stats().volume_lit);
    CHECK(ref_eng.stats().nonprintable_execs == flat_eng.stats().nonprintable_execs);

    for (StockLocate loc : locates) {
        const RefBook* rb = ref_eng.book(loc);
        const FlatBook* fb = flat_eng.book(loc);
        REQUIRE(rb != nullptr);
        REQUIRE(fb != nullptr);
        REQUIRE(fb->validate());
        REQUIRE(rb->validate());
        CHECK(rb->live_orders() == fb->live_orders());
        CHECK(rb->best_bid() == fb->best_bid());
        CHECK(rb->best_ask() == fb->best_ask());
        for (Side s : {Side::Bid, Side::Ask}) {
            const auto rl2 = rb->l2(s);
            const auto fl2 = fb->l2(s);
            REQUIRE(rl2 == fl2);
            // FIFO order at every level, not just aggregates.
            for (const auto& lvl : rl2) {
                CHECK(rb->level_fifo(s, lvl.price) == fb->level_fifo(s, lvl.price));
            }
        }
    }

    // The stream must actually have exercised the interesting paths.
    std::uint64_t growths = 0;
    std::uint64_t overflow = 0;
    for (StockLocate loc : locates) {
        growths += flat_eng.book(loc)->stats().band_growths;
        overflow += flat_eng.book(loc)->stats().overflow_hits;
    }
    CHECK(growths > 0);
    CHECK(overflow > 0);
    CHECK(flat_eng.stats().unknown_ref > 0);
    CHECK(flat_eng.stats().clamped > 0);
    CHECK(res.ids.growths() == 0);  // sized right: no rehash during the run
}
