#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "book/ref_book.hpp"

using ob::OrderId;
using ob::Price;
using ob::Qty;
using ob::Side;
using ob::book::Apply;
using ob::book::LevelView;
using ob::book::RefBook;

TEST_CASE("add establishes best bid/ask and L2 shape") {
    RefBook b;
    CHECK(b.add(1, Side::Bid, 10000, 100).result == Apply::ok);
    CHECK(b.add(2, Side::Bid, 9990, 50).result == Apply::ok);
    CHECK(b.add(3, Side::Ask, 10010, 70).result == Apply::ok);
    CHECK(b.best_bid() == Price{10000});
    CHECK(b.best_ask() == Price{10010});
    CHECK(b.l2(Side::Bid) == std::vector<LevelView>{{10000, 100, 1}, {9990, 50, 1}});
    CHECK(b.l2(Side::Ask) == std::vector<LevelView>{{10010, 70, 1}});
    CHECK(b.validate());
}

TEST_CASE("same-price adds queue FIFO; partial exec keeps head priority") {
    RefBook b;
    b.add(1, Side::Bid, 10000, 100);
    b.add(2, Side::Bid, 10000, 200);
    CHECK(b.level_fifo(Side::Bid, 10000) == std::vector<OrderId>{1, 2});

    auto e = b.execute(1, 40);  // partial: order 1 stays at the head (§5.3)
    CHECK(e.result == Apply::ok);
    CHECK(e.applied == 40);
    CHECK(e.level_qty_after == 260);
    CHECK_FALSE(e.order_removed);
    CHECK(b.level_fifo(Side::Bid, 10000) == std::vector<OrderId>{1, 2});
    CHECK(b.order_snapshot(1)->remaining == Qty{60});
    CHECK(b.validate());
}

TEST_CASE("fill-to-zero unlinks; last order out empties the level") {
    RefBook b;
    b.add(1, Side::Ask, 10010, 100);
    b.add(2, Side::Ask, 10010, 50);
    auto e = b.execute(1, 100);
    CHECK(e.order_removed);
    CHECK_FALSE(e.level_removed);
    CHECK(b.level_fifo(Side::Ask, 10010) == std::vector<OrderId>{2});

    e = b.execute(2, 50);
    CHECK(e.order_removed);
    CHECK(e.level_removed);
    CHECK(e.level_qty_after == 0);
    CHECK_FALSE(b.best_ask().has_value());
    CHECK(b.live_orders() == 0);
    CHECK(b.validate());
}

TEST_CASE("best price repairs when the touch level empties") {
    RefBook b;
    b.add(1, Side::Bid, 10000, 100);
    b.add(2, Side::Bid, 9990, 50);
    b.remove(1);
    CHECK(b.best_bid() == Price{9990});
    CHECK(b.validate());
}

TEST_CASE("cancel reducing to zero behaves as delete") {
    RefBook b;
    b.add(1, Side::Bid, 10000, 100);
    auto e = b.cancel(1, 100);
    CHECK(e.result == Apply::ok);
    CHECK(e.order_removed);
    CHECK(e.level_removed);
    CHECK(b.live_orders() == 0);
}

TEST_CASE("cancel beyond remaining clamps and flags") {
    RefBook b;
    b.add(1, Side::Bid, 10000, 100);
    auto e = b.cancel(1, 250);
    CHECK(e.result == Apply::clamped);
    CHECK(e.applied == 100);
    CHECK(e.order_removed);
    CHECK(b.live_orders() == 0);
    CHECK(b.validate());
}

TEST_CASE("unknown refs are reported, never applied") {
    RefBook b;
    CHECK(b.execute(42, 10).result == Apply::unknown_ref);
    CHECK(b.cancel(42, 10).result == Apply::unknown_ref);
    CHECK(b.remove(42).result == Apply::unknown_ref);
    CHECK(b.live_orders() == 0);
}

TEST_CASE("duplicate add ref is rejected without mutation") {
    RefBook b;
    b.add(1, Side::Bid, 10000, 100);
    auto e = b.add(1, Side::Ask, 10010, 50);
    CHECK(e.result == Apply::duplicate_ref);
    CHECK(b.l2(Side::Ask).empty());
    CHECK(b.order_snapshot(1)->remaining == Qty{100});
    CHECK(b.validate());
}

TEST_CASE("delete reports remaining shares as cancelled") {
    RefBook b;
    b.add(1, Side::Bid, 10000, 100);
    b.execute(1, 30);
    auto e = b.remove(1);
    CHECK(e.applied == 70);
    CHECK(e.order_removed);
}

TEST_CASE("crossed book is representable (pre-open is legitimate)") {
    RefBook b;
    b.add(1, Side::Bid, 10020, 100);  // bid above ask: §4.1, normal pre-open
    b.add(2, Side::Ask, 10000, 100);
    CHECK(b.best_bid() == Price{10020});
    CHECK(b.best_ask() == Price{10000});
    CHECK(b.validate());  // structural invariants hold even when crossed
}
