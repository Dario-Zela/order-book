#pragma once

// Reference order book: deliberately simple std::map + std::list +
// std::unordered_map. This is the differential-testing oracle (DESIGN §9.1)
// — it is kept forever, never optimised, and the flat-array book must agree
// with it message for message. Clarity beats speed here by design.

#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "core/types.hpp"

namespace ob::book {

enum class Apply : std::uint8_t {
    ok,
    unknown_ref,    // Execute/Cancel/Delete/Replace for an id we never saw (§4.1)
    duplicate_ref,  // Add with a live ref, or Replace onto a live new_ref
    clamped,        // exec/cancel qty exceeded remaining; applied at remaining
};

struct LevelView {
    Price price;
    Qty qty;
    std::uint32_t count;
    bool operator==(const LevelView&) const = default;
};

class RefBook {
public:
    // What one message did to the book — enough for the engine to emit
    // listener callbacks without re-querying.
    struct Effect {
        Apply result = Apply::ok;
        Side side{};
        Price price = 0;        // resting level touched (C decrements here, not exec px)
        Qty level_qty_after = 0;
        Qty applied = 0;        // qty actually executed/cancelled (post-clamp)
        bool order_removed = false;
        bool level_removed = false;
    };

    Effect add(OrderId ref, Side side, Price price, Qty qty) {
        if (orders_.contains(ref)) {
            return {.result = Apply::duplicate_ref};
        }
        return side == Side::Bid ? add_impl(bids_, ref, side, price, qty)
                                 : add_impl(asks_, ref, side, price, qty);
    }

    // Handles both E and C: the C execution price never moves book qty from a
    // different level — removal is keyed by ref at the order's resting price.
    Effect execute(OrderId ref, Qty qty) { return reduce(ref, qty); }

    // X: partial cancel. Reducing to zero behaves as delete (§5.3).
    Effect cancel(OrderId ref, Qty qty) { return reduce(ref, qty); }

    // D: unconditional removal; Effect.applied reports the shares cancelled.
    // Replace (U) is composed by the engine as snapshot + remove + add — the
    // side-inheritance rule lives there, once, for every book implementation.
    Effect remove(OrderId ref) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) {
            return {.result = Apply::unknown_ref};
        }
        return unlink(it, /*applied=*/it->second.remaining);
    }

    [[nodiscard]] std::optional<Price> best_bid() const {
        return bids_.empty() ? std::nullopt : std::optional{bids_.begin()->first};
    }
    [[nodiscard]] std::optional<Price> best_ask() const {
        return asks_.empty() ? std::nullopt : std::optional{asks_.begin()->first};
    }

    // Top `depth` levels from the touch (0 = all). Used by golden snapshots.
    [[nodiscard]] std::vector<LevelView> l2(Side side, std::size_t depth = 0) const {
        std::vector<LevelView> out;
        auto emit = [&](const auto& levels) {
            for (const auto& [px, lvl] : levels) {
                if (depth != 0 && out.size() == depth) break;
                out.push_back({px, lvl.qty, static_cast<std::uint32_t>(lvl.fifo.size())});
            }
        };
        side == Side::Bid ? emit(bids_) : emit(asks_);
        return out;
    }

    // Test/audit introspection: order ids in time priority at one level.
    [[nodiscard]] std::vector<OrderId> level_fifo(Side side, Price price) const {
        auto grab = [&](const auto& levels) -> std::vector<OrderId> {
            auto it = levels.find(price);
            if (it == levels.end()) return {};
            return {it->second.fifo.begin(), it->second.fifo.end()};
        };
        return side == Side::Bid ? grab(bids_) : grab(asks_);
    }

    struct OrderSnap {
        Side side;
        Price price;
        Qty remaining;
    };
    [[nodiscard]] std::optional<OrderSnap> order_snapshot(OrderId ref) const {
        auto it = orders_.find(ref);
        if (it == orders_.end()) return std::nullopt;
        return OrderSnap{it->second.side, it->second.price, it->second.remaining};
    }

    [[nodiscard]] std::size_t live_orders() const { return orders_.size(); }

    // Invariant sweep (DESIGN §9.2): level totals match member orders, FIFO
    // membership matches the id map. The book-not-crossed check is NOT here —
    // it is market-phase-gated and belongs to the caller (§4.1).
    [[nodiscard]] bool validate() const {
        std::size_t seen = 0;
        auto check = [&](const auto& levels, Side side) {
            for (const auto& [px, lvl] : levels) {
                if (lvl.fifo.empty()) return false;  // empty levels must be erased
                Qty sum = 0;
                for (OrderId id : lvl.fifo) {
                    auto it = orders_.find(id);
                    if (it == orders_.end()) return false;
                    const RefOrder& o = it->second;
                    if (o.side != side || o.price != px || o.remaining == 0) return false;
                    sum += o.remaining;
                    ++seen;
                }
                if (sum != lvl.qty) return false;
            }
            return true;
        };
        return check(bids_, Side::Bid) && check(asks_, Side::Ask) && seen == orders_.size();
    }

private:
    struct Level {
        Qty qty = 0;
        std::list<OrderId> fifo;  // front = oldest = first to fill
    };
    using Bids = std::map<Price, Level, std::greater<Price>>;
    using Asks = std::map<Price, Level>;

    struct RefOrder {
        Side side;
        Price price;
        Qty remaining;
        std::list<OrderId>::iterator pos;
    };

    template <typename Levels>
    Effect add_impl(Levels& levels, OrderId ref, Side side, Price price, Qty qty) {
        Level& lvl = levels[price];
        lvl.fifo.push_back(ref);
        lvl.qty += qty;
        orders_.emplace(ref, RefOrder{side, price, qty, std::prev(lvl.fifo.end())});
        return {.result = Apply::ok,
                .side = side,
                .price = price,
                .level_qty_after = lvl.qty};
    }

    Effect reduce(OrderId ref, Qty qty) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) {
            return {.result = Apply::unknown_ref};
        }
        RefOrder& o = it->second;
        const bool clamp = qty > o.remaining;  // hostile input (§4.1): clamp, flag
        const Qty applied = clamp ? o.remaining : qty;
        if (applied == o.remaining) {
            Effect e = unlink(it, applied);
            if (clamp) e.result = Apply::clamped;
            return e;
        }
        o.remaining -= applied;
        Level& lvl = level_of(o);
        lvl.qty -= applied;
        // Partial fill: the order keeps its FIFO position — time priority
        // retained (§5.3).
        return {.result = Apply::ok,
                .side = o.side,
                .price = o.price,
                .level_qty_after = lvl.qty,
                .applied = applied};
    }

    // Remove the order entirely; `applied` is the executed/cancelled qty to report.
    Effect unlink(std::unordered_map<OrderId, RefOrder>::iterator it, Qty applied) {
        const RefOrder o = it->second;
        Effect e{.result = Apply::ok,
                 .side = o.side,
                 .price = o.price,
                 .applied = applied,
                 .order_removed = true};
        auto erase_from = [&](auto& levels) {
            auto lit = levels.find(o.price);
            lit->second.fifo.erase(o.pos);
            lit->second.qty -= o.remaining;
            e.level_qty_after = lit->second.qty;
            if (lit->second.fifo.empty()) {
                levels.erase(lit);
                e.level_removed = true;
            }
        };
        o.side == Side::Bid ? erase_from(bids_) : erase_from(asks_);
        orders_.erase(it);
        return e;
    }

    Level& level_of(const RefOrder& o) {
        return o.side == Side::Bid ? bids_.find(o.price)->second : asks_.find(o.price)->second;
    }

    Bids bids_;
    Asks asks_;
    std::unordered_map<OrderId, RefOrder> orders_;
};

}  // namespace ob::book
