#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <numeric>
#include <random>
#include <unordered_map>
#include <vector>

#include "core/id_map.hpp"

using Map = ob::IdMap<std::uint64_t>;

TEST_CASE("insert, find, erase basics") {
    Map m(16);
    CHECK(m.insert(1, 100));
    CHECK(m.insert(2, 200));
    CHECK_FALSE(m.insert(1, 999));  // duplicate: no mutation
    REQUIRE(m.find(1) != nullptr);
    CHECK(*m.find(1) == 100);
    CHECK(m.find(3) == nullptr);
    CHECK(m.size() == 2);

    CHECK(m.erase(1));
    CHECK_FALSE(m.erase(1));
    CHECK(m.find(1) == nullptr);
    CHECK(*m.find(2) == 200);
    CHECK(m.size() == 1);
}

TEST_CASE("sequential ITCH-like keys stay findable under heavy churn") {
    // Replay-realistic pattern: dense sequential refs, deletes as frequent
    // as inserts (§5.2). Mirror against std::unordered_map as oracle.
    Map m(1024);
    std::unordered_map<std::uint64_t, std::uint64_t> oracle;
    std::mt19937_64 rng(42);
    std::vector<std::uint64_t> live;
    std::uint64_t next_ref = 1;

    for (int i = 0; i < 200'000; ++i) {
        const bool do_insert = live.empty() || (rng() % 100 < 55);
        if (do_insert) {
            const auto ref = next_ref++;
            m.insert(ref, ref * 7);
            oracle.emplace(ref, ref * 7);
            live.push_back(ref);
        } else {
            const auto idx = rng() % live.size();
            const auto ref = live[idx];
            live[idx] = live.back();
            live.pop_back();
            CHECK(m.erase(ref));
            oracle.erase(ref);
        }
    }
    REQUIRE(m.size() == oracle.size());
    for (const auto& [k, v] : oracle) {
        const auto* found = m.find(k);
        REQUIRE(found != nullptr);
        CHECK(*found == v);
    }
    // Half of ~110k inserted keys were erased; none may still be findable.
    std::uint64_t ghosts = 0;
    for (std::uint64_t ref = 1; ref < next_ref; ++ref) {
        if (!oracle.contains(ref) && m.find(ref) != nullptr) ++ghosts;
    }
    CHECK(ghosts == 0);
}

TEST_CASE("backward-shift deletion keeps probe lengths short under churn") {
    Map m(1 << 14);
    std::mt19937_64 rng(7);
    std::vector<std::uint64_t> live;
    std::uint64_t next_ref = 1;
    // Steady state near half of capacity*load: constant insert/delete churn.
    for (int i = 0; i < 500'000; ++i) {
        if (live.size() < 4000 || rng() % 2 == 0) {
            m.insert(next_ref, next_ref);
            live.push_back(next_ref++);
        } else {
            const auto idx = rng() % live.size();
            m.erase(live[idx]);
            live[idx] = live.back();
            live.pop_back();
        }
    }
    CHECK(m.growths() == 0);  // sized correctly: no rehash happened
    const auto hist = m.probe_histogram();
    const auto total = std::accumulate(hist.begin(), hist.end(), std::size_t{0});
    CHECK(total == m.size());
    // With tombstones this degrades all day; with backward shift the mean
    // probe stays ~1.x at 0.5 load. Generous bound to stay non-flaky.
    double mean = 0;
    for (std::size_t d = 0; d < hist.size(); ++d) {
        mean += static_cast<double>(hist[d]) * static_cast<double>(d + 1);
    }
    mean /= static_cast<double>(total);
    CHECK(mean < 3.0);
    CHECK(m.max_probe() < 32);
}

TEST_CASE("growth preserves contents") {
    Map m(4);  // deliberately undersized
    for (std::uint64_t k = 1; k <= 1000; ++k) m.insert(k, k + 1);
    CHECK(m.growths() > 0);
    CHECK(m.size() == 1000);
    for (std::uint64_t k = 1; k <= 1000; ++k) {
        REQUIRE(m.find(k) != nullptr);
        CHECK(*m.find(k) == k + 1);
    }
}

TEST_CASE("key zero is a valid key") {
    // Empty slots are marked by dist==0 metadata, not a sentinel key — ref 0
    // (hostile input) must behave like any other key.
    Map m(16);
    CHECK(m.insert(0, 42));
    REQUIRE(m.find(0) != nullptr);
    CHECK(*m.find(0) == 42);
    CHECK(m.erase(0));
    CHECK(m.find(0) == nullptr);
}
