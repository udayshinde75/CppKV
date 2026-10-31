#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace cppkv {

// Upper bounds accepted on the command line (sanity limits, not tuning advice).
inline constexpr std::size_t kMaxIoThreads = 64;
inline constexpr std::size_t kMaxConnectionsLimit = 10'000;

// When the append-only log is flushed to stable storage (docs/adr/0003-persistence.md).
enum class SyncPolicy {
    always,     // before every acknowledgement: OK survives power loss
    interval,   // at most every sync_interval: OK survives a crash of the process
    never,      // left to the OS: OK survives a crash of the process
};

// What startup does when the log is damaged somewhere other than its tail.
enum class RecoveryMode {
    strict,     // refuse to start; the file is left untouched for inspection
    truncate,   // keep the valid prefix, discard everything from the damage on
};

[[nodiscard]] std::string_view to_string(SyncPolicy policy) noexcept;

struct PersistenceConfig {
    bool enabled = true;                              // --in-memory disables it
    std::string data_dir = "data";                    // the log is <data_dir>/cppkv.log
    SyncPolicy sync = SyncPolicy::always;
    std::uint32_t sync_interval_ms = 1000;            // for SyncPolicy::interval
    RecoveryMode recovery = RecoveryMode::strict;
};

// Runtime settings of the server. Defaults match docs/architecture.md §10.
struct ServerConfig {
    // Loopback by default: the server has no authentication, so listening on
    // all interfaces must be an explicit decision (--bind 0.0.0.0).
    std::string bind_address = "127.0.0.1";

    // 0 asks the OS for any free port (used by tests; the server logs the port).
    std::uint16_t port = 7379;

    // Maximum request line length in bytes, excluding the "\n" or "\r\n"
    // terminator. Not yet configurable from the command line (Phase 7).
    std::size_t max_line_bytes = 64 * 1024;

    // Threads running the I/O event loop (and therefore request handling).
    // A fixed, small pool: the thread count never depends on the number of
    // clients. 4 is a starting point; Phase 8 measures what works best.
    std::size_t io_threads = 4;

    // Simultaneous client connections. A further client receives
    // "ERR BUSY ..." and is disconnected. Kept well below typical
    // file-descriptor limits (macOS Terminal 2560, Linux often 1024).
    std::size_t max_connections = 128;

    PersistenceConfig persistence;
};

enum class CliAction { run_server, print_version, print_help, usage_error };

struct CliParseResult {
    CliAction action = CliAction::run_server;
    ServerConfig config;
    std::string error;   // human-readable reason; set only when action == usage_error
};

// Parses command-line arguments, excluding the program name (argv[0]).
// Invalid input is reported as a value (action == usage_error), not as an
// exception: a typo on the command line is an expected outcome.
//
// Supported: --bind <address>, --port <0-65535>, --threads <1-64>,
// --max-connections <1-10000>, --data-dir <path>, --sync always|interval|never,
// --recovery strict|truncate, --in-memory, --version, --help / -h.
[[nodiscard]] CliParseResult parse_command_line(std::span<const std::string_view> args);

} // namespace cppkv
