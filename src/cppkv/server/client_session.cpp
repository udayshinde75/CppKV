#include "cppkv/server/client_session.hpp"

#include "cppkv/server/endpoint.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>

#include <array>
#include <exception>
#include <utility>

namespace asio = boost::asio;

namespace cppkv {

ClientSession::ClientSession(asio::ip::tcp::socket socket, CommandDispatcher& dispatcher, Logger& logger,
                             std::size_t max_line_bytes)
    : socket_{std::move(socket)}, handler_{dispatcher, max_line_bytes}, logger_{logger} {
    // remote_endpoint() fails if the client already disconnected; that is not
    // an error worth aborting for, so use the error_code overload.
    boost::system::error_code ec;
    const auto remote = socket_.remote_endpoint(ec);
    peer_ = ec ? std::string{"unknown"} : format_endpoint(remote);
}

void ClientSession::run() {
    logger_.info("client connected peer=" + peer_);
    try {
        serve();
    } catch (const std::exception& e) {
        // Never let one client's failure escape: report, answer if possible,
        // close this connection only.
        logger_.error("internal error peer=" + peer_ + ": " + e.what() + ", closing connection");
        send("ERR INTERNAL internal server error\r\n");
        close();
    }
}

void ClientSession::serve() {
    std::array<char, kReadChunkBytes> chunk{};
    std::string output;

    for (;;) {
        // 1. Answer everything that is already buffered, in bounded batches.
        //    All responses produced from one read go out in one write() call.
        ConnectionHandler::Progress progress{};
        do {
            progress = handler_.process(output, kOutputFlushBytes);
            if (!output.empty()) {
                if (!send(output)) {
                    return;
                }
                output.clear();
            }
        } while (progress == ConnectionHandler::Progress::output_ready);

        if (progress == ConnectionHandler::Progress::close) {
            logger_.info("closing connection peer=" + peer_ + ": " + std::string{handler_.close_reason()});
            close();
            return;
        }

        // 2. Need more input. read_some() returns as soon as *some* bytes have
        //    arrived — anything from 1 byte to a whole chunk of many requests.
        boost::system::error_code ec;
        const std::size_t received = socket_.read_some(asio::buffer(chunk), ec);

        if (ec == asio::error::eof) {
            if (handler_.buffered_bytes() == 0) {
                logger_.info("client disconnected peer=" + peer_);
            } else {
                logger_.warn("client disconnected mid-line peer=" + peer_ + ", discarding " +
                             std::to_string(handler_.buffered_bytes()) + " byte(s)");
            }
            return;
        }
        if (ec) {
            // connection_reset, timed_out, ...: the client is gone.
            logger_.warn("read failed peer=" + peer_ + ": " + ec.message());
            return;
        }

        handler_.feed(std::string_view{chunk.data(), received});
    }
}

bool ClientSession::send(std::string_view bytes) {
    boost::system::error_code ec;
    // asio::write loops until every byte is written: a single send() system
    // call may accept only part of the data. While the client does not read,
    // this blocks — natural back-pressure in the synchronous model.
    asio::write(socket_, asio::buffer(bytes), ec);
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
    socket_.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
    socket_.close(ignored);
}

} // namespace cppkv
