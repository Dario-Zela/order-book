#pragma once

// Decoded (host-endian) views of the NASDAQ TotalView-ITCH 5.0 v1 message
// subset (DESIGN §4). Field offsets follow the spec layout exactly; offset 0
// is the message-type byte. Every message shares an 11-byte header:
//   type(1) | stock locate(2) | tracking number(2) | timestamp(6, ns since midnight)

#include <array>
#include <cstdint>

#include "core/types.hpp"

namespace ob::itch {

using Stock = std::array<char, 8>;  // right-padded with spaces, as on the wire

struct Header {
    StockLocate locate;
    std::uint16_t tracking;
    Timestamp timestamp;
};

// 'S', 12 bytes. Event codes: 'O' start of messages, 'S' start of system
// hours, 'Q' start of market hours, 'M' end of market hours, 'E' end of
// system hours, 'C' end of messages. 'Q' gates the book-not-crossed
// invariant (DESIGN §4.1: pre-open books cross legitimately).
struct SystemEvent {
    Header h;
    char event;
};

// 'R', 39 bytes. Announces the locate code -> symbol binding for the day.
// Only the fields the engine consumes are decoded; the rest are skipped.
struct StockDirectory {
    Header h;
    Stock stock;
    char market_category;
    char financial_status;
    Qty round_lot_size;
};

// 'H', 25 bytes. States: 'H' halted, 'P' paused, 'Q' quotation-only, 'T' trading.
struct TradingAction {
    Header h;
    Stock stock;
    char state;
    std::array<char, 4> reason;
};

// 'A' (36 bytes) and 'F' (40 bytes, adds MPID attribution) both surface here:
// the book treats them identically.
struct AddOrder {
    Header h;
    OrderId ref;
    Side side;
    Qty shares;
    Stock stock;
    Price price;
    bool has_mpid;
    std::array<char, 4> attribution;  // meaningful only when has_mpid
};

// 'E', 31 bytes. Execution at the resting order's price.
struct OrderExecuted {
    Header h;
    OrderId ref;
    Qty shares;
    std::uint64_t match;
};

// 'C', 36 bytes. Execution price may differ from the resting display price
// (price improvement) — book removal is keyed by ref, not price. Non-printable
// ('N') executions decrement the book but must not count toward trade volume
// (DESIGN §4.1).
struct OrderExecutedWithPrice {
    Header h;
    OrderId ref;
    Qty shares;
    std::uint64_t match;
    bool printable;
    Price price;
};

// 'X', 23 bytes. Partial cancel: reduces remaining shares.
struct OrderCancel {
    Header h;
    OrderId ref;
    Qty cancelled;
};

// 'D', 19 bytes.
struct OrderDelete {
    Header h;
    OrderId ref;
};

// 'U', 35 bytes. Carries only {orig ref, new ref, new TOTAL shares, new
// price} — no side field: side is inherited from the original order
// (DESIGN §4.1). New ref means new time priority, even at the same price.
struct OrderReplace {
    Header h;
    OrderId orig_ref;
    OrderId new_ref;
    Qty shares;  // new total, not a delta
    Price price;
};

// 'P', 44 bytes. Execution against a NON-displayed order: no book mutation
// at all, volume stats only (DESIGN §4.1).
struct Trade {
    Header h;
    OrderId ref;  // always 0 in ITCH 5.0, retained for spec fidelity
    Side side;
    Qty shares;
    Stock stock;
    Price price;
    std::uint64_t match;
};

// 'Q', 40 bytes. Cross volume print — no direct book mutation; displayed
// participants get their own E/C messages. NOTE: shares is 8 bytes here,
// unlike every other message (DESIGN §4). Cross types: 'O' opening,
// 'C' closing, 'H' halt/IPO reopen.
struct CrossTrade {
    Header h;
    std::uint64_t shares;
    Stock stock;
    Price price;
    std::uint64_t match;
    char cross_type;
};

// Wire sizes (including the type byte) for exact-length validation.
constexpr std::size_t wire_size(char type) noexcept {
    switch (type) {
        case 'S': return 12;
        case 'R': return 39;
        case 'H': return 25;
        case 'A': return 36;
        case 'F': return 40;
        case 'E': return 31;
        case 'C': return 36;
        case 'X': return 23;
        case 'D': return 19;
        case 'U': return 35;
        case 'P': return 44;
        case 'Q': return 40;
        default: return 0;  // not in the v1 subset
    }
}

}  // namespace ob::itch
