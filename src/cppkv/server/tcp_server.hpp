#pragma once

#include "cppkv/common/config.hpp"
#include "cppkv/common/logger.hpp"
#include "cppkv/server/command_dispatcher.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <cstddef>

namespace cppkv {

// Synchronous, iterative TCP server (Phase 3): accepts one client, serves it
// until it leaves, then accepts the next. Further clients wait in the kernel's
// listen backlog meanwhile. Phase 5 replaces this with a concurrent model.
//
// Lifetime and ownership
// - Owns the io_context and the listening acceptor. Member order matters:
//   io_ is declared first, so it is constructed before acceptor_ (which needs
//   it) and destroyed after it.
// - `dispatcher` and `logger` are non-owning references and must outlive the
//   server; every session uses the same dispatcher (and so the same store).
//
// Thread safety
// - run() must be called from one thread.
// - stop() and local_endpoint() may be called from any thread, concurrently
//   with run(). stop() never touches the acceptor (Asio objects are not safe
//   to share between threads); it only sets an atomic flag and opens its own
//   short-lived connection to wake up the blocked accept().
class TcpServer {
public:
    // Opens, binds and listens. Throws boost::system::system_error if the
    // address cannot be used (e.g. port already in use): a startup failure.
    TcpServer(const ServerConfig& config, CommandDispatcher& dispatcher, Logger& logger);

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // The bound address and port; useful when the config asked for port 0.
    [[nodiscard]] boost::asio::ip::tcp::endpoint local_endpoint() const noexcept { return local_endpoint_; }

    // Serves clients until stop() is called. Never throws for per-client
    // problems; a session that throws is logged and dropped.
    void run();

    // Requests run() to return. Idempotent. Does NOT interrupt a client that
    // is currently being served: run() returns after that client leaves.
    void stop();

private:
    boost::asio::io_context io_;
    boost::asio::ip::tcp::acceptor acceptor_;
    const boost::asio::ip::tcp::endpoint local_endpoint_;
    CommandDispatcher& dispatcher_;
    Logger& logger_;
    const std::size_t max_line_bytes_;
    std::atomic<bool> stop_requested_{false};
};

} // namespace cppkv
