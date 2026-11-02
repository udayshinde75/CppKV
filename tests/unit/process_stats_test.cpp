#include "cppkv/common/process_stats.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace {

TEST(ProcessStats, PeakRssGrowsWhenMemoryIsTouched) {
    const std::uint64_t before = cppkv::peak_rss_bytes();
    ASSERT_GT(before, 0U);

    // 64 MiB, every page written, so it is resident (not just reserved).
    constexpr std::size_t kBytes = std::size_t{64} * 1024 * 1024;
    const auto block = std::make_unique<unsigned char[]>(kBytes);
    // Written through a volatile pointer: the block is never read, so an
    // optimiser may otherwise remove the allocation and the writes entirely
    // (Clang -O3 did: the peak stayed at 4.6 MB).
    volatile unsigned char* const bytes = block.get();
    for (std::size_t i = 0; i < kBytes; i += 4096) {
        bytes[i] = 1;
    }
    const std::uint64_t after = cppkv::peak_rss_bytes();

    // The 64 MiB block is resident right now, so the peak is at least that.
    // (Not "before + 64 MiB": an earlier test may already have set a higher
    // peak.) Reading KiB as bytes would make the value 1024x too small.
    EXPECT_GE(after, kBytes) << "before=" << before << " after=" << after;
    EXPECT_LT(after, std::uint64_t{64} * 1024 * 1024 * 1024) << "implausibly large: wrong unit?";
}

TEST(ProcessStats, CpuTimeAdvancesWhileWorking) {
    const cppkv::CpuTimes before = cppkv::cpu_times();
    // Burn about 50 ms of user CPU; volatile keeps the loop from being removed.
    volatile std::uint64_t sink = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds{50};
    while (std::chrono::steady_clock::now() < until) {
        for (int i = 0; i < 1000; ++i) {
            sink = sink + static_cast<std::uint64_t>(i);
        }
    }
    const cppkv::CpuTimes after = cppkv::cpu_times();
    EXPECT_GT(after.user + after.system, before.user + before.system);
    EXPECT_GE(after.user, before.user);
}

TEST(ProcessStats, OpenFileLimitIsReported) {
    // Every supported platform has a finite default soft limit.
    EXPECT_GT(cppkv::open_file_limit(), 0U);
}

} // namespace
