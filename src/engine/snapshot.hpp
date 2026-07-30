#pragma once

// Book snapshot + restore (DESIGN §11 stretch): serialize an engine's full
// state — market phase, per-symbol trading state and names, engine counters,
// and every book's levels with orders in FIFO order — so a replay can resume
// mid-day. Restoring replays each order through the book's own add(), which
// reconstructs queue positions exactly; the differential/goldens machinery
// then applies unchanged to a restored engine.
//
// Format: host-endian, versioned magic, snapshots are same-machine artifacts
// (a cross-host format would need explicit endianness — deliberately out of
// scope). Works with any Book that offers l2/level_fifo/order_snapshot/add —
// both the flat book and the reference oracle qualify.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

#include "core/types.hpp"
#include "engine/engine.hpp"

namespace ob::engine {

constexpr char kSnapMagic[8] = {'O', 'B', 'S', 'N', 'A', 'P', '0', '2'};

struct SnapshotMeta {
    std::uint64_t messages = 0;     // messages consumed when snapped
    std::uint64_t file_offset = 0;  // parser byte offset to resume from
};

namespace detail {
template <typename T>
bool put(std::FILE* f, const T& v) {
    return std::fwrite(&v, sizeof(T), 1, f) == 1;
}
template <typename T>
bool get(std::FILE* f, T& v) {
    return std::fread(&v, sizeof(T), 1, f) == 1;
}
}  // namespace detail

template <typename Book, typename Listener, typename MakeBook>
bool save_snapshot(const Engine<Book, Listener, MakeBook>& eng, SnapshotMeta meta,
                   std::FILE* f) {
    using detail::put;
    if (std::fwrite(kSnapMagic, sizeof(kSnapMagic), 1, f) != 1) return false;
    if (!put(f, meta.messages) || !put(f, meta.file_offset)) return false;
    if (!put(f, static_cast<std::uint8_t>(eng.phase()))) return false;
    if (!put(f, eng.stats())) return false;

    // Trading states + symbols: only non-default locates.
    std::uint32_t n_state = 0;
    for (std::uint32_t l = 0; l < Engine<Book, Listener, MakeBook>::kMaxLocates; ++l) {
        const auto loc = static_cast<StockLocate>(l);
        if (eng.trading_state(loc) != 'T' || eng.symbol(loc)[0] != '\0') ++n_state;
    }
    if (!put(f, n_state)) return false;
    for (std::uint32_t l = 0; l < Engine<Book, Listener, MakeBook>::kMaxLocates; ++l) {
        const auto loc = static_cast<StockLocate>(l);
        if (eng.trading_state(loc) == 'T' && eng.symbol(loc)[0] == '\0') continue;
        if (!put(f, static_cast<std::uint16_t>(l))) return false;
        if (!put(f, eng.trading_state(loc))) return false;
        if (std::fwrite(eng.symbol(loc).data(), 8, 1, f) != 1) return false;
    }

    // Books: levels best-first per side, orders in FIFO order within each.
    std::uint32_t n_books = 0;
    for (std::uint32_t l = 0; l < Engine<Book, Listener, MakeBook>::kMaxLocates; ++l) {
        if (eng.book(static_cast<StockLocate>(l)) != nullptr) ++n_books;
    }
    if (!put(f, n_books)) return false;
    for (std::uint32_t l = 0; l < Engine<Book, Listener, MakeBook>::kMaxLocates; ++l) {
        const Book* b = eng.book(static_cast<StockLocate>(l));
        if (b == nullptr) continue;
        if (!put(f, static_cast<std::uint16_t>(l))) return false;
        for (const Side side : {Side::Bid, Side::Ask}) {
            const auto levels = b->l2(side);
            if (!put(f, static_cast<std::uint32_t>(levels.size()))) return false;
            for (const auto& lvl : levels) {
                if (!put(f, lvl.price)) return false;
                const auto fifo = b->level_fifo(side, lvl.price);
                if (!put(f, static_cast<std::uint32_t>(fifo.size()))) return false;
                for (const OrderId ref : fifo) {
                    const auto snap = b->order_snapshot(ref);
                    if (!snap) return false;  // impossible on a valid book
                    if (!put(f, ref) || !put(f, snap->remaining)) return false;
                }
            }
        }
    }
    return true;
}

template <typename Book, typename Listener, typename MakeBook>
std::optional<SnapshotMeta> load_snapshot(Engine<Book, Listener, MakeBook>& eng,
                                          std::FILE* f) {
    using detail::get;
    char magic[8];
    if (std::fread(magic, sizeof(magic), 1, f) != 1) return std::nullopt;
    for (int i = 0; i < 8; ++i) {
        if (magic[i] != kSnapMagic[i]) return std::nullopt;
    }
    SnapshotMeta meta;
    std::uint8_t phase = 0;
    if (!get(f, meta.messages) || !get(f, meta.file_offset) || !get(f, phase)) {
        return std::nullopt;
    }
    eng.restore_phase(static_cast<MarketPhase>(phase));
    if (!get(f, eng.mutable_stats())) return std::nullopt;

    std::uint32_t n_state = 0;
    if (!get(f, n_state)) return std::nullopt;
    for (std::uint32_t i = 0; i < n_state; ++i) {
        std::uint16_t loc = 0;
        char state = 'T';
        itch::Stock sym{};
        if (!get(f, loc) || !get(f, state)) return std::nullopt;
        if (std::fread(sym.data(), 8, 1, f) != 1) return std::nullopt;
        eng.restore_trading_state(loc, state);
        eng.restore_symbol(loc, sym);
    }

    std::uint32_t n_books = 0;
    if (!get(f, n_books)) return std::nullopt;
    for (std::uint32_t i = 0; i < n_books; ++i) {
        std::uint16_t loc = 0;
        if (!get(f, loc)) return std::nullopt;
        Book& book = eng.ensure_book(loc);
        for (const Side side : {Side::Bid, Side::Ask}) {
            std::uint32_t n_levels = 0;
            if (!get(f, n_levels)) return std::nullopt;
            for (std::uint32_t li = 0; li < n_levels; ++li) {
                Price price = 0;
                std::uint32_t n_orders = 0;
                if (!get(f, price) || !get(f, n_orders)) return std::nullopt;
                for (std::uint32_t oi = 0; oi < n_orders; ++oi) {
                    OrderId ref = 0;
                    Qty remaining = 0;
                    if (!get(f, ref) || !get(f, remaining)) return std::nullopt;
                    // add() in file order = FIFO order: queue positions
                    // reconstruct exactly.
                    if (book.add(ref, side, price, remaining).result !=
                        book::Apply::ok) {
                        return std::nullopt;
                    }
                }
            }
        }
    }
    return meta;
}

}  // namespace ob::engine
