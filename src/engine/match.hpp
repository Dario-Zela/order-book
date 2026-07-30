#pragma once

// Match mode (DESIGN §6): synthetic order flow. An incoming marketable order
// walks opposite-side levels from the touch, filling FIFO within each level
// (price-time priority) at the MAKER's resting price (price improvement goes
// to the taker); the remainder rests (limit) or is discarded (IOC). Market
// orders are expressed as limits at the far band edge by the caller.
//
// The matcher is composed entirely from book primitives that reconstruct
// mode already exercises and the differential suite already verifies:
// best_*/front_order/execute/add. It is generic over the book, so RefBook
// and FlatBook must produce identical fill sequences — match mode gets the
// same oracle treatment as reconstruct mode (§9.5).
//
// Halt rejection is NOT here: trading state is per-symbol engine knowledge;
// Engine::submit checks it before calling this.

#include <algorithm>
#include <cstdint>

#include "book/types.hpp"
#include "core/types.hpp"

namespace ob::engine {

enum class Tif : std::uint8_t { limit, ioc };

struct Fill {
    OrderId taker;
    OrderId maker;
    Price price;  // maker's resting price
    Qty qty;
};

enum class SubmitStatus : std::uint8_t {
    rested,          // no fill, resting on the book
    partial_rested,  // some fill, remainder resting
    filled,          // fully filled
    ioc_cancelled,   // IOC: whatever didn't fill was discarded
    rejected_duplicate,
    rejected_invalid,  // zero qty
    rejected_halted,   // set by Engine::submit, never here
};

struct SubmitResult {
    SubmitStatus status{};
    Qty filled = 0;
    Qty resting = 0;
    Qty cancelled = 0;  // IOC remainder discarded
};

// Hooks receive each fill and the resting add, with the book Effect so the
// caller can forward level changes to its listener without re-querying.
struct NullMatchHooks {
    void on_fill(const Fill&, const book::Effect&) {}
    void on_rest(const book::Effect&) {}
};

template <typename Book, typename Hooks>
SubmitResult submit(Book& book, OrderId ref, Side side, Price limit, Qty qty, Tif tif,
                    Hooks&& hooks) {
    if (qty == 0) {
        return {.status = SubmitStatus::rejected_invalid};
    }
    if (book.order_snapshot(ref)) {
        return {.status = SubmitStatus::rejected_duplicate};
    }
    const Side opp = opposite(side);
    Qty remaining = qty;
    Qty filled = 0;
    while (remaining > 0) {
        const auto best = opp == Side::Ask ? book.best_ask() : book.best_bid();
        if (!best) break;
        const bool crosses = side == Side::Bid ? *best <= limit : *best >= limit;
        if (!crosses) break;
        // best_* guarantees a non-empty level, so front_order must exist.
        const auto front = book.front_order(opp, *best);
        const Qty f = std::min(remaining, front->remaining);
        const auto effect = book.execute(front->ref, f);
        hooks.on_fill(Fill{ref, front->ref, *best, f}, effect);
        remaining -= f;
        filled += f;
    }
    if (remaining == 0) {
        return {.status = SubmitStatus::filled, .filled = filled};
    }
    if (tif == Tif::ioc) {
        return {.status = SubmitStatus::ioc_cancelled, .filled = filled, .cancelled = remaining};
    }
    const auto effect = book.add(ref, side, limit, remaining);
    hooks.on_rest(effect);
    const auto status = filled > 0 ? SubmitStatus::partial_rested : SubmitStatus::rested;
    return {.status = status, .filled = filled, .resting = remaining};
}

}  // namespace ob::engine
