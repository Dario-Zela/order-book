#pragma once

// Two-level occupancy bitmap over a price band (DESIGN §5.3 experiment):
// bit i of words_ = "level i non-empty"; bit j of summary_ = "word j has any
// set bit". prev_set/next_set find the nearest non-empty level with a couple
// of countl_zero/countr_zero calls instead of a linear level scan — an
// O(1)-ish worst case vs the scan's rare-but-unbounded tail: a p99.9 story,
// not a mean story. At the band cap (2^17 levels) this is 2048 words + a
// 32-word summary; the summary walk is bounded by 32 iterations and
// typically hits in one.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ob::book {

class LevelBitmap {
public:
    void reset(std::size_t levels) {
        words_.assign((levels + 63) / 64, 0);
        summary_.assign((words_.size() + 63) / 64, 0);
    }

    // Rebuild-preserving grow: old bit i becomes bit i + shift.
    void grow(std::size_t new_levels, std::size_t shift) {
        std::vector<std::uint64_t> old = std::move(words_);
        reset(new_levels);
        for (std::size_t w = 0; w < old.size(); ++w) {
            std::uint64_t bits = old[w];
            while (bits != 0) {
                const auto b = static_cast<std::size_t>(std::countr_zero(bits));
                set(w * 64 + b + shift);
                bits &= bits - 1;
            }
        }
    }

    void set(std::size_t i) noexcept {
        words_[i / 64] |= 1ull << (i % 64);
        summary_[(i / 64) / 64] |= 1ull << ((i / 64) % 64);
    }

    void clear(std::size_t i) noexcept {
        auto& w = words_[i / 64];
        w &= ~(1ull << (i % 64));
        if (w == 0) {
            summary_[(i / 64) / 64] &= ~(1ull << ((i / 64) % 64));
        }
    }

    [[nodiscard]] bool test(std::size_t i) const noexcept {
        return (words_[i / 64] >> (i % 64)) & 1u;
    }

    // Largest set index <= i, or -1 (bid-side repair scans downward).
    [[nodiscard]] std::ptrdiff_t prev_set(std::size_t i) const noexcept {
        std::size_t w = i / 64;
        const std::uint64_t first = words_[w] << (63 - i % 64);  // keep bits <= i
        if (first != 0) {
            return static_cast<std::ptrdiff_t>(i - static_cast<std::size_t>(
                                                       std::countl_zero(first)));
        }
        // Walk the summary downward for the nearest earlier non-empty word.
        std::size_t s = w / 64;
        std::uint64_t sbits =
            (w % 64) == 0 ? 0 : summary_[s] & (~0ull >> (64 - w % 64));
        while (true) {
            if (sbits != 0) {
                const auto sw = 63 - static_cast<std::size_t>(std::countl_zero(sbits));
                const std::size_t word = s * 64 + sw;
                const auto bit = 63 - static_cast<std::size_t>(std::countl_zero(words_[word]));
                return static_cast<std::ptrdiff_t>(word * 64 + bit);
            }
            if (s == 0) return -1;
            --s;
            sbits = summary_[s];
        }
    }

    // Smallest set index >= i, or -1 (ask-side repair scans upward).
    [[nodiscard]] std::ptrdiff_t next_set(std::size_t i) const noexcept {
        std::size_t w = i / 64;
        const std::uint64_t first = words_[w] >> (i % 64);
        if (first != 0) {
            return static_cast<std::ptrdiff_t>(
                i + static_cast<std::size_t>(std::countr_zero(first)));
        }
        std::size_t s = w / 64;
        std::uint64_t sbits =
            (w % 64) == 63 ? 0 : summary_[s] & (~0ull << (w % 64 + 1));
        while (true) {
            if (sbits != 0) {
                const auto sw = static_cast<std::size_t>(std::countr_zero(sbits));
                const std::size_t word = s * 64 + sw;
                const auto bit = static_cast<std::size_t>(std::countr_zero(words_[word]));
                const auto idx = word * 64 + bit;
                return static_cast<std::ptrdiff_t>(idx);
            }
            if (++s >= summary_.size()) return -1;
            sbits = summary_[s];
        }
    }

    [[nodiscard]] std::size_t capacity_levels() const noexcept { return words_.size() * 64; }

private:
    std::vector<std::uint64_t> words_;
    std::vector<std::uint64_t> summary_;
};

}  // namespace ob::book
