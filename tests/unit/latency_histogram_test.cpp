#include "loadgen/latency_histogram.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

using cppkv::loadgen::LatencyHistogram;

namespace {

TEST(LatencyHistogram, EmptyHistogramReportsZero) {
    const LatencyHistogram h;
    EXPECT_EQ(h.count(), 0U);
    EXPECT_EQ(h.min(), 0U);
    EXPECT_EQ(h.max(), 0U);
    EXPECT_EQ(h.mean(), 0.0);
    EXPECT_EQ(h.percentile(0.99), 0U);
}

TEST(LatencyHistogram, SmallValuesAreExact) {
    LatencyHistogram h;
    for (std::uint64_t v = 1; v <= 100; ++v) {
        h.record(v);
    }
    EXPECT_EQ(h.count(), 100U);
    EXPECT_EQ(h.min(), 1U);
    EXPECT_EQ(h.max(), 100U);
    EXPECT_DOUBLE_EQ(h.mean(), 50.5);
    EXPECT_EQ(h.percentile(0.0), 1U);    // rank 1
    EXPECT_EQ(h.percentile(0.50), 50U);  // 50 of 100 samples are <= 50
    EXPECT_EQ(h.percentile(0.95), 95U);
    EXPECT_EQ(h.percentile(0.99), 99U);
    EXPECT_EQ(h.percentile(1.0), 100U);
}

// Every bucket's range starts right after the previous one ends: no value is
// lost between buckets and no two buckets overlap, up to the largest uint64_t.
TEST(LatencyHistogram, BucketsTileTheWholeRangeWithoutGaps) {
    std::uint64_t expected_lower = 0;
    for (std::size_t b = 0; b < LatencyHistogram::kBucketCount; ++b) {
        const std::uint64_t upper = LatencyHistogram::bucket_upper_bound(b);
        ASSERT_GE(upper, expected_lower) << "bucket " << b;
        ASSERT_EQ(LatencyHistogram::bucket_of(expected_lower), b) << "lower end of bucket " << b;
        ASSERT_EQ(LatencyHistogram::bucket_of(upper), b) << "upper end of bucket " << b;
        if (b + 1 < LatencyHistogram::kBucketCount) {
            expected_lower = upper + 1;
        } else {
            EXPECT_EQ(upper, std::numeric_limits<std::uint64_t>::max());
        }
    }
}

// The guarantee that makes percentiles trustworthy: a bucket is never wider
// than 1/128 of the values in it.
TEST(LatencyHistogram, RelativeErrorIsBelowOnePercent) {
    std::mt19937_64 rng{3};
    for (int i = 0; i < 100'000; ++i) {
        const std::uint64_t v = rng() >> (rng() % 64);   // spread over all magnitudes
        const std::uint64_t upper = LatencyHistogram::bucket_upper_bound(LatencyHistogram::bucket_of(v));
        ASSERT_GE(upper, v);
        ASSERT_LE(upper - v, v / 128) << "v=" << v;
    }
}

TEST(LatencyHistogram, PercentilesOfLargeValuesAreWithinTheErrorBound) {
    LatencyHistogram h;
    // 1 ms, 2 ms, ..., 1000 ms in nanoseconds.
    for (std::uint64_t k = 1; k <= 1'000; ++k) {
        h.record(k * 1'000'000);
    }
    for (const auto& [q, exact] : std::vector<std::pair<double, std::uint64_t>>{
             {0.50, 500'000'000}, {0.95, 950'000'000}, {0.99, 990'000'000}, {0.999, 999'000'000}}) {
        const std::uint64_t reported = h.percentile(q);
        EXPECT_GE(reported, exact) << "q=" << q;   // never under-reports
        EXPECT_LE(reported, exact + exact / 128) << "q=" << q;
    }
    EXPECT_EQ(h.percentile(1.0), 1'000'000'000U) << "p100 is the exact maximum";
}

TEST(LatencyHistogram, PercentileNeverExceedsTheMaximum) {
    LatencyHistogram h;
    h.record(1'000'001);   // in a bucket whose upper bound is larger
    EXPECT_EQ(h.percentile(0.5), 1'000'001U);
}

TEST(LatencyHistogram, MergingEqualsRecordingEverythingInOne) {
    LatencyHistogram a;
    LatencyHistogram b;
    LatencyHistogram all;
    std::mt19937_64 rng{9};
    for (int i = 0; i < 10'000; ++i) {
        const std::uint64_t v = rng() % 10'000'000;
        (i % 3 == 0 ? a : b).record(v);
        all.record(v);
    }
    a.merge(b);
    a.merge(LatencyHistogram{});   // merging an empty histogram changes nothing
    EXPECT_EQ(a.count(), all.count());
    EXPECT_EQ(a.min(), all.min());
    EXPECT_EQ(a.max(), all.max());
    EXPECT_DOUBLE_EQ(a.mean(), all.mean());
    for (const double q : {0.1, 0.5, 0.9, 0.99, 0.999}) {
        EXPECT_EQ(a.percentile(q), all.percentile(q)) << "q=" << q;
    }
}

TEST(LatencyHistogram, MergeIntoEmptyTakesTheOtherMinimum) {
    LatencyHistogram empty;
    LatencyHistogram other;
    other.record(500);
    other.record(700);
    empty.merge(other);
    EXPECT_EQ(empty.min(), 500U);
    EXPECT_EQ(empty.max(), 700U);
}

} // namespace
