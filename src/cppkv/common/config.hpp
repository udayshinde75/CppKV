#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace cppkv {

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
// Supported: --bind <address>, --port <0-65535>, --version, --help / -h.
[[nodiscard]] CliParseResult parse_command_line(std::span<const std::string_view> args);

} // namespace cppkv
