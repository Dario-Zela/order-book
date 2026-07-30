#pragma once

// Vocabulary shared by every book implementation. The engine is generic over
// the book type; RefBook (oracle) and FlatBook (hot path) must agree on this
// interface exactly — that's what makes differential testing (§9.1) honest.

#include <cstdint>

#include "core/types.hpp"

namespace ob::book {

enum class Apply : std::uint8_t {
    ok,
    unknown_ref,    // Execute/Cancel/Delete/Replace for an id we never saw (§4.1)
    duplicate_ref,  // Add with a live ref
    clamped,        // exec/cancel qty exceeded remaining; applied at remaining
};

struct LevelView {
    Price price;
    Qty qty;
    std::uint32_t count;
    bool operator==(const LevelView&) const = default;
};

struct OrderSnap {
    Side side;
    Price price;
    Qty remaining;
};

// What one message did to the book — enough for the engine to emit listener
// callbacks without re-querying.
struct Effect {
    Apply result = Apply::ok;
    Side side{};
    Price price = 0;        // resting level touched (C decrements here, not exec px)
    Qty level_qty_after = 0;
    Qty applied = 0;        // qty actually executed/cancelled (post-clamp)
    bool order_removed = false;
    bool level_removed = false;
};

}  // namespace ob::book
