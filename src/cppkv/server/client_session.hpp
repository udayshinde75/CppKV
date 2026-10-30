#pragma once

#include "cppkv/common/logger.hpp"
#include "cppkv/server/command_dispatcher.hpp"
#include "cppkv/server/connection_handler.hpp"
#include "cppkv/server/session_registry.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace cppkv {

// Serves one connected client with asynchronous reads and writes.
//
// Responsibility: move bytes between the socket and a ConnectionHandler (which
// holds all protocol logic), log the connection's life, and tear it down.
//
// Lifetime: self-owned while I/O is pending
// - Sessions are created with std::make_shared. Every asynchronous operation
//   captures `shared_from_this()` in its completion handler, so the session
//   stays alive exactly as long as an operation is pending. When the last
//   handler returns without starting a new operation, the last shared_ptr
//   disappears and the session (and its socket) is destroyed.
// - Nobody calls delete, and no handler can run on a destroyed session.
//
// Concurrency: one strand per session
// - The socket's executor is a strand: Asio runs this session's handlers one
//   at a time, even though the server runs the event loop on several threads.
//   Members are therefore accessed without locks. Different sessions run in
//   parallel on different threads.
// - request_close() is the only member function called from outside; it posts
//   to the strand instead of touching the socket directly.
// - Shared state reached from here is itself thread-safe: the dispatcher
//   (mutex), the logger (mutex) and the registry (mutex).
//
// Errors: network errors and disconnects end the session and are logged;
// exceptions from request handling (std::bad_alloc) produce "ERR INTERNAL"
// and close this connection only. Nothing escapes into the event loop.
class ClientSession : public std::enable_shared_from_this<ClientSession> {
public:
    // Bytes requested per read, and the output size at which responses are
    // written out (see ConnectionHandler for why output must be bounded).
    static constexpr std::size_t kReadChunkBytes = 4096;
    static constexpr std::size_t kOutputFlushBytes = 64 * 1024;

    // `socket` must use a strand executor (TcpServer accepts onto a new strand).
    // `dispatcher`, `logger` and `registry` must outlive the session.
    ClientSession(boost::asio::ip::tcp::socket socket, CommandDispatcher& dispatcher, Logger& logger,
                  SessionRegistry& registry, std::size_t max_line_bytes);

    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    // Begins serving. Call once, after the session is owned by a shared_ptr.
    void start();

    // Sends `message` (a complete response line) and closes, without serving
    // any request. Used for "ERR BUSY". Call instead of start().
    void reject(std::string message);

    // Thread-safe: asks the session to close. Pending operations are cancelled
    // and the session is destroyed once their handlers have run.
    void request_close();

    [[nodiscard]] const std::string& peer() const noexcept { return peer_; }

private:
    void process(std::string_view new_bytes);
    void write_output(ConnectionHandler::Progress after_write);
    void continue_after_output(ConnectionHandler::Progress progress);
    void read_more();
    void finish(std::string_view reason, LogLevel level);

    boost::asio::ip::tcp::socket socket_;
    ConnectionHandler handler_;
    Logger& logger_;
    SessionRegistry& registry_;
    std::string peer_;   // "ip:port" of the client, for log messages

    std::array<char, kReadChunkBytes> read_buffer_{};
    // Responses being written. Not modified while an async_write is pending:
    // the buffer must stay valid until the write completes.
    std::string output_;
    bool finished_ = false;
};

} // namespace cppkv
