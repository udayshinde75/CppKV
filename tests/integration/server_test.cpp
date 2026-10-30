// Integration tests: a real TcpServer on 127.0.0.1 with an OS-chosen port,
// driven by real TCP clients.
//
// Synchronisation: tests never sleep. A test waits only for observable
// events (a response line, EOF, a joined thread). If the server misbehaves and
// a read blocks forever, the CTest TIMEOUT property fails the test.

#include "test_client.hpp"

#include "cppkv/common/config.hpp"
#include "cppkv/common/logger.hpp"
#include "cppkv/server/command_dispatcher.hpp"
#include "cppkv/server/tcp_server.hpp"
#include "cppkv/store/kv_store.hpp"

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
        server_ = std::make_unique<cppkv::TcpServer>(config, dispatcher_, logger_);
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

    // Declaration order = construction order: each member is declared after
    // everything it references (logger_ -> log_, dispatcher_ -> store_,
    // server_ -> both, server_thread_ -> server_). Members are destroyed in
    // reverse, so the thread is joined before the server and store die.
    std::ostringstream log_;
    cppkv::Logger logger_{log_, cppkv::LogLevel::debug};
    cppkv::KvStore store_;
    cppkv::CommandDispatcher dispatcher_{store_};
    std::unique_ptr<cppkv::TcpServer> server_;
    std::jthread server_thread_;
};

// --- PING, errors, case ------------------------------------------------------

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

// --- Data commands over TCP (Phase 4) ----------------------------------------

TEST_F(ServerTest, SetThenGetReturnsValueWithLength) {
    start_server();
    TestClient client{endpoint()};
    client.send("SET city Frankfurt am Main\n");
    EXPECT_EQ(client.read_line(), "OK");
    client.send("GET city\n");
    EXPECT_EQ(client.read_line(), "VALUE 17 Frankfurt am Main");
}

TEST_F(ServerTest, AllCommandsInOnePipelinedWrite) {
    start_server();
    TestClient client{endpoint()};
    client.send("SET a 1\r\nSET b  two\r\nGET b\nEXISTS a\nEXISTS z\nDBSIZE\nDEL a\nDEL a\nGET a\nSET e \nGET e\n");
    EXPECT_EQ(client.read_line(), "OK");
    EXPECT_EQ(client.read_line(), "OK");
    EXPECT_EQ(client.read_line(), "VALUE 4  two");   // leading space is part of the value
    EXPECT_EQ(client.read_line(), "INT 1");
    EXPECT_EQ(client.read_line(), "INT 0");
    EXPECT_EQ(client.read_line(), "INT 2");
    EXPECT_EQ(client.read_line(), "INT 1");
    EXPECT_EQ(client.read_line(), "INT 0");
    EXPECT_EQ(client.read_line(), "NIL");
    EXPECT_EQ(client.read_line(), "OK");
    EXPECT_EQ(client.read_line(), "VALUE 0 ");
}

TEST_F(ServerTest, SetSentByteByByteIsReassembled) {
    start_server();
    TestClient client{endpoint()};
    for (const char c : std::string{"SET name hello\r\nGET name\r\n"}) {
        client.send(std::string(1, c));
    }
    EXPECT_EQ(client.read_line(), "OK");
    EXPECT_EQ(client.read_line(), "VALUE 5 hello");
}

TEST_F(ServerTest, ValueWithNulByteRoundTrips) {
    using namespace std::string_literals;
    start_server();
    TestClient client{endpoint()};
    client.send("SET bin a\0b\n"s);
    EXPECT_EQ(client.read_line(), "OK");
    client.send("GET bin\n");
    EXPECT_EQ(client.read_line(), "VALUE 3 a\0b"s);
}

TEST_F(ServerTest, LargeValueRoundTrips) {
    start_server(64 * 1024);
    TestClient client{endpoint()};
    const std::string value(60'000, 'v');
    client.send("SET big " + value + "\n");
    EXPECT_EQ(client.read_line(), "OK");
    client.send("GET big\n");
    EXPECT_EQ(client.read_line(), "VALUE 60000 " + value);
}

TEST_F(ServerTest, ManyPipelinedGetsForLargeValueAllArrive) {
    // ~6 MB of responses for ~1 KB of requests: exercises bounded output
    // batches and blocking writes while the client is not yet reading.
    start_server(64 * 1024);
    TestClient client{endpoint()};
    const std::string value(30'000, 'v');
    client.send("SET big " + value + "\n");
    ASSERT_EQ(client.read_line(), "OK");

    constexpr int kGets = 200;
    std::string burst;
    for (int i = 0; i < kGets; ++i) {
        burst += "GET big\n";
    }
    client.send(burst);
    const std::string expected = "VALUE 30000 " + value;
    for (int i = 0; i < kGets; ++i) {
        ASSERT_EQ(client.read_line(), expected) << "response " << i;
    }
}

TEST_F(ServerTest, DataIsSharedBetweenConnections) {
    start_server();
    {
        TestClient writer{endpoint()};
        writer.send("SET shared yes\nQUIT\n");
        ASSERT_EQ(writer.read_line(), "OK");
        ASSERT_EQ(writer.read_line(), "BYE");
    }
    TestClient reader{endpoint()};
    reader.send("GET shared\nDBSIZE\n");
    EXPECT_EQ(reader.read_line(), "VALUE 3 yes");
    EXPECT_EQ(reader.read_line(), "INT 1");
}

TEST_F(ServerTest, ProtocolErrorsKeepConnectionOpen) {
    start_server();
    TestClient client{endpoint()};
    client.send("GET\nGET a b\nGET  a\nSET k\nGET bad\x01key\nPI\rNG\nPING\n");
    EXPECT_EQ(client.read_line(), "ERR WRONG_ARGS 'GET' takes exactly one argument: <key>");
    EXPECT_EQ(client.read_line(), "ERR WRONG_ARGS 'GET' takes exactly one argument: <key>");
    EXPECT_EQ(client.read_line(), "ERR PROTOCOL empty token (leading, trailing or repeated space)");
    EXPECT_EQ(client.read_line(), "ERR WRONG_ARGS 'SET' takes a key and a value");
    EXPECT_EQ(client.read_line(), "ERR INVALID_KEY key must be 1-512 printable ASCII characters without spaces");
    EXPECT_EQ(client.read_line(), "ERR PROTOCOL bare CR inside request");
    EXPECT_EQ(client.read_line(), "PONG");
}

TEST_F(ServerTest, RequestsAfterQuitDoNotModifyTheStore) {
    start_server();
    {
        TestClient client{endpoint()};
        client.send("SET before 1\nQUIT\nSET after 2\n");
        ASSERT_EQ(client.read_line(), "OK");
        ASSERT_EQ(client.read_line(), "BYE");
        EXPECT_TRUE(client.next_read_is_closed());
    }
    stop_server();   // after this, reading store_ from the test thread is race-free
    EXPECT_TRUE(store_.contains("before"));
    EXPECT_FALSE(store_.contains("after"));
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
    // 17 bytes without '\n' (and not ending in '\r') can never become a valid
    // 16-byte line, so the server rejects them without waiting for more. The
    // whole burst fits in one server read, so nothing is left unread when the
    // server closes (see "closing with unread data" in ADR 0001).
    client.send(std::string(17, 'x'));
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
    cppkv::KvStore store;
    cppkv::CommandDispatcher dispatcher{store};
    cppkv::ServerConfig config;
    config.port = 0;
    cppkv::TcpServer server{config, dispatcher, logger};

    server.stop();
    server.stop();   // idempotent
    server.run();    // returns without accepting
    EXPECT_NE(log.str().find("server stopped"), std::string::npos);
}

TEST(TcpServer, ReportsTheOsChosenPort) {
    std::ostringstream log;
    cppkv::Logger logger{log};
    cppkv::KvStore store;
    cppkv::CommandDispatcher dispatcher{store};
    cppkv::ServerConfig config;
    config.port = 0;
    const cppkv::TcpServer server{config, dispatcher, logger};
    EXPECT_NE(server.local_endpoint().port(), 0);
    EXPECT_EQ(server.local_endpoint().address().to_string(), "127.0.0.1");
}

TEST(TcpServer, ConstructorThrowsWhenPortIsInUse) {
    asio::io_context io;
    const asio::ip::tcp::acceptor occupant{io, {asio::ip::address_v4::loopback(), 0}};

    std::ostringstream log;
    cppkv::Logger logger{log};
    cppkv::KvStore store;
    cppkv::CommandDispatcher dispatcher{store};
    cppkv::ServerConfig config;
    config.port = occupant.local_endpoint().port();

    EXPECT_THROW(cppkv::TcpServer(config, dispatcher, logger), boost::system::system_error);
}

TEST(TcpServer, StopWorksWhenBoundToWildcardAddress) {
#if defined(__APPLE__)
    // Listening on 0.0.0.0 can trigger the macOS firewall's "accept incoming
    // connections?" dialog for the unsigned test binary.
    GTEST_SKIP() << "wildcard bind not exercised on macOS";
#else
    std::ostringstream log;
    cppkv::Logger logger{log};
    cppkv::KvStore store;
    cppkv::CommandDispatcher dispatcher{store};
    cppkv::ServerConfig config;
    config.bind_address = "0.0.0.0";
    config.port = 0;
    cppkv::TcpServer server{config, dispatcher, logger};

    std::jthread runner{[&server] { server.run(); }};
    server.stop();   // must reach the server via 127.0.0.1
    runner.join();
    EXPECT_NE(log.str().find("server stopped"), std::string::npos);
#endif
}

} // namespace
