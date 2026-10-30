#pragma once

#include "cppkv/common/logger.hpp"

#include <boost/asio/ip/tcp.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace cppkv {

// Serves one connected client with blocking reads and writes (Phase 3).
//
// Ownership and lifetime
// - The session owns the socket (moved in). When the session is destroyed,
//   the socket's destructor closes the connection: RAII, no manual cleanup.
// - `logger` is a non-owning reference; the server's logger outlives every
//   session by construction (the server creates sessions inside run()).
//
// Framing
// - TCP delivers a byte stream, not messages. The session keeps all received
//   bytes in `buffer_` and extracts one '\n'-terminated line at a time, so a
//   request split across several reads, or several requests in one read, are
//   both handled. Bytes after the extracted line stay buffered.
//
// Commands (Phase 3 subset; Phase 4 replaces this with parser + dispatcher)
// - PING -> "PONG", QUIT -> "BYE" then close, empty line -> no reply,
//   anything else -> "ERR UNKNOWN_COMMAND ..." (connection stays open).
//
// Errors
// - Network errors and disconnects end the session and are logged; they are
//   never thrown. run() can only throw for resource exhaustion
//   (std::bad_alloc); the server catches that per session.
//
// Thread safety: none needed; a session is used by exactly one thread.
class ClientSession {
public:
    ClientSession(boost::asio::ip::tcp::socket socket, Logger& logger, std::size_t max_line_bytes);

    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    // Blocks until the client disconnects, sends QUIT, sends an oversized
    // line, or a network error occurs.
    void run();

private:
    // Sends `text` to the client. Returns false (and logs) if the write failed.
    bool send(std::string_view text);

    // Sends any pending FIN and releases the socket. Used after QUIT and after
    // protocol violations that end the connection.
    void close();

    boost::asio::ip::tcp::socket socket_;
    Logger& logger_;
    const std::size_t max_line_bytes_;
    std::string peer_;     // "ip:port" of the client, for log messages
    std::string buffer_;   // received bytes not yet consumed
};

} // namespace cppkv
