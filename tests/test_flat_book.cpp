#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "book/flat_book.hpp"

using ob::OrderId;
using ob::Price;
using ob::Qty;
using ob::Side;
using ob::book::Apply;
using ob::book::BandConfig;
using ob::book::BookResources;
using ob::book::FlatBook;
using ob::book::LevelView;

namespace {
struct Fixture {
    BookResources res{1024};
    FlatBook book{res, 1};
};
}  // namespace

TEST_CASE_METHOD(Fixture, "flat: add/best/l2 basics") {
    CHECK(book.add(1, Side::Bid, 10000, 100).result == Apply::ok);
    CHECK(book.add(2, Side::Bid, 9990, 50).result == Apply::ok);
    CHECK(book.add(3, Side::Ask, 10010, 70).result == Apply::ok);
    CHECK(book.best_bid() == Price{10000});
    CHECK(book.best_ask() == Price{10010});
    CHECK(book.l2(Side::Bid) == std::vector<LevelView>{{10000, 100, 1}, {9990, 50, 1}});
    CHECK(book.l2(Side::Ask) == std::vector<LevelView>{{10010, 70, 1}});
    CHECK(book.validate());
}

TEST_CASE_METHOD(Fixture, "flat: FIFO discipline and partial exec at head") {
    book.add(1, Side::Bid, 10000, 100);
    book.add(2, Side::Bid, 10000, 200);
    CHECK(book.level_fifo(Side::Bid, 10000) == std::vector<OrderId>{1, 2});
    auto e = book.execute(1, 40);
    CHECK(e.applied == 40);
    CHECK(e.level_qty_after == 260);
    CHECK(book.level_fifo(Side::Bid, 10000) == std::vector<OrderId>{1, 2});
    CHECK(book.order_snapshot(1)->remaining == Qty{60});
    CHECK(book.validate());
}

TEST_CASE_METHOD(Fixture, "flat: unlink middle of FIFO, then head, then tail") {
    book.add(1, Side::Ask, 10010, 10);
    book.add(2, Side::Ask, 10010, 20);
    book.add(3, Side::Ask, 10010, 30);
    book.remove(2);
    CHECK(book.level_fifo(Side::Ask, 10010) == std::vector<OrderId>{1, 3});
    book.remove(1);
    CHECK(book.level_fifo(Side::Ask, 10010) == std::vector<OrderId>{3});
    book.remove(3);
    CHECK(book.level_fifo(Side::Ask, 10010).empty());
    CHECK(book.live_orders() == 0);
    CHECK(book.validate());
}

TEST_CASE_METHOD(Fixture, "flat: best repairs by scanning toward centre") {
    book.add(1, Side::Bid, 10000, 100);
    book.add(2, Side::Bid, 9980, 50);
    book.add(3, Side::Bid, 9800, 25);
    book.remove(1);  // touch empties: scan finds 9980
    CHECK(book.best_bid() == Price{9980});
    book.remove(2);  // longer scan to 9800
    CHECK(book.best_bid() == Price{9800});
    book.remove(3);
    CHECK_FALSE(book.best_bid().has_value());
    CHECK(book.stats().best_repairs == 3);
    CHECK(book.stats().repair_steps >= 2 + 180);  // second scan walks 180 ticks
    CHECK(book.validate());
}

TEST_CASE("flat: band grows geometrically on out-of-band adds") {
    BookResources res{1024};
    FlatBook book{res, 1, BandConfig{.initial_half_width = 16, .max_width = 4096}};
    book.add(1, Side::Bid, 10000, 100);
    CHECK(book.stats().band_growths == 0);
    book.add(2, Side::Bid, 10100, 50);  // outside +-16: grow, not overflow
    CHECK(book.stats().band_growths == 1);
    CHECK(book.stats().overflow_hits == 0);
    CHECK(book.best_bid() == Price{10100});
    CHECK(book.l2(Side::Bid) == std::vector<LevelView>{{10100, 50, 1}, {10000, 100, 1}});
    CHECK(book.validate());
}

TEST_CASE("flat: beyond max width goes to overflow and comes back") {
    BookResources res{1024};
    FlatBook book{res, 1, BandConfig{.initial_half_width = 16, .max_width = 64}};
    book.add(1, Side::Bid, 10000, 100);
    book.add(2, Side::Bid, 500, 50);  // fat-finger print far below: overflow
    CHECK(book.stats().overflow_hits > 0);
    CHECK(book.best_bid() == Price{10000});
    CHECK(book.l2(Side::Bid) == std::vector<LevelView>{{10000, 100, 1}, {500, 50, 1}});
    CHECK(book.level_fifo(Side::Bid, 500) == std::vector<OrderId>{2});
    CHECK(book.order_snapshot(2)->remaining == Qty{50});

    auto e = book.execute(2, 50);  // executes out of the overflow level
    CHECK(e.result == Apply::ok);
    CHECK(e.level_removed);
    CHECK(book.l2(Side::Bid) == std::vector<LevelView>{{10000, 100, 1}});
    CHECK(book.validate());
}

TEST_CASE("flat: overflow best can beat band best") {
    BookResources res{1024};
    FlatBook book{res, 1, BandConfig{.initial_half_width = 16, .max_width = 64}};
    book.add(1, Side::Ask, 10000, 100);
    book.add(2, Side::Ask, 200, 10);  // crazy-low ask lands in overflow
    CHECK(book.best_ask() == Price{200});
    book.remove(2);
    CHECK(book.best_ask() == Price{10000});
    CHECK(book.validate());
}

TEST_CASE("flat: far overflow levels stay coherent across later growths") {
    BookResources res{1024};
    FlatBook book{res, 1, BandConfig{.initial_half_width = 4, .max_width = 1u << 16}};
    book.add(1, Side::Bid, 10000, 100);
    book.add(2, Side::Bid, 60000, 50);  // far above: beyond max growth from 10000
    // 60000-10000 < 2^16 so this actually grows... pick truly unreachable:
    book.add(3, Side::Bid, 9'000'000, 25);  // definitely overflow
    CHECK(book.l2(Side::Bid).size() == 3);
    const auto growths_before = book.stats().band_growths;
    book.add(4, Side::Bid, 59'990, 10);  // triggers growth near 60000's region?
    CHECK(book.stats().band_growths >= growths_before);
    // Whatever the internal layout, the public view must stay coherent:
    CHECK(book.l2(Side::Bid) ==
          std::vector<LevelView>{{9'000'000, 25, 1}, {60'000, 50, 1}, {59'990, 10, 1},
                                 {10'000, 100, 1}});
    CHECK(book.best_bid() == Price{9'000'000});
    CHECK(book.validate());
}

TEST_CASE_METHOD(Fixture, "flat: unknown/duplicate/clamp policies match the oracle's") {
    CHECK(book.execute(42, 10).result == Apply::unknown_ref);
    CHECK(book.remove(42).result == Apply::unknown_ref);
    book.add(1, Side::Bid, 10000, 100);
    CHECK(book.add(1, Side::Bid, 10000, 100).result == Apply::duplicate_ref);
    auto e = book.cancel(1, 500);
    CHECK(e.result == Apply::clamped);
    CHECK(e.applied == 100);
    CHECK(book.live_orders() == 0);
    CHECK(book.validate());
}

TEST_CASE("flat: shared id map, per-book locate guard") {
    BookResources res{64};
    FlatBook a{res, 1};
    FlatBook b{res, 2};
    a.add(1, Side::Bid, 10000, 100);
    // Hostile input: locate-2 message referencing locate-1's order must be
    // treated as unknown, not applied to the wrong book.
    CHECK(b.execute(1, 10).result == Apply::unknown_ref);
    CHECK(b.remove(1).result == Apply::unknown_ref);
    CHECK(a.order_snapshot(1)->remaining == Qty{100});
    CHECK(a.validate());
    CHECK(b.validate());
}

TEST_CASE("flat: arena and id map are shared day-wide resources") {
    BookResources res{4};  // deliberately tiny: forces growth, counted
    FlatBook a{res, 1};
    for (OrderId r = 1; r <= 100; ++r) a.add(r, Side::Bid, 10000 + Price(r % 7), 10);
    CHECK(res.arena.live() == 100);
    CHECK(res.arena.high_water() == 100);
    CHECK(res.arena.growths() > 0);  // sized wrong on purpose; visibly counted
    for (OrderId r = 1; r <= 100; ++r) a.remove(r);
    CHECK(res.arena.live() == 0);
    CHECK(res.ids.size() == 0);
    CHECK(a.validate());
}

TEST_CASE("flat: crossed pre-open book is representable") {
    BookResources res{64};
    FlatBook book{res, 1};
    book.add(1, Side::Bid, 10020, 100);
    book.add(2, Side::Ask, 10000, 100);
    CHECK(book.best_bid() == Price{10020});
    CHECK(book.best_ask() == Price{10000});
    CHECK(book.validate());
}
