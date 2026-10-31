// cppkv-server entry point.
//
// Parses the command line, then serves clients concurrently on a fixed pool
// of I/O threads (docs/adr/0002-concurrency-model.md).
// Stop the server with Ctrl-C (graceful shutdown arrives in Phase 7; until
// then the OS closes the listening socket and any client connection).

#include "cppkv/common/build_info.hpp"
#include "cppkv/common/config.hpp"
#include "cppkv/common/logger.hpp"
#include "cppkv/persistence/append_log.hpp"
#include "cppkv/server/command_dispatcher.hpp"
#include "cppkv/server/tcp_server.hpp"
#include "cppkv/store/kv_store.hpp"

#include <boost/system/system_error.hpp>

#include <cstddef>
#include <cstdlib>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
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
    out << "Usage: cppkv-server [options]\n"
           "\n"
           "  --bind <address>         IPv4 or IPv6 address to listen on (default 127.0.0.1)\n"
           "  --port <port>            TCP port, 0-65535; 0 picks a free port (default 7379)\n"
           "  --threads <n>            I/O threads, 1-64 (default 4)\n"
           "  --max-connections <n>    Simultaneous clients, 1-10000 (default 128)\n"
           "  --data-dir <path>        Directory of the append-only log (default ./data)\n"
           "  --sync <policy>          always (default): fsync before every OK\n"
           "                           interval: fsync about once per second\n"
           "                           never: leave flushing to the OS\n"
           "  --recovery <mode>        strict (default): refuse to start if the log is corrupted\n"
           "                           before its end; truncate: keep the valid part\n"
           "  --in-memory              No persistence: data is lost when the server stops\n"
           "  --version                Print version and build information, then exit\n"
           "  --help                   Print this message, then exit\n";
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

    // Declaration order is construction order; destruction runs in reverse.
    // The server (declared last) is destroyed first, so the dispatcher, log,
    // store and logger it refers to are still alive while it shuts down.
    cppkv::Logger logger{std::cerr, cppkv::LogLevel::info};
    cppkv::KvStore store;

    // Recovery happens before the server accepts its first client, on this
    // thread only, so replaying into the store needs no locking.
    std::unique_ptr<cppkv::AppendLog> log;
    const cppkv::PersistenceConfig& persistence = parsed.config.persistence;
    if (persistence.enabled) {
        const std::filesystem::path log_path = std::filesystem::path{persistence.data_dir} / "cppkv.log";
        try {
            std::filesystem::create_directories(persistence.data_dir);
            cppkv::LogOptions options;
            options.sync = persistence.sync;
            options.sync_interval = std::chrono::milliseconds{persistence.sync_interval_ms};
            options.recovery = persistence.recovery;
            cppkv::RecoveryReport report;
            log = cppkv::AppendLog::open(log_path, store, options, logger, report);
            logger.info("persistence: " + log_path.string() + ", sync=" + std::string{cppkv::to_string(options.sync)} +
                        ", keys=" + std::to_string(store.size()));
        } catch (const cppkv::LogCorruptedError& e) {
            logger.error(e.what());
            return kExitFailure;
        } catch (const std::exception& e) {
            logger.error(std::string{"cannot open log: "} + e.what());
            return kExitFailure;
        }
    } else {
        logger.warn("persistence disabled (--in-memory): data is lost when the server stops");
    }

    cppkv::CommandDispatcher dispatcher{store, log.get()};
    try {
        cppkv::TcpServer server{parsed.config, dispatcher, logger};
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
