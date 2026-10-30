#include "cppkv/server/client_session.hpp"

#include "cppkv/server/endpoint.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>

#include <exception>
#include <utility>

namespace asio = boost::asio;

namespace cppkv {

ClientSession::ClientSession(asio::ip::tcp::socket socket, CommandDispatcher& dispatcher, Logger& logger,
                             SessionRegistry& registry, std::size_t max_line_bytes)
    : socket_{std::move(socket)},
      handler_{dispatcher, max_line_bytes},
      logger_{logger},
      registry_{registry} {
    // remote_endpoint() fails if the client already disconnected; that is not
    // an error worth aborting for, so use the error_code overload.
    boost::system::error_code ec;
    const auto remote = socket_.remote_endpoint(ec);
    peer_ = ec ? std::string{"unknown"} : format_endpoint(remote);
}

void ClientSession::start() {
    // start() is called on the acceptor's strand, not ours. Hop onto our own
    // strand before touching the socket, so that a concurrent request_close()
    // can never run at the same time as the first read.
    asio::post(socket_.get_executor(), [self = shared_from_this()] {
        if (self->finished_) {
            return;   // stop() was faster: request_close() already ran on this strand
        }
        self->logger_.info("client connected peer=" + self->peer_);
        self->process({});
    });
}

void ClientSession::reject(std::string message) {
    asio::post(socket_.get_executor(), [self = shared_from_this(), message = std::move(message)]() mutable {
        if (self->finished_) {
            return;
        }
        self->output_ = std::move(message);
        self->write_output(ConnectionHandler::Progress::close);
    });
}

void ClientSession::request_close() {
    asio::post(socket_.get_executor(), [self = shared_from_this()] {
        self->finish("server stopping", LogLevel::info);
    });
}

void ClientSession::process(std::string_view new_bytes) {
    ConnectionHandler::Progress progress{};
    try {
        if (!new_bytes.empty()) {
            handler_.feed(new_bytes);
        }
        progress = handler_.process(output_, kOutputFlushBytes);
    } catch (const std::exception& e) {
        // Never let an exception escape into the event loop: it would unwind
        // io_context::run() on this thread. Answer if possible, close this
        // connection only.
        logger_.error("internal error peer=" + peer_ + ": " + e.what());
        output_ += "ERR INTERNAL internal server error\r\n";
        progress = ConnectionHandler::Progress::close;
    }

    if (!output_.empty()) {
        write_output(progress);
        return;
    }
    continue_after_output(progress);
}

void ClientSession::write_output(ConnectionHandler::Progress after_write) {
    // async_write keeps writing until every byte of output_ is sent (or an
    // error occurs). output_ must not change until the handler runs.
    asio::async_write(socket_, asio::buffer(output_),
                      [self = shared_from_this(), after_write](const boost::system::error_code& ec, std::size_t) {
                          if (self->finished_) {
                              return;   // closed meanwhile (request_close)
                          }
                          if (ec) {
                              self->finish("write failed: " + ec.message(), LogLevel::warn);
                              return;
                          }
                          self->output_.clear();
                          self->continue_after_output(after_write);
                      });
}

void ClientSession::continue_after_output(ConnectionHandler::Progress progress) {
    switch (progress) {
    case ConnectionHandler::Progress::output_ready:
        process({});   // more requests are buffered; answer the next batch
        return;
    case ConnectionHandler::Progress::need_input:
        read_more();
        return;
    case ConnectionHandler::Progress::close: {
        const std::string_view reason = handler_.close_reason();
        finish(reason.empty() ? std::string_view{"connection rejected"} : reason, LogLevel::info);
        return;
    }
    }
}

void ClientSession::read_more() {
    // At most one read is pending at any time, so read_buffer_ is never shared.
    socket_.async_read_some(
        asio::buffer(read_buffer_), [self = shared_from_this()](const boost::system::error_code& ec, std::size_t n) {
            if (self->finished_) {
                return;   // the socket was closed by request_close(); ec is operation_aborted
            }
            if (ec == asio::error::eof) {
                const std::size_t pending = self->handler_.buffered_bytes();
                if (pending == 0) {
                    self->finish("client disconnected", LogLevel::info);
                } else {
                    self->finish("client disconnected mid-line, discarding " + std::to_string(pending) + " byte(s)",
                                 LogLevel::warn);
                }
                return;
            }
            if (ec) {
                self->finish("read failed: " + ec.message(), LogLevel::warn);
                return;
            }
            self->process(std::string_view{self->read_buffer_.data(), n});
        });
}

void ClientSession::finish(std::string_view reason, LogLevel level) {
    if (finished_) {
        return;
    }
    finished_ = true;

    // Release the connection slot BEFORE closing the socket. The client can
    // only observe the close (EOF) after this point, so a client that reconnects
    // after seeing EOF is guaranteed to find the slot free.
    registry_.remove(this);

    logger_.log(level, "connection closed peer=" + peer_ + ": " + std::string{reason});

    // Closing cancels any pending read or write; their handlers run with
    // operation_aborted, see finished_ == true and return. After that, no
    // shared_ptr remains and the session is destroyed.
    // Errors are ignored deliberately: the peer may already be gone, and the
    // file descriptor is released by close() in every case.
    boost::system::error_code ignored;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
    socket_.close(ignored);
}

} // namespace cppkv
