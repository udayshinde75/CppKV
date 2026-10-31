#pragma once

#include "cppkv/common/logger.hpp"
#include "cppkv/server/command_dispatcher.hpp"
#include "cppkv/server/connection_handler.hpp"
#include "cppkv/server/session_registry.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace cppkv {

// Serves one connected client with asynchronous reads and writes.
//
// Responsibility: move bytes between the socket and a ConnectionHandler (which
// holds all protocol logic), enforce the I/O timeout, log the connection's
// life, and tear it down — gracefully where possible.
//
// Lifetime: self-owned while I/O is pending
// - Sessions are created with std::make_shared. Every asynchronous operation
//   (socket read/write, timer wait) captures `shared_from_this()`, so the
//   session lives exactly as long as an operation is pending. When the last
//   handler returns without starting a new operation, the session is destroyed.
//
// Concurrency: one strand per session
// - The socket and timer use the session's strand: its handlers never run
//   concurrently, so members need no locks. request_close() / request_drain()
//   are the only entry points from other threads; they post to the strand.
//
// Ending a connection (docs/adr/0004-operations.md)
// - Lingering close (QUIT, TOO_LARGE, BUSY, graceful shutdown): send FIN after
//   the last reply, then read and discard what the client still sends, for at
//   most kLingerTimeout / kLingerMaxBytes, before closing. Closing a socket
//   that still has unread input makes the kernel send RST, which can destroy
//   the final reply before the client reads it; draining first avoids that.
// - Immediate close (client already gone, I/O error, timeout, forced stop).
// - Drain (graceful shutdown): stop reading, answer every request that has
//   already arrived complete, then lingering close.
//
// Errors: network errors and disconnects end the session and are logged;
// exceptions from request handling (std::bad_alloc) produce "ERR INTERNAL"
// and close this connection only. Nothing escapes into the event loop.
class ClientSession : public std::enable_shared_from_this<ClientSession> {
public:
    static constexpr std::size_t kReadChunkBytes = 4096;
    static constexpr std::size_t kOutputFlushBytes = 64 * 1024;
    static constexpr std::chrono::seconds kLingerTimeout{2};
    static constexpr std::size_t kLingerMaxBytes = 64 * 1024;

    struct Options {
        std::size_t max_line_bytes = 64 * 1024;
        std::chrono::steady_clock::duration io_timeout{};   // zero: no timeout
    };

    // `socket` must use a strand executor (TcpServer accepts onto a new strand).
    // `dispatcher`, `logger` and `registry` must outlive the session.
    ClientSession(boost::asio::ip::tcp::socket socket, CommandDispatcher& dispatcher, Logger& logger,
                  SessionRegistry& registry, const Options& options);

    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    // Begins serving. Call once, after the session is owned by a shared_ptr.
    void start();

    // Sends `message` (a complete response line) and closes, without serving
    // any request. Used for "ERR BUSY". Call instead of start().
    void reject(std::string message);

    // Thread-safe. Close now; pending operations are cancelled.
    void request_close();

    // Thread-safe. Finish the requests already received, then close.
    void request_drain();

    [[nodiscard]] const std::string& peer() const noexcept { return peer_; }

private:
    enum class Io { none, reading, writing };
    enum class Close { immediate, linger };

    void process(std::string_view new_bytes);
    void write_output(ConnectionHandler::Progress after_write);
    void continue_after_output(ConnectionHandler::Progress progress);
    void read_more();
    void arm_timeout();
    void finish(std::string_view reason, LogLevel level, Close how);
    void linger_read();
    void close_socket();

    boost::asio::ip::tcp::socket socket_;
    boost::asio::steady_timer timer_;   // I/O timeout while serving, then the linger deadline
    ConnectionHandler handler_;
    Logger& logger_;
    SessionRegistry& registry_;
    const Options options_;
    std::string peer_;   // "ip:port" of the client, for log messages
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();

    std::array<char, kReadChunkBytes> read_buffer_{};
    // Responses being written. Not modified while an async_write is pending:
    // the buffer must stay valid until the write completes.
    std::string output_;

    Io io_ = Io::none;           // the one operation in flight
    bool finished_ = false;      // finish() ran; only lingering may continue
    bool draining_ = false;      // graceful shutdown requested
    std::size_t lingered_bytes_ = 0;
};

} // namespace cppkv
