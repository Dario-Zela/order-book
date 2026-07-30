// End-to-end reconstruct-mode tests: wire bytes -> Parser -> Engine<RefBook>.
// Each §4.1 protocol subtlety gets an explicit test.

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "book/ref_book.hpp"
#include "engine/engine.hpp"
#include "itch/parser.hpp"
#include "wire.hpp"

using ob::OrderId;
using ob::Price;
using ob::Qty;
using ob::Side;
using ob::book::LevelView;
using ob::book::RefBook;
using ob::engine::Engine;
using ob::engine::MarketPhase;
using obtest::Wire;

namespace {

Engine<RefBook> replay(const Wire& w) {
    Engine<RefBook> eng;
    ob::itch::Parser p(w.bytes());
    p.run(eng);
    return eng;
}

}  // namespace

TEST_CASE("adds and executions reconstruct the book") {
    Wire w;
    w.add(1, 'B', 100, 10000).add(2, 'S', 80, 10010).exec(1, 40);
    auto eng = replay(w);
    REQUIRE(eng.book(1) != nullptr);
    CHECK(eng.book(1)->l2(Side::Bid) == std::vector<LevelView>{{10000, 60, 1}});
    CHECK(eng.book(1)->l2(Side::Ask) == std::vector<LevelView>{{10010, 80, 1}});
    CHECK(eng.stats().volume_lit == 40);
    CHECK(eng.book(1)->validate());
}

TEST_CASE("replace inherits side, re-queues at the tail, new ref") {
    Wire w;
    w.add(1, 'B', 100, 10000).add(2, 'B', 50, 10000);
    w.replace(1, 10, 100, 10000);  // same price: still goes to the BACK (§4.1)
    auto eng = replay(w);
    const auto* b = eng.book(1);
    CHECK(b->level_fifo(Side::Bid, 10000) == std::vector<OrderId>{2, 10});
    CHECK(b->order_snapshot(10)->side == Side::Bid);  // side inherited: U has none
    CHECK_FALSE(b->order_snapshot(1).has_value());
    CHECK(b->validate());
}

TEST_CASE("replace to a new price moves side and total shares") {
    Wire w;
    w.add(1, 'S', 100, 10010).replace(1, 2, 70, 10020);
    auto eng = replay(w);
    const auto* b = eng.book(1);
    CHECK(b->l2(Side::Ask) == std::vector<LevelView>{{10020, 70, 1}});
    CHECK(b->order_snapshot(2)->remaining == Qty{70});  // new TOTAL, not delta
}

TEST_CASE("replace with unknown original is skipped atomically") {
    Wire w;
    w.add(1, 'B', 100, 10000).replace(99, 2, 50, 10000);
    auto eng = replay(w);
    CHECK(eng.stats().unknown_ref == 1);
    CHECK_FALSE(eng.book(1)->order_snapshot(2).has_value());
    CHECK(eng.book(1)->l2(Side::Bid) == std::vector<LevelView>{{10000, 100, 1}});
}

TEST_CASE("non-printable C decrements the book but not volume") {
    Wire w;
    w.add(1, 'B', 100, 10000);
    w.exec_price(1, 30, 'N', 10005);  // price-improved, non-printable
    w.exec_price(1, 20, 'Y', 10005);
    auto eng = replay(w);
    // Book removal keyed by ref at the RESTING level 10000, not 10005 (§4.1).
    CHECK(eng.book(1)->l2(Side::Bid) == std::vector<LevelView>{{10000, 50, 1}});
    CHECK(eng.stats().volume_lit == 20);  // only the printable leg
    CHECK(eng.stats().nonprintable_execs == 1);
}

TEST_CASE("trade (P) mutates nothing; cross (Q) mutates nothing") {
    Wire w;
    w.add(1, 'B', 100, 10000);
    w.trade(500, 10000);
    w.cross(5'000'000'000ull, 10000, 'O');
    auto eng = replay(w);
    CHECK(eng.book(1)->l2(Side::Bid) == std::vector<LevelView>{{10000, 100, 1}});
    CHECK(eng.stats().volume_hidden == 500);
    CHECK(eng.stats().volume_cross == 5'000'000'000ull);
}

TEST_CASE("unknown refs on E/X/D are counted and skipped") {
    Wire w;
    w.exec(42, 10).cancel(43, 10).del(44);
    auto eng = replay(w);
    CHECK(eng.stats().unknown_ref == 3);
}

TEST_CASE("crossed pre-open book replays without complaint") {
    Wire w;  // no 'Q' system event: market still pre-open
    w.add(1, 'B', 100, 10020).add(2, 'S', 100, 10000);
    auto eng = replay(w);
    CHECK(eng.phase() == MarketPhase::pre_open);
    CHECK(eng.book(1)->best_bid() == Price{10020});
    CHECK(eng.book(1)->best_ask() == Price{10000});  // crossed: legitimate (§4.1)
    CHECK(eng.book(1)->validate());
}

TEST_CASE("system events drive market phase") {
    Wire w;
    w.sys_event('Q');
    auto eng = replay(w);
    CHECK(eng.phase() == MarketPhase::open);
    Wire w2;
    w2.sys_event('Q').sys_event('M');
    CHECK(replay(w2).phase() == MarketPhase::closed);
}

TEST_CASE("halts are tracked; reconstruct still applies events") {
    Wire w;
    w.halt('H').add(1, 'B', 100, 10000);  // feed is the truth (§4.1)
    auto eng = replay(w);
    CHECK(eng.halted(1));
    CHECK(eng.book(1)->l2(Side::Bid) == std::vector<LevelView>{{10000, 100, 1}});
    Wire w2;
    w2.halt('H').halt('T');
    CHECK_FALSE(replay(w2).halted(1));
}

TEST_CASE("per-locate books are independent") {
    Wire w;
    w.add(1, 'B', 100, 10000, /*locate=*/1, "AAPL    ");
    w.add(2, 'B', 200, 50000, /*locate=*/7, "MSFT    ");
    w.exec(2, 50, 1, /*locate=*/7);
    auto eng = replay(w);
    CHECK(eng.book(1)->l2(Side::Bid) == std::vector<LevelView>{{10000, 100, 1}});
    CHECK(eng.book(7)->l2(Side::Bid) == std::vector<LevelView>{{50000, 150, 1}});
    CHECK(eng.book(9) == nullptr);  // untouched locate: no book allocated
}

TEST_CASE("listener sees adds, execs, cancels, and level changes") {
    struct Tape : ob::book::NullListener {
        std::vector<char> events;
        std::vector<Qty> level_qtys;
        void on_add(ob::StockLocate, OrderId, Side, Price, Qty) { events.push_back('a'); }
        void on_exec(ob::StockLocate, OrderId, Side, Price, Qty, bool) {
            events.push_back('e');
        }
        void on_cancel(ob::StockLocate, OrderId, Side, Price, Qty) { events.push_back('c'); }
        void on_level_change(ob::StockLocate, Side, Price, Qty q) { level_qtys.push_back(q); }
    };
    Wire w;
    w.add(1, 'B', 100, 10000).exec(1, 40).del(1);
    Engine<RefBook, Tape> eng;
    ob::itch::Parser p(w.bytes());
    p.run(eng);
    CHECK(eng.listener().events == std::vector<char>{'a', 'e', 'c'});
    CHECK(eng.listener().level_qtys == std::vector<Qty>{100, 60, 0});
}
