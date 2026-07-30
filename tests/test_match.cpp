// Match mode (DESIGN §6, §9.5): unit tests for the walk semantics, property
// tests over random flow (conservation, priority, no-crossed-book), and the
// oracle treatment — RefBook and FlatBook must emit IDENTICAL fill streams.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>
#include <vector>

#include "book/flat_book.hpp"
#include "book/ref_book.hpp"
#include "engine/engine.hpp"
#include "engine/match.hpp"

using ob::OrderId;
using ob::Price;
using ob::Qty;
using ob::Side;
using ob::StockLocate;
using ob::book::BookResources;
using ob::book::FlatBook;
using ob::book::RefBook;
using ob::engine::Fill;
using ob::engine::SubmitStatus;
using ob::engine::Tif;

namespace {

struct FillTape {
    std::vector<Fill> fills;
    void on_fill(const Fill& f, const ob::book::Effect&) { fills.push_back(f); }
    void on_rest(const ob::book::Effect&) {}
};

bool same_fills(const std::vector<Fill>& a, const std::vector<Fill>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].taker != b[i].taker || a[i].maker != b[i].maker ||
            a[i].price != b[i].price || a[i].qty != b[i].qty) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("match: non-marketable limit rests; marketable fills at maker price") {
    RefBook b;
    FillTape t;
    auto r = ob::engine::submit(b, 1, Side::Ask, 10010, 100, Tif::limit, t);
    CHECK(r.status == SubmitStatus::rested);
    CHECK(b.best_ask() == Price{10010});

    // Buy limit ABOVE the ask: price improvement — fills AT 10010, not 10020.
    r = ob::engine::submit(b, 2, Side::Bid, 10020, 60, Tif::limit, t);
    CHECK(r.status == SubmitStatus::filled);
    CHECK(r.filled == 60);
    REQUIRE(t.fills.size() == 1);
    CHECK(t.fills[0].taker == 2);
    CHECK(t.fills[0].maker == 1);
    CHECK(t.fills[0].price == 10010);
    CHECK(t.fills[0].qty == 60);
    CHECK(b.order_snapshot(1)->remaining == Qty{40});  // maker partially filled
    CHECK(b.validate());
}

TEST_CASE("match: walks levels in price order, FIFO within each level") {
    RefBook b;
    FillTape t;
    ob::engine::submit(b, 1, Side::Ask, 10010, 50, Tif::limit, t);
    ob::engine::submit(b, 2, Side::Ask, 10010, 50, Tif::limit, t);  // behind 1
    ob::engine::submit(b, 3, Side::Ask, 10020, 50, Tif::limit, t);  // worse level
    t.fills.clear();

    auto r = ob::engine::submit(b, 10, Side::Bid, 10020, 120, Tif::limit, t);
    CHECK(r.status == SubmitStatus::filled);
    REQUIRE(t.fills.size() == 3);
    CHECK(t.fills[0].maker == 1);  // best level first, oldest first
    CHECK(t.fills[1].maker == 2);
    CHECK(t.fills[2].maker == 3);  // then the next level
    CHECK(t.fills[0].price == 10010);
    CHECK(t.fills[2].price == 10020);
    CHECK(t.fills[2].qty == 20);
    CHECK(b.order_snapshot(3)->remaining == Qty{30});
    CHECK(b.validate());
}

TEST_CASE("match: partial fill rests the remainder at the limit") {
    RefBook b;
    FillTape t;
    ob::engine::submit(b, 1, Side::Ask, 10010, 30, Tif::limit, t);
    auto r = ob::engine::submit(b, 2, Side::Bid, 10010, 100, Tif::limit, t);
    CHECK(r.status == SubmitStatus::partial_rested);
    CHECK(r.filled == 30);
    CHECK(r.resting == 70);
    CHECK(b.best_bid() == Price{10010});
    CHECK_FALSE(b.best_ask().has_value());
    CHECK(b.order_snapshot(2)->remaining == Qty{70});
    CHECK(b.validate());
}

TEST_CASE("match: IOC discards the remainder instead of resting") {
    RefBook b;
    FillTape t;
    ob::engine::submit(b, 1, Side::Ask, 10010, 30, Tif::limit, t);
    auto r = ob::engine::submit(b, 2, Side::Bid, 10015, 100, Tif::ioc, t);
    CHECK(r.status == SubmitStatus::ioc_cancelled);
    CHECK(r.filled == 30);
    CHECK(r.cancelled == 70);
    CHECK_FALSE(b.order_snapshot(2).has_value());  // nothing rested
    CHECK(b.validate());
}

TEST_CASE("match: duplicate and zero-qty submissions are rejected") {
    RefBook b;
    FillTape t;
    ob::engine::submit(b, 1, Side::Bid, 10000, 100, Tif::limit, t);
    CHECK(ob::engine::submit(b, 1, Side::Bid, 9000, 10, Tif::limit, t).status ==
          SubmitStatus::rejected_duplicate);
    CHECK(ob::engine::submit(b, 2, Side::Bid, 10000, 0, Tif::limit, t).status ==
          SubmitStatus::rejected_invalid);
    CHECK(b.live_orders() == 1);
}

TEST_CASE("match: engine rejects submits on halted symbols") {
    ob::engine::Engine<RefBook> eng;
    eng.on_trading_action({{1, 0, 0}, {}, 'H', {}});
    auto r = eng.submit(1, 1, Side::Bid, 10000, 100);
    CHECK(r.status == SubmitStatus::rejected_halted);
    CHECK(eng.stats().rejected_halted == 1);
    CHECK(eng.book(1) == nullptr);  // nothing touched the book

    eng.on_trading_action({{1, 0, 0}, {}, 'T', {}});
    CHECK(eng.submit(1, 1, Side::Bid, 10000, 100).status == SubmitStatus::rested);
}

TEST_CASE("match: engine replace re-prices through the matching path") {
    ob::engine::Engine<RefBook> eng;
    eng.submit(1, 1, Side::Ask, 10010, 50);
    eng.submit(1, 2, Side::Bid, 10000, 80);
    // Replace the bid up to 10010: side inherited, becomes marketable, fills.
    auto r = eng.replace(1, 2, 3, 10010, 80);
    CHECK(r.status == SubmitStatus::partial_rested);
    CHECK(r.filled == 50);
    CHECK(r.resting == 30);
    CHECK(eng.book(1)->order_snapshot(3)->side == Side::Bid);
    CHECK(eng.stats().volume_matched == 50);
    CHECK(eng.book(1)->validate());
}

TEST_CASE("match property: conservation, priority, never-crossed — and the "
          "flat book emits identical fills to the oracle") {
    // Random synthetic session driven simultaneously into RefBook and
    // FlatBook via the SAME matcher. Checks after every submit:
    //   conservation: qty == filled + resting + cancelled
    //   fill sequence identical across implementations (oracle check)
    //   post-state: best_bid < best_ask (a matched book never rests crossed)
    RefBook ref;
    BookResources res(1 << 14);
    FlatBook flat(res, 1);
    std::mt19937_64 rng(20260730);
    std::uint64_t total_matched = 0;

    for (int i = 0; i < 20'000; ++i) {
        const auto ref_id = static_cast<OrderId>(i + 1);
        const Side side = (rng() % 2 == 0) ? Side::Bid : Side::Ask;
        const Price mid = 10'000;
        const Price px = mid + static_cast<Price>(rng() % 60) - 30;
        const auto qty = static_cast<Qty>((1 + rng() % 9) * 100);
        const Tif tif = (rng() % 4 == 0) ? Tif::ioc : Tif::limit;

        FillTape rt;
        FillTape ft;
        const auto rr = ob::engine::submit(ref, ref_id, side, px, qty, tif, rt);
        const auto fr = ob::engine::submit(flat, ref_id, side, px, qty, tif, ft);

        REQUIRE(rr.status == fr.status);
        REQUIRE(rr.filled == fr.filled);
        REQUIRE(rr.resting == fr.resting);
        REQUIRE(rr.cancelled == fr.cancelled);
        REQUIRE(same_fills(rt.fills, ft.fills));
        REQUIRE(qty == rr.filled + rr.resting + rr.cancelled);  // conservation
        for (const Fill& f : rt.fills) {
            REQUIRE(f.qty > 0);
            // Taker never trades through its limit.
            REQUIRE((side == Side::Bid ? f.price <= px : f.price >= px));
        }
        total_matched += rr.filled;

        // A matched book can never rest crossed.
        const auto bb = flat.best_bid();
        const auto ba = flat.best_ask();
        if (bb && ba) REQUIRE(*bb < *ba);

        if (i % 500 == 0) {
            REQUIRE(flat.validate());
            REQUIRE(ref.validate());
            for (Side s : {Side::Bid, Side::Ask}) {
                REQUIRE(ref.l2(s) == flat.l2(s));
            }
        }
    }
    CHECK(total_matched > 0);
    REQUIRE(flat.validate());
    REQUIRE(ref.validate());
    for (Side s : {Side::Bid, Side::Ask}) {
        REQUIRE(ref.l2(s) == flat.l2(s));
        for (const auto& lvl : ref.l2(s)) {
            REQUIRE(ref.level_fifo(s, lvl.price) == flat.level_fifo(s, lvl.price));
        }
    }
}
