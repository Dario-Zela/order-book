// Golden-file snapshots (DESIGN §9.1).
//
//   golden dump  <itch-file> <golden.txt> [--every=N] [--top=K] [--depth=D]
//   golden check <itch-file> <golden.txt> [--every=N] [--top=K] [--depth=D]
//
// dump replays with the REFERENCE book (the oracle bootstraps goldens) and
// writes, every N messages, the top-D L2 levels per side for the K
// most-active symbols so far. check replays with the FLAT book and compares
// its snapshots line by line — two independent implementations must agree
// at every checkpoint, and committed goldens catch semantic drift in either
// implementation forever after. Symbol selection depends only on the input
// stream (add counts), so both modes pick identical sets.

#include <cstdlib>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

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

struct Config {
    std::uint64_t every = 10'000'000;
    std::size_t top = 20;
    std::size_t depth = 10;
};

// Counts adds per locate while forwarding to the engine; the count drives
// deterministic symbol selection.
template <typename Inner>
struct AddCounter : ob::itch::NullVisitor {
    Inner& inner;
    std::vector<std::uint64_t> adds = std::vector<std::uint64_t>(65536, 0);
    explicit AddCounter(Inner& in) : inner(in) {}

    void on_system_event(const ob::itch::SystemEvent& m) { inner.on_system_event(m); }
    void on_stock_directory(const ob::itch::StockDirectory& m) {
        inner.on_stock_directory(m);
    }
    void on_trading_action(const ob::itch::TradingAction& m) { inner.on_trading_action(m); }
    void on_add_order(const ob::itch::AddOrder& m) {
        ++adds[m.h.locate];
        inner.on_add_order(m);
    }
    void on_order_executed(const ob::itch::OrderExecuted& m) { inner.on_order_executed(m); }
    void on_order_executed_with_price(const ob::itch::OrderExecutedWithPrice& m) {
        inner.on_order_executed_with_price(m);
    }
    void on_order_cancel(const ob::itch::OrderCancel& m) { inner.on_order_cancel(m); }
    void on_order_delete(const ob::itch::OrderDelete& m) { inner.on_order_delete(m); }
    void on_order_replace(const ob::itch::OrderReplace& m) { inner.on_order_replace(m); }
    void on_trade(const ob::itch::Trade& m) { inner.on_trade(m); }
    void on_cross_trade(const ob::itch::CrossTrade& m) { inner.on_cross_trade(m); }
};

template <typename Engine>
std::string snapshot(std::uint64_t msg_count, const Engine& eng,
                     const std::vector<std::uint64_t>& adds, const Config& cfg) {
    std::vector<std::uint32_t> locs;
    for (std::uint32_t l = 0; l < adds.size(); ++l) {
        if (adds[l] > 0 && eng.book(static_cast<StockLocate>(l)) != nullptr) {
            locs.push_back(l);
        }
    }
    std::stable_sort(locs.begin(), locs.end(), [&](std::uint32_t a, std::uint32_t b) {
        return adds[a] != adds[b] ? adds[a] > adds[b] : a < b;
    });
    if (locs.size() > cfg.top) locs.resize(cfg.top);
    std::sort(locs.begin(), locs.end());  // stable output order

    std::string out = "checkpoint " + std::to_string(msg_count) + "\n";
    char line[128];
    for (const auto l : locs) {
        const auto* book = eng.book(static_cast<StockLocate>(l));
        for (const Side s : {Side::Bid, Side::Ask}) {
            for (const auto& lvl : book->l2(s, cfg.depth)) {
                std::snprintf(line, sizeof(line), "%u %c %u %u %u\n", l,
                              s == Side::Bid ? 'B' : 'A', lvl.price, lvl.qty, lvl.count);
                out += line;
            }
        }
    }
    return out;
}

template <typename Engine, typename MakeBook>
std::string collect(const ob::itch::MmapFile& file, const Config& cfg, MakeBook mk) {
    Engine eng({}, mk);
    AddCounter<Engine> counter(eng);
    ob::itch::Parser p(file.bytes());
    std::string out;
    std::uint64_t n = 0;
    while (p.next(counter)) {
        if (++n % cfg.every == 0) {
            out += snapshot(n, eng, counter.adds, cfg);
        }
    }
    out += "end " + std::to_string(n) + "\n";
    out += snapshot(n, eng, counter.adds, cfg);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    const char* mode = nullptr;
    const char* in_path = nullptr;
    const char* golden_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--every=", 8) == 0) {
            cfg.every = std::strtoull(argv[i] + 8, nullptr, 10);
        } else if (std::strncmp(argv[i], "--top=", 6) == 0) {
            cfg.top = std::strtoull(argv[i] + 6, nullptr, 10);
        } else if (std::strncmp(argv[i], "--depth=", 8) == 0) {
            cfg.depth = std::strtoull(argv[i] + 8, nullptr, 10);
        } else if (mode == nullptr) {
            mode = argv[i];
        } else if (in_path == nullptr) {
            in_path = argv[i];
        } else {
            golden_path = argv[i];
        }
    }
    if (mode == nullptr || in_path == nullptr || golden_path == nullptr ||
        (std::strcmp(mode, "dump") != 0 && std::strcmp(mode, "check") != 0)) {
        std::fprintf(stderr,
                     "usage: %s dump|check <itch-file> <golden.txt>\n"
                     "          [--every=N] [--top=K] [--depth=D]\n",
                     argv[0]);
        return 2;
    }
    try {
        const ob::itch::MmapFile file(in_path);
        if (std::strcmp(mode, "dump") == 0) {
            using RefEngine = ob::engine::Engine<RefBook>;
            const auto out = collect<RefEngine>(
                file, cfg, ob::engine::detail::DefaultMakeBook<RefBook>{});
            std::FILE* f = std::fopen(golden_path, "wb");
            if (f == nullptr) {
                std::fprintf(stderr, "cannot open %s\n", golden_path);
                return 1;
            }
            std::fwrite(out.data(), 1, out.size(), f);
            std::fclose(f);
            std::printf("golden written: %s (%zu bytes, oracle: reference book)\n",
                        golden_path, out.size());
            return 0;
        }
        // check: flat book must reproduce the oracle's snapshots exactly.
        BookResources res(1u << 22);
        using FlatEngine =
            ob::engine::Engine<FlatBook, ob::book::NullListener, FlatFactory>;
        const auto got = collect<FlatEngine>(file, cfg, FlatFactory{&res});
        const ob::itch::MmapFile want_file(golden_path);
        const auto want_bytes = want_file.bytes();
        const std::string want(reinterpret_cast<const char*>(want_bytes.data()),
                               want_bytes.size());
        if (got == want) {
            std::printf("GOLDEN PASS: flat book reproduces the oracle snapshots\n");
            return 0;
        }
        // First differing line, for context.
        std::size_t line_no = 1;
        std::size_t i = 0;
        const std::size_t lim = std::min(got.size(), want.size());
        while (i < lim && got[i] == want[i]) {
            if (got[i] == '\n') ++line_no;
            ++i;
        }
        std::fprintf(stderr, "GOLDEN FAIL at line %zu (byte %zu)\n", line_no, i);
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
