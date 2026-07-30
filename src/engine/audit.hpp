#pragma once

// Execution-at-front audit (DESIGN §9.3): in reconstruct mode, when an
// Execute (E/C) hits order X during continuous trading, price-time priority
// implies X should be at the FRONT of its price level's FIFO. A high pass
// rate is strong stream-derived evidence the FIFO ordering is right — with
// NO external oracle needed, the stream audits itself.
//
// Counted, never asserted: legitimate exceptions exist (§9.3) — pre-open
// and cross executions (matched by auction logic, not book priority),
// halted-symbol reopens, and mid-stream starts (orders added before the
// stream began). Exceptions are categorised so the README can explain them.
//
// Implemented as a wrapping visitor: the check runs BEFORE the event is
// forwarded (afterwards the filled order may already be gone).

#include <cstdint>

#include "core/types.hpp"
#include "engine/engine.hpp"
#include "itch/messages.hpp"
#include "itch/parser.hpp"

namespace ob::engine {

template <typename Inner>
class FrontAudit : public itch::NullVisitor {
public:
    // E and C are tracked separately: E executes at the resting price (book
    // priority should hold), while C is a price-improved execution — a
    // midpoint or better print may legitimately hit an order that is NOT at
    // the front of its displayed level. Separating them turns "1.5% failed"
    // into an explanation (§9.3: explained exceptions are the deliverable).
    struct Stats {
        std::uint64_t checked_e = 0;
        std::uint64_t at_front_e = 0;
        std::uint64_t checked_c = 0;
        std::uint64_t at_front_c = 0;
        std::uint64_t skipped_preopen = 0;   // before System Event 'Q' / after 'M'
        std::uint64_t skipped_halted = 0;    // symbol not in state 'T'
        std::uint64_t skipped_unknown = 0;   // ref not in book (mid-stream start)
        [[nodiscard]] std::uint64_t checked() const { return checked_e + checked_c; }
        [[nodiscard]] std::uint64_t at_front() const { return at_front_e + at_front_c; }
        [[nodiscard]] static double rate(std::uint64_t hit, std::uint64_t total) {
            return total == 0 ? 0.0
                              : static_cast<double>(hit) / static_cast<double>(total);
        }
        [[nodiscard]] double pass_rate() const { return rate(at_front(), checked()); }
        [[nodiscard]] double pass_rate_e() const { return rate(at_front_e, checked_e); }
        [[nodiscard]] double pass_rate_c() const { return rate(at_front_c, checked_c); }
    };

    explicit FrontAudit(Inner& inner) : inner_(inner) {}

    void on_order_executed(const itch::OrderExecuted& m) {
        check(m.h.locate, m.ref, stats_.checked_e, stats_.at_front_e);
        inner_.on_order_executed(m);
    }
    void on_order_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        check(m.h.locate, m.ref, stats_.checked_c, stats_.at_front_c);
        inner_.on_order_executed_with_price(m);
    }

    // Everything else passes straight through.
    void on_system_event(const itch::SystemEvent& m) { inner_.on_system_event(m); }
    void on_stock_directory(const itch::StockDirectory& m) { inner_.on_stock_directory(m); }
    void on_trading_action(const itch::TradingAction& m) { inner_.on_trading_action(m); }
    void on_add_order(const itch::AddOrder& m) { inner_.on_add_order(m); }
    void on_order_cancel(const itch::OrderCancel& m) { inner_.on_order_cancel(m); }
    void on_order_delete(const itch::OrderDelete& m) { inner_.on_order_delete(m); }
    void on_order_replace(const itch::OrderReplace& m) { inner_.on_order_replace(m); }
    void on_trade(const itch::Trade& m) { inner_.on_trade(m); }
    void on_cross_trade(const itch::CrossTrade& m) { inner_.on_cross_trade(m); }

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    void check(StockLocate loc, OrderId ref, std::uint64_t& checked, std::uint64_t& hit) {
        if (inner_.phase() != MarketPhase::open) {
            ++stats_.skipped_preopen;
            return;
        }
        if (inner_.trading_state(loc) != 'T') {
            ++stats_.skipped_halted;
            return;
        }
        const auto* book = inner_.book(loc);
        if (book == nullptr) {
            ++stats_.skipped_unknown;
            return;
        }
        const auto snap = book->order_snapshot(ref);
        if (!snap) {
            ++stats_.skipped_unknown;
            return;
        }
        ++checked;
        const auto front = book->front_order(snap->side, snap->price);
        if (front && front->ref == ref) {
            ++hit;
        }
    }

    Inner& inner_;
    Stats stats_;
};

}  // namespace ob::engine
