#include "cppkv/common/process_stats.hpp"

#include <sys/resource.h>

namespace cppkv {

std::uint64_t peak_rss_bytes() noexcept {
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0) {
        return 0;
    }
    const auto max_rss = static_cast<std::uint64_t>(usage.ru_maxrss);
#if defined(__APPLE__)
    return max_rss;          // bytes on macOS
#else
    return max_rss * 1024;   // KiB on Linux
#endif
}

namespace {
std::chrono::microseconds to_micros(const timeval& t) noexcept {
    return std::chrono::seconds{t.tv_sec} + std::chrono::microseconds{t.tv_usec};
}
} // namespace

CpuTimes cpu_times() noexcept {
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        return {};
    }
    return CpuTimes{to_micros(usage.ru_utime), to_micros(usage.ru_stime)};
}

std::uint64_t open_file_limit() noexcept {
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0 || limit.rlim_cur == RLIM_INFINITY) {
        return 0;
    }
    return static_cast<std::uint64_t>(limit.rlim_cur);
}

} // namespace cppkv
