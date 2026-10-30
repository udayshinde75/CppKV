// Integration tests: a real TcpServer on 127.0.0.1 with an OS-chosen port,
// driven by real TCP clients.
//
// Synchronisation: tests never sleep. A test waits only for observable
// events (a response line, EOF, a joined thread). If the server misbehaves and
// a read blocks forever, the CTest TIMEOUT property fails the test.

#include "test_client.hpp"

#include "cppkv/common/config.hpp"
#include "cppkv/common/logger.hpp"
#include "cppkv/server/tcp_server.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/system_error.hpp>
#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>
#include <thread>

namespace asio = boost::asio;
using cppkv::test::TestClient;

namespace {

class ServerTest : public ::testing::Test {
protected:
    void start_server(std::size_t max_line_bytes = 1024) {
        cppkv::ServerConfig config;
        config.port = 0;   // OS picks a free port: no collisions between parallel test runs
        config.max_line_bytes = max_line_bytes;
        server_ = std::make_unique<cppkv::TcpServer>(config, logger_);
        server_thread_ = std::jthread{[this] { server_->run(); }};
    }

    // Stops the server and waits until run() has returned. After this, the
    // server thread no longer writes to log_, so the test may read it.
    // Precondition: no TestClient of the calling test is still connected
    // (stop() does not interrupt an active session in Phase 3).
    void stop_server() {
        if (server_) {
            server_->stop();
            if (server_thread_.joinable()) {
                server_thread_.join();
            }
        }
    }

    void TearDown() override { stop_server(); }

    [[nodiscard]] asio::ip::tcp::endpoint endpoint() const { return server_->local_endpoint(); }

    // Declaration order = construction order: log_ before logger_ (which
    // references it), server_ before server_thread_ (which uses it). Members
    // are destroyed in reverse, so the thread is joined before the server dies.
    std::ostringstream log_;
    cppkv::Logger logger_{log_, cppkv::LogLevel::debug};
    std::unique_ptr<cppkv::TcpServer> server_;
    std::jthread server_thread_;
};

// --- Basic commands ----------------------------------------------------------

TEST_F(ServerTest, PingReturnsPong) {
    start_server();
    TestClient client{endpoint()};
    client.send("PING\n");
    EXPECT_EQ(client.read_line(), "PONG");
}

TEST_F(ServerTest, CommandNamesAreCaseInsensitive) {
    start_server();
    TestClient client{endpoint()};
    client.send("ping\nPiNg\n");
    EXPECT_EQ(client.read_line(), "PONG");
    EXPECT_EQ(client.read_line(), "PONG");
}

TEST_F(ServerTest, AcceptsCrLfLineEndings) {
    start_server();
    TestClient client{endpoint()};
    client.send("PING\r\n");
    EXPECT_EQ(client.read_line(), "PONG");
}

TEST_F(ServerTest, PingWithArgumentsIsRejectedButConnectionStaysOpen) {
    start_server();
    TestClient client{endpoint()};
    client.send("PING hello\n");
    EXPECT_EQ(client.read_line(), "ERR WRONG_ARGS 'PING' takes no arguments");
    client.send("PING\n");
    EXPECT_EQ(client.read_line(), "PONG");
}

TEST_F(ServerTest, UnknownCommandIsRejectedButConnectionStaysOpen) {
    start_server();
    TestClient client{endpoint()};
    client.send("FLY away\n");
    EXPECT_EQ(client.read_line(), "ERR UNKNOWN_COMMAND unknown command 'FLY'");
    client.send("PING\n");
    EXPECT_EQ(client.read_line(), "PONG");
}

TEST_F(ServerTest, UnknownCommandEchoIsSanitised) {
    start_server();
    TestClient client{endpoint()};
    client.send("A\x01" "B\n");   // control byte must not be echoed back raw
    EXPECT_EQ(client.read_line(), "ERR UNKNOWN_COMMAND unknown command 'A?B'");
}

TEST_F(ServerTest, EmptyLinesGetNoReply) {
    start_server();
    TestClient client{endpoint()};
    client.send("\n\r\n\nPING\n");
    // If an empty line produced any reply, it would be read here instead.
    EXPECT_EQ(client.read_line(), "PONG");
    client.send("QUIT\n");
    EXPECT_EQ(client.read_line(), "BYE");
}

// --- TCP is a byte stream ----------------------------------------------------

TEST_F(ServerTest, RequestSplitAcrossManyWritesIsReassembled) {
    start_server();
    TestClient client{endpoint()};
    // Byte by byte, with Nagle disabled: the server sees several partial reads.
    for (const char c : std::string{"PING\r\n"}) {
        client.send(std::string(1, c));
    }
    EXPECT_EQ(client.read_line(), "PONG");
}

TEST_F(ServerTest, SeveralRequestsInOneWriteGetOneReplyEach) {
    start_server();
    TestClient client{endpoint()};
    client.send("PING\nPING\r\nFLY\nPING\n");
    EXPECT_EQ(client.read_line(), "PONG");
    EXPECT_EQ(client.read_line(), "PONG");
    EXPECT_EQ(client.read_line(), "ERR UNKNOWN_COMMAND unknown command 'FLY'");
    EXPECT_EQ(client.read_line(), "PONG");
}

TEST_F(ServerTest, RequestBoundaryInsideAWrite) {
    start_server();
    TestClient client{endpoint()};
    client.send("PI");
    client.send("NG\nPI");   // end of request 1 and start of request 2 together
    client.send("NG\n");
    EXPECT_EQ(client.read_line(), "PONG");
    EXPECT_EQ(client.read_line(), "PONG");
}

// --- QUIT and disconnects ----------------------------------------------------

TEST_F(ServerTest, QuitRepliesByeAndClosesConnection) {
    start_server();
    TestClient client{endpoint()};
    client.send("QUIT\n");
    ASSERT_EQ(client.read_line(), "BYE");
    EXPECT_TRUE(client.next_read_is_closed());
}

TEST_F(ServerTest, RequestsAfterQuitAreNotExecuted) {
    start_server();
    TestClient client{endpoint()};
    client.send("QUIT\nPING\n");
    ASSERT_EQ(client.read_line(), "BYE");
    EXPECT_TRUE(client.next_read_is_closed()) << "PING after QUIT must not be answered";
}

TEST_F(ServerTest, ClientDisconnectingMidLineIsLoggedAndServerKeepsRunning) {
    start_server();
    {
        TestClient client{endpoint()};
        client.send("PIN");     // incomplete request
        client.shutdown_send();
        EXPECT_TRUE(client.next_read_is_closed());
    }

    {
        TestClient next{endpoint()};
        next.send("PING\n");
        EXPECT_EQ(next.read_line(), "PONG");
    }   // `next` disconnects here

    // stop() waits for the client being served to leave (iterative server),
    // so every client of this test must be gone before stop_server() — or the
    // test would wait for itself.
    stop_server();
    EXPECT_NE(log_.str().find("client disconnected mid-line"), std::string::npos) << log_.str();
    EXPECT_NE(log_.str().find("discarding 3 byte(s)"), std::string::npos) << log_.str();
}

TEST_F(ServerTest, ServerSurvivesClientReset) {
    start_server();
    {
        TestClient client{endpoint()};
        client.send("PING\n");
        EXPECT_EQ(client.read_line(), "PONG");
        client.reset();   // RST instead of FIN
    }

    TestClient next{endpoint()};
    next.send("PING\n");
    EXPECT_EQ(next.read_line(), "PONG");
}

TEST_F(ServerTest, ServerSurvivesWritingToResetClient) {
    // The client sends many requests and resets without reading the replies.
    // The server's writes then fail (EPIPE / ECONNRESET). Without SIGPIPE
    // suppression this would kill the whole test process.
    start_server();
    {
        TestClient client{endpoint()};
        std::string burst;
        for (int i = 0; i < 1000; ++i) {
            burst += "PING\n";
        }
        client.send(burst);
        client.reset();
    }

    TestClient next{endpoint()};
    next.send("PING\n");
    EXPECT_EQ(next.read_line(), "PONG");
}

TEST_F(ServerTest, ManySequentialClients) {
    start_server();
    for (int i = 0; i < 50; ++i) {
        TestClient client{endpoint()};
        client.send("PING\nQUIT\n");
        ASSERT_EQ(client.read_line(), "PONG") << "client " << i;
        ASSERT_EQ(client.read_line(), "BYE") << "client " << i;
    }
}

TEST_F(ServerTest, QueuedClientIsServedAfterPreviousClientLeaves) {
    // Documents the Phase 3 model: one client at a time. The second client's
    // connection completes (kernel backlog) and its request is buffered, but
    // it is only answered once the first client has gone.
    start_server();
    TestClient first{endpoint()};
    first.send("PING\n");
    ASSERT_EQ(first.read_line(), "PONG");   // the server is now inside first's session

    TestClient second{endpoint()};
    second.send("PING\n");

    first.send("QUIT\n");
    EXPECT_EQ(first.read_line(), "BYE");
    EXPECT_EQ(second.read_line(), "PONG");
}

// --- Request size limit ------------------------------------------------------

TEST_F(ServerTest, LineAtSizeLimitIsAccepted) {
    start_server(16);
    TestClient client{endpoint()};
    client.send("ABCDEFGHIJKLMNOP\r\n");   // exactly 16 bytes + CRLF
    EXPECT_EQ(client.read_line(), "ERR UNKNOWN_COMMAND unknown command 'ABCDEFGHIJKLMNOP'");
    client.send("PING\n");
    EXPECT_EQ(client.read_line(), "PONG");
}

TEST_F(ServerTest, TerminatedLineOverSizeLimitIsRejectedAndClosed) {
    start_server(16);
    TestClient client{endpoint()};
    client.send("ABCDEFGHIJKLMNOPQ\n");   // 17 bytes + LF
    ASSERT_EQ(client.read_line(), "ERR TOO_LARGE request exceeds 16 bytes");
    EXPECT_TRUE(client.next_read_is_closed());
}

TEST_F(ServerTest, UnterminatedLineFillingBufferIsRejectedAndClosed) {
    start_server(16);
    TestClient client{endpoint()};
    // 18 bytes without '\n' fill the 16 + 2 byte buffer. Exactly 18 so the
    // server has read everything we sent (see "closing with unread data" in
    // docs/adr/0001-synchronous-iterative-server.md).
    client.send(std::string(18, 'x'));
    ASSERT_EQ(client.read_line(), "ERR TOO_LARGE request exceeds 16 bytes");
    EXPECT_TRUE(client.next_read_is_closed());
}

// --- Server lifecycle --------------------------------------------------------

TEST_F(ServerTest, StopWithoutClientsReturnsPromptly) {
    start_server();
    stop_server();   // must not hang; CTest TIMEOUT catches a hang
    EXPECT_NE(log_.str().find("server stopped"), std::string::npos) << log_.str();
}

TEST(TcpServer, StopBeforeRunMakesRunReturnImmediately) {
    std::ostringstream log;
    cppkv::Logger logger{log};
    cppkv::ServerConfig config;
    config.port = 0;
    cppkv::TcpServer server{config, logger};

    server.stop();
    server.stop();   // idempotent
    server.run();    // returns without accepting
    EXPECT_NE(log.str().find("server stopped"), std::string::npos);
}

TEST(TcpServer, ReportsTheOsChosenPort) {
    std::ostringstream log;
    cppkv::Logger logger{log};
    cppkv::ServerConfig config;
    config.port = 0;
    const cppkv::TcpServer server{config, logger};
    EXPECT_NE(server.local_endpoint().port(), 0);
    EXPECT_EQ(server.local_endpoint().address().to_string(), "127.0.0.1");
}

TEST(TcpServer, ConstructorThrowsWhenPortIsInUse) {
    asio::io_context io;
    const asio::ip::tcp::acceptor occupant{io, {asio::ip::address_v4::loopback(), 0}};

    std::ostringstream log;
    cppkv::Logger logger{log};
    cppkv::ServerConfig config;
    config.port = occupant.local_endpoint().port();

    EXPECT_THROW(cppkv::TcpServer(config, logger), boost::system::system_error);
}

TEST(TcpServer, StopWorksWhenBoundToWildcardAddress) {
#if defined(__APPLE__)
    // Listening on 0.0.0.0 can trigger the macOS firewall's "accept incoming
    // connections?" dialog for the unsigned test binary.
    GTEST_SKIP() << "wildcard bind not exercised on macOS";
#else
    std::ostringstream log;
    cppkv::Logger logger{log};
    cppkv::ServerConfig config;
    config.bind_address = "0.0.0.0";
    config.port = 0;
    cppkv::TcpServer server{config, logger};

    std::jthread runner{[&server] { server.run(); }};
    server.stop();   // must reach the server via 127.0.0.1
    runner.join();
    EXPECT_NE(log.str().find("server stopped"), std::string::npos);
#endif
}

} // namespace
