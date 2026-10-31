#pragma once

// Minimal TCP client for integration tests.
//
// Reads have a deadline: if the server does not answer within kReadTimeout,
// the read throws and GoogleTest reports the test as failed with a clear
// message. The deadline is a failure detector only — a passing test never
// waits for it — so it does not make tests timing-dependent.
//
// How the deadline works: a plain blocking read cannot be given a timeout in
// Asio (SO_RCVTIMEO is not honoured by Asio's synchronous operations). So the
// read is started asynchronously and io_context::run_for() drives it for at
// most kReadTimeout. If it has not completed by then, the socket is cancelled,
// which completes the read with operation_aborted.

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/system_error.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cppkv::test {

class TestClient {
public:
    static constexpr std::chrono::seconds kReadTimeout{5};

    explicit TestClient(const boost::asio::ip::tcp::endpoint& server) : socket_{io_} { connect(server); }

    // Two-phase construction: the socket (and its file descriptor) is created
    // now, the connection later. Used when no new descriptor may be created at
    // connect time (file-descriptor exhaustion test).
    struct Deferred {};
    explicit TestClient(Deferred) : socket_{io_} { socket_.open(boost::asio::ip::tcp::v4()); }

    void connect(const boost::asio::ip::tcp::endpoint& server) {
        socket_.connect(server);   // throws on failure: the test cannot continue
        // Disable Nagle's algorithm so small writes leave immediately, which
        // makes "send in pieces" tests actually send separate segments.
        socket_.set_option(boost::asio::ip::tcp::no_delay{true});
    }

    enum class Closed { eof, reset };

    // Reads every remaining response line until the server closes, and
    // reports how it closed: FIN (eof) or RST (reset). Throws on timeout.
    std::vector<std::string> read_lines_until_closed(Closed& how) {
        std::vector<std::string> lines;
        for (;;) {
            while (true) {
                const auto end = buffer_.find("\r\n");
                if (end == std::string::npos) {
                    break;
                }
                lines.push_back(buffer_.substr(0, end));
                buffer_.erase(0, end + 2);
            }
            std::array<char, 65536> scratch{};
            boost::system::error_code ec;
            std::size_t n = 0;
            bool done = false;
            socket_.async_read_some(boost::asio::buffer(scratch), [&](const boost::system::error_code& e, std::size_t bytes) {
                ec = e;
                n = bytes;
                done = true;
            });
            wait_for(done, "the connection to close");
            buffer_.append(scratch.data(), n);
            if (ec == boost::asio::error::eof) {
                how = Closed::eof;
                return lines;
            }
            if (ec) {
                how = Closed::reset;
                return lines;
            }
        }
    }

    void send(std::string_view bytes) { boost::asio::write(socket_, boost::asio::buffer(bytes)); }

    // Returns the next response line without its "\r\n".
    // Throws on EOF, network error or timeout.
    std::string read_line() {
        boost::system::error_code ec;
        std::size_t n = 0;
        bool done = false;
        boost::asio::async_read_until(socket_, boost::asio::dynamic_buffer(buffer_), "\r\n",
                                      [&](const boost::system::error_code& e, std::size_t bytes) {
                                          ec = e;
                                          n = bytes;
                                          done = true;
                                      });
        wait_for(done, "a response line");
        if (ec) {
            throw boost::system::system_error{ec, "read_line"};
        }
        std::string line = buffer_.substr(0, n - 2);
        buffer_.erase(0, n);
        return line;
    }

    // True if the server has closed the connection and sent nothing else.
    // An RST (connection_reset) also counts as closed. Throws on timeout.
    bool next_read_is_closed() {
        if (!buffer_.empty()) {
            return false;   // unread response data
        }
        std::array<char, 64> scratch{};
        boost::system::error_code ec;
        std::size_t n = 0;
        bool done = false;
        socket_.async_read_some(boost::asio::buffer(scratch), [&](const boost::system::error_code& e, std::size_t bytes) {
            ec = e;
            n = bytes;
            done = true;
        });
        wait_for(done, "the connection to close");
        if (n > 0) {
            buffer_.append(scratch.data(), n);
            return false;
        }
        return ec == boost::asio::error::eof || ec == boost::asio::error::connection_reset;
    }

    // Closes the connection normally (FIN).
    void close() {
        boost::system::error_code ignored;
        socket_.close(ignored);
    }

    // Sends FIN: "I will not send more", but keeps reading possible.
    void shutdown_send() { socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_send); }

    // Closes abruptly with RST instead of FIN (SO_LINGER with timeout 0).
    void reset() {
        socket_.set_option(boost::asio::socket_base::linger{true, 0});
        socket_.close();
    }

private:
    // Runs the io_context until `done` is set or the deadline passes.
    void wait_for(const bool& done, const char* what) {
        io_.restart();   // run_for() returns when out of work; restart() makes it runnable again
        io_.run_for(kReadTimeout);
        if (!done) {
            socket_.cancel();   // completes the pending read with operation_aborted
            io_.restart();
            io_.run();          // run the cancelled handler so nothing refers to our locals
            throw std::runtime_error{std::string{"timed out after 5 s waiting for "} + what};
        }
    }

    boost::asio::io_context io_;
    boost::asio::ip::tcp::socket socket_;
    std::string buffer_;
};

} // namespace cppkv::test
