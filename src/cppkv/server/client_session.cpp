#include "cppkv/server/client_session.hpp"

#include "cppkv/server/endpoint.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>

#include <optional>
#include <string_view>
#include <utility>

namespace asio = boost::asio;

namespace cppkv {

namespace {

struct Reply {
    std::string text;            // complete response line including "\r\n"
    bool close_after = false;    // close the connection after sending
};

constexpr char ascii_upper(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

// ASCII-only, locale-independent case-insensitive comparison.
// (std::toupper(char) is undefined behaviour for negative char values,
// e.g. bytes >= 0x80 on platforms where char is signed.)
bool equals_ignore_case(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_upper(a[i]) != ascii_upper(b[i])) {
            return false;
        }
    }
    return true;
}

// Makes client-supplied text safe to echo inside a single response line:
// non-printable bytes become '?', and long input is truncated.
std::string printable_excerpt(std::string_view text) {
    constexpr std::size_t kMaxEcho = 32;
    std::string out;
    for (const char c : text.substr(0, kMaxEcho)) {
        out += (c >= 0x20 && c <= 0x7E) ? c : '?';
    }
    if (text.size() > kMaxEcho) {
        out += "...";
    }
    return out;
}

// Phase 3 command handling. Returns nullopt for lines that get no reply.
std::optional<Reply> handle_line(std::string_view line) {
    if (line.empty()) {
        return std::nullopt;
    }

    const std::size_t space = line.find(' ');
    const std::string_view name = line.substr(0, space);
    const bool has_arguments = space != std::string_view::npos;

    if (equals_ignore_case(name, "PING")) {
        if (has_arguments) {
            return Reply{"ERR WRONG_ARGS 'PING' takes no arguments\r\n"};
        }
        return Reply{"PONG\r\n"};
    }
    if (equals_ignore_case(name, "QUIT")) {
        if (has_arguments) {
            return Reply{"ERR WRONG_ARGS 'QUIT' takes no arguments\r\n"};
        }
        return Reply{"BYE\r\n", true};
    }
    return Reply{"ERR UNKNOWN_COMMAND unknown command '" + printable_excerpt(name) + "'\r\n"};
}

} // namespace

ClientSession::ClientSession(asio::ip::tcp::socket socket, Logger& logger, std::size_t max_line_bytes)
    : socket_{std::move(socket)}, logger_{logger}, max_line_bytes_{max_line_bytes} {
    // remote_endpoint() fails if the client already disconnected; that is not
    // an error worth aborting for, so use the error_code overload.
    boost::system::error_code ec;
    const auto remote = socket_.remote_endpoint(ec);
    peer_ = ec ? std::string{"unknown"} : format_endpoint(remote);
}

void ClientSession::run() {
    logger_.info("client connected peer=" + peer_);

    // The buffer may hold one full line plus its "\r\n". When it is full and
    // contains no '\n', read_until stops with error::not_found instead of
    // growing without bound: this is the request-size limit.
    const std::size_t buffer_limit = max_line_bytes_ + 2;

    for (;;) {
        boost::system::error_code ec;
        // If buffer_ already contains a '\n' (pipelined requests), read_until
        // returns immediately without touching the socket.
        const std::size_t consumed =
            asio::read_until(socket_, asio::dynamic_buffer(buffer_, buffer_limit), '\n', ec);

        if (ec == asio::error::eof) {
            if (buffer_.empty()) {
                logger_.info("client disconnected peer=" + peer_);
            } else {
                logger_.warn("client disconnected mid-line peer=" + peer_ + ", discarding " +
                             std::to_string(buffer_.size()) + " byte(s)");
            }
            return;
        }
        if (ec == asio::error::not_found) {
            logger_.warn("request too large peer=" + peer_ + ", closing connection");
            send("ERR TOO_LARGE request exceeds " + std::to_string(max_line_bytes_) + " bytes\r\n");
            close();
            return;
        }
        if (ec) {
            // connection_reset, broken_pipe, timed_out, ...: the client is gone.
            logger_.warn("read failed peer=" + peer_ + ": " + ec.message());
            return;
        }

        // `consumed` counts the bytes up to and including '\n'.
        std::string_view line{buffer_.data(), consumed - 1};
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        // A 17-byte line + "\n" fits an 18-byte buffer, so check explicitly.
        if (line.size() > max_line_bytes_) {
            logger_.warn("request too large peer=" + peer_ + ", closing connection");
            send("ERR TOO_LARGE request exceeds " + std::to_string(max_line_bytes_) + " bytes\r\n");
            close();
            return;
        }

        // `line` is a view into buffer_. Build the reply (which copies what it
        // needs) BEFORE erasing: erase() would leave `line` dangling.
        std::optional<Reply> reply = handle_line(line);
        buffer_.erase(0, consumed);

        if (!reply) {
            continue;
        }
        if (!send(reply->text)) {
            return;
        }
        if (reply->close_after) {
            logger_.info("client quit peer=" + peer_);
            close();
            return;
        }
    }
}

bool ClientSession::send(std::string_view text) {
    boost::system::error_code ec;
    // asio::write loops until every byte is written: a single send() call may
    // write only part of the data.
    asio::write(socket_, asio::buffer(text), ec);
    if (ec) {
        logger_.warn("write failed peer=" + peer_ + ": " + ec.message());
        return false;
    }
    return true;
}

void ClientSession::close() {
    // Errors are ignored deliberately: the peer may already be gone, and in
    // every case the file descriptor is released by close().
    boost::system::error_code ignored;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);   // send FIN after queued data
    socket_.close(ignored);
}

} // namespace cppkv
