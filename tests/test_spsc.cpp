// SPSC ring stress tests (DESIGN §7): run under every preset; the tsan
// preset is the data-race evidence. TSan silence is evidence, not proof —
// the written ordering argument lives in spsc_ring.hpp.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>
#include <thread>
#include <vector>

#include "spsc/spsc_ring.hpp"

namespace {

struct Payload {
    std::uint64_t seq;
    std::uint64_t check;  // mixed function of seq: detects torn/stale slots
};

constexpr std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return x;
}

}  // namespace

TEST_CASE("spsc: single-thread fill/drain and wrap-around") {
    ob::spsc::SpscRing<Payload, 8> ring;
    Payload p{};
    CHECK_FALSE(ring.try_pop(p));
    for (std::uint64_t round = 0; round < 100; ++round) {  // many wraps
        for (std::uint64_t i = 0; i < 8; ++i) {
            CHECK(ring.try_push({round * 8 + i, mix(round * 8 + i)}));
        }
        CHECK_FALSE(ring.try_push({0, 0}));  // full
        for (std::uint64_t i = 0; i < 8; ++i) {
            REQUIRE(ring.try_pop(p));
            CHECK(p.seq == round * 8 + i);
            CHECK(p.check == mix(p.seq));
        }
        CHECK_FALSE(ring.try_pop(p));  // empty
    }
}

TEST_CASE("spsc: two-thread stress with randomised stalls") {
    constexpr std::uint64_t kCount = 2'000'000;
    ob::spsc::SpscRing<Payload, 1024> ring;

    std::thread producer([&] {
        std::mt19937_64 rng(1);
        for (std::uint64_t i = 0; i < kCount; ++i) {
            ring.push({i, mix(i)});
            if ((rng() & 0xFFF) == 0) {  // occasional stall: forces full/empty edges
                for (int spin = 0; spin < 500; ++spin) ob::spsc::cpu_relax();
            }
        }
    });

    std::uint64_t next = 0;
    std::uint64_t xor_sum = 0;
    std::mt19937_64 rng(2);
    Payload p{};
    while (next < kCount) {
        if (ring.try_pop(p)) {
            REQUIRE(p.seq == next);  // strict FIFO, no loss, no duplication
            REQUIRE(p.check == mix(p.seq));
            xor_sum ^= p.check;
            ++next;
            if ((rng() & 0xFFF) == 0) {
                for (int spin = 0; spin < 500; ++spin) ob::spsc::cpu_relax();
            }
        } else {
            ob::spsc::cpu_relax();
        }
    }
    producer.join();

    std::uint64_t expect = 0;
    for (std::uint64_t i = 0; i < kCount; ++i) expect ^= mix(i);
    CHECK(xor_sum == expect);
    CHECK(ring.occupancy_high_water() <= ring.capacity());
    CHECK(ring.occupancy_high_water() > 0);
}

TEST_CASE("spsc: batch pop drains in order") {
    constexpr std::uint64_t kCount = 500'000;
    ob::spsc::SpscRing<Payload, 512> ring;

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount; ++i) ring.push({i, mix(i)});
    });

    Payload batch[64];
    std::uint64_t next = 0;
    while (next < kCount) {
        const std::size_t n = ring.pop_n(batch, 64);
        for (std::size_t i = 0; i < n; ++i) {
            REQUIRE(batch[i].seq == next);
            REQUIRE(batch[i].check == mix(batch[i].seq));
            ++next;
        }
        if (n == 0) ob::spsc::cpu_relax();
    }
    producer.join();
    CHECK(next == kCount);
}
