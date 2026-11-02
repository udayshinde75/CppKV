#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cppkv::loadgen {

// A histogram of latencies in nanoseconds with bounded relative error, in the
// style of HdrHistogram: fixed memory (58 KiB), O(1) record, mergeable.
//
// Why not store every sample and sort? A 60 s run at 200k requests/s is 12
// million samples (96 MB) per run. Why not fixed-width buckets? Latencies span
// microseconds to seconds; any one width is either too coarse or too many.
//
// Layout ("log-linear"): values 0..255 have one bucket each (exact). Above
// that, every power-of-two range [2^k, 2^(k+1)) is split into 128 equal
// buckets. A bucket's width is therefore at most 1/128 of its lower bound, so
// a reported percentile is at most 0.79 % above the true sample value, and
// never below it.
class LatencyHistogram {
public:
    static constexpr unsigned kSubBucketBits = 7;                       // 128 buckets per power of two
    static constexpr std::size_t kSubBuckets = std::size_t{1} << kSubBucketBits;
    // 0..255 take 2 * 128 buckets; each further bit width 9..64 takes 128.
    static constexpr std::size_t kBucketCount = (65 - kSubBucketBits) * kSubBuckets;   // 7424: all of uint64_t

    LatencyHistogram() : counts_(kBucketCount, 0) {}

    void record(std::uint64_t nanos) noexcept;
    void merge(const LatencyHistogram& other) noexcept;

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t min() const noexcept { return count_ == 0 ? 0 : min_; }   // exact
    [[nodiscard]] std::uint64_t max() const noexcept { return max_; }                    // exact
    [[nodiscard]] double mean() const noexcept;                                          // exact

    // The smallest value v such that at least a fraction q (0..1) of the
    // samples are <= v, rounded up to the end of v's bucket and capped at
    // max(). percentile(0.99) is p99. 0 for an empty histogram.
    [[nodiscard]] std::uint64_t percentile(double q) const noexcept;

    // Exposed for tests: the bucket of a value, and the largest value in a bucket.
    [[nodiscard]] static std::size_t bucket_of(std::uint64_t nanos) noexcept;
    [[nodiscard]] static std::uint64_t bucket_upper_bound(std::size_t bucket) noexcept;

private:
    std::vector<std::uint64_t> counts_;
    std::uint64_t count_ = 0;
    std::uint64_t min_ = 0;
    std::uint64_t max_ = 0;
    std::uint64_t sum_ = 0;   // overflows only after 584 years of summed latency
};

} // namespace cppkv::loadgen
