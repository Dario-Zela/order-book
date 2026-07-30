#pragma once

// Open-addressing OrderId -> V map (DESIGN §5.2): robin-hood probing,
// power-of-two capacity, max load factor 0.5. Deletion is BACKWARD-SHIFT,
// not tombstones: deletes are as frequent as inserts over a full-day replay
// (every Delete, every fully-executed order, every Replace), and tombstones
// would degrade probe lengths monotonically all day. Backward shift keeps
// clusters canonical, as if the deleted key never existed.
//
// Sized at init from the expected high-water mark with headroom; growth is
// implemented (correctness first) but counted, so a grow during a benchmark
// run is a visible sizing bug rather than a silent rehash.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/types.hpp"

namespace ob {

template <typename V>
class IdMap {
public:
    // Rounds capacity up so `expected` entries fit under 0.5 load.
    explicit IdMap(std::size_t expected) {
        std::size_t cap = 16;
        while (cap < expected * 2) cap <<= 1;
        slots_.resize(cap);
    }

    // False (no mutation) if the key is already present.
    bool insert(OrderId key, V value) {
        if (find(key) != nullptr) return false;
        if ((size_ + 1) * 2 > slots_.size()) {
            grow();
            ++growths_;
        }
        insert_unchecked(Slot{key, value, 1});
        ++size_;
        return true;
    }

    // Pointer to the mapped value, or nullptr. Robin-hood early exit: probing
    // can stop as soon as our would-be distance exceeds the resident entry's —
    // an absent key can never live beyond that point.
    [[nodiscard]] V* find(OrderId key) noexcept {
        const std::size_t mask = slots_.size() - 1;
        std::size_t i = hash(key) & mask;
        std::uint8_t dist = 1;
        while (true) {
            Slot& s = slots_[i];
            if (s.dist == 0 || s.dist < dist) return nullptr;
            if (s.key == key) return &s.value;
            i = (i + 1) & mask;
            ++dist;
        }
    }
    [[nodiscard]] const V* find(OrderId key) const noexcept {
        return const_cast<IdMap*>(this)->find(key);
    }

    // False if absent. Backward-shift: pull the following cluster back one
    // slot until an empty slot or a home (dist==1) entry.
    bool erase(OrderId key) noexcept {
        const std::size_t mask = slots_.size() - 1;
        std::size_t i = hash(key) & mask;
        std::uint8_t dist = 1;
        while (true) {
            Slot& s = slots_[i];
            if (s.dist == 0 || s.dist < dist) return false;
            if (s.key == key) break;
            i = (i + 1) & mask;
            ++dist;
        }
        std::size_t j = (i + 1) & mask;
        while (slots_[j].dist > 1) {
            slots_[i] = slots_[j];
            --slots_[i].dist;
            i = j;
            j = (j + 1) & mask;
        }
        slots_[i].dist = 0;
        --size_;
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
    [[nodiscard]] std::uint64_t growths() const noexcept { return growths_; }
    [[nodiscard]] std::uint8_t max_probe() const noexcept { return max_probe_; }
    [[nodiscard]] std::size_t memory_bytes() const noexcept {
        return slots_.size() * sizeof(Slot);
    }

    // Probe-length histogram over live entries (§5.2: the backward-shift
    // before/after evidence). hist[d-1] = entries at probe distance d.
    [[nodiscard]] std::vector<std::size_t> probe_histogram() const {
        std::vector<std::size_t> h;
        for (const Slot& s : slots_) {
            if (s.dist == 0) continue;
            if (h.size() < s.dist) h.resize(s.dist);
            ++h[s.dist - 1u];
        }
        return h;
    }

private:
    struct Slot {
        OrderId key = 0;
        V value{};
        std::uint8_t dist = 0;  // probe distance + 1; 0 = empty
    };

    // splitmix64 finalizer: ITCH refs are dense sequential integers — a
    // multiply-xorshift mix spreads them across the table.
    static std::uint64_t hash(OrderId k) noexcept {
        std::uint64_t x = k;
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ull;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebull;
        x ^= x >> 31;
        return x;
    }

    // Precondition: entry.dist == 1 (fresh insert or post-grow re-insert).
    void insert_unchecked(Slot entry) {
        const std::size_t mask = slots_.size() - 1;
        std::size_t i = hash(entry.key) & mask;
        while (true) {
            Slot& s = slots_[i];
            if (s.dist == 0) {
                if (entry.dist > max_probe_) max_probe_ = entry.dist;
                s = entry;
                return;
            }
            if (s.dist < entry.dist) {  // rob the rich: displace the closer entry
                if (entry.dist > max_probe_) max_probe_ = entry.dist;
                std::swap(s, entry);
            }
            i = (i + 1) & mask;
            ++entry.dist;  // u8: load 0.5 keeps clusters far below 255; guard anyway
            if (entry.dist == 0) {  // wrapped: table pathologically clustered
                grow();
                ++growths_;
                entry.dist = 1;
                insert_unchecked(entry);
                return;
            }
        }
    }

    void grow() {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(old.size() * 2, Slot{});
        max_probe_ = 0;
        for (Slot& s : old) {
            if (s.dist != 0) {
                s.dist = 1;
                insert_unchecked(s);
            }
        }
    }

    std::vector<Slot> slots_;
    std::size_t size_ = 0;
    std::uint64_t growths_ = 0;
    std::uint8_t max_probe_ = 0;
};

}  // namespace ob
