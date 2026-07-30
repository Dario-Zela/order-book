#pragma once

// Cursor-style parser over an ITCH 5.0 file dump: a sequence of
// [2-byte big-endian length][message] records (the emi.nasdaq.com "BinaryFILE"
// framing). Zero copies, zero allocation: the parser walks a span and hands
// decoded, host-endian message structs to a compile-time visitor — no virtual
// calls in the hot path (DESIGN §4).
//
// Robustness policy (DESIGN §4.1, fuzzing): unknown message types and
// length-mismatched messages are counted and skipped via on_unknown /
// on_malformed; a truncated final record ends the stream. Never crash,
// never allocate.

#include <cstddef>
#include <cstring>
#include <span>

#include "core/endian.hpp"
#include "core/types.hpp"
#include "itch/messages.hpp"

namespace ob::itch {

// Deriving from NullVisitor lets a visitor implement only the handlers it
// cares about. Dispatch is static (template parameter), so overriding is by
// name-shadowing — no virtuals involved.
struct NullVisitor {
    void on_system_event(const SystemEvent&) {}
    void on_stock_directory(const StockDirectory&) {}
    void on_trading_action(const TradingAction&) {}
    void on_add_order(const AddOrder&) {}
    void on_order_executed(const OrderExecuted&) {}
    void on_order_executed_with_price(const OrderExecutedWithPrice&) {}
    void on_order_cancel(const OrderCancel&) {}
    void on_order_delete(const OrderDelete&) {}
    void on_order_replace(const OrderReplace&) {}
    void on_trade(const Trade&) {}
    void on_cross_trade(const CrossTrade&) {}
    void on_unknown(char /*type*/, std::span<const std::byte> /*msg*/) {}
    void on_malformed(char /*type*/, std::span<const std::byte> /*msg*/) {}
};

namespace detail {

inline Header decode_header(const std::byte* p) noexcept {
    return Header{
        .locate = load_be<std::uint16_t>(p + 1),
        .tracking = load_be<std::uint16_t>(p + 3),
        .timestamp = load_be48(p + 5),
    };
}

inline Stock decode_stock(const std::byte* p) noexcept {
    Stock s;
    std::memcpy(s.data(), p, s.size());
    return s;
}

// 'B' -> Bid, 'S' -> Ask; anything else is a wire error the caller must treat
// as malformed rather than guess at.
inline bool decode_side(std::byte b, Side& out) noexcept {
    const char c = static_cast<char>(b);
    if (c == 'B') {
        out = Side::Bid;
        return true;
    }
    if (c == 'S') {
        out = Side::Ask;
        return true;
    }
    return false;
}

}  // namespace detail

class Parser {
public:
    struct Stats {
        std::uint64_t messages = 0;   // framed records processed (incl. unknown/malformed)
        std::uint64_t unknown = 0;    // types outside the v1 subset
        std::uint64_t malformed = 0;  // subset type with wrong length / bad field
        bool truncated = false;       // stream ended mid-record
    };

    explicit Parser(std::span<const std::byte> data) noexcept : data_(data) {}

    // Parse one framed message and dispatch it. Returns false at end of
    // stream (clean end, 0-length sentinel, or truncated tail — see stats()).
    template <typename V>
    bool next(V& v) {
        if (pos_ + 2 > data_.size()) {
            return false;  // clean end
        }
        const auto len = load_be<std::uint16_t>(data_.data() + pos_);
        if (len == 0) {
            return false;  // BinaryFILE end-of-session sentinel
        }
        if (pos_ + 2 + len > data_.size()) {
            stats_.truncated = true;
            return false;
        }
        dispatch(data_.subspan(pos_ + 2, len), v);
        pos_ += 2 + std::size_t{len};
        ++stats_.messages;
        return true;
    }

    // Drain the stream; returns messages dispatched by this call.
    template <typename V>
    std::uint64_t run(V& v) {
        const auto before = stats_.messages;
        while (next(v)) {
        }
        return stats_.messages - before;
    }

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t offset() const noexcept { return pos_; }

    // Dispatch one already-unframed message — MoldUDP64 blocks are bare ITCH
    // messages, so the network receiver feeds them here (same policy: never
    // crash, unknown/malformed go to the visitor's handlers).
    template <typename V>
    static void dispatch_one(std::span<const std::byte> m, V& v) {
        Stats sink;
        dispatch_impl(m, v, sink);
    }

private:
    template <typename V>
    void dispatch(std::span<const std::byte> m, V& v) {
        dispatch_impl(m, v, stats_);
    }

    template <typename V>
    static void dispatch_impl(std::span<const std::byte> m, V& v, Stats& stats) {
        const char type = static_cast<char>(m[0]);
        const std::size_t expect = wire_size(type);
        if (expect == 0) {
            ++stats.unknown;
            v.on_unknown(type, m);
            return;
        }
        if (m.size() != expect) {
            ++stats.malformed;
            v.on_malformed(type, m);
            return;
        }
        const std::byte* p = m.data();
        const Header h = detail::decode_header(p);
        switch (type) {
            case 'S':
                v.on_system_event(SystemEvent{h, static_cast<char>(p[11])});
                break;
            case 'R':
                v.on_stock_directory(StockDirectory{
                    h, detail::decode_stock(p + 11), static_cast<char>(p[19]),
                    static_cast<char>(p[20]), load_be<std::uint32_t>(p + 21)});
                break;
            case 'H':
                v.on_trading_action(TradingAction{
                    h, detail::decode_stock(p + 11), static_cast<char>(p[19]),
                    {static_cast<char>(p[21]), static_cast<char>(p[22]),
                     static_cast<char>(p[23]), static_cast<char>(p[24])}});
                break;
            case 'A':
            case 'F': {
                Side side{};
                if (!detail::decode_side(p[19], side)) {
                    ++stats.malformed;
                    v.on_malformed(type, m);
                    return;
                }
                AddOrder msg{h,
                             load_be<std::uint64_t>(p + 11),
                             side,
                             load_be<std::uint32_t>(p + 20),
                             detail::decode_stock(p + 24),
                             load_be<std::uint32_t>(p + 32),
                             /*has_mpid=*/type == 'F',
                             {}};
                if (msg.has_mpid) {
                    std::memcpy(msg.attribution.data(), p + 36, 4);
                }
                v.on_add_order(msg);
                break;
            }
            case 'E':
                v.on_order_executed(OrderExecuted{h, load_be<std::uint64_t>(p + 11),
                                                  load_be<std::uint32_t>(p + 19),
                                                  load_be<std::uint64_t>(p + 23)});
                break;
            case 'C':
                v.on_order_executed_with_price(OrderExecutedWithPrice{
                    h, load_be<std::uint64_t>(p + 11), load_be<std::uint32_t>(p + 19),
                    load_be<std::uint64_t>(p + 23),
                    /*printable=*/static_cast<char>(p[31]) == 'Y',
                    load_be<std::uint32_t>(p + 32)});
                break;
            case 'X':
                v.on_order_cancel(OrderCancel{h, load_be<std::uint64_t>(p + 11),
                                              load_be<std::uint32_t>(p + 19)});
                break;
            case 'D':
                v.on_order_delete(OrderDelete{h, load_be<std::uint64_t>(p + 11)});
                break;
            case 'U':
                v.on_order_replace(OrderReplace{h, load_be<std::uint64_t>(p + 11),
                                                load_be<std::uint64_t>(p + 19),
                                                load_be<std::uint32_t>(p + 27),
                                                load_be<std::uint32_t>(p + 31)});
                break;
            case 'P': {
                Side side{};
                if (!detail::decode_side(p[19], side)) {
                    ++stats.malformed;
                    v.on_malformed(type, m);
                    return;
                }
                v.on_trade(Trade{h, load_be<std::uint64_t>(p + 11), side,
                                 load_be<std::uint32_t>(p + 20), detail::decode_stock(p + 24),
                                 load_be<std::uint32_t>(p + 32), load_be<std::uint64_t>(p + 36)});
                break;
            }
            case 'Q':
                // shares is u64 here, unlike every other message (DESIGN §4)
                v.on_cross_trade(CrossTrade{h, load_be<std::uint64_t>(p + 11),
                                            detail::decode_stock(p + 19),
                                            load_be<std::uint32_t>(p + 27),
                                            load_be<std::uint64_t>(p + 31),
                                            static_cast<char>(p[39])});
                break;
        }
    }

    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
    Stats stats_{};
};

}  // namespace ob::itch
