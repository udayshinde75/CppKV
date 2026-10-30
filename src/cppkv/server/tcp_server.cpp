#include "cppkv/server/tcp_server.hpp"

#include "cppkv/server/client_session.hpp"
#include "cppkv/server/endpoint.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>

#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace asio = boost::asio;
using asio::ip::tcp;

namespace cppkv {

namespace {

tcp::endpoint make_listen_endpoint(const ServerConfig& config) {
    // Throws boost::system::system_error on an invalid address; the command
    // line parser has already validated it, so this is a programming error.
    return tcp::endpoint{asio::ip::make_address(config.bind_address), config.port};
}

} // namespace

TcpServer::TcpServer(const ServerConfig& config, CommandDispatcher& dispatcher, Logger& logger)
    // This acceptor constructor performs open + SO_REUSEADDR + bind + listen
    // and throws on failure. SO_REUSEADDR lets the server restart immediately
    // even while connections from the previous run sit in TIME_WAIT.
    : acceptor_{asio::make_strand(io_), make_listen_endpoint(config)},
      local_endpoint_{acceptor_.local_endpoint()},
      dispatcher_{dispatcher},
      logger_{logger},
      max_line_bytes_{config.max_line_bytes},
      io_threads_{config.io_threads == 0 ? 1 : config.io_threads},
      registry_{config.max_connections} {}

void TcpServer::run() {
    logger_.info("listening on " + format_endpoint(local_endpoint_) + " threads=" + std::to_string(io_threads_));

    // Queue the first accept on the acceptor's strand. This also gives the
    // io_context work before run() is called; an io_context with no work
    // returns from run() immediately.
    asio::post(acceptor_.get_executor(), [this] {
        if (!registry_.is_closed()) {
            start_accept();
        }
    });

    {
        std::vector<std::jthread> workers;
        workers.reserve(io_threads_ - 1);
        for (std::size_t i = 1; i < io_threads_; ++i) {
            workers.emplace_back([this] { run_event_loop(); });
        }
        run_event_loop();   // the calling thread is one of the I/O threads
    }   // jthread destructors join the workers here

    logger_.info("server stopped");
}

void TcpServer::run_event_loop() {
    // io_context::run() returns when there is no more work: the acceptor is
    // closed and every session has been destroyed. If a handler throws despite
    // the sessions' own try/catch, report it and keep the thread serving
    // rather than letting the exception escape the thread (std::terminate).
    for (;;) {
        try {
            io_.run();
            return;
        } catch (const std::exception& e) {
            logger_.error(std::string{"unhandled exception in I/O thread: "} + e.what());
        }
    }
}

void TcpServer::start_accept() {
    // Each accepted socket gets a new strand as its executor: that strand
    // serialises all handlers of the session that will own the socket.
    acceptor_.async_accept(asio::make_strand(io_), [this](const boost::system::error_code& ec, tcp::socket socket) {
        on_accept(ec, std::move(socket));
    });
}

void TcpServer::on_accept(const boost::system::error_code& ec, tcp::socket socket) {
    if (ec == asio::error::operation_aborted || !acceptor_.is_open()) {
        return;   // stop() closed the acceptor
    }
    if (ec) {
        // E.g. the client reset the connection while it was still queued.
        logger_.warn("accept failed: " + ec.message());
        start_accept();
        return;
    }

    auto session = std::make_shared<ClientSession>(std::move(socket), dispatcher_, logger_, registry_, max_line_bytes_);

    switch (registry_.try_add(session)) {
    case SessionRegistry::AddResult::added:
        session->start();
        break;
    case SessionRegistry::AddResult::full:
        logger_.warn("rejecting connection peer=" + session->peer() + ": too many connections");
        session->reject("ERR BUSY too many connections\r\n");
        break;
    case SessionRegistry::AddResult::closed:
        // stop() is in progress. Dropping the last shared_ptr destroys the
        // session and closes its socket.
        return;
    }
    start_accept();
}

void TcpServer::stop() {
    // 1. Refuse new sessions and collect the live ones (atomically).
    const auto sessions = registry_.close_and_collect();

    // 2. Close the acceptor on its strand. The pending async_accept completes
    //    with operation_aborted and is not restarted.
    asio::post(acceptor_.get_executor(), [this] {
        boost::system::error_code ignored;
        acceptor_.close(ignored);
    });

    // 3. Ask every session to close (each on its own strand).
    for (const auto& session : sessions) {
        session->request_close();
    }
    // When the last session handler has run, io_context runs out of work and
    // every thread's run() returns.
}

} // namespace cppkv
