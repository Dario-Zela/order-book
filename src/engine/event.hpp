#pragma once

// Normalized, host-endian book event — the SPSC ring's slot type (DESIGN
// §7): decode cost stays on the producer core; the consumer applies fixed
// 48-byte PODs. EventEncoder turns parsed ITCH messages into events;
// apply_event turns an event back into visitor calls, so the SAME Engine
// consumes either raw parser output (single-thread mode) or ring events
// (pipeline mode) with no separate code path to drift.

#include <cstdint>
#include <cstring>
#include <type_traits>

#include "core/types.hpp"
#include "itch/messages.hpp"
#include "itch/parser.hpp"

namespace ob::engine {

enum class EventKind : std::uint8_t {
    system,
    directory,
    trading_action,
    add,
    exec,
    exec_price,
    cancel,
    del,
    replace,
    trade,
    cross,
};

struct BookEvent {
    EventKind kind{};
    Side side{};            // add only
    char flag{};            // system: event code; trading_action: state;
                            // exec_price: 'Y'/'N'
    char flag2{};           // cross: cross type
    StockLocate locate{};
    std::uint16_t _pad{};
    Price price = 0;
    Qty qty = 0;
    // Overloaded by kind (documented per case): order ref for order events;
    // the 8 stock bytes for directory; the 64-bit share count for cross.
    std::uint64_t ref = 0;
    std::uint64_t ref2 = 0;  // replace: new ref
    Timestamp ts = 0;        // ITCH event time (ns since midnight)
    std::uint64_t ingress_ns = 0;  // measurement stamp (§8), set by the producer
};

static_assert(sizeof(BookEvent) == 48, "fits well under one cache line");
static_assert(std::is_trivially_copyable_v<BookEvent>);

// ITCH visitor that forwards each message as a BookEvent to Emit. P and Q
// are forwarded too: they carry no book mutation but the consumer keeps the
// volume stats (§4.1).
template <typename Emit>
class EventEncoder : public itch::NullVisitor {
public:
    explicit EventEncoder(Emit emit) : emit_(std::move(emit)) {}

    void on_system_event(const itch::SystemEvent& m) {
        emit_(BookEvent{.kind = EventKind::system,
                        .flag = m.event,
                        .locate = m.h.locate,
                        .ts = m.h.timestamp});
    }
    void on_stock_directory(const itch::StockDirectory& m) {
        BookEvent ev{.kind = EventKind::directory, .locate = m.h.locate, .ts = m.h.timestamp};
        std::memcpy(&ev.ref, m.stock.data(), 8);  // stock bytes ride in ref
        emit_(ev);
    }
    void on_trading_action(const itch::TradingAction& m) {
        emit_(BookEvent{.kind = EventKind::trading_action,
                        .flag = m.state,
                        .locate = m.h.locate,
                        .ts = m.h.timestamp});
    }
    void on_add_order(const itch::AddOrder& m) {
        emit_(BookEvent{.kind = EventKind::add,
                        .side = m.side,
                        .locate = m.h.locate,
                        .price = m.price,
                        .qty = m.shares,
                        .ref = m.ref,
                        .ts = m.h.timestamp});
    }
    void on_order_executed(const itch::OrderExecuted& m) {
        emit_(BookEvent{.kind = EventKind::exec,
                        .locate = m.h.locate,
                        .qty = m.shares,
                        .ref = m.ref,
                        .ts = m.h.timestamp});
    }
    void on_order_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        emit_(BookEvent{.kind = EventKind::exec_price,
                        .flag = m.printable ? 'Y' : 'N',
                        .locate = m.h.locate,
                        .price = m.price,
                        .qty = m.shares,
                        .ref = m.ref,
                        .ts = m.h.timestamp});
    }
    void on_order_cancel(const itch::OrderCancel& m) {
        emit_(BookEvent{.kind = EventKind::cancel,
                        .locate = m.h.locate,
                        .qty = m.cancelled,
                        .ref = m.ref,
                        .ts = m.h.timestamp});
    }
    void on_order_delete(const itch::OrderDelete& m) {
        emit_(BookEvent{.kind = EventKind::del,
                        .locate = m.h.locate,
                        .ref = m.ref,
                        .ts = m.h.timestamp});
    }
    void on_order_replace(const itch::OrderReplace& m) {
        emit_(BookEvent{.kind = EventKind::replace,
                        .locate = m.h.locate,
                        .price = m.price,
                        .qty = m.shares,
                        .ref = m.orig_ref,
                        .ref2 = m.new_ref,
                        .ts = m.h.timestamp});
    }
    void on_trade(const itch::Trade& m) {
        emit_(BookEvent{.kind = EventKind::trade,
                        .locate = m.h.locate,
                        .price = m.price,
                        .qty = m.shares,
                        .ts = m.h.timestamp});
    }
    void on_cross_trade(const itch::CrossTrade& m) {
        emit_(BookEvent{.kind = EventKind::cross,
                        .flag2 = m.cross_type,
                        .locate = m.h.locate,
                        .price = m.price,
                        .ref = m.shares,  // 64-bit cross shares ride in ref
                        .ts = m.h.timestamp});
    }

private:
    Emit emit_;
};

// Feed one event to any ITCH visitor (in practice: the Engine).
template <typename V>
void apply_event(const BookEvent& ev, V& v) {
    const itch::Header h{ev.locate, 0, ev.ts};
    switch (ev.kind) {
        case EventKind::system:
            v.on_system_event({h, ev.flag});
            break;
        case EventKind::directory: {
            itch::StockDirectory m{h, {}, ' ', ' ', 0};
            std::memcpy(m.stock.data(), &ev.ref, 8);
            v.on_stock_directory(m);
            break;
        }
        case EventKind::trading_action:
            v.on_trading_action({h, {}, ev.flag, {}});
            break;
        case EventKind::add:
            v.on_add_order({h, ev.ref, ev.side, ev.qty, {}, ev.price, false, {}});
            break;
        case EventKind::exec:
            v.on_order_executed({h, ev.ref, ev.qty, 0});
            break;
        case EventKind::exec_price:
            v.on_order_executed_with_price({h, ev.ref, ev.qty, 0, ev.flag == 'Y', ev.price});
            break;
        case EventKind::cancel:
            v.on_order_cancel({h, ev.ref, ev.qty});
            break;
        case EventKind::del:
            v.on_order_delete({h, ev.ref});
            break;
        case EventKind::replace:
            v.on_order_replace({h, ev.ref, ev.ref2, ev.qty, ev.price});
            break;
        case EventKind::trade:
            v.on_trade({h, 0, Side::Bid, ev.qty, {}, ev.price, 0});
            break;
        case EventKind::cross:
            v.on_cross_trade({h, ev.ref, {}, ev.price, 0, ev.flag2});
            break;
    }
}

}  // namespace ob::engine
