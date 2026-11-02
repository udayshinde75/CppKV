#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace cppkv::loadgen {

enum class OutputFormat { text, csv };

struct LoadOptions {
    std::string host = "127.0.0.1";
    std::uint16_t port = 7379;

    unsigned clients = 4;          // TCP connections, one client thread each (two in open loop)
    unsigned pipeline = 1;         // closed loop: requests in flight per connection
    double rate = 0;               // open loop: total requests/s over all connections; 0 = closed loop

    std::chrono::milliseconds duration{10'000};   // measured window
    std::chrono::milliseconds warmup{2'000};      // load before the window, not measured

    std::size_t keys = 100'000;    // key space "key:0000000000" .. ; uniformly chosen
    std::size_t value_bytes = 16;  // size of every SET value
    unsigned read_percent = 90;    // share of GETs; the rest are SETs
    bool populate = true;          // SET every key once before the run, so GETs hit
    std::uint64_t seed = 1;

    OutputFormat format = OutputFormat::text;
    std::string label;             // copied into the CSV row, e.g. "sync=always"
};

inline constexpr unsigned kMaxClients = 1'024;
inline constexpr unsigned kMaxPipeline = 1'024;
inline constexpr std::size_t kMaxKeys = 100'000'000;
inline constexpr std::size_t kMaxValueBytes = 60'000;   // fits the server's default 64 KiB line limit
inline constexpr double kMaxRate = 10'000'000;

[[nodiscard]] bool is_open_loop(const LoadOptions& options) noexcept;

enum class LoadAction { run, help, usage_error };

struct LoadParseResult {
    LoadAction action = LoadAction::run;
    LoadOptions options;
    std::string error;
};

[[nodiscard]] LoadParseResult parse_load_options(std::span<const std::string_view> args);

} // namespace cppkv::loadgen
