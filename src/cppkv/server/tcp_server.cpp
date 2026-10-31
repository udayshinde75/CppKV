#include "cppkv/server/tcp_server.hpp"

#include "cppkv/server/client_session.hpp"
#include "cppkv/server/endpoint.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/system/errc.hpp>

#include <csignal>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace asio = boost::asio;
using asio::ip::tcp;

namespace cppkv {

namespace {

tcp::endpoint make_listen_endpoint(const ServerConfig& config) {
    // Throws boost::system::system_error on an invalid address; the command
    // line parser has already validated it, so this is a programming error.
    return tcp::endpoint{asio::ip::make_address(config.bind_address), config.port};
}

// Errors meaning "the process is out of a resource right now". Retrying at
// once would fail again immediately: with a connection still waiting in the
// backlog, the event loop would spin at 100% CPU, flooding the log.
bool is_resource_exhaustion(const boost::system::error_code& ec) {
    namespace errc = boost::system::errc;
    return ec == errc::too_many_files_open || ec == errc::too_many_files_open_in_system ||
           ec == errc::no_buffer_space || ec == errc::not_enough_memory;
}

constexpr std::chrono::milliseconds kAcceptRetryDelay{100};

} // namespace

TcpServer::TcpServer(const ServerConfig& config, CommandDispatcher& dispatcher, Logger& logger)
    // This acceptor constructor performs open + SO_REUSEADDR + bind + listen
    // and throws on failure. SO_REUSEADDR lets the server restart immediately
    // even while connections from the previous run sit in TIME_WAIT.
    : acceptor_{asio::make_strand(io_), make_listen_endpoint(config)},
      accept_retry_timer_{acceptor_.get_executor()},
      shutdown_deadline_{acceptor_.get_executor()},
      local_endpoint_{acceptor_.local_endpoint()},
      dispatcher_{dispatcher},
      logger_{logger},
      max_line_bytes_{config.max_line_bytes},
      max_connections_{config.max_connections},
      io_threads_{config.io_threads == 0 ? 1 : config.io_threads},
      idle_timeout_{config.idle_timeout_seconds},
      shutdown_timeout_{config.shutdown_timeout_seconds},
      registry_{config.max_connections} {}

void TcpServer::handle_signals() {
    // asio::signal_set turns asynchronous signals into ordinary completion
    // handlers on an I/O thread: none of the server's code ever runs inside a
    // real signal handler, where almost nothing (no locks, no allocation, no
    // logging) would be safe.
    signals_.emplace(acceptor_.get_executor(), SIGINT, SIGTERM);
    wait_for_signal();
}

void TcpServer::wait_for_signal() {
    signals_->async_wait([this](const boost::system::error_code& ec, int signal_number) {
        if (ec) {
            return;   // cancelled by finish_shutdown()
        }
        const char* name = signal_number == SIGINT ? "SIGINT" : signal_number == SIGTERM ? "SIGTERM" : "signal";
        if (state_ == State::running) {
            logger_.info(std::string{"received "} + name + ", shutting down gracefully (send again to stop immediately)");
            begin_shutdown(true);
            wait_for_signal();   // a second signal forces the stop
        } else {
            logger_.warn(std::string{"received "} + name + " again, stopping immediately");
            begin_shutdown(false);
        }
    });
}

void TcpServer::run() {
    logger_.info("listening on " + format_endpoint(local_endpoint_) + " threads=" + std::to_string(io_threads_) +
                 " max_connections=" + std::to_string(max_connections_) + " max_line_bytes=" +
                 std::to_string(max_line_bytes_) + " idle_timeout_s=" + std::to_string(idle_timeout_.count()) +
                 " shutdown_timeout_s=" + std::to_string(shutdown_timeout_.count()));
    // Queue the first accept on the control strand. This also gives the
    // io_context work before run() is called; an io_context with no work
    // returns from run() immediately.
    asio::post(acceptor_.get_executor(), [this] {
        if (state_ == State::running) {
            start_accept();
        }
    });

    {
        std::vector<std::jthread> workers;
        workers.reserve(io_threads_ - 1);
        for (std::size_t i = 1; i < io_threads_; ++i) {
            workers.emplace_back([this] { run_event_loop(); });
        }
        run_event_loop();   // the calling thread is one of the I/O threads
    }   // jthread destructors join the workers here

    logger_.info("server stopped: accepted=" + std::to_string(accepted_.load()) +
                 " rejected=" + std::to_string(rejected_.load()));
}

void TcpServer::run_event_loop() {
    // io_context::run() returns when there is no more work: the acceptor is
    // closed, timers and signal waits are cancelled, and every session has been
    // destroyed. If a handler throws despite the sessions' own try/catch,
    // report it and keep the thread serving rather than letting the exception
    // escape the thread (std::terminate).
    for (;;) {
        try {
            io_.run();
            return;
        } catch (const std::exception& e) {
            logger_.error(std::string{"unhandled exception in I/O thread: "} + e.what());
        }
    }
}

void TcpServer::start_accept() {
    // Each accepted socket gets a new strand as its executor: that strand
    // serialises all handlers of the session that will own the socket.
    acceptor_.async_accept(asio::make_strand(io_), [this](const boost::system::error_code& ec, tcp::socket socket) {
        on_accept(ec, std::move(socket));
    });
}

void TcpServer::on_accept(const boost::system::error_code& ec, tcp::socket socket) {
    if (state_ != State::running || ec == asio::error::operation_aborted) {
        return;   // shutting down: the acceptor was closed
    }
    if (ec) {
        if (is_resource_exhaustion(ec)) {
            logger_.warn("accept failed: " + ec.message() + "; retrying in " +
                         std::to_string(kAcceptRetryDelay.count()) + " ms");
            accept_retry_timer_.expires_after(kAcceptRetryDelay);
            accept_retry_timer_.async_wait([this](const boost::system::error_code& wait_ec) {
                if (!wait_ec && state_ == State::running) {
                    start_accept();
                }
            });
        } else {
            // E.g. the client reset the connection while it was still queued.
            logger_.warn("accept failed: " + ec.message());
            start_accept();
        }
        return;
    }

    ++accepted_;
    ClientSession::Options options;
    options.max_line_bytes = max_line_bytes_;
    options.io_timeout = idle_timeout_;
    auto session = std::make_shared<ClientSession>(std::move(socket), dispatcher_, logger_, registry_, options);

    switch (registry_.try_add(session)) {
    case SessionRegistry::AddResult::added:
        session->start();
        break;
    case SessionRegistry::AddResult::full:
        ++rejected_;
        logger_.warn("rejecting connection peer=" + session->peer() + ": too many connections");
        session->reject("ERR BUSY too many connections\r\n");
        break;
    case SessionRegistry::AddResult::closed:
        // Shutdown began on another thread. Dropping the last shared_ptr
        // destroys the session and closes its socket.
        return;
    }
    start_accept();
}

void TcpServer::shutdown() {
    asio::post(acceptor_.get_executor(), [this] { begin_shutdown(true); });
}

void TcpServer::stop() {
    asio::post(acceptor_.get_executor(), [this] { begin_shutdown(false); });
}

void TcpServer::begin_shutdown(bool graceful) {
    if (state_ == State::stopped) {
        return;
    }
    if (state_ == State::draining) {
        if (!graceful) {
            force_close_remaining();   // escalation: stop() or a second signal
        }
        return;
    }
    state_ = State::draining;

    boost::system::error_code ignored;
    acceptor_.close(ignored);        // the pending async_accept completes with operation_aborted
    accept_retry_timer_.cancel();

    // Refuse new sessions, collect the live ones, and get told (from whichever
    // thread removes the last session) when none are left.
    const auto sessions = registry_.close_and_collect([this] {
        asio::post(acceptor_.get_executor(), [this] { finish_shutdown(); });
    });
    logger_.info(std::string{graceful ? "draining " : "closing "} + std::to_string(sessions.size()) + " connection(s)");

    for (const auto& session : sessions) {
        if (graceful) {
            session->request_drain();
        } else {
            session->request_close();
        }
    }

    if (graceful && !sessions.empty()) {
        shutdown_deadline_.expires_after(shutdown_timeout_);
        shutdown_deadline_.async_wait([this](const boost::system::error_code& ec) {
            if (!ec && state_ == State::draining) {
                logger_.warn("shutdown timeout after " + std::to_string(shutdown_timeout_.count()) + " s: closing " +
                             std::to_string(registry_.size()) + " remaining connection(s)");
                force_close_remaining();
            }
        });
    }
}

void TcpServer::force_close_remaining() {
    for (const auto& session : registry_.close_and_collect()) {
        session->request_close();
    }
}

void TcpServer::finish_shutdown() {
    if (state_ == State::stopped) {
        return;
    }
    state_ = State::stopped;
    // Cancel everything that would otherwise keep io_context::run() busy.
    shutdown_deadline_.cancel();
    accept_retry_timer_.cancel();
    if (signals_) {
        boost::system::error_code ignored;
        signals_->cancel(ignored);
    }
    logger_.info("all connections closed");
}

} // namespace cppkv
