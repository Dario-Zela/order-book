#pragma once

// HDR-style fixed-bucket histogram (DESIGN §8): log2 major buckets with 32
// linear sub-buckets each — ≤ ~3.1% relative error, zero allocation, O(1)
// record. Values 0..31 are exact. Good for nanosecond latencies across nine
// decades; percentile queries return the upper edge of the containing
// sub-bucket (conservative: reported p99 is never below the true p99).

#include <array>
#include <bit>
#include <cstdint>

namespace ob {

class LogHistogram {
public:
    static constexpr int kSubBits = 5;                  // 32 sub-buckets
    static constexpr int kSub = 1 << kSubBits;
    static constexpr int kMajors = 64 - kSubBits;       // covers full u64 range
    static constexpr std::size_t kBuckets = static_cast<std::size_t>(kMajors + 1) * kSub;

    void record(std::uint64_t v) noexcept {
        ++counts_[index_of(v)];
        ++total_;
        if (v > max_) max_ = v;
        if (v < min_) min_ = v;
        sum_ += v;
    }

    [[nodiscard]] std::uint64_t total() const noexcept { return total_; }
    [[nodiscard]] std::uint64_t max() const noexcept { return total_ == 0 ? 0 : max_; }
    [[nodiscard]] std::uint64_t min() const noexcept { return total_ == 0 ? 0 : min_; }
    [[nodiscard]] double mean() const noexcept {
        return total_ == 0 ? 0.0
                           : static_cast<double>(sum_) / static_cast<double>(total_);
    }

    // Value at quantile q in [0,1]: upper edge of the sub-bucket containing
    // the ceil(q * total)-th smallest sample. max() tightens the top bucket.
    [[nodiscard]] std::uint64_t quantile(double q) const noexcept {
        if (total_ == 0) return 0;
        if (q <= 0.0) return min();
        std::uint64_t rank = static_cast<std::uint64_t>(q * static_cast<double>(total_));
        if (rank >= total_) rank = total_ - 1;
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            seen += counts_[i];
            if (seen > rank) {
                const std::uint64_t hi = upper_edge(i);
                return hi > max_ ? max_ : hi;
            }
        }
        return max_;
    }

    void merge(const LogHistogram& o) noexcept {
        for (std::size_t i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
        total_ += o.total_;
        sum_ += o.sum_;
        if (o.total_ != 0) {
            if (o.max_ > max_) max_ = o.max_;
            if (o.min_ < min_) min_ = o.min_;
        }
    }

    void reset() noexcept { *this = LogHistogram{}; }

private:
    static std::size_t index_of(std::uint64_t v) noexcept {
        if (v < kSub) return static_cast<std::size_t>(v);  // exact low range
        const int msb = 63 - std::countl_zero(v);
        const int shift = msb - kSubBits;                  // >= 1 here
        const auto sub = static_cast<std::size_t>(v >> shift) - kSub;  // 0..31
        return static_cast<std::size_t>(shift + 1) * kSub + sub;
    }

    // Largest value mapping to bucket i (inclusive).
    static std::uint64_t upper_edge(std::size_t i) noexcept {
        if (i < kSub) return i;
        const int shift = static_cast<int>(i / kSub) - 1;
        const std::uint64_t sub = i % kSub;
        return (((sub + kSub) + 1) << shift) - 1;
    }

    std::array<std::uint64_t, kBuckets> counts_{};
    std::uint64_t total_ = 0;
    std::uint64_t sum_ = 0;
    std::uint64_t max_ = 0;
    std::uint64_t min_ = ~std::uint64_t{0};
};

}  // namespace ob
