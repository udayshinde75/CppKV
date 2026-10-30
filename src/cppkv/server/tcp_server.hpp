#pragma once

#include "cppkv/common/config.hpp"
#include "cppkv/common/logger.hpp"
#include "cppkv/server/command_dispatcher.hpp"
#include "cppkv/server/session_registry.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

#include <cstddef>

namespace cppkv {

// Asynchronous TCP server with a fixed pool of I/O threads (ADR 0002).
//
// - One io_context runs on `config.io_threads` threads (the thread calling
//   run() plus io_threads - 1 more). The number of threads never depends on
//   the number of clients.
// - Every client gets a ClientSession on its own strand: handlers of one
//   session never run concurrently; different sessions run in parallel.
// - At most `config.max_connections` sessions exist at once; a further client
//   gets "ERR BUSY too many connections" and is disconnected.
//
// Lifetime and ownership
// - Owns the io_context, the acceptor and the session registry. Member order
//   matters: io_ is declared first, so it is constructed first and destroyed
//   last, after everything that uses it.
// - `dispatcher` and `logger` are non-owning references and must outlive the
//   server. All sessions share the dispatcher (and so the store).
// - run() returns only after every session has been destroyed, so no session
//   can outlive the server.
//
// Thread safety
// - run() must be called once, from one thread.
// - stop(), local_endpoint() and active_sessions() may be called from any
//   thread at any time.
class TcpServer {
public:
    // Opens, binds and listens. Throws boost::system::system_error if the
    // address cannot be used (e.g. port already in use): a startup failure.
    TcpServer(const ServerConfig& config, CommandDispatcher& dispatcher, Logger& logger);

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // The bound address and port; useful when the config asked for port 0.
    [[nodiscard]] boost::asio::ip::tcp::endpoint local_endpoint() const noexcept { return local_endpoint_; }

    // Serves clients until stop() is called, then returns once all sessions
    // are closed and all I/O threads have finished.
    void run();

    // Stops accepting, closes every client connection (in-flight responses
    // may be cut off; graceful draining is Phase 7) and makes run() return.
    // Idempotent; may be called before run().
    void stop();

    [[nodiscard]] std::size_t active_sessions() const { return registry_.size(); }

private:
    void start_accept();
    void on_accept(const boost::system::error_code& ec, boost::asio::ip::tcp::socket socket);
    void run_event_loop();

    boost::asio::io_context io_;
    // The acceptor lives on its own strand: async_accept completions and the
    // close() posted by stop() are serialised, so they never race.
    boost::asio::ip::tcp::acceptor acceptor_;
    const boost::asio::ip::tcp::endpoint local_endpoint_;
    CommandDispatcher& dispatcher_;
    Logger& logger_;
    const std::size_t max_line_bytes_;
    const std::size_t io_threads_;
    SessionRegistry registry_;
};

} // namespace cppkv
