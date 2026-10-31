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
                             SessionRegistry& registry, const Options& options)
    : socket_{std::move(socket)},
      timer_{socket_.get_executor()},   // same strand as the socket
      handler_{dispatcher, options.max_line_bytes},
      logger_{logger},
      registry_{registry},
      options_{options} {
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
        self->finish("server stopping", LogLevel::info, Close::immediate);
    });
}

void ClientSession::request_drain() {
    asio::post(socket_.get_executor(), [self = shared_from_this()] {
        if (self->finished_ || self->draining_) {
            return;
        }
        self->draining_ = true;
        if (self->io_ == Io::reading) {
            // Waiting for input that will no longer be served. Cancel the read;
            // its handler sees draining_ and closes. A pending WRITE is not
            // cancelled: it carries replies to requests already executed, and
            // continue_after_output() checks draining_ once it completes.
            boost::system::error_code ignored;
            self->socket_.cancel(ignored);
        }
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
    io_ = Io::writing;
    arm_timeout();
    // async_write keeps writing until every byte of output_ is sent (or an
    // error occurs). output_ must not change until the handler runs.
    asio::async_write(socket_, asio::buffer(output_),
                      [self = shared_from_this(), after_write](const boost::system::error_code& ec, std::size_t) {
                          self->io_ = Io::none;
                          if (self->finished_) {
                              return;   // closed meanwhile (request_close, I/O timeout)
                          }
                          if (ec) {
                              self->finish("write failed: " + ec.message(), LogLevel::warn, Close::immediate);
                              return;
                          }
                          self->output_.clear();
                          self->continue_after_output(after_write);
                      });
}

void ClientSession::continue_after_output(ConnectionHandler::Progress progress) {
    switch (progress) {
    case ConnectionHandler::Progress::output_ready:
        process({});   // more complete requests are buffered: answer them, even while draining
        return;
    case ConnectionHandler::Progress::need_input:
        read_more();
        return;
    case ConnectionHandler::Progress::close: {
        const std::string_view reason = handler_.close_reason();
        finish(reason.empty() ? std::string_view{"connection rejected"} : reason, LogLevel::info, Close::linger);
        return;
    }
    }
}

void ClientSession::read_more() {
    if (draining_) {
        // Every request that arrived complete has been answered.
        const std::size_t partial = handler_.buffered_bytes();
        finish(partial == 0 ? std::string{"server shutting down"}
                            : "server shutting down, discarding " + std::to_string(partial) + " byte(s) of a partial request",
               LogLevel::info, Close::linger);
        return;
    }

    io_ = Io::reading;
    arm_timeout();
    // At most one read is pending at any time, so read_buffer_ is never shared.
    socket_.async_read_some(
        asio::buffer(read_buffer_), [self = shared_from_this()](const boost::system::error_code& ec, std::size_t n) {
            self->io_ = Io::none;
            if (self->finished_) {
                return;   // the socket was closed by request_close(); ec is operation_aborted
            }
            if (ec == asio::error::operation_aborted) {
                self->read_more();   // cancelled by request_drain(): read_more() sees draining_ and closes
                return;
            }
            if (ec == asio::error::eof) {
                const std::size_t pending = self->handler_.buffered_bytes();
                if (pending == 0) {
                    self->finish("client disconnected", LogLevel::info, Close::immediate);
                } else {
                    self->finish("client disconnected mid-line, discarding " + std::to_string(pending) + " byte(s)",
                                 LogLevel::warn, Close::immediate);
                }
                return;
            }
            if (ec) {
                self->finish("read failed: " + ec.message(), LogLevel::warn, Close::immediate);
                return;
            }
            self->process(std::string_view{self->read_buffer_.data(), n});
        });
}

void ClientSession::arm_timeout() {
    if (options_.io_timeout == std::chrono::steady_clock::duration::zero()) {
        return;
    }
    // expires_after() cancels a previous wait; that handler then runs with
    // operation_aborted. But a wait that had ALREADY expired may be queued and
    // can no longer be cancelled — so the handler also checks expiry() against
    // the clock: if the timer was re-armed meanwhile, the expiry lies in the
    // future and the stale completion is ignored.
    timer_.expires_after(options_.io_timeout);
    timer_.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
        if (ec == asio::error::operation_aborted || self->finished_ || self->io_ == Io::none ||
            self->timer_.expiry() > std::chrono::steady_clock::now()) {
            return;
        }
        // No progress for the whole timeout: close the connection.
        //
        // Closing — not socket_.cancel() — is deliberate. cancel() only aborts
        // operations queued in the reactor AT THIS MOMENT. An async_write is a
        // chain of write_some steps; between two steps nothing is queued, so a
        // cancel() would be lost and the next step could then block forever
        // with no timer left to stop it. After close(), every pending or later
        // operation on the socket fails, so the shutdown cannot be missed.
        self->finish(self->io_ == Io::writing ? "write stalled: client did not read for the I/O timeout"
                                              : "idle timeout",
                     self->io_ == Io::writing ? LogLevel::warn : LogLevel::info, Close::immediate);
    });
}

void ClientSession::finish(std::string_view reason, LogLevel level, Close how) {
    if (finished_) {
        return;
    }
    finished_ = true;

    // Release the connection slot BEFORE the client can observe the close
    // (FIN), so a client that reconnects after seeing EOF finds the slot free.
    registry_.remove(this);

    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_);
    logger_.log(level, "connection closed peer=" + peer_ + " requests=" + std::to_string(handler_.requests_handled()) +
                           " duration_ms=" + std::to_string(duration.count()) + ": " + std::string{reason});

    timer_.cancel();   // the I/O timeout no longer applies

    if (how == Close::immediate) {
        close_socket();
        return;
    }

    // Lingering close: FIN now (after any queued data), then discard input
    // until the client closes, kLingerMaxBytes arrive, or kLingerTimeout passes.
    boost::system::error_code ec;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_send, ec);
    if (ec) {
        close_socket();
        return;
    }
    timer_.expires_after(kLingerTimeout);
    timer_.async_wait([self = shared_from_this()](const boost::system::error_code& wait_ec) {
        if (!wait_ec) {
            self->close_socket();   // deadline: cancels the pending linger read
        }
    });
    linger_read();
}

void ClientSession::linger_read() {
    socket_.async_read_some(
        asio::buffer(read_buffer_), [self = shared_from_this()](const boost::system::error_code& ec, std::size_t n) {
            self->lingered_bytes_ += n;
            if (ec || self->lingered_bytes_ > kLingerMaxBytes) {
                self->close_socket();   // EOF (the normal end), error, deadline, or too much input
                return;
            }
            self->linger_read();
        });
}

void ClientSession::close_socket() {
    // Errors are ignored deliberately: the peer may already be gone, and the
    // file descriptor is released by close() in every case. Closing cancels
    // pending operations; their handlers see finished_ and return.
    timer_.cancel();
    boost::system::error_code ignored;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
}

} // namespace cppkv
