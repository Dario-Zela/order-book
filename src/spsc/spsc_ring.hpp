#pragma once

// Single-producer single-consumer ring (DESIGN §7).
//
// Memory-ordering argument (the TSan stress test is evidence, THIS is the
// proof): the producer writes the slot, then publishes it with a RELEASE
// store to tail_ — release orders the slot write before the index store. The
// consumer reads tail_ with ACQUIRE — acquire orders the index load before
// the slot read, so a consumer that observes tail_ == t also observes every
// slot write up to t. Symmetrically for head_ on the reclaim side: the
// consumer's release store to head_ publishes "slot consumed", and the
// producer's acquire load observes it before overwriting. Loads of your OWN
// index are RELAXED: only you ever store it, so there is nothing to
// synchronise with. seq_cst would add nothing but a full fence on x86
// (xchg/mfence) and heavier barriers on ARM: release/acquire compiles to
// plain MOVs under x86-TSO and stlr/ldar on ARM.
//
// Indices are free-running 64-bit counters masked on access — no modulo, no
// wrap branch. At even 10^9 ops/s a 64-bit counter wraps after ~584 years;
// wrap-around is a non-issue at any achievable rate.
//
// Cached-index optimisation: the producer keeps a local copy of the last
// head_ it saw and only re-reads the shared line when the ring LOOKS full
// (consumer mirrors with tail_). Without it, every push/pop bounces the
// other side's cache line across cores; with it, steady-state ops touch
// only your own line plus the slot.

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ob::spsc {

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

// kCacheIndices=false disables the cached-index optimisation (every
// space/empty check reads the other side's atomic) — the §7 experiment
// variant; measured, not asserted.
template <typename T, std::size_t N, bool kCacheIndices = true>
class SpscRing {
    static_assert((N & (N - 1)) == 0 && N >= 2, "N must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "slots are raw copies");
    static_assert(sizeof(T) <= 64, "one slot should not straddle cache lines");

public:
    SpscRing() = default;
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // --- producer side ----------------------------------------------------

    [[nodiscard]] bool try_push(const T& v) noexcept {
        const std::uint64_t t = prod_.tail.load(std::memory_order_relaxed);
        if (!kCacheIndices || t - prod_.cached_head == N) {
            prod_.cached_head = cons_.head.load(std::memory_order_acquire);
            if (t - prod_.cached_head == N) return false;  // genuinely full
        }
        buf_[t & kMask] = v;
        prod_.tail.store(t + 1, std::memory_order_release);
        const std::uint64_t occ = t + 1 - prod_.cached_head;  // approximate
        if (occ > prod_.occupancy_hw) prod_.occupancy_hw = occ;
        return true;
    }

    // Backpressure policy (§7): spin with a pause hint; the ring is sized so
    // replay rarely blocks — occupancy_high_water() tells you if it did.
    void push(const T& v) noexcept {
        while (!try_push(v)) cpu_relax();
    }

    // Producer-local view; approximate (cached head may be stale upward).
    [[nodiscard]] std::uint64_t occupancy_high_water() const noexcept {
        return prod_.occupancy_hw;
    }

    // --- consumer side ----------------------------------------------------

    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::uint64_t h = cons_.head.load(std::memory_order_relaxed);
        if (!kCacheIndices || h == cons_.cached_tail) {
            cons_.cached_tail = prod_.tail.load(std::memory_order_acquire);
            if (h == cons_.cached_tail) return false;  // genuinely empty
        }
        out = buf_[h & kMask];
        cons_.head.store(h + 1, std::memory_order_release);
        return true;
    }

    // Batch pop: one acquire + one release amortised over up to max_n slots.
    std::size_t pop_n(T* out, std::size_t max_n) noexcept {
        const std::uint64_t h = cons_.head.load(std::memory_order_relaxed);
        std::uint64_t avail = kCacheIndices ? cons_.cached_tail - h : 0;
        if (avail == 0) {
            cons_.cached_tail = prod_.tail.load(std::memory_order_acquire);
            avail = cons_.cached_tail - h;
            if (avail == 0) return 0;
        }
        const std::size_t n = avail < max_n ? static_cast<std::size_t>(avail) : max_n;
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = buf_[(h + i) & kMask];
        }
        cons_.head.store(h + n, std::memory_order_release);
        return n;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }

private:
    static constexpr std::uint64_t kMask = N - 1;

    // One cache line per role: the atomic the OTHER side reads, plus this
    // side's private cached copy of the other index (never shared).
    struct alignas(64) Producer {
        std::atomic<std::uint64_t> tail{0};
        std::uint64_t cached_head = 0;
        std::uint64_t occupancy_hw = 0;
    };
    struct alignas(64) Consumer {
        std::atomic<std::uint64_t> head{0};
        std::uint64_t cached_tail = 0;
    };

    Producer prod_;
    Consumer cons_;
    alignas(64) std::array<T, N> buf_;
};

}  // namespace ob::spsc
