#pragma once

#include "cppkv/common/logger.hpp"
#include "cppkv/server/command_dispatcher.hpp"
#include "cppkv/server/connection_handler.hpp"

#include <boost/asio/ip/tcp.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace cppkv {

// Serves one connected client with blocking reads and writes.
//
// Responsibility: move bytes between the socket and a ConnectionHandler,
// which contains all protocol logic. The session only does I/O, logging and
// connection teardown.
//
// Ownership and lifetime
// - Owns the socket (moved in) and the ConnectionHandler. When the session is
//   destroyed, the socket's destructor closes the connection (RAII).
// - `dispatcher` and `logger` are non-owning references; the server's objects
//   outlive every session by construction.
//
// Errors
// - Network errors and disconnects end the session and are logged; never thrown.
// - An exception from request handling (only std::bad_alloc is realistic) is
//   caught here: the client gets "ERR INTERNAL", the connection is closed and
//   the server carries on with the next client.
//
// Thread safety: none needed; a session is used by exactly one thread.
class ClientSession {
public:
    // Bytes requested per read() and the output size at which responses are
    // written out (see ConnectionHandler for why output must be bounded).
    static constexpr std::size_t kReadChunkBytes = 4096;
    static constexpr std::size_t kOutputFlushBytes = 64 * 1024;

    ClientSession(boost::asio::ip::tcp::socket socket, CommandDispatcher& dispatcher, Logger& logger,
                  std::size_t max_line_bytes);

    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    // Blocks until the client disconnects, sends QUIT, violates the protocol
    // in a way that ends the connection, or a network error occurs.
    void run();

private:
    void serve();

    // Sends `bytes` to the client. Returns false (and logs) if the write failed.
    bool send(std::string_view bytes);

    // Sends FIN after queued data and releases the socket.
    void close();

    boost::asio::ip::tcp::socket socket_;
    ConnectionHandler handler_;
    Logger& logger_;
    std::string peer_;   // "ip:port" of the client, for log messages
};

} // namespace cppkv
