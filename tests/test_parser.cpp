#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "itch/parser.hpp"
#include "wire.hpp"

namespace {

using obtest::Wire;

// Records everything it sees; tests assert against the recorded messages
// plus the parser's own stats.
struct Recorder : ob::itch::NullVisitor {
    std::vector<ob::itch::SystemEvent> system_events;
    std::vector<ob::itch::StockDirectory> directories;
    std::vector<ob::itch::TradingAction> actions;
    std::vector<ob::itch::AddOrder> adds;
    std::vector<ob::itch::OrderExecuted> execs;
    std::vector<ob::itch::OrderExecutedWithPrice> exec_prices;
    std::vector<ob::itch::OrderCancel> cancels;
    std::vector<ob::itch::OrderDelete> deletes;
    std::vector<ob::itch::OrderReplace> replaces;
    std::vector<ob::itch::Trade> trades;
    std::vector<ob::itch::CrossTrade> crosses;
    std::vector<char> unknowns;
    std::vector<char> malformed;

    void on_system_event(const ob::itch::SystemEvent& m) { system_events.push_back(m); }
    void on_stock_directory(const ob::itch::StockDirectory& m) { directories.push_back(m); }
    void on_trading_action(const ob::itch::TradingAction& m) { actions.push_back(m); }
    void on_add_order(const ob::itch::AddOrder& m) { adds.push_back(m); }
    void on_order_executed(const ob::itch::OrderExecuted& m) { execs.push_back(m); }
    void on_order_executed_with_price(const ob::itch::OrderExecutedWithPrice& m) {
        exec_prices.push_back(m);
    }
    void on_order_cancel(const ob::itch::OrderCancel& m) { cancels.push_back(m); }
    void on_order_delete(const ob::itch::OrderDelete& m) { deletes.push_back(m); }
    void on_order_replace(const ob::itch::OrderReplace& m) { replaces.push_back(m); }
    void on_trade(const ob::itch::Trade& m) { trades.push_back(m); }
    void on_cross_trade(const ob::itch::CrossTrade& m) { crosses.push_back(m); }
    void on_unknown(char type, std::span<const std::byte>) { unknowns.push_back(type); }
    void on_malformed(char type, std::span<const std::byte>) { malformed.push_back(type); }
};

constexpr std::string_view kStock = "AAPL    ";

Wire add_order(std::uint64_t ref, char side, std::uint32_t shares, std::uint32_t price) {
    Wire w;
    w.msg().ch('A').hdr(1, 0, 1000).u64(ref).ch(side).u32(shares).str(kStock).u32(price).end_msg();
    return w;
}

}  // namespace

TEST_CASE("system event decodes") {
    Wire w;
    w.msg().ch('S').hdr(0, 7, 123456789).ch('Q').end_msg();
    ob::itch::Parser p(w.bytes());
    Recorder r;
    CHECK(p.run(r) == 1);
    REQUIRE(r.system_events.size() == 1);
    CHECK(r.system_events[0].h.locate == 0);
    CHECK(r.system_events[0].h.tracking == 7);
    CHECK(r.system_events[0].h.timestamp == 123456789);
    CHECK(r.system_events[0].event == 'Q');
}

TEST_CASE("stock directory decodes the consumed prefix") {
    Wire w;
    w.msg()
        .ch('R')
        .hdr(42, 0, 1)
        .str(kStock)
        .ch('Q')     // market category
        .ch('N')     // financial status
        .u32(100)    // round lot size
        .ch('N')     // round lots only
        .ch('C')     // issue classification
        .str("Z ")   // issue sub-type
        .ch('P')     // authenticity
        .ch('N')     // short sale threshold
        .ch('N')     // IPO flag
        .ch('1')     // LULD reference price tier
        .ch('N')     // ETP flag
        .u32(0)      // ETP leverage factor
        .ch('N')     // inverse indicator
        .end_msg();
    ob::itch::Parser p(w.bytes());
    Recorder r;
    CHECK(p.run(r) == 1);
    REQUIRE(r.directories.size() == 1);
    CHECK(r.directories[0].h.locate == 42);
    CHECK(std::string_view(r.directories[0].stock.data(), 8) == kStock);
    CHECK(r.directories[0].market_category == 'Q');
    CHECK(r.directories[0].round_lot_size == 100);
}

TEST_CASE("trading action decodes state and reason") {
    Wire w;
    w.msg().ch('H').hdr(42, 0, 2).str(kStock).ch('H').ch(' ').str("LUDP").end_msg();
    ob::itch::Parser p(w.bytes());
    Recorder r;
    CHECK(p.run(r) == 1);
    REQUIRE(r.actions.size() == 1);
    CHECK(r.actions[0].state == 'H');
    CHECK(std::string_view(r.actions[0].reason.data(), 4) == "LUDP");
}

TEST_CASE("add order decodes all fields") {
    auto w = add_order(9001, 'B', 500, 1'823'400);  // $182.34
    ob::itch::Parser p(w.bytes());
    Recorder r;
    CHECK(p.run(r) == 1);
    REQUIRE(r.adds.size() == 1);
    const auto& a = r.adds[0];
    CHECK(a.h.locate == 1);
    CHECK(a.ref == 9001);
    CHECK(a.side == ob::Side::Bid);
    CHECK(a.shares == 500);
    CHECK(std::string_view(a.stock.data(), 8) == kStock);
    CHECK(a.price == 1'823'400);
    CHECK_FALSE(a.has_mpid);
}

TEST_CASE("add order with MPID carries attribution") {
    Wire w;
    w.msg()
        .ch('F')
        .hdr(1, 0, 1000)
        .u64(9002)
        .ch('S')
        .u32(200)
        .str(kStock)
        .u32(1'823'500)
        .str("JPMC")
        .end_msg();
    ob::itch::Parser p(w.bytes());
    Recorder r;
    CHECK(p.run(r) == 1);
    REQUIRE(r.adds.size() == 1);
    CHECK(r.adds[0].side == ob::Side::Ask);
    CHECK(r.adds[0].has_mpid);
    CHECK(std::string_view(r.adds[0].attribution.data(), 4) == "JPMC");
}

TEST_CASE("order executed decodes") {
    Wire w;
    w.msg().ch('E').hdr(1, 0, 2000).u64(9001).u32(100).u64(777).end_msg();
    ob::itch::Parser p(w.bytes());
    Recorder r;
    CHECK(p.run(r) == 1);
    REQUIRE(r.execs.size() == 1);
    CHECK(r.execs[0].ref == 9001);
    CHECK(r.execs[0].shares == 100);
    CHECK(r.execs[0].match == 777);
}

TEST_CASE("executed-with-price decodes printable flag both ways") {
    Wire w;
    w.msg().ch('C').hdr(1, 0, 2000).u64(9001).u32(50).u64(778).ch('Y').u32(1'823'450).end_msg();
    w.msg().ch('C').hdr(1, 0, 2001).u64(9001).u32(50).u64(779).ch('N').u32(1'823'450).end_msg();
    ob::itch::Parser p(w.bytes());
    Recorder r;
    CHECK(p.run(r) == 2);
    REQUIRE(r.exec_prices.size() == 2);
    CHECK(r.exec_prices[0].printable);
    CHECK(r.exec_prices[0].price == 1'823'450);
    CHECK_FALSE(r.exec_prices[1].printable);
}

TEST_CASE("cancel and delete decode") {
    Wire w;
    w.msg().ch('X').hdr(1, 0, 3000).u64(9001).u32(25).end_msg();
    w.msg().ch('D').hdr(1, 0, 3001).u64(9001).end_msg();
    ob::itch::Parser p(w.bytes());
    Recorder r;
    CHECK(p.run(r) == 2);
    REQUIRE(r.cancels.size() == 1);
    CHECK(r.cancels[0].cancelled == 25);
    REQUIRE(r.deletes.size() == 1);
    CHECK(r.deletes[0].ref == 9001);
}

TEST_CASE("replace carries no side and new total shares") {
    Wire w;
    w.msg().ch('U').hdr(1, 0, 4000).u64(9001).u64(9100).u32(300).u32(1'823'300).end_msg();
    ob::itch::Parser p(w.bytes());
    Recorder r;
    CHECK(p.run(r) == 1);
    REQUIRE(r.replaces.size() == 1);
    CHECK(r.replaces[0].orig_ref == 9001);
    CHECK(r.replaces[0].new_ref == 9100);
    CHECK(r.replaces[0].shares == 300);
    CHECK(r.replaces[0].price == 1'823'300);
}

TEST_CASE("trade (P) decodes") {
    Wire w;
    w.msg()
        .ch('P')
        .hdr(1, 0, 5000)
        .u64(0)
        .ch('B')
        .u32(100)
        .str(kStock)
        .u32(1'823'400)
        .u64(801)
        .end_msg();
    ob::itch::Parser p(w.bytes());
    Recorder r;
    CHECK(p.run(r) == 1);
    REQUIRE(r.trades.size() == 1);
    CHECK(r.trades[0].shares == 100);
    CHECK(r.trades[0].match == 801);
}

TEST_CASE("cross trade decodes 8-byte shares") {
    Wire w;
    w.msg()
        .ch('Q')
        .hdr(1, 0, 6000)
        .u64(5'000'000'000ull)  // > 2^32: exercises the u64 width
        .str(kStock)
        .u32(1'820'000)
        .u64(900)
        .ch('O')
        .end_msg();
    ob::itch::Parser p(w.bytes());
    Recorder r;
    CHECK(p.run(r) == 1);
    REQUIRE(r.crosses.size() == 1);
    CHECK(r.crosses[0].shares == 5'000'000'000ull);
    CHECK(r.crosses[0].cross_type == 'O');
    CHECK(r.crosses[0].price == 1'820'000);
}

TEST_CASE("unknown types are counted and skipped") {
    Wire w;
    w.msg().ch('I').str("0123456789012345678901234567890123456789012345678").end_msg();  // NOII
    auto a = add_order(1, 'B', 10, 100);
    for (auto b : a.bytes()) w.u8(static_cast<std::uint8_t>(b));
    ob::itch::Parser p(w.bytes());
    Recorder r;
    CHECK(p.run(r) == 2);
    REQUIRE(r.unknowns.size() == 1);
    CHECK(r.unknowns[0] == 'I');
    CHECK(r.adds.size() == 1);  // parsing continues past unknowns
    CHECK(p.stats().unknown == 1);
}

TEST_CASE("length mismatch on a known type is malformed, not fatal") {
    Wire w;
    w.msg().ch('A').hdr(1, 0, 1).u64(1).ch('B').u32(10).end_msg();  // truncated A body
    auto a = add_order(2, 'B', 10, 100);
    for (auto b : a.bytes()) w.u8(static_cast<std::uint8_t>(b));
    ob::itch::Parser p(w.bytes());
    Recorder r;
    CHECK(p.run(r) == 2);
    REQUIRE(r.malformed.size() == 1);
    CHECK(r.malformed[0] == 'A');
    CHECK(r.adds.size() == 1);
    CHECK(p.stats().malformed == 1);
}

TEST_CASE("invalid side byte is malformed") {
    auto w = add_order(1, 'X', 10, 100);
    ob::itch::Parser p(w.bytes());
    Recorder r;
    p.run(r);
    CHECK(r.adds.empty());
    REQUIRE(r.malformed.size() == 1);
    CHECK(p.stats().malformed == 1);
}

TEST_CASE("truncated tail ends the stream and sets the flag") {
    auto w = add_order(1, 'B', 10, 100);
    w.raw_u16(36).ch('A').hdr(1, 0, 1);  // claims 36 bytes, delivers 11
    ob::itch::Parser p(w.bytes());
    Recorder r;
    CHECK(p.run(r) == 1);
    CHECK(p.stats().truncated);
    CHECK(r.adds.size() == 1);
}

TEST_CASE("zero-length sentinel ends the stream cleanly") {
    auto w = add_order(1, 'B', 10, 100);
    w.raw_u16(0);
    auto tail = add_order(2, 'B', 10, 100);  // must never be reached
    for (auto b : tail.bytes()) w.u8(static_cast<std::uint8_t>(b));
    ob::itch::Parser p(w.bytes());
    Recorder r;
    CHECK(p.run(r) == 1);
    CHECK_FALSE(p.stats().truncated);
    CHECK(r.adds.size() == 1);
}

TEST_CASE("empty input is a clean end") {
    ob::itch::Parser p({});
    Recorder r;
    CHECK(p.run(r) == 0);
    CHECK_FALSE(p.stats().truncated);
}
