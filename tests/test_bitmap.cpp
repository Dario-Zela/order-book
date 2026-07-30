#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>
#include <set>

#include "book/level_bitmap.hpp"

using ob::book::LevelBitmap;

namespace {

// Oracle prev/next over a std::set of set indices.
std::ptrdiff_t oracle_prev(const std::set<std::size_t>& s, std::size_t i) {
    auto it = s.upper_bound(i);
    if (it == s.begin()) return -1;
    return static_cast<std::ptrdiff_t>(*std::prev(it));
}
std::ptrdiff_t oracle_next(const std::set<std::size_t>& s, std::size_t i) {
    auto it = s.lower_bound(i);
    if (it == s.end()) return -1;
    return static_cast<std::ptrdiff_t>(*it);
}

}  // namespace

TEST_CASE("bitmap: set/clear/test basics across word boundaries") {
    LevelBitmap bm;
    bm.reset(300);
    for (std::size_t i : {0u, 63u, 64u, 127u, 128u, 191u, 192u, 299u}) {
        CHECK_FALSE(bm.test(i));
        bm.set(i);
        CHECK(bm.test(i));
    }
    bm.clear(64);
    CHECK_FALSE(bm.test(64));
    CHECK(bm.test(63));
    CHECK(bm.test(127));
}

TEST_CASE("bitmap: prev_set and next_set at exact positions and edges") {
    LevelBitmap bm;
    bm.reset(1000);
    bm.set(500);
    CHECK(bm.prev_set(500) == 500);  // inclusive
    CHECK(bm.next_set(500) == 500);
    CHECK(bm.prev_set(499) == -1);
    CHECK(bm.next_set(501) == -1);
    CHECK(bm.prev_set(999) == 500);
    CHECK(bm.next_set(0) == 500);

    bm.set(10);
    bm.set(990);
    CHECK(bm.prev_set(499) == 10);   // crosses several summary words down
    CHECK(bm.next_set(501) == 990);  // and up
}

TEST_CASE("bitmap: randomised agreement with a std::set oracle") {
    // Band-cap sized: 2^17 levels — exercises the two-level summary hard.
    constexpr std::size_t kN = 1u << 17;
    LevelBitmap bm;
    bm.reset(kN);
    std::set<std::size_t> oracle;
    std::mt19937_64 rng(99);
    for (int round = 0; round < 20'000; ++round) {
        const std::size_t i = rng() % kN;
        if (rng() % 3 != 0) {
            bm.set(i);
            oracle.insert(i);
        } else {
            bm.clear(i);
            oracle.erase(i);
        }
        const std::size_t q = rng() % kN;
        REQUIRE(bm.prev_set(q) == oracle_prev(oracle, q));
        REQUIRE(bm.next_set(q) == oracle_next(oracle, q));
    }
}

TEST_CASE("bitmap: grow preserves bits at their shifted positions") {
    LevelBitmap bm;
    bm.reset(128);
    bm.set(0);
    bm.set(70);
    bm.set(127);
    bm.grow(512, 100);  // as when a band extends 100 ticks downward
    CHECK(bm.test(100));
    CHECK(bm.test(170));
    CHECK(bm.test(227));
    CHECK_FALSE(bm.test(0));
    CHECK(bm.prev_set(511) == 227);
    CHECK(bm.next_set(0) == 100);
}
