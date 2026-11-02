#pragma once

#include "cppkv/common/logger.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace cppkv {

// Upper bounds accepted on the command line (sanity limits, not tuning advice).
inline constexpr std::size_t kMaxIoThreads = 64;
inline constexpr std::size_t kMaxConnectionsLimit = 10'000;
// The line limit must leave room for a SET record inside one log record
// (log_format::kMaxPayloadBytes = 16 MiB); 8 MiB keeps a wide margin.
inline constexpr std::size_t kMinLineBytes = 16;
inline constexpr std::size_t kMaxLineBytes = std::size_t{8} * 1024 * 1024;
inline constexpr std::uint32_t kMaxTimeoutSeconds = 86'400;   // one day

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
    // terminator (--max-line-bytes).
    std::size_t max_line_bytes = std::size_t{64} * 1024;

    // Threads running the I/O event loop (and therefore request handling).
    // A fixed, small pool: the thread count never depends on the number of
    // clients. 4 is a starting point; Phase 8 measures what works best.
    std::size_t io_threads = 4;

    // Simultaneous client connections. A further client receives
    // "ERR BUSY ..." and is disconnected. Kept well below typical
    // file-descriptor limits (macOS Terminal 2560, Linux often 1024).
    std::size_t max_connections = 128;

    // A connection on which no read or write makes progress for this long is
    // closed: an idle client, a client sending half a line and then nothing,
    // or a client that stops reading its responses. Bounds how long one
    // connection can hold a slot. 0 disables the timeout.
    std::uint32_t idle_timeout_seconds = 300;

    // How long a graceful shutdown (SIGINT/SIGTERM) waits for connections to
    // finish their in-flight requests before closing them forcibly.
    std::uint32_t shutdown_timeout_seconds = 10;

    LogLevel log_level = LogLevel::info;

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
// --max-connections <1-10000>, --max-line-bytes <16-8388608>,
// --idle-timeout <seconds, 0 = off>, --shutdown-timeout <seconds>,
// --log-level debug|info|warn|error, --data-dir <path>,
// --sync always|interval|never, --recovery strict|truncate, --in-memory,
// --version, --help / -h.
[[nodiscard]] CliParseResult parse_command_line(std::span<const std::string_view> args);

} // namespace cppkv
