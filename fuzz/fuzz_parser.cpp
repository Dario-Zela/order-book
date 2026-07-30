// Parser fuzz target (DESIGN §9.4): arbitrary bytes through the framed
// parser must never crash, never read out of bounds, and account for every
// record (dispatched + unknown + malformed + truncation all consistent).
// Build with -fsanitize=fuzzer where the runtime exists (Linux CI, brew
// llvm); the standalone driver feeds random buffers under ASan otherwise.

#include <cstddef>
#include <cstdint>
#include <span>

#include "itch/parser.hpp"

namespace {

struct CountingVisitor : ob::itch::NullVisitor {
    std::uint64_t seen = 0;
    void on_system_event(const ob::itch::SystemEvent&) { ++seen; }
    void on_stock_directory(const ob::itch::StockDirectory&) { ++seen; }
    void on_trading_action(const ob::itch::TradingAction&) { ++seen; }
    void on_add_order(const ob::itch::AddOrder&) { ++seen; }
    void on_order_executed(const ob::itch::OrderExecuted&) { ++seen; }
    void on_order_executed_with_price(const ob::itch::OrderExecutedWithPrice&) { ++seen; }
    void on_order_cancel(const ob::itch::OrderCancel&) { ++seen; }
    void on_order_delete(const ob::itch::OrderDelete&) { ++seen; }
    void on_order_replace(const ob::itch::OrderReplace&) { ++seen; }
    void on_trade(const ob::itch::Trade&) { ++seen; }
    void on_cross_trade(const ob::itch::CrossTrade&) { ++seen; }
    void on_unknown(char, std::span<const std::byte>) { ++seen; }
    void on_malformed(char, std::span<const std::byte>) { ++seen; }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    ob::itch::Parser p({reinterpret_cast<const std::byte*>(data), size});
    CountingVisitor v;
    const auto n = p.run(v);
    // Accounting invariant: every framed record hit exactly one callback.
    if (v.seen != n || p.stats().messages != n) __builtin_trap();
    if (p.offset() > size) __builtin_trap();
    return 0;
}
