#pragma once

#include <chrono>
#include <cstdint>

namespace cppkv {

// Peak resident set size (RSS) of this process so far, in bytes, from
// getrusage(). 0 if the OS does not report it.
// Linux reports ru_maxrss in KiB, macOS in bytes; this function hides that.
[[nodiscard]] std::uint64_t peak_rss_bytes() noexcept;

// CPU time this process has used so far: in user space (our code and the
// libraries) and in the kernel on its behalf (system calls: network, disk).
// The split tells where an optimisation could help (docs/benchmarks.md).
struct CpuTimes {
    std::chrono::microseconds user{0};
    std::chrono::microseconds system{0};
};
[[nodiscard]] CpuTimes cpu_times() noexcept;

// Soft RLIMIT_NOFILE: the highest file descriptor number + 1 this process may
// open, which bounds the number of clients (one descriptor each). 0 if
// unlimited or unavailable.
[[nodiscard]] std::uint64_t open_file_limit() noexcept;

} // namespace cppkv
