#include "cppkv/server/tcp_server.hpp"

#include "cppkv/server/client_session.hpp"
#include "cppkv/server/endpoint.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>

#include <exception>
#include <string>
#include <utility>

namespace asio = boost::asio;
using asio::ip::tcp;

namespace cppkv {

namespace {

tcp::endpoint make_listen_endpoint(const ServerConfig& config) {
    // Throws boost::system::system_error on an invalid address; the command
    // line parser has already validated it, so this is a programming error.
    return tcp::endpoint{asio::ip::make_address(config.bind_address), config.port};
}

// Address that stop() connects to. A wildcard bind (0.0.0.0 or ::) is not a
// valid destination on every platform, so use loopback of the same family.
tcp::endpoint wake_up_endpoint(const tcp::endpoint& listening) {
    if (!listening.address().is_unspecified()) {
        return listening;
    }
    const asio::ip::address loopback = listening.address().is_v6()
                                           ? asio::ip::address{asio::ip::address_v6::loopback()}
                                           : asio::ip::address{asio::ip::address_v4::loopback()};
    return tcp::endpoint{loopback, listening.port()};
}

} // namespace

TcpServer::TcpServer(const ServerConfig& config, Logger& logger)
    // This acceptor constructor performs open + SO_REUSEADDR + bind + listen
    // and throws on failure. SO_REUSEADDR lets the server restart immediately
    // even while connections from the previous run sit in TIME_WAIT.
    : acceptor_{io_, make_listen_endpoint(config)},
      local_endpoint_{acceptor_.local_endpoint()},
      logger_{logger},
      max_line_bytes_{config.max_line_bytes} {}

void TcpServer::run() {
    logger_.info("listening on " + format_endpoint(local_endpoint_));

    while (!stop_requested_.load()) {
        boost::system::error_code ec;
        tcp::socket socket = acceptor_.accept(ec);   // blocks until a client connects

        // Checked after accept() returns: the connection may be stop()'s
        // wake-up call. The socket is closed by its destructor.
        if (stop_requested_.load()) {
            break;
        }
        if (ec) {
            // E.g. the client reset the connection while it was still queued.
            logger_.warn("accept failed: " + ec.message());
            continue;
        }

        // The session is a local object: it lives exactly as long as the
        // client is being served, and its destructor closes the socket.
        try {
            ClientSession session{std::move(socket), logger_, max_line_bytes_};
            session.run();
        } catch (const std::exception& e) {
            // One misbehaving session must not take the whole server down.
            logger_.error(std::string{"session aborted: "} + e.what());
        }
    }

    logger_.info("server stopped");
}

void TcpServer::stop() {
    if (stop_requested_.exchange(true)) {
        return;   // already requested
    }

    // run() may be blocked inside accept(). Connecting to ourselves makes
    // accept() return, after which run() sees the flag and exits.
    // A separate io_context and socket keep this thread away from acceptor_.
    asio::io_context wake_io;
    tcp::socket wake_socket{wake_io};
    boost::system::error_code ec;
    wake_socket.connect(wake_up_endpoint(local_endpoint_), ec);
    if (ec) {
        logger_.warn("stop: wake-up connection failed: " + ec.message());
    }
}

} // namespace cppkv
