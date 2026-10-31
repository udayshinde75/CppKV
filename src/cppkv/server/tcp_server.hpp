#pragma once

#include "cppkv/common/config.hpp"
#include "cppkv/common/logger.hpp"
#include "cppkv/server/command_dispatcher.hpp"
#include "cppkv/server/session_registry.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace cppkv {

// Asynchronous TCP server with a fixed pool of I/O threads (ADR 0002) and
// graceful shutdown (ADR 0004).
//
// - One io_context runs on `config.io_threads` threads (the thread calling
//   run() plus io_threads - 1 more). The number of threads never depends on
//   the number of clients.
// - Every client gets a ClientSession on its own strand.
// - At most `config.max_connections` sessions exist at once; a further client
//   gets "ERR BUSY too many connections" and is disconnected.
//
// Shutdown
// - shutdown(): graceful. Stop accepting, let every session answer the
//   requests it has already received, then close. Sessions still busy after
//   `config.shutdown_timeout_seconds` are closed forcibly.
// - stop(): immediate. Stop accepting, close every session now.
// - handle_signals(): SIGINT/SIGTERM start a graceful shutdown; a second
//   signal during the shutdown stops immediately.
// - run() returns once every session is gone and all I/O threads finished.
//
// Lifetime and ownership
// - Owns the io_context, acceptor, timers, signal set and session registry.
//   Member order matters: io_ is declared first, so it is constructed first and
//   destroyed last, after everything that uses it.
// - `dispatcher` and `logger` are non-owning references and must outlive the
//   server. All sessions share the dispatcher (and so the store and log).
//
// Thread safety
// - run() must be called once, from one thread; handle_signals() before it.
// - shutdown(), stop(), local_endpoint(), active_sessions() and the counters
//   may be called from any thread at any time. Shutdown state lives on the
//   control strand (the acceptor's executor), so it needs no locks.
class TcpServer {
public:
    // Opens, binds and listens. Throws boost::system::system_error if the
    // address cannot be used (e.g. port already in use): a startup failure.
    TcpServer(const ServerConfig& config, CommandDispatcher& dispatcher, Logger& logger);

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    [[nodiscard]] boost::asio::ip::tcp::endpoint local_endpoint() const noexcept { return local_endpoint_; }

    // Install SIGINT/SIGTERM handling for this server. Call before run().
    void handle_signals();

    void run();
    void shutdown();   // graceful; idempotent; may be called before run()
    void stop();       // immediate; idempotent; may be called before run()

    [[nodiscard]] std::size_t active_sessions() const { return registry_.size(); }
    [[nodiscard]] std::uint64_t accepted_total() const noexcept { return accepted_.load(); }
    [[nodiscard]] std::uint64_t rejected_total() const noexcept { return rejected_.load(); }

private:
    enum class State { running, draining, stopped };

    void start_accept();
    void on_accept(const boost::system::error_code& ec, boost::asio::ip::tcp::socket socket);
    void wait_for_signal();
    void begin_shutdown(bool graceful);   // control strand only
    void force_close_remaining();         // control strand only
    void finish_shutdown();               // control strand only
    void run_event_loop();

    boost::asio::io_context io_;
    // The acceptor lives on the control strand: accept completions, timers,
    // signal handling and shutdown steps are serialised there.
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::steady_timer accept_retry_timer_;
    boost::asio::steady_timer shutdown_deadline_;
    std::optional<boost::asio::signal_set> signals_;
    const boost::asio::ip::tcp::endpoint local_endpoint_;
    CommandDispatcher& dispatcher_;
    Logger& logger_;
    const std::size_t max_line_bytes_;
    const std::size_t max_connections_;
    const std::size_t io_threads_;
    const std::chrono::seconds idle_timeout_;
    const std::chrono::seconds shutdown_timeout_;
    SessionRegistry registry_;

    State state_ = State::running;   // control strand only
    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> rejected_{0};
};

} // namespace cppkv
