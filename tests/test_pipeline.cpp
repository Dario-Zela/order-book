// Pipeline integrity (DESIGN §7/§9): parse -> encode -> SPSC ring -> apply
// on another thread must produce books identical to a direct single-thread
// replay. This is the differential test again, aimed at the event layer.

#include <catch2/catch_test_macros.hpp>

#include <thread>
#include <vector>

#include "book/flat_book.hpp"
#include "engine/engine.hpp"
#include "engine/event.hpp"
#include "itch/parser.hpp"
#include "spsc/spsc_ring.hpp"
#include "stream_gen.hpp"
#include "wire.hpp"

using ob::Side;
using ob::StockLocate;
using ob::book::BandConfig;
using ob::book::BookResources;
using ob::book::FlatBook;
using ob::engine::BookEvent;
using ob::engine::Engine;
using obtest::Wire;

namespace {

struct FlatFactory {
    BookResources* res;
    std::unique_ptr<FlatBook> operator()(StockLocate loc) const {
        return std::make_unique<FlatBook>(*res, loc, BandConfig{});
    }
};

using FlatEngine = Engine<FlatBook, ob::book::NullListener, FlatFactory>;

}  // namespace

TEST_CASE("pipeline: two-thread ring replay equals direct replay") {
    std::vector<StockLocate> locates;
    const Wire w = obtest::make_stream(777, 40'000, locates);

    // Direct single-thread replay (the baseline that justifies the SPSC
    // design with numbers, and the correctness anchor here).
    BookResources res_direct(1 << 16);
    FlatEngine direct({}, FlatFactory{&res_direct});
    {
        ob::itch::Parser p(w.bytes());
        p.run(direct);
    }

    // Two-thread: producer parses + encodes, consumer applies.
    BookResources res_piped(1 << 16);
    FlatEngine piped({}, FlatFactory{&res_piped});
    ob::spsc::SpscRing<BookEvent, 4096> ring;
    std::atomic<bool> done{false};

    std::thread producer([&] {
        auto emit = [&](const BookEvent& ev) { ring.push(ev); };
        ob::engine::EventEncoder<decltype(emit)> enc(emit);
        ob::itch::Parser p(w.bytes());
        p.run(enc);
        done.store(true, std::memory_order_release);
    });

    BookEvent batch[128];
    while (true) {
        const std::size_t n = ring.pop_n(batch, 128);
        for (std::size_t i = 0; i < n; ++i) {
            ob::engine::apply_event(batch[i], piped);
        }
        if (n == 0) {
            if (done.load(std::memory_order_acquire) && ring.pop_n(batch, 1) == 0) break;
            ob::spsc::cpu_relax();
        }
    }
    producer.join();

    // Engines must agree completely.
    CHECK(direct.stats().unknown_ref == piped.stats().unknown_ref);
    CHECK(direct.stats().clamped == piped.stats().clamped);
    CHECK(direct.stats().volume_lit == piped.stats().volume_lit);
    CHECK(direct.stats().volume_hidden == piped.stats().volume_hidden);
    CHECK(direct.stats().volume_cross == piped.stats().volume_cross);
    CHECK(direct.phase() == piped.phase());
    for (StockLocate loc : locates) {
        REQUIRE(direct.book(loc) != nullptr);
        REQUIRE(piped.book(loc) != nullptr);
        REQUIRE(piped.book(loc)->validate());
        CHECK(direct.book(loc)->live_orders() == piped.book(loc)->live_orders());
        for (Side s : {Side::Bid, Side::Ask}) {
            REQUIRE(direct.book(loc)->l2(s) == piped.book(loc)->l2(s));
        }
        CHECK(direct.trading_state(loc) == piped.trading_state(loc));
    }
    CHECK(ring.occupancy_high_water() > 0);
}

TEST_CASE("pipeline: directory events carry symbols through the ring") {
    Wire w;
    w.msg()
        .ch('R')
        .hdr(5, 0, 1)
        .str("TSLA    ")
        .ch('Q')
        .ch('N')
        .u32(100)
        .ch('N')
        .ch('C')
        .str("Z ")
        .ch('P')
        .ch('N')
        .ch('N')
        .ch('1')
        .ch('N')
        .u32(0)
        .ch('N')
        .end_msg();

    std::vector<BookEvent> events;
    auto emit = [&](const BookEvent& ev) { events.push_back(ev); };
    ob::engine::EventEncoder<decltype(emit)> enc(emit);
    ob::itch::Parser p(w.bytes());
    p.run(enc);
    REQUIRE(events.size() == 1);

    BookResources res(64);
    FlatEngine eng({}, FlatFactory{&res});
    for (const auto& ev : events) ob::engine::apply_event(ev, eng);
    CHECK(std::string_view(eng.symbol(5).data(), 8) == "TSLA    ");
}
