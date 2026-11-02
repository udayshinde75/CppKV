#pragma once

#include "loadgen/latency_histogram.hpp"
#include "loadgen/load_options.hpp"

#include <cstdint>
#include <string>

namespace cppkv::loadgen {

// Results of one run. Counts and latencies cover only requests SENT (closed
// loop) or SCHEDULED (open loop) inside the measured window, i.e. after the
// warm-up; replies to them may arrive shortly after the window ends.
struct LoadReport {
    std::uint64_t requests = 0;     // replies received to measured requests
    std::uint64_t gets = 0;
    std::uint64_t sets = 0;
    std::uint64_t get_misses = 0;   // GET answered NIL
    std::uint64_t errors = 0;       // ERR replies or replies of the wrong type
    double window_seconds = 0;
    LatencyHistogram latency;       // nanoseconds
    // Open loop only: how late each measured request was actually written,
    // relative to its scheduled time. It is part of `latency`. If it is large,
    // the client (thread wake-up, CPU contention) and not the server explains
    // the latency, and the run should be repeated with fewer client threads.
    LatencyHistogram send_lag;
    std::string failure;            // non-empty: the run did not complete (e.g. connection lost)

    [[nodiscard]] double throughput() const noexcept {
        return window_seconds > 0 ? static_cast<double>(requests) / window_seconds : 0;
    }
};

// Runs the workload against a live server and blocks until it is done
// (warm-up + duration + time to collect outstanding replies). Never throws for
// network errors; they are reported in LoadReport::failure.
//
// Closed loop (rate == 0): each connection sends `pipeline` requests, waits for
// all their replies, and repeats. Latency = reply time - send time. The
// offered load adapts to the server: a slow server is asked less.
//
// Open loop (rate > 0): each connection sends on a fixed schedule (rate /
// clients per second, connections staggered), whatever the replies do, and a
// second thread per connection reads replies. Latency = reply time - SCHEDULED
// send time, so a stall that delays sending is counted against the server
// instead of being hidden ("coordinated omission").
[[nodiscard]] LoadReport run_load(const LoadOptions& options);

[[nodiscard]] std::string format_text(const LoadOptions& options, const LoadReport& report);
[[nodiscard]] std::string csv_header();
[[nodiscard]] std::string format_csv_row(const LoadOptions& options, const LoadReport& report);

} // namespace cppkv::loadgen
