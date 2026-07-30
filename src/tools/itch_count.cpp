// Weekend-1 acceptance tool (DESIGN §11): mmap an ITCH file, count messages
// per type, print a table to cross-check against the sample day's published
// totals. Also reports wall time and msgs/s as a first, informal throughput
// look (real numbers come from the bench harness with stated methodology).

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>

#include "itch/mmap_file.hpp"
#include "itch/parser.hpp"

namespace {

struct TypeCounter : ob::itch::NullVisitor {
    std::array<std::uint64_t, 256> by_type{};

    // Count subset types on dispatch; everything else lands in on_unknown.
    void on_system_event(const ob::itch::SystemEvent& m) { bump('S', m); }
    void on_stock_directory(const ob::itch::StockDirectory& m) { bump('R', m); }
    void on_trading_action(const ob::itch::TradingAction& m) { bump('H', m); }
    void on_add_order(const ob::itch::AddOrder& m) { bump(m.has_mpid ? 'F' : 'A', m); }
    void on_order_executed(const ob::itch::OrderExecuted& m) { bump('E', m); }
    void on_order_executed_with_price(const ob::itch::OrderExecutedWithPrice& m) {
        bump('C', m);
    }
    void on_order_cancel(const ob::itch::OrderCancel& m) { bump('X', m); }
    void on_order_delete(const ob::itch::OrderDelete& m) { bump('D', m); }
    void on_order_replace(const ob::itch::OrderReplace& m) { bump('U', m); }
    void on_trade(const ob::itch::Trade& m) { bump('P', m); }
    void on_cross_trade(const ob::itch::CrossTrade& m) { bump('Q', m); }
    void on_unknown(char type, std::span<const std::byte>) {
        ++by_type[static_cast<unsigned char>(type)];
    }

private:
    template <typename M>
    void bump(char type, const M&) {
        ++by_type[static_cast<unsigned char>(type)];
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <itch-file>\n", argv[0]);
        return 2;
    }
    try {
        const ob::itch::MmapFile file(argv[1]);
        ob::itch::Parser parser(file.bytes());
        TypeCounter counter;

        const auto t0 = std::chrono::steady_clock::now();
        const auto n = parser.run(counter);
        const auto t1 = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(t1 - t0).count();

        std::printf("type      count\n");
        std::printf("----  ---------\n");
        for (int t = 0; t < 256; ++t) {
            if (counter.by_type[static_cast<std::size_t>(t)] > 0) {
                std::printf("   %c  %9llu\n", static_cast<char>(t),
                            static_cast<unsigned long long>(
                                counter.by_type[static_cast<std::size_t>(t)]));
            }
        }
        const auto& s = parser.stats();
        std::printf("----  ---------\n");
        std::printf("total %9llu  (unknown %llu, malformed %llu%s)\n",
                    static_cast<unsigned long long>(s.messages),
                    static_cast<unsigned long long>(s.unknown),
                    static_cast<unsigned long long>(s.malformed),
                    s.truncated ? ", TRUNCATED TAIL" : "");
        std::printf("%.2f s, %.1fM msgs/s (single pass, includes page faults on cold cache)\n",
                    secs, static_cast<double>(n) / secs / 1e6);
        return s.truncated ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
