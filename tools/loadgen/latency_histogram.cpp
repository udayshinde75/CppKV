#include "loadgen/latency_histogram.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace cppkv::loadgen {

// Bucket of v: with e = max(0, bit_width(v) - 8), the bucket is e * 128 + (v >> e).
// For v < 256, e = 0 and the bucket is v itself. For larger v, v >> e is in
// [128, 255], so each bit width gets 128 consecutive buckets, continuing where
// the previous one stopped.
std::size_t LatencyHistogram::bucket_of(std::uint64_t nanos) noexcept {
    const auto width = static_cast<unsigned>(std::bit_width(nanos));
    const unsigned shift = width > kSubBucketBits + 1 ? width - (kSubBucketBits + 1) : 0;
    // uint64_t -> size_t: same width on every supported platform (64-bit only).
    return static_cast<std::size_t>(shift) * kSubBuckets + (nanos >> shift);
}

std::uint64_t LatencyHistogram::bucket_upper_bound(std::size_t bucket) noexcept {
    if (bucket < 2 * kSubBuckets) {
        return bucket;   // exact buckets
    }
    const std::size_t shift = bucket / kSubBuckets - 1;
    const std::uint64_t mantissa = bucket - shift * kSubBuckets;   // in [128, 255]
    const std::uint64_t lower = mantissa << shift;
    return lower + ((std::uint64_t{1} << shift) - 1);   // written so the top bucket does not overflow
}

void LatencyHistogram::record(std::uint64_t nanos) noexcept {
    ++counts_[bucket_of(nanos)];
    min_ = count_ == 0 ? nanos : std::min(min_, nanos);
    max_ = std::max(max_, nanos);
    sum_ += nanos;
    ++count_;
}

void LatencyHistogram::merge(const LatencyHistogram& other) noexcept {
    if (other.count_ == 0) {
        return;
    }
    for (std::size_t i = 0; i < kBucketCount; ++i) {
        counts_[i] += other.counts_[i];
    }
    min_ = count_ == 0 ? other.min_ : std::min(min_, other.min_);
    max_ = std::max(max_, other.max_);
    sum_ += other.sum_;
    count_ += other.count_;
}

double LatencyHistogram::mean() const noexcept {
    return count_ == 0 ? 0.0 : static_cast<double>(sum_) / static_cast<double>(count_);
}

std::uint64_t LatencyHistogram::percentile(double q) const noexcept {
    if (count_ == 0) {
        return 0;
    }
    q = std::clamp(q, 0.0, 1.0);
    // The rank (1-based) of the sample at quantile q: ceil(q * count), at least 1.
    auto rank = static_cast<std::uint64_t>(std::ceil(q * static_cast<double>(count_)));
    rank = std::clamp<std::uint64_t>(rank, 1, count_);
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < kBucketCount; ++i) {
        seen += counts_[i];
        if (seen >= rank) {
            return std::min(bucket_upper_bound(i), max_);
        }
    }
    return max_;   // unreachable: the buckets sum to count_
}

} // namespace cppkv::loadgen
