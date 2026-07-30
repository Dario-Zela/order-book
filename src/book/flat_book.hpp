#pragma once

// Hot-path book (DESIGN §5): per side, a dense vector of PriceLevel indexed
// by tick offset from a base price — O(1) level lookup, neighbouring levels
// share cache lines, the touch region stays resident in L1/L2. Bands are
// sized adaptively: allocated lazily on a symbol's first add, grown
// geometrically on out-of-band hits (counted), capped at max_width_ticks;
// beyond the cap, a std::map overflow absorbs opening prints and fat
// fingers (counted — expect ≪0.1%).
//
// Best bid/ask are cached band indices repaired by scanning toward the
// centre on level-empty: emptied levels sit near the touch, so scans are
// short (scan lengths are counted — the §5.3 bitmap experiment needs the
// distribution). Orders come from a shared arena and are found via the
// shared robin-hood id map (§5.2); both are per-day global resources.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "book/order.hpp"
#include "book/types.hpp"
#include "core/id_map.hpp"
#include "core/pool.hpp"
#include "core/types.hpp"

namespace ob::book {

// Shared across every symbol's book: ITCH refs are unique for the whole day,
// so one arena and one id map serve all locates (§5.2). Size from the
// measured high-water mark with headroom; growths are counted, not silent.
struct BookResources {
    explicit BookResources(std::size_t expected_live_orders)
        : arena(expected_live_orders), ids(expected_live_orders) {}
    Pool<Order> arena;
    IdMap<Order*> ids;
};

struct BandConfig {
    std::uint32_t initial_half_width = 2048;  // ticks each side of first price
    std::uint32_t max_width = 1u << 17;       // hard cap; beyond -> overflow map
};

class FlatBook {
public:
    using Effect = book::Effect;

    struct Stats {
        std::uint64_t band_growths = 0;
        std::uint64_t overflow_hits = 0;   // ops applied to overflow levels
        std::uint64_t best_repairs = 0;    // touch-level-emptied scans
        std::uint64_t repair_steps = 0;    // total levels stepped over in repairs
    };

    FlatBook(BookResources& res, StockLocate locate, BandConfig cfg = {})
        : res_(res), locate_(locate), cfg_(cfg) {}

    Effect add(OrderId ref, Side side, Price price, Qty qty) {
        if (res_.ids.find(ref) != nullptr) {
            return {.result = Apply::duplicate_ref};
        }
        Order* o = res_.arena.alloc();
        o->id = ref;
        o->price = price;
        o->remaining = qty;
        o->side = side;
        o->locate = locate_;
        PriceLevel& lvl = level_create(band(side), side, price);
        fifo_push_back(lvl, o);
        lvl.total_qty += qty;
        ++lvl.order_count;
        ++live_orders_;
        res_.ids.insert(ref, o);
        return {.result = Apply::ok,
                .side = side,
                .price = price,
                .level_qty_after = lvl.total_qty};
    }

    Effect execute(OrderId ref, Qty qty) { return reduce(ref, qty); }
    Effect cancel(OrderId ref, Qty qty) { return reduce(ref, qty); }

    Effect remove(OrderId ref) {
        Order* o = lookup(ref);
        if (o == nullptr) {
            return {.result = Apply::unknown_ref};
        }
        return unlink(o, /*applied=*/o->remaining);
    }

    [[nodiscard]] std::optional<OrderSnap> order_snapshot(OrderId ref) const {
        const Order* o = const_cast<FlatBook*>(this)->lookup(ref);
        if (o == nullptr) return std::nullopt;
        return OrderSnap{o->side, o->price, o->remaining};
    }

    [[nodiscard]] std::optional<Price> best_bid() const { return best_price(Side::Bid); }
    [[nodiscard]] std::optional<Price> best_ask() const { return best_price(Side::Ask); }

    // Cold query (goldens, dumps): collect non-empty levels best-first.
    [[nodiscard]] std::vector<LevelView> l2(Side side, std::size_t depth = 0) const {
        const SideBand& b = band(side);
        std::vector<LevelView> out;
        for (const auto& [px, lvl] : b.overflow) {
            out.push_back({px, lvl.total_qty, lvl.order_count});
        }
        for (std::size_t i = 0; i < b.levels.size(); ++i) {
            const PriceLevel& lvl = b.levels[i];
            if (lvl.order_count > 0) {
                out.push_back({b.base + static_cast<Price>(i), lvl.total_qty,
                               lvl.order_count});
            }
        }
        if (side == Side::Bid) {
            std::sort(out.begin(), out.end(),
                      [](const LevelView& x, const LevelView& y) { return x.price > y.price; });
        } else {
            std::sort(out.begin(), out.end(),
                      [](const LevelView& x, const LevelView& y) { return x.price < y.price; });
        }
        if (depth != 0 && out.size() > depth) out.resize(depth);
        return out;
    }

    [[nodiscard]] std::vector<OrderId> level_fifo(Side side, Price price) const {
        const SideBand& b = band(side);
        const PriceLevel* lvl = nullptr;
        if (in_band(b, price)) {
            lvl = &b.levels[static_cast<std::size_t>(price - b.base)];
        } else if (auto it = b.overflow.find(price); it != b.overflow.end()) {
            lvl = &it->second;
        }
        std::vector<OrderId> out;
        if (lvl != nullptr) {
            for (const Order* o = lvl->head; o != nullptr; o = o->next) {
                out.push_back(o->id);
            }
        }
        return out;
    }

    [[nodiscard]] std::size_t live_orders() const noexcept { return live_orders_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t memory_bytes() const noexcept {
        return (bids_.levels.capacity() + asks_.levels.capacity()) * sizeof(PriceLevel);
    }

    // Structural invariant sweep (§9.2). The book-not-crossed check stays
    // market-phase-gated at the caller (§4.1).
    [[nodiscard]] bool validate() const {
        std::size_t seen = 0;
        auto check_level = [&](const PriceLevel& lvl, Side side, Price px) {
            if (lvl.order_count == 0) {
                return lvl.total_qty == 0 && lvl.head == nullptr && lvl.tail == nullptr;
            }
            Qty sum = 0;
            std::uint32_t n = 0;
            const Order* prev = nullptr;
            for (const Order* o = lvl.head; o != nullptr; o = o->next) {
                if (o->prev != prev || o->side != side || o->price != px ||
                    o->locate != locate_ || o->remaining == 0) {
                    return false;
                }
                Order* const* mapped = res_.ids.find(o->id);
                if (mapped == nullptr || *mapped != o) return false;
                sum += o->remaining;
                ++n;
                prev = o;
            }
            if (lvl.tail != prev) return false;
            seen += n;
            return sum == lvl.total_qty && n == lvl.order_count;
        };
        auto check_side = [&](const SideBand& b, Side side) {
            for (std::size_t i = 0; i < b.levels.size(); ++i) {
                if (!check_level(b.levels[i], side, b.base + static_cast<Price>(i))) {
                    return false;
                }
            }
            for (const auto& [px, lvl] : b.overflow) {
                if (lvl.order_count == 0) return false;  // empty overflow must be erased
                if (in_band(b, px)) return false;        // shadowed by the band: bug
                if (!check_level(lvl, side, px)) return false;
            }
            // Cached best must equal a fresh scan.
            const auto scanned = scan_best(b, side);
            if (scanned != b.best) return false;
            return true;
        };
        return check_side(bids_, Side::Bid) && check_side(asks_, Side::Ask) &&
               seen == live_orders_;
    }

private:
    struct SideBand {
        Price base = 0;  // price of levels[0]; valid only when !levels.empty()
        std::vector<PriceLevel> levels;
        std::map<Price, PriceLevel> overflow;
        std::ptrdiff_t best = -1;      // band index of best level, -1 = none
        std::size_t band_orders = 0;   // live orders resident in the band
    };

    [[nodiscard]] SideBand& band(Side s) noexcept { return s == Side::Bid ? bids_ : asks_; }
    [[nodiscard]] const SideBand& band(Side s) const noexcept {
        return s == Side::Bid ? bids_ : asks_;
    }
    [[nodiscard]] static bool in_band(const SideBand& b, Price px) noexcept {
        return !b.levels.empty() && px >= b.base &&
               static_cast<std::size_t>(px - b.base) < b.levels.size();
    }

    Order* lookup(OrderId ref) {
        Order** slot = res_.ids.find(ref);
        if (slot == nullptr || (*slot)->locate != locate_) {
            return nullptr;  // wrong-locate hit = hostile input, treat as unknown
        }
        return *slot;
    }

    static void fifo_push_back(PriceLevel& lvl, Order* o) {
        o->next = nullptr;
        o->prev = lvl.tail;
        if (lvl.tail != nullptr) {
            lvl.tail->next = o;
        } else {
            lvl.head = o;
        }
        lvl.tail = o;
    }

    static void fifo_unlink(PriceLevel& lvl, Order* o) {
        (o->prev != nullptr ? o->prev->next : lvl.head) = o->next;
        (o->next != nullptr ? o->next->prev : lvl.tail) = o->prev;
    }

    Effect reduce(OrderId ref, Qty qty) {
        Order* o = lookup(ref);
        if (o == nullptr) {
            return {.result = Apply::unknown_ref};
        }
        const bool clamp = qty > o->remaining;  // hostile input (§4.1): clamp, flag
        const Qty applied = clamp ? o->remaining : qty;
        if (applied == o->remaining) {
            Effect e = unlink(o, applied);
            if (clamp) e.result = Apply::clamped;
            return e;
        }
        // Partial: order keeps its FIFO position — time priority retained (§5.3).
        o->remaining -= applied;
        PriceLevel* lvl = level_find(band(o->side), o->price);
        lvl->total_qty -= applied;
        return {.result = Apply::ok,
                .side = o->side,
                .price = o->price,
                .level_qty_after = lvl->total_qty,
                .applied = applied};
    }

    Effect unlink(Order* o, Qty applied) {
        SideBand& b = band(o->side);
        Effect e{.result = Apply::ok,
                 .side = o->side,
                 .price = o->price,
                 .applied = applied,
                 .order_removed = true};
        if (in_band(b, o->price)) {
            const auto idx = static_cast<std::ptrdiff_t>(o->price - b.base);
            PriceLevel& lvl = b.levels[static_cast<std::size_t>(idx)];
            fifo_unlink(lvl, o);
            lvl.total_qty -= o->remaining;
            --lvl.order_count;
            --b.band_orders;
            e.level_qty_after = lvl.total_qty;
            if (lvl.order_count == 0) {
                e.level_removed = true;
                if (idx == b.best) repair_best(b, o->side);
            }
        } else {
            ++stats_.overflow_hits;
            auto it = b.overflow.find(o->price);
            PriceLevel& lvl = it->second;
            fifo_unlink(lvl, o);
            lvl.total_qty -= o->remaining;
            --lvl.order_count;
            e.level_qty_after = lvl.total_qty;
            if (lvl.order_count == 0) {
                e.level_removed = true;
                b.overflow.erase(it);
            }
        }
        res_.ids.erase(o->id);
        res_.arena.free(o);
        --live_orders_;
        return e;
    }

    // Find an existing level for (side, price); nullptr if none.
    PriceLevel* level_find(SideBand& b, Price px) {
        if (in_band(b, px)) {
            return &b.levels[static_cast<std::size_t>(px - b.base)];
        }
        auto it = b.overflow.find(px);
        if (it == b.overflow.end()) return nullptr;
        ++stats_.overflow_hits;
        return &it->second;
    }

    // Find-or-create the level for an add, maintaining the cached best.
    PriceLevel& level_create(SideBand& b, Side side, Price px) {
        if (b.levels.empty()) {
            // Lazy first allocation, centred on the first price seen (§5.1).
            const Price half = cfg_.initial_half_width;
            b.base = px > half ? px - half : 0;
            b.levels.assign(static_cast<std::size_t>(half) * 2 + 1, PriceLevel{});
        }
        if (!in_band(b, px) && growable_to(b, px)) {
            grow(b, px);
        }
        if (in_band(b, px)) {
            const auto idx = static_cast<std::ptrdiff_t>(px - b.base);
            // Invariant: best == -1 iff band_orders == 0, so the empty case
            // needs no better() call (which never fires for asks vs -1).
            if (b.band_orders == 0 || better(side, idx, b.best)) {
                b.best = idx;
            }
            ++b.band_orders;
            return b.levels[static_cast<std::size_t>(idx)];
        }
        ++stats_.overflow_hits;
        return b.overflow[px];  // creates if absent; map nodes are stable
    }

    [[nodiscard]] static bool better(Side s, std::ptrdiff_t a, std::ptrdiff_t b) noexcept {
        return s == Side::Bid ? a > b : a < b;
    }

    // Would the band stay within max_width if extended to cover px?
    [[nodiscard]] bool growable_to(const SideBand& b, Price px) const noexcept {
        const Price lo = px < b.base ? px : b.base;
        const Price top = b.base + static_cast<Price>(b.levels.size() - 1);
        const Price hi = px > top ? px : top;
        return (hi - lo) < cfg_.max_width;
    }

    // Extend coverage to include px plus geometric margin; migrate any
    // overflow levels the new range swallows (they'd be shadowed otherwise).
    void grow(SideBand& b, Price px) {
        ++stats_.band_growths;
        const Price old_top = b.base + static_cast<Price>(b.levels.size() - 1);
        const Price margin = static_cast<Price>(b.levels.size() / 2);
        Price new_base = b.base;
        Price new_top = old_top;
        // Extend only toward px, with geometric margin; clamp the margin (never
        // the old coverage — shrinking it would drop live levels) at max_width.
        // growable_to() guarantees px itself stays inside the clamped range.
        if (px < b.base) {
            new_base = px > margin ? px - margin : 0;
            if (new_top - new_base >= cfg_.max_width) {
                new_base = new_top - (cfg_.max_width - 1);
            }
        } else {
            new_top = px < ~Price{0} - margin ? px + margin : ~Price{0};
            if (new_top - new_base >= cfg_.max_width) {
                new_top = new_base + cfg_.max_width - 1;
            }
        }
        std::vector<PriceLevel> grown(static_cast<std::size_t>(new_top - new_base) + 1);
        const auto shift = static_cast<std::size_t>(b.base - new_base);
        for (std::size_t i = 0; i < b.levels.size(); ++i) {
            grown[i + shift] = b.levels[i];
        }
        b.levels = std::move(grown);
        b.base = new_base;
        if (b.best >= 0) b.best += static_cast<std::ptrdiff_t>(shift);
        // Swallow overflow levels now inside the band (they would be
        // shadowed otherwise). NOTE: unreachable under the current
        // grow-toward-price policy — an overflow key's distance from the
        // anchored base never shrinks — but a future re-centring rebase
        // (§5.1) makes this live, so it is kept and covered by validate().
        for (auto it = b.overflow.begin(); it != b.overflow.end();) {
            if (in_band(b, it->first)) {
                const auto idx = static_cast<std::ptrdiff_t>(it->first - b.base);
                b.levels[static_cast<std::size_t>(idx)] = it->second;
                b.band_orders += it->second.order_count;
                const Side side = it->second.head->side;
                if (b.band_orders == it->second.order_count || better(side, idx, b.best)) {
                    b.best = idx;
                }
                it = b.overflow.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Touch level emptied: scan toward the centre for the next non-empty
    // level (§5.3 — short in practice because emptied levels sit near the
    // touch; lengths counted for the bitmap-experiment comparison).
    void repair_best(SideBand& b, Side side) {
        ++stats_.best_repairs;
        if (b.band_orders == 0) {
            b.best = -1;
            return;
        }
        std::ptrdiff_t i = b.best;
        const std::ptrdiff_t step = side == Side::Bid ? -1 : 1;
        const auto n = static_cast<std::ptrdiff_t>(b.levels.size());
        while (true) {
            i += step;
            ++stats_.repair_steps;
            if (i < 0 || i >= n) {  // unreachable while band_orders > 0
                b.best = -1;
                return;
            }
            if (b.levels[static_cast<std::size_t>(i)].order_count > 0) {
                b.best = i;
                return;
            }
        }
    }

    [[nodiscard]] std::optional<Price> best_price(Side side) const {
        const SideBand& b = band(side);
        std::optional<Price> band_best;
        if (b.best >= 0) {
            band_best = b.base + static_cast<Price>(b.best);
        }
        if (!b.overflow.empty()) {  // rare: fat fingers / opening prints
            const Price ov = side == Side::Bid ? b.overflow.rbegin()->first
                                               : b.overflow.begin()->first;
            if (!band_best || better(side, static_cast<std::ptrdiff_t>(ov),
                                     static_cast<std::ptrdiff_t>(*band_best))) {
                return ov;
            }
        }
        return band_best;
    }

    // Fresh best scan for validate(): must match the cached index.
    [[nodiscard]] static std::ptrdiff_t scan_best(const SideBand& b, Side side) {
        if (b.band_orders == 0) return -1;
        const auto n = static_cast<std::ptrdiff_t>(b.levels.size());
        if (side == Side::Bid) {
            for (std::ptrdiff_t i = n - 1; i >= 0; --i) {
                if (b.levels[static_cast<std::size_t>(i)].order_count > 0) return i;
            }
        } else {
            for (std::ptrdiff_t i = 0; i < n; ++i) {
                if (b.levels[static_cast<std::size_t>(i)].order_count > 0) return i;
            }
        }
        return -1;
    }

    BookResources& res_;
    StockLocate locate_;
    BandConfig cfg_;
    SideBand bids_;
    SideBand asks_;
    std::size_t live_orders_ = 0;
    Stats stats_;
};

}  // namespace ob::book
