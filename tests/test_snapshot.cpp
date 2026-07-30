// Snapshot/restore (§11 stretch): replaying N messages, snapshotting,
// restoring into a fresh engine, and continuing must be indistinguishable
// from a straight replay — books (FIFO-exact), counters, phase, halts.

#include <string>
#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

#include "book/flat_book.hpp"
#include "book/ref_book.hpp"
#include "engine/engine.hpp"
#include "engine/snapshot.hpp"
#include "itch/parser.hpp"
#include "stream_gen.hpp"
#include "wire.hpp"

using ob::Side;
using ob::StockLocate;
using ob::book::BandConfig;
using ob::book::BookResources;
using ob::book::FlatBook;
using ob::book::RefBook;
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

std::string tmp_snap(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

}  // namespace

TEST_CASE("snapshot: save at N, restore, continue == straight replay") {
    std::vector<StockLocate> locates;
    const Wire w = obtest::make_stream(1234, 40'000, locates);

    // Straight replay: ground truth.
    BookResources res_truth(1 << 16);
    FlatEngine truth({}, FlatFactory{&res_truth});
    {
        ob::itch::Parser p(w.bytes());
        p.run(truth);
    }

    // Replay half, snapshot, restore into a FRESH engine, continue.
    const auto path = tmp_snap("ob_snap_test.bin");
    ob::engine::SnapshotMeta meta;
    {
        BookResources res(1 << 16);
        FlatEngine half({}, FlatFactory{&res});
        ob::itch::Parser p(w.bytes());
        std::uint64_t n = 0;
        while (n < 20'000 && p.next(half)) ++n;
        meta = {n, p.offset()};
        std::FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        REQUIRE(ob::engine::save_snapshot(half, meta, f));
        std::fclose(f);
    }
    BookResources res_restored(1 << 16);
    FlatEngine restored({}, FlatFactory{&res_restored});
    {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        REQUIRE(f != nullptr);
        const auto loaded = ob::engine::load_snapshot(restored, f);
        std::fclose(f);
        REQUIRE(loaded.has_value());
        CHECK(loaded->messages == 20'000);
        ob::itch::Parser p(w.bytes().subspan(loaded->file_offset));
        p.run(restored);
    }
    std::filesystem::remove(path);

    // Indistinguishable from the straight replay.
    CHECK(truth.phase() == restored.phase());
    CHECK(truth.stats().volume_lit == restored.stats().volume_lit);
    CHECK(truth.stats().volume_hidden == restored.stats().volume_hidden);
    CHECK(truth.stats().unknown_ref == restored.stats().unknown_ref);
    CHECK(truth.stats().clamped == restored.stats().clamped);
    for (StockLocate loc : locates) {
        REQUIRE(restored.book(loc) != nullptr);
        REQUIRE(restored.book(loc)->validate());
        CHECK(truth.trading_state(loc) == restored.trading_state(loc));
        CHECK(truth.book(loc)->live_orders() == restored.book(loc)->live_orders());
        for (Side s : {Side::Bid, Side::Ask}) {
            const auto tl2 = truth.book(loc)->l2(s);
            REQUIRE(tl2 == restored.book(loc)->l2(s));
            for (const auto& lvl : tl2) {  // FIFO-exact restore
                REQUIRE(truth.book(loc)->level_fifo(s, lvl.price) ==
                        restored.book(loc)->level_fifo(s, lvl.price));
            }
        }
    }
}

TEST_CASE("snapshot: works generically — reference book too") {
    std::vector<StockLocate> locates;
    const Wire w = obtest::make_stream(99, 5'000, locates);
    Engine<RefBook> src;
    ob::itch::Parser p(w.bytes());
    p.run(src);

    const auto path = tmp_snap("ob_snap_ref.bin");
    std::FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(ob::engine::save_snapshot(src, {5'000, p.offset()}, f));
    std::fclose(f);

    Engine<RefBook> dst;
    f = std::fopen(path.c_str(), "rb");
    REQUIRE(ob::engine::load_snapshot(dst, f).has_value());
    std::fclose(f);
    std::filesystem::remove(path);

    for (StockLocate loc : locates) {
        REQUIRE(dst.book(loc) != nullptr);
        CHECK(dst.book(loc)->validate());
        for (Side s : {Side::Bid, Side::Ask}) {
            CHECK(src.book(loc)->l2(s) == dst.book(loc)->l2(s));
        }
    }
}

TEST_CASE("snapshot: corrupt magic is rejected cleanly") {
    const auto path = tmp_snap("ob_snap_bad.bin");
    std::FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite("NOTASNAP", 8, 1, f);
    std::fclose(f);
    Engine<RefBook> eng;
    f = std::fopen(path.c_str(), "rb");
    CHECK_FALSE(ob::engine::load_snapshot(eng, f).has_value());
    std::fclose(f);
    std::filesystem::remove(path);
}
