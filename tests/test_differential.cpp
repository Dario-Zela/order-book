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
#include "stream_gen.hpp"
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

using obtest::make_stream;

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
