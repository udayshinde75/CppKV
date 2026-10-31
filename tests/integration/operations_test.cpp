// Phase 7: shutdown, timeouts, lingering close, resource exhaustion and
// signals — the error paths and cleanup behaviour of the server.

#include "../support/temp_dir.hpp"
#include "server_process.hpp"
#include "test_client.hpp"

#include "cppkv/common/config.hpp"
#include "cppkv/common/logger.hpp"
#include "cppkv/server/command_dispatcher.hpp"
#include "cppkv/server/tcp_server.hpp"
#include "cppkv/store/kv_store.hpp"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#ifndef CPPKV_SERVER_BINARY
#error "CPPKV_SERVER_BINARY must be defined by the build"
#endif

using cppkv::test::ServerProcess;
using cppkv::test::TempDir;
using cppkv::test::TestClient;
using Closed = TestClient::Closed;

namespace {

int count_occurrences(const std::string& text, const std::string& needle) {
    int count = 0;
    for (auto pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

class OperationsTest : public ::testing::Test {
protected:
    void start_server(const cppkv::ServerConfig& config) {
        server_ = std::make_unique<cppkv::TcpServer>(config, dispatcher_, logger_);
        server_thread_ = std::jthread{[this] { server_->run(); }};
    }

    static cppkv::ServerConfig base_config() {
        cppkv::ServerConfig config;
        config.port = 0;
        config.io_threads = 2;
        config.idle_timeout_seconds = 0;   // tests that need it set it explicitly
        return config;
    }

    void join_server() {
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }

    void TearDown() override {
        if (server_) {
            server_->stop();
            join_server();
        }
    }

    [[nodiscard]] boost::asio::ip::tcp::endpoint endpoint() const { return server_->local_endpoint(); }

    // Makes the server queue tens of MB of responses that `client` does not
    // read, so the server's writes to it block (a "stalled reader").
    static void stall(TestClient& client) {
        const std::string value(60'000, 'v');
        client.send("SET big " + value + "\n");
        ASSERT_EQ(client.read_line(), "OK");
        std::string burst;
        for (int i = 0; i < 1000; ++i) {
            burst += "GET big\n";   // ~60 MB of responses for 8 KB of requests
        }
        client.send(burst);
        // Synchronise on an observable event: the first reply proves the server
        // has read the burst and is executing it. Sending alone proves nothing —
        // the bytes may still be in flight when the test moves on.
        ASSERT_EQ(client.read_line(), "VALUE 60000 " + value);
    }

    std::ostringstream log_;
    cppkv::Logger logger_{log_, cppkv::LogLevel::debug};
    cppkv::KvStore store_;
    cppkv::CommandDispatcher dispatcher_{store_};
    std::unique_ptr<cppkv::TcpServer> server_;
    std::jthread server_thread_;
};

// --- Graceful shutdown -------------------------------------------------------

TEST_F(OperationsTest, GracefulShutdownAnswersReceivedRequestsThenCloses) {
    start_server(base_config());
    TestClient client{endpoint()};
    std::string burst;
    for (int i = 0; i < 2000; ++i) {
        burst += "SET k" + std::to_string(i) + " v\n";
    }
    client.send(burst);
    ASSERT_EQ(client.read_line(), "OK");   // the server is busy with the burst

    server_->shutdown();

    Closed how{};
    const auto lines = client.read_lines_until_closed(how);
    client.close();   // like a real client after EOF; ends the server's lingering close at once
    join_server();

    // Every reply is OK; the connection ends with FIN, not RST; and exactly the
    // answered requests were executed — none executed without a reply, no
    // reply lost. (How many requests the server had already read when the
    // shutdown began is timing-dependent; the invariant is not.)
    for (const auto& line : lines) {
        ASSERT_EQ(line, "OK");
    }
    EXPECT_EQ(how, Closed::eof);
    EXPECT_EQ(store_.size(), lines.size() + 1);
    EXPECT_NE(log_.str().find("server shutting down"), std::string::npos) << log_.str();
    EXPECT_EQ(server_->active_sessions(), 0U);
}

TEST_F(OperationsTest, GracefulShutdownClosesIdleClientsPromptly) {
    start_server(base_config());
    TestClient a{endpoint()};
    TestClient b{endpoint()};
    a.send("PING\n");
    ASSERT_EQ(a.read_line(), "PONG");
    b.send("PING\nSET partial");   // b leaves half a request behind
    ASSERT_EQ(b.read_line(), "PONG");

    const auto started = std::chrono::steady_clock::now();
    server_->shutdown();

    Closed how_a{};
    Closed how_b{};
    EXPECT_TRUE(a.read_lines_until_closed(how_a).empty());
    EXPECT_TRUE(b.read_lines_until_closed(how_b).empty());
    a.close();
    b.close();
    join_server();
    // Idle clients are not waited for: no shutdown timeout, no linger delay.
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds{1500});
    EXPECT_EQ(how_a, Closed::eof);
    EXPECT_NE(log_.str().find("discarding 11 byte(s) of a partial request"), std::string::npos) << log_.str();
    EXPECT_NE(log_.str().find("all connections closed"), std::string::npos);
}

TEST_F(OperationsTest, ShutdownTimeoutForcesStalledClientsClosed) {
    auto config = base_config();
    config.max_line_bytes = 64 * 1024;
    config.shutdown_timeout_seconds = 1;
    start_server(config);
    TestClient stalled{endpoint()};
    stall(stalled);

    const auto started = std::chrono::steady_clock::now();
    server_->shutdown();
    join_server();
    const auto took = std::chrono::steady_clock::now() - started;

    EXPECT_NE(log_.str().find("shutdown timeout after 1 s: closing 1 remaining connection(s)"), std::string::npos)
        << log_.str();
    EXPECT_LT(took, std::chrono::seconds{5});
}

// --- Lingering close: the final reply must not be destroyed by RST -----------

TEST_F(OperationsTest, ByeArrivesEvenWhenTheClientKeepsSending) {
    start_server(base_config());
    TestClient client{endpoint()};
    // QUIT followed by ~30 KB the server will never execute. Closing with that
    // input unread would make the kernel send RST, which can discard BYE.
    std::string burst = "QUIT\n";
    for (int i = 0; i < 6000; ++i) {
        burst += "PING\n";
    }
    client.send(burst);

    Closed how{};
    const auto lines = client.read_lines_until_closed(how);
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_EQ(lines.front(), "BYE");
    EXPECT_EQ(how, Closed::eof) << "the connection must end with FIN, not RST";
}

TEST_F(OperationsTest, TooLargeErrorArrivesEvenWhenTheClientKeepsSending) {
    auto config = base_config();
    config.max_line_bytes = 16;
    start_server(config);
    TestClient client{endpoint()};
    client.send(std::string(30'000, 'x'));   // one huge line, still arriving

    Closed how{};
    const auto lines = client.read_lines_until_closed(how);
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_EQ(lines.front(), "ERR TOO_LARGE request exceeds 16 bytes");
    EXPECT_EQ(how, Closed::eof);
}

// --- Timeouts ----------------------------------------------------------------

TEST_F(OperationsTest, IdleTimeoutClosesASilentClient) {
    auto config = base_config();
    config.idle_timeout_seconds = 1;
    config.max_connections = 1;
    start_server(config);
    {
        TestClient idle{endpoint()};
        idle.send("PING\nPI");   // then silence, mid-request
        ASSERT_EQ(idle.read_line(), "PONG");
        Closed how{};
        EXPECT_TRUE(idle.read_lines_until_closed(how).empty());   // closed by the server after ~1 s
    }
    // The slot is free again.
    {
        TestClient next{endpoint()};
        next.send("PING\n");
        EXPECT_EQ(next.read_line(), "PONG");
    }
    // Read the log only after the server has stopped: until then I/O threads
    // may still write to it (TSan reported exactly this race in this test).
    server_->stop();
    join_server();
    EXPECT_NE(log_.str().find("idle timeout"), std::string::npos) << log_.str();
}

TEST_F(OperationsTest, StalledReaderIsDisconnectedAndItsSlotFreed) {
    auto config = base_config();
    config.idle_timeout_seconds = 1;
    config.max_connections = 1;
    config.max_line_bytes = 64 * 1024;
    start_server(config);
    {
        TestClient stalled{endpoint()};
        stall(stalled);
        // Stop reading for longer than the 1 s timeout (a deliberate stimulus:
        // "a client that does not read"), then drain what was buffered until
        // the server's close (FIN or RST) arrives.
        std::this_thread::sleep_for(std::chrono::seconds{2});
        Closed how{};
        (void)stalled.read_lines_until_closed(how);
    }
    {
        TestClient next{endpoint()};
        next.send("PING\n");
        EXPECT_EQ(next.read_line(), "PONG");
    }
    server_->stop();
    join_server();   // see IdleTimeoutClosesASilentClient
    EXPECT_NE(log_.str().find("write stalled"), std::string::npos) << log_.str();
}

TEST_F(OperationsTest, ActiveClientIsNotClosedByTheIdleTimeout) {
    auto config = base_config();
    config.idle_timeout_seconds = 1;
    start_server(config);
    TestClient client{endpoint()};
    // 30 round trips spread over ~1.5 s: never idle for a whole second.
    for (int i = 0; i < 30; ++i) {
        client.send("PING\n");
        ASSERT_EQ(client.read_line(), "PONG") << "round trip " << i;
        std::this_thread::sleep_for(std::chrono::milliseconds{50});   // stimulus pacing, not synchronisation
    }
}

// --- Resource exhaustion -----------------------------------------------------

// Lowers RLIMIT_NOFILE so that no new file descriptor can be created, and
// restores it on destruction.
class NoNewFileDescriptors {
public:
    NoNewFileDescriptors() {
        ::getrlimit(RLIMIT_NOFILE, &saved_);
        const int probe = ::open("/dev/null", O_RDONLY);   // the lowest free descriptor number
        ::close(probe);
        rlimit lowered = saved_;
        lowered.rlim_cur = static_cast<rlim_t>(probe);       // descriptors must be < limit: none free
        ::setrlimit(RLIMIT_NOFILE, &lowered);
    }
    ~NoNewFileDescriptors() { ::setrlimit(RLIMIT_NOFILE, &saved_); }
    NoNewFileDescriptors(const NoNewFileDescriptors&) = delete;
    NoNewFileDescriptors& operator=(const NoNewFileDescriptors&) = delete;

private:
    rlimit saved_{};
};

TEST_F(OperationsTest, AcceptBacksOffWhenOutOfFileDescriptorsAndRecovers) {
#if defined(CPPKV_UBSAN)
    // While no descriptor is free, UBSan's vptr check fails inside its own
    // runtime and reports "invalid vptr ... memory cannot be printed" on the
    // logger's stream: a false positive of the sanitizer, not of this code.
    GTEST_SKIP() << "UBSan's runtime needs a free file descriptor; this test leaves none";
#endif
    auto config = base_config();
    config.io_threads = 1;
    start_server(config);
    TestClient client{TestClient::Deferred{}};   // descriptor created before the limit drops
    {
        const NoNewFileDescriptors exhausted;
        client.connect(endpoint());   // the kernel completes the handshake; accept() fails with EMFILE
        // Hold the exhausted state for a while. (A deliberate stimulus duration:
        // the property under test is what the server does during this time.)
        std::this_thread::sleep_for(std::chrono::milliseconds{350});
    }
    client.send("PING\n");
#if defined(__APPLE__)
    // macOS drops a connection whose accept() failed for lack of a descriptor
    // (observed: the client sees EOF or a reset), so no server can answer it.
    // Linux leaves it queued, and the server must pick it up after recovering.
    Closed how{};
    EXPECT_TRUE(client.read_lines_until_closed(how).empty()) << "macOS was expected to drop this connection";
#else
    EXPECT_EQ(client.read_line(), "PONG") << "the server must accept the queued connection once descriptors are available";
#endif
    // On every platform: the server has recovered and serves new clients.
    TestClient fresh{endpoint()};
    fresh.send("PING\n");
    EXPECT_EQ(fresh.read_line(), "PONG") << "the server must accept new connections after recovering";

    server_->stop();
    join_server();
    const int failures = count_occurrences(log_.str(), "accept failed");
    EXPECT_GE(failures, 1) << log_.str();
    // With the 100 ms back-off: about 4. Without it the event loop retries
    // immediately and logs thousands of failures in the same 350 ms.
    EXPECT_LE(failures, 20) << "accept retried without back-off";
    EXPECT_NE(log_.str().find("retrying in 100 ms"), std::string::npos);
}

// --- The real binary: signals, stderr, startup errors ------------------------

class ProcessTest : public ::testing::Test {
protected:
    bool start(ServerProcess& process, std::vector<std::string> extra) {
        std::vector<std::string> args{"--port", "0", "--data-dir", dir_.path().string(), "--threads", "2"};
        args.insert(args.end(), extra.begin(), extra.end());
        return process.start(CPPKV_SERVER_BINARY, args);
    }

    static bool exited_with(int status, int code) { return WIFEXITED(status) && WEXITSTATUS(status) == code; }

    TempDir dir_;
};

TEST_F(ProcessTest, SigtermShutsDownGracefullyAndKeepsData) {
    {
        ServerProcess server;
        ASSERT_TRUE(start(server, {"--sync", "interval"})) << server.stderr_text();
        TestClient idle{server.endpoint()};
        TestClient writer{server.endpoint()};
        writer.send("SET a 1\nSET b 2\n");
        ASSERT_EQ(writer.read_line(), "OK");
        ASSERT_EQ(writer.read_line(), "OK");

        const int status = server.kill_and_wait(SIGTERM);
        EXPECT_TRUE(exited_with(status, 0)) << server.stderr_text();
        EXPECT_NE(server.stderr_text().find("received SIGTERM"), std::string::npos) << server.stderr_text();
        EXPECT_NE(server.stderr_text().find("shutdown complete"), std::string::npos) << server.stderr_text();
        Closed how{};
        EXPECT_TRUE(idle.read_lines_until_closed(how).empty());
        EXPECT_EQ(how, Closed::eof);
    }
    ServerProcess restarted;
    ASSERT_TRUE(start(restarted, {})) << restarted.stderr_text();
    TestClient client{restarted.endpoint()};
    client.send("GET a\nGET b\n");
    EXPECT_EQ(client.read_line(), "VALUE 1 1");
    EXPECT_EQ(client.read_line(), "VALUE 1 2");
}

TEST_F(ProcessTest, SigintShutsDownGracefully) {
    ServerProcess server;
    ASSERT_TRUE(start(server, {})) << server.stderr_text();
    const int status = server.kill_and_wait(SIGINT);
    EXPECT_TRUE(exited_with(status, 0)) << server.stderr_text();
    EXPECT_NE(server.stderr_text().find("received SIGINT"), std::string::npos) << server.stderr_text();
}

TEST_F(ProcessTest, SecondSignalStopsImmediately) {
    ServerProcess server;
    ASSERT_TRUE(start(server, {"--max-line-bytes", "65536", "--shutdown-timeout", "60", "--idle-timeout", "0"}))
        << server.stderr_text();
    TestClient stalled{server.endpoint()};
    stalled.send("SET big " + std::string(60'000, 'v') + "\n");
    ASSERT_EQ(stalled.read_line(), "OK");
    std::string burst;
    for (int i = 0; i < 1000; ++i) {
        burst += "GET big\n";
    }
    stalled.send(burst);
    // Wait until the server is executing the burst (first reply), then stop
    // reading: the drain cannot finish.
    ASSERT_EQ(stalled.read_line(), "VALUE 60000 " + std::string(60'000, 'v'));

    server.send_signal(SIGTERM);
    ASSERT_TRUE(server.wait_for_output("draining 1 connection(s)")) << server.stderr_text();
    const auto started = std::chrono::steady_clock::now();
    const int status = server.kill_and_wait(SIGTERM);
    EXPECT_TRUE(exited_with(status, 0)) << server.stderr_text();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds{10}) << "must not wait for the 60 s timeout";
    EXPECT_NE(server.stderr_text().find("stopping immediately"), std::string::npos) << server.stderr_text();
}

TEST_F(ProcessTest, ServerSurvivesAClosedStderrPipe) {
    // Without ignoring SIGPIPE, the first log line after the reader of stderr
    // went away (e.g. `cppkv-server 2>&1 | head -1`) would kill the server.
    ServerProcess server;
    ASSERT_TRUE(start(server, {})) << server.stderr_text();
    server.close_stderr();

    for (int i = 0; i < 3; ++i) {   // each connection logs two lines
        TestClient client{server.endpoint()};
        client.send("PING\n");
        ASSERT_EQ(client.read_line(), "PONG") << "connection " << i;
    }
    const int status = server.kill_and_wait(SIGTERM);
    EXPECT_TRUE(exited_with(status, 0)) << "status " << status;
}

TEST_F(ProcessTest, PortInUseIsAStartupError) {
    ServerProcess first;
    ASSERT_TRUE(start(first, {"--in-memory"})) << first.stderr_text();
    const std::string port = std::to_string(first.endpoint().port());

    ServerProcess second;
    EXPECT_FALSE(second.start(CPPKV_SERVER_BINARY, {"--port", port, "--in-memory"}));
    EXPECT_TRUE(exited_with(second.wait(), 1));
    EXPECT_NE(second.stderr_text().find("cannot listen"), std::string::npos) << second.stderr_text();
}

TEST_F(ProcessTest, UnusableDataDirectoryIsAStartupError) {
    const auto not_a_directory = dir_.path() / "file";
    cppkv::test::write_file(not_a_directory, "x");
    ServerProcess server;
    EXPECT_FALSE(server.start(CPPKV_SERVER_BINARY, {"--port", "0", "--data-dir", not_a_directory.string()}));
    EXPECT_TRUE(exited_with(server.wait(), 1));
    EXPECT_NE(server.stderr_text().find("cannot open log"), std::string::npos) << server.stderr_text();
}

} // namespace
