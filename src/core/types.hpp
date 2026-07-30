#pragma once

#include <cstdint>

namespace ob {

// ITCH prices arrive as big-endian unsigned integers in units of 1/10,000 USD
// (DESIGN §4). They stay in tick units end to end; render to decimal only at
// the edges.
using Price = std::uint32_t;
using Qty = std::uint32_t;
using OrderId = std::uint64_t;  // ITCH order reference number, unique for the day
using StockLocate = std::uint16_t;
using Timestamp = std::uint64_t;  // nanoseconds since midnight (48-bit on the wire)

enum class Side : std::uint8_t { Bid = 0, Ask = 1 };

constexpr Side opposite(Side s) noexcept {
    return s == Side::Bid ? Side::Ask : Side::Bid;
}

}  // namespace ob
