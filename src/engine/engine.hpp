#pragma once

// Reconstruct-mode engine (DESIGN §6): applies ITCH events verbatim to
// per-symbol books. The exchange already matched — this mode contains ZERO
// matching logic, or crossed pre-open books (§4.1) would silently corrupt
// state. It is an ITCH visitor: parser -> Engine -> Book -> Listener, all
// dispatch resolved at compile time.
//
// Symbol dispatch (§4): the uint16 stock locate indexes a flat array of
// Book* — the exchange hands you a perfect hash; use it. Books are created
// lazily on first touch, so resident memory tracks ACTIVE symbols.

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "book/listener.hpp"
#include "core/types.hpp"
#include "engine/match.hpp"
#include "itch/messages.hpp"
#include "itch/parser.hpp"

namespace ob::engine {

enum class MarketPhase : std::uint8_t { pre_open, open, closed };

namespace detail {
template <typename Book>
struct DefaultMakeBook {
    std::unique_ptr<Book> operator()(StockLocate) const { return std::make_unique<Book>(); }
};
}  // namespace detail

// MakeBook lets book types with shared resources (FlatBook: global arena +
// id map) be constructed lazily per locate; the default works for any
// default-constructible book.
template <typename Book, typename Listener = book::NullListener,
          typename MakeBook = detail::DefaultMakeBook<Book>>
class Engine : public itch::NullVisitor {
public:
    static constexpr std::size_t kMaxLocates = 65536;

    struct Stats {
        std::uint64_t unknown_ref = 0;    // count-and-skip per §4.1
        std::uint64_t duplicate_ref = 0;
        std::uint64_t clamped = 0;        // exec/cancel qty exceeded remaining
        std::uint64_t volume_lit = 0;     // E + printable C, shares
        std::uint64_t volume_hidden = 0;  // P prints (non-displayed liquidity)
        std::uint64_t volume_cross = 0;   // Q prints, by definition no book effect
        std::uint64_t nonprintable_execs = 0;  // C with printable='N'
        std::uint64_t volume_matched = 0;      // match-mode fills (synthetic flow)
        std::uint64_t rejected_halted = 0;     // match-mode submits on halted symbols
    };

    explicit Engine(Listener listener = {}, MakeBook make_book = {})
        : listener_(std::move(listener)), make_book_(std::move(make_book)) {
        books_.resize(kMaxLocates);
        trading_state_.assign(kMaxLocates, 'T');
        symbols_.resize(kMaxLocates);
    }

    // --- ITCH visitor handlers -------------------------------------------

    void on_system_event(const itch::SystemEvent& m) {
        if (m.event == 'Q') phase_ = MarketPhase::open;
        if (m.event == 'M') phase_ = MarketPhase::closed;
    }

    void on_stock_directory(const itch::StockDirectory& m) {
        symbols_[m.h.locate] = m.stock;
    }

    void on_trading_action(const itch::TradingAction& m) {
        // Reconstruct mode applies events regardless of halt state (the feed
        // is the truth, §4.1); the state exists for match mode to reject on.
        trading_state_[m.h.locate] = m.state;
    }

    void on_add_order(const itch::AddOrder& m) {
        auto e = book_for(m.h.locate).add(m.ref, m.side, m.price, m.shares);
        if (!applied(e)) return;
        listener_.on_add(m.h.locate, m.ref, e.side, e.price, m.shares);
        listener_.on_level_change(m.h.locate, e.side, e.price, e.level_qty_after);
    }

    void on_order_executed(const itch::OrderExecuted& m) {
        auto e = book_for(m.h.locate).execute(m.ref, m.shares);
        if (!applied(e)) return;
        stats_.volume_lit += e.applied;
        // E executes at the resting price, always printable.
        listener_.on_exec(m.h.locate, m.ref, e.side, e.price, e.applied, true);
        listener_.on_level_change(m.h.locate, e.side, e.price, e.level_qty_after);
    }

    void on_order_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        // Book removal is keyed by ref at the RESTING price; m.price is the
        // (possibly improved) print price and only feeds the tape (§4.1).
        auto e = book_for(m.h.locate).execute(m.ref, m.shares);
        if (!applied(e)) return;
        if (m.printable) {
            stats_.volume_lit += e.applied;
        } else {
            ++stats_.nonprintable_execs;  // book decremented, volume untouched
        }
        listener_.on_exec(m.h.locate, m.ref, e.side, m.price, e.applied, m.printable);
        listener_.on_level_change(m.h.locate, e.side, e.price, e.level_qty_after);
    }

    void on_order_cancel(const itch::OrderCancel& m) {
        auto e = book_for(m.h.locate).cancel(m.ref, m.cancelled);
        if (!applied(e)) return;
        listener_.on_cancel(m.h.locate, m.ref, e.side, e.price, e.applied);
        listener_.on_level_change(m.h.locate, e.side, e.price, e.level_qty_after);
    }

    void on_order_delete(const itch::OrderDelete& m) {
        auto e = book_for(m.h.locate).remove(m.ref);
        if (!applied(e)) return;
        listener_.on_cancel(m.h.locate, m.ref, e.side, e.price, e.applied);
        listener_.on_level_change(m.h.locate, e.side, e.price, e.level_qty_after);
    }

    void on_order_replace(const itch::OrderReplace& m) {
        // U carries no side (§4.1): snapshot the original, remove it, add the
        // replacement on the inherited side at the tail of its new level.
        // A missing original in this locate's book is the locate-consistency
        // assert in practice — counted, skipped, no partial state change.
        auto& bk = book_for(m.h.locate);
        const auto orig = bk.order_snapshot(m.orig_ref);
        if (!orig) {
            ++stats_.unknown_ref;
            return;
        }
        if (bk.order_snapshot(m.new_ref)) {
            ++stats_.duplicate_ref;
            return;
        }
        auto del = bk.remove(m.orig_ref);
        listener_.on_cancel(m.h.locate, m.orig_ref, del.side, del.price, del.applied);
        listener_.on_level_change(m.h.locate, del.side, del.price, del.level_qty_after);
        auto add = bk.add(m.new_ref, orig->side, m.price, m.shares);
        listener_.on_add(m.h.locate, m.new_ref, add.side, add.price, m.shares);
        listener_.on_level_change(m.h.locate, add.side, add.price, add.level_qty_after);
    }

    void on_trade(const itch::Trade& m) {
        // Execution against non-displayed liquidity: NO book mutation (§4.1).
        stats_.volume_hidden += m.shares;
    }

    void on_cross_trade(const itch::CrossTrade& m) {
        // Cross volume print; displayed participants get their own E/C (§4.1).
        stats_.volume_cross += m.shares;
    }

    // --- match mode (§6): synthetic flow ---------------------------------
    // Not for mixing with reconstruct on the same symbols: reconstruct
    // mirrors an exchange that already matched; submit() IS the exchange.

    template <typename OnFill>
    SubmitResult submit(StockLocate loc, OrderId ref, Side side, Price limit, Qty qty,
                        Tif tif, OnFill&& on_fill) {
        if (halted(loc)) {
            ++stats_.rejected_halted;
            return {.status = SubmitStatus::rejected_halted};
        }
        struct Hooks {
            Engine& eng;
            StockLocate loc;
            Side taker_side;
            OnFill& user_fill;
            void on_fill(const Fill& f, const typename Book::Effect& e) {
                eng.stats_.volume_matched += f.qty;
                // The maker is the book resident; the tape shows its side.
                eng.listener_.on_exec(loc, f.maker, opposite(taker_side), f.price, f.qty,
                                      true);
                eng.listener_.on_level_change(loc, opposite(taker_side), f.price,
                                              e.level_qty_after);
                user_fill(f);
            }
            void on_rest(const typename Book::Effect& e) {
                eng.listener_.on_level_change(loc, e.side, e.price, e.level_qty_after);
            }
        };
        Hooks hooks{*this, loc, side, on_fill};
        auto res = engine::submit(book_for(loc), ref, side, limit, qty, tif, hooks);
        if (res.resting > 0) {
            listener_.on_add(loc, ref, side, limit, res.resting);
        }
        return res;
    }

    SubmitResult submit(StockLocate loc, OrderId ref, Side side, Price limit, Qty qty,
                        Tif tif = Tif::limit) {
        auto noop = [](const Fill&) {};
        return submit(loc, ref, side, limit, qty, tif, noop);
    }

    // Synthetic cancel/replace reuse the reconstruct plumbing exactly.
    void cancel(StockLocate loc, OrderId ref, Qty qty) {
        on_order_cancel(itch::OrderCancel{{loc, 0, 0}, ref, qty});
    }
    void erase(StockLocate loc, OrderId ref) {
        on_order_delete(itch::OrderDelete{{loc, 0, 0}, ref});
    }
    // Replace re-prices through the MATCHING path: side inherited from the
    // original, and the new order may execute immediately if now marketable.
    SubmitResult replace(StockLocate loc, OrderId orig, OrderId new_ref, Price limit,
                         Qty qty, Tif tif = Tif::limit) {
        auto& bk = book_for(loc);
        const auto snap = bk.order_snapshot(orig);
        if (!snap) {
            ++stats_.unknown_ref;
            return {.status = SubmitStatus::rejected_invalid};
        }
        erase(loc, orig);
        return submit(loc, new_ref, snap->side, limit, qty, tif);
    }

    // --- introspection ----------------------------------------------------

    [[nodiscard]] MarketPhase phase() const noexcept { return phase_; }
    [[nodiscard]] char trading_state(StockLocate loc) const { return trading_state_[loc]; }
    [[nodiscard]] bool halted(StockLocate loc) const {
        const char s = trading_state_[loc];
        return s == 'H' || s == 'P';
    }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] const itch::Stock& symbol(StockLocate loc) const { return symbols_[loc]; }
    [[nodiscard]] Listener& listener() noexcept { return listener_; }

    // Null when the locate was never touched (books are lazy).
    [[nodiscard]] const Book* book(StockLocate loc) const { return books_[loc].get(); }
    [[nodiscard]] Book* book(StockLocate loc) { return books_[loc].get(); }

private:
    Book& book_for(StockLocate loc) {
        auto& slot = books_[loc];
        if (!slot) slot = make_book_(loc);
        return *slot;
    }

    bool applied(const typename Book::Effect& e) {
        switch (e.result) {
            case book::Apply::ok:
                return true;
            case book::Apply::clamped:
                ++stats_.clamped;
                return true;  // clamped ops still mutated the book
            case book::Apply::unknown_ref:
                ++stats_.unknown_ref;
                return false;
            case book::Apply::duplicate_ref:
                ++stats_.duplicate_ref;
                return false;
        }
        return false;
    }

    std::vector<std::unique_ptr<Book>> books_;
    std::vector<char> trading_state_;
    std::vector<itch::Stock> symbols_;
    Listener listener_;
    MakeBook make_book_;
    Stats stats_{};
    MarketPhase phase_ = MarketPhase::pre_open;
};

}  // namespace ob::engine
