#pragma once

// Hot-path order representation (DESIGN §5.2): one cache line, pooled,
// intrusively linked into its price level's FIFO.
//
// Design decision — NO PriceLevel backpointer: with a flat band, price ->
// level is O(1) arithmetic, so (side, price) IS the backpointer. This kills
// the §5.1 rebase fix-up problem entirely (band growth/rebase moves levels
// in memory; nothing points at them). The out-of-band overflow map pays an
// O(log n) lookup on its ≪0.1% path instead. The 64B-vs-packed experiment
// (§5.2) stays a compile-flag change away.

#include <cstdint>
#include <type_traits>

#include "core/types.hpp"

namespace ob::book {

struct alignas(64) Order {
    OrderId id = 0;          // ITCH order reference number
    Price price = 0;
    Qty remaining = 0;
    Side side = Side::Bid;
    StockLocate locate = 0;  // guards cross-book hits via the shared id map
    Order* next = nullptr;   // intrusive doubly-linked FIFO within the level
    Order* prev = nullptr;
};

static_assert(sizeof(Order) == 64, "target: exactly one cache line (§5.2)");
static_assert(std::is_trivially_destructible_v<Order>, "pooled, never destroyed");

struct PriceLevel {
    Qty total_qty = 0;            // maintained incrementally (§5.3)
    std::uint32_t order_count = 0;
    Order* head = nullptr;        // FIFO front = oldest = first to fill
    Order* tail = nullptr;
};

}  // namespace ob::book
