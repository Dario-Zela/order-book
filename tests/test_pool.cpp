#include <catch2/catch_test_macros.hpp>

#include <set>
#include <vector>

#include "core/pool.hpp"

namespace {
struct Thing {
    std::uint64_t a;
    void* b;
};
}  // namespace

TEST_CASE("pool allocates distinct slots and recycles LIFO") {
    ob::Pool<Thing> pool(4);
    Thing* x = pool.alloc();
    Thing* y = pool.alloc();
    CHECK(x != y);
    CHECK(pool.live() == 2);

    pool.free(y);
    Thing* z = pool.alloc();
    CHECK(z == y);  // LIFO: hottest recently-freed slot handed out first
    CHECK(pool.live() == 2);
}

TEST_CASE("pool grows past initial capacity and counts it") {
    ob::Pool<Thing> pool(2);
    std::vector<Thing*> all;
    for (int i = 0; i < 10; ++i) all.push_back(pool.alloc());
    CHECK(pool.growths() > 0);
    CHECK(pool.live() == 10);
    CHECK(pool.high_water() == 10);
    std::set<Thing*> uniq(all.begin(), all.end());
    CHECK(uniq.size() == 10);  // growth never hands out an address twice

    for (Thing* p : all) pool.free(p);
    CHECK(pool.live() == 0);
    CHECK(pool.high_water() == 10);  // high water is a peak, not a level
}

TEST_CASE("high water tracks peak concurrent live, not total allocs") {
    ob::Pool<Thing> pool(8);
    Thing* a = pool.alloc();
    pool.free(a);
    Thing* b = pool.alloc();
    pool.free(b);
    CHECK(pool.high_water() == 1);  // churn of 2 allocs, never 2 live at once
}
