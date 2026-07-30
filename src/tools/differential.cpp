// Full-file differential run (DESIGN §9.1): replay the same ITCH file into
// Engine<RefBook> and Engine<FlatBook>, then compare EVERY active book's
// L2 (all levels) plus per-level FIFO order, live counts and engine stats.
// The reference book is the oracle; disagreement fails loudly with context.
// This is the slow nightly/local job — the unit differential over synthetic
// hostile streams stays in the test suite.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>

#include "book/flat_book.hpp"
#include "book/ref_book.hpp"
#include "engine/engine.hpp"
#include "itch/mmap_file.hpp"
#include "itch/parser.hpp"

namespace {

using ob::Side;
using ob::StockLocate;
using ob::book::BandConfig;
using ob::book::BookResources;
using ob::book::FlatBook;
using ob::book::RefBook;

struct FlatFactory {
    BookResources* res;
    std::unique_ptr<FlatBook> operator()(StockLocate loc) const {
        return std::make_unique<FlatBook>(*res, loc, BandConfig{});
    }
};

using RefEngine = ob::engine::Engine<RefBook>;
using FlatEngine = ob::engine::Engine<FlatBook, ob::book::NullListener, FlatFactory>;

std::uint64_t g_mismatches = 0;

void mismatch(StockLocate loc, const char* what) {
    ++g_mismatches;
    if (g_mismatches <= 20) {
        std::fprintf(stderr, "MISMATCH locate=%u: %s\n", loc, what);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <itch-file>\n", argv[0]);
        return 2;
    }
    try {
        const ob::itch::MmapFile file(argv[1]);

        std::printf("replaying reference (std::map oracle)...\n");
        RefEngine ref;
        {
            const auto t0 = std::chrono::steady_clock::now();
            ob::itch::Parser p(file.bytes());
            const auto n = p.run(ref);
            const double secs =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
                    .count();
            std::printf("  %llu messages, %.2f s (%.2fM msgs/s)\n",
                        static_cast<unsigned long long>(n), secs,
                        static_cast<double>(n) / secs / 1e6);
        }
        std::printf("replaying flat book...\n");
        BookResources res(1u << 22);
        FlatEngine flat({}, FlatFactory{&res});
        {
            const auto t0 = std::chrono::steady_clock::now();
            ob::itch::Parser p(file.bytes());
            const auto n = p.run(flat);
            const double secs =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
                    .count();
            std::printf("  %llu messages, %.2f s (%.2fM msgs/s)\n",
                        static_cast<unsigned long long>(n), secs,
                        static_cast<double>(n) / secs / 1e6);
        }

        // Engine counters must agree exactly.
        const auto& rs = ref.stats();
        const auto& fs = flat.stats();
        if (rs.unknown_ref != fs.unknown_ref) mismatch(0, "unknown_ref");
        if (rs.duplicate_ref != fs.duplicate_ref) mismatch(0, "duplicate_ref");
        if (rs.clamped != fs.clamped) mismatch(0, "clamped");
        if (rs.volume_lit != fs.volume_lit) mismatch(0, "volume_lit");
        if (rs.volume_hidden != fs.volume_hidden) mismatch(0, "volume_hidden");
        if (rs.volume_cross != fs.volume_cross) mismatch(0, "volume_cross");
        if (rs.nonprintable_execs != fs.nonprintable_execs) mismatch(0, "nonprintable");

        std::uint64_t books = 0;
        std::uint64_t levels = 0;
        std::uint64_t orders = 0;
        for (std::uint32_t li = 0; li < FlatEngine::kMaxLocates; ++li) {
            const auto loc = static_cast<StockLocate>(li);
            const RefBook* rb = ref.book(loc);
            const FlatBook* fb = flat.book(loc);
            if ((rb == nullptr) != (fb == nullptr)) {
                mismatch(loc, "book existence");
                continue;
            }
            if (rb == nullptr) continue;
            ++books;
            if (!fb->validate()) mismatch(loc, "flat validate()");
            if (rb->live_orders() != fb->live_orders()) mismatch(loc, "live_orders");
            orders += rb->live_orders();
            for (Side s : {Side::Bid, Side::Ask}) {
                const auto rl2 = rb->l2(s);
                const auto fl2 = fb->l2(s);
                if (rl2 != fl2) {
                    mismatch(loc, "l2");
                    continue;
                }
                levels += rl2.size();
                for (const auto& lvl : rl2) {
                    if (rb->level_fifo(s, lvl.price) != fb->level_fifo(s, lvl.price)) {
                        mismatch(loc, "level FIFO order");
                        break;
                    }
                }
            }
        }
        std::printf("compared        %llu books, %llu levels, %llu live orders\n",
                    static_cast<unsigned long long>(books),
                    static_cast<unsigned long long>(levels),
                    static_cast<unsigned long long>(orders));
        if (g_mismatches == 0) {
            std::printf("DIFFERENTIAL PASS: flat book == reference oracle\n");
            return 0;
        }
        std::fprintf(stderr, "DIFFERENTIAL FAIL: %llu mismatches\n",
                     static_cast<unsigned long long>(g_mismatches));
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
