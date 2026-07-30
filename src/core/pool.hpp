#pragma once

// Slab pool + intrusive free-list (DESIGN §5.2): pre-allocate at startup,
// alloc() pops the free list (LIFO — hottest recently-freed slots first),
// free() pushes. Zero new/delete per op in the steady state. If the initial
// sizing was wrong we allocate another slab (geometric) rather than dying —
// but count it, so a growth in a benchmark run is a visible sizing bug, not
// a silent allocation. Slab addresses are stable: grown pools never move
// live objects.

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <vector>

#include "core/huge_alloc.hpp"

namespace ob {

template <typename T>
class Pool {
    static_assert(std::is_trivially_destructible_v<T>,
                  "Pool hands storage back without running destructors");

public:
    explicit Pool(std::size_t initial_capacity) { add_slab(initial_capacity); }

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    T* alloc() {
        if (free_head_ != nullptr) {
            FreeNode* n = free_head_;
            free_head_ = n->next;
            ++live_;
            return ::new (static_cast<void*>(n)) T{};
        }
        if (bump_ == slab_end_) {
            add_slab(capacity_);  // double total capacity
            ++growths_;
        }
        ++live_;
        if (live_ > high_water_) high_water_ = live_;
        return ::new (static_cast<void*>(bump_++)) T{};
    }

    void free(T* p) {
        auto* n = std::launder(reinterpret_cast<FreeNode*>(p));
        n->next = free_head_;
        free_head_ = n;
        --live_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t live() const noexcept { return live_; }
    // Peak concurrent live objects — the number that sizes the arena for a
    // full-day replay (§5.2: publish it).
    [[nodiscard]] std::size_t high_water() const noexcept { return high_water_; }
    [[nodiscard]] std::uint64_t growths() const noexcept { return growths_; }
    [[nodiscard]] std::size_t memory_bytes() const noexcept { return capacity_ * sizeof(T); }

private:
    struct FreeNode {
        FreeNode* next;
    };
    static_assert(sizeof(T) >= sizeof(FreeNode) && alignof(T) >= alignof(FreeNode),
                  "free-list node is punned into freed slots");

    struct alignas(alignof(T)) Storage {
        std::byte bytes[sizeof(T)];
    };

    void add_slab(std::size_t count) {
        if (count == 0) count = 1;
        slabs_.emplace_back(count);  // huge-page-backed on Linux (2MB+ slabs)
        bump_ = reinterpret_cast<T*>(slabs_.back().data());
        slab_end_ = bump_ + count;
        capacity_ += count;
    }

    std::vector<std::vector<Storage, HugeAlloc<Storage>>> slabs_;
    T* bump_ = nullptr;
    T* slab_end_ = nullptr;
    FreeNode* free_head_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t live_ = 0;
    std::size_t high_water_ = 0;
    std::uint64_t growths_ = 0;
};

}  // namespace ob
