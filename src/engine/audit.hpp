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
    struct Stats {
        std::uint64_t checked = 0;        // executions audited (market open, trading)
        std::uint64_t at_front = 0;       // ... of which hit the FIFO front
        std::uint64_t skipped_preopen = 0;   // before System Event 'Q' / after 'M'
        std::uint64_t skipped_halted = 0;    // symbol not in state 'T'
        std::uint64_t skipped_unknown = 0;   // ref not in book (mid-stream start)
        [[nodiscard]] double pass_rate() const {
            return checked == 0 ? 0.0
                                : static_cast<double>(at_front) / static_cast<double>(checked);
        }
    };

    explicit FrontAudit(Inner& inner) : inner_(inner) {}

    void on_order_executed(const itch::OrderExecuted& m) {
        check(m.h.locate, m.ref);
        inner_.on_order_executed(m);
    }
    void on_order_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        check(m.h.locate, m.ref);
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
    void check(StockLocate loc, OrderId ref) {
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
        ++stats_.checked;
        const auto front = book->front_order(snap->side, snap->price);
        if (front && front->ref == ref) {
            ++stats_.at_front;
        }
    }

    Inner& inner_;
    Stats stats_;
};

}  // namespace ob::engine
