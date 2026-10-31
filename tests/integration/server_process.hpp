#pragma once

// Runs the real cppkv-server binary as a child process, so tests can kill it
// with SIGKILL — a genuine crash, not a simulated one.
//
// The child's stderr is a pipe read by the test. Synchronisation is by
// content: start() blocks until the server logs "listening on <addr>:<port>",
// with poll() deadlines as failure detectors only.

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

extern char** environ;

namespace cppkv::test {

class ServerProcess {
public:
    static constexpr int kTimeoutMs = 10'000;

    ServerProcess() = default;
    ~ServerProcess() {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            int status = 0;
            ::waitpid(pid_, &status, 0);
        }
        if (stderr_fd_ >= 0) {
            ::close(stderr_fd_);
        }
    }
    ServerProcess(const ServerProcess&) = delete;
    ServerProcess& operator=(const ServerProcess&) = delete;

    // Launches the server and waits until it listens. Returns false if the
    // process exited instead (e.g. refused to start); stderr() then has why.
    bool start(const std::string& binary, std::vector<std::string> args) {
        int pipe_fds[2];
        if (::pipe(pipe_fds) != 0) {
            throw std::system_error{errno, std::generic_category(), "pipe"};
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
        posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);

        args.insert(args.begin(), binary);
        std::vector<char*> argv;
        for (auto& arg : args) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);

        const int rc = ::posix_spawn(&pid_, binary.c_str(), &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(pipe_fds[1]);   // the child owns the write end now
        stderr_fd_ = pipe_fds[0];
        if (rc != 0) {
            pid_ = -1;
            throw std::system_error{rc, std::generic_category(), "posix_spawn " + binary};
        }

        // Read stderr until the "listening on" line, or until the child exits.
        const std::string marker = "listening on ";
        for (;;) {
            const auto at = stderr_.find(marker);
            const auto line_end = at == std::string::npos ? std::string::npos : stderr_.find('\n', at);
            if (line_end != std::string::npos) {
                const std::string address_port = stderr_.substr(at + marker.size(), stderr_.find(' ', at + marker.size()) -
                                                                                         (at + marker.size()));
                const auto colon = address_port.rfind(':');
                port_ = static_cast<unsigned short>(std::stoi(address_port.substr(colon + 1)));
                return true;
            }
            if (!read_some_stderr()) {
                return false;   // EOF: the server exited before listening
            }
        }
    }

    [[nodiscard]] boost::asio::ip::tcp::endpoint endpoint() const {
        return {boost::asio::ip::address_v4::loopback(), port_};
    }

    // Sends `signal`, waits for the process and returns its wait() status.
    int kill_and_wait(int signal) {
        ::kill(pid_, signal);
        return wait();
    }

    // Waits for exit (draining stderr so the child never blocks on a full pipe).
    int wait() {
        while (stderr_fd_ >= 0 && read_some_stderr()) {
        }
        int status = 0;
        ::waitpid(pid_, &status, 0);
        pid_ = -1;
        return status;
    }

    [[nodiscard]] const std::string& stderr_text() const noexcept { return stderr_; }

    // Blocks until the server has written `text` to stderr. Returns false if
    // it exited first.
    bool wait_for_output(const std::string& text) {
        while (stderr_.find(text) == std::string::npos) {
            if (!read_some_stderr()) {
                return false;
            }
        }
        return true;
    }

    // Closes our end of the stderr pipe: the server's next log line then
    // writes to a pipe without a reader (EPIPE / SIGPIPE).
    void close_stderr() {
        if (stderr_fd_ >= 0) {
            ::close(stderr_fd_);
            stderr_fd_ = -1;
        }
    }

    void send_signal(int signal) { ::kill(pid_, signal); }

private:
    // Appends available stderr bytes. Returns false on EOF.
    bool read_some_stderr() {
        pollfd fd{stderr_fd_, POLLIN, 0};
        const int ready = ::poll(&fd, 1, kTimeoutMs);
        if (ready == 0) {
            throw std::runtime_error{"timed out waiting for server output; so far:\n" + stderr_};
        }
        if (ready < 0) {
            if (errno == EINTR) {
                return true;
            }
            throw std::system_error{errno, std::generic_category(), "poll"};
        }
        char buffer[4096];
        const ssize_t n = ::read(stderr_fd_, buffer, sizeof buffer);
        if (n < 0) {
            if (errno == EINTR) {
                return true;
            }
            throw std::system_error{errno, std::generic_category(), "read"};
        }
        if (n == 0) {
            return false;
        }
        stderr_.append(buffer, static_cast<std::size_t>(n));
        return true;
    }

    pid_t pid_ = -1;
    int stderr_fd_ = -1;
    std::string stderr_;
    unsigned short port_ = 0;
};

} // namespace cppkv::test
