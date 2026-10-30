// cppkv-server entry point.
//
// Phase 3: parses --bind/--port, then serves clients one at a time.
// Stop the server with Ctrl-C (graceful shutdown arrives in Phase 7; until
// then the OS closes the listening socket and any client connection).

#include "cppkv/common/build_info.hpp"
#include "cppkv/common/config.hpp"
#include "cppkv/common/logger.hpp"
#include "cppkv/server/tcp_server.hpp"

#include <boost/system/system_error.hpp>

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Exit codes. 2 for usage errors follows the common Unix convention.
constexpr int kExitOk = EXIT_SUCCESS;
constexpr int kExitFailure = EXIT_FAILURE;
constexpr int kExitUsage = 2;

void print_usage(std::ostream& out) {
    out << "Usage: cppkv-server [--bind <address>] [--port <port>] [--version] [--help]\n"
           "\n"
           "  --bind <address>  IPv4 or IPv6 address to listen on (default 127.0.0.1)\n"
           "  --port <port>     TCP port, 0-65535; 0 picks a free port (default 7379)\n"
           "  --version         Print version and build information, then exit\n"
           "  --help            Print this message, then exit\n";
}

int run(std::span<char* const> argv) {
    // argv[0] is the program name. argc can legally be 0, so guard the subspan.
    const auto arguments = argv.size() > 1 ? argv.subspan(1) : std::span<char* const>{};
    const std::vector<std::string_view> args(arguments.begin(), arguments.end());

    const cppkv::CliParseResult parsed = cppkv::parse_command_line(args);
    switch (parsed.action) {
    case cppkv::CliAction::print_version:
        std::cout << cppkv::build_summary() << '\n';
        return kExitOk;
    case cppkv::CliAction::print_help:
        print_usage(std::cout);
        return kExitOk;
    case cppkv::CliAction::usage_error:
        std::cerr << "cppkv-server: error: " << parsed.error << "\n\n";
        print_usage(std::cerr);
        return kExitUsage;
    case cppkv::CliAction::run_server:
        break;
    }

    // SIGPIPE: writing to a socket whose peer has gone would normally kill the
    // process with SIGPIPE. Asio already prevents that for its sockets
    // (MSG_NOSIGNAL on Linux, SO_NOSIGPIPE on macOS), so no handler is needed.

    cppkv::Logger logger{std::cerr, cppkv::LogLevel::info};
    try {
        cppkv::TcpServer server{parsed.config, logger};
        server.run();
    } catch (const boost::system::system_error& e) {
        // Most likely "Address already in use" or "Permission denied" (port < 1024).
        logger.error("cannot listen on " + parsed.config.bind_address + ":" +
                     std::to_string(parsed.config.port) + ": " + e.code().message());
        return kExitFailure;
    }
    return kExitOk;
}

} // namespace

int main(int argc, char* argv[]) {
    // An exception escaping main() calls std::terminate() without a useful
    // message, so catch everything here, report it, and fail cleanly.
    try {
        return run(std::span<char* const>{argv, static_cast<std::size_t>(argc)});
    } catch (const std::exception& e) {
        std::cerr << "cppkv-server: fatal: " << e.what() << '\n';
    } catch (...) {
        std::cerr << "cppkv-server: fatal: unknown exception\n";
    }
    return kExitFailure;
}
