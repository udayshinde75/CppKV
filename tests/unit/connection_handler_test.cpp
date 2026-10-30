// The whole protocol of one connection, tested without sockets.

#include "cppkv/server/connection_handler.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using Progress = cppkv::ConnectionHandler::Progress;

namespace {

constexpr std::size_t kNoOutputLimit = static_cast<std::size_t>(-1);

struct Conversation {
    std::string output;
    bool closed = false;
};

// Feeds each chunk and processes until more input is needed or the
// connection closes, like ClientSession does with real reads.
Conversation converse(const std::vector<std::string>& chunks, std::size_t max_line_bytes = 1024) {
    cppkv::KvStore store;
    cppkv::CommandDispatcher dispatcher{store};
    cppkv::ConnectionHandler handler{dispatcher, max_line_bytes};

    Conversation result;
    for (const std::string& chunk : chunks) {
        handler.feed(chunk);
        const Progress progress = handler.process(result.output, kNoOutputLimit);
        if (progress == Progress::close) {
            result.closed = true;
            return result;
        }
    }
    return result;
}

const std::string kSession =
    "PING\r\n"
    "SET name hello world\r\n"
    "GET name\n"
    "\n"
    "EXISTS name\n"
    "SET empty \n"
    "GET empty\n"
    "GET nope\n"
    "DBSIZE\n"
    "FLY\n"
    "DEL name\r\n"
    "DEL name\n"
    "QUIT\n"
    "PING\n";   // after QUIT: must not be answered

const std::string kExpected =
    "PONG\r\n"
    "OK\r\n"
    "VALUE 11 hello world\r\n"
    "INT 1\r\n"
    "OK\r\n"
    "VALUE 0 \r\n"
    "NIL\r\n"
    "INT 2\r\n"
    "ERR UNKNOWN_COMMAND unknown command 'FLY'\r\n"
    "INT 1\r\n"
    "INT 0\r\n"
    "BYE\r\n";

TEST(ConnectionHandler, FullSessionInOneChunk) {
    const Conversation c = converse({kSession});
    EXPECT_EQ(c.output, kExpected);
    EXPECT_TRUE(c.closed);
}

TEST(ConnectionHandler, FullSessionByteByByte) {
    std::vector<std::string> bytes;
    for (const char ch : kSession) {
        bytes.emplace_back(1, ch);
    }
    const Conversation c = converse(bytes);
    EXPECT_EQ(c.output, kExpected);
    EXPECT_TRUE(c.closed);
}

TEST(ConnectionHandler, FullSessionSplitAtEveryPairOfPositions) {
    // Every way of cutting the 120-byte stream into three reads: 7'381 conversations.
    for (std::size_t i = 0; i <= kSession.size(); ++i) {
        for (std::size_t j = i; j <= kSession.size(); ++j) {
            const Conversation c = converse({kSession.substr(0, i), kSession.substr(i, j - i), kSession.substr(j)});
            ASSERT_EQ(c.output, kExpected) << "split at " << i << " and " << j;
            ASSERT_TRUE(c.closed);
        }
    }
}

TEST(ConnectionHandler, ErrorsThatKeepTheConnectionOpen) {
    const Conversation c = converse({"GET\nPING x\n PING\nGET bad\x01key\nSET k\nPING\n"});
    EXPECT_EQ(c.output,
              "ERR WRONG_ARGS 'GET' takes exactly one argument: <key>\r\n"
              "ERR WRONG_ARGS 'PING' takes no arguments\r\n"
              "ERR PROTOCOL empty token (leading, trailing or repeated space)\r\n"
              "ERR INVALID_KEY key must be 1-512 printable ASCII characters without spaces\r\n"
              "ERR WRONG_ARGS 'SET' takes a key and a value\r\n"
              "PONG\r\n");
    EXPECT_FALSE(c.closed);
}

TEST(ConnectionHandler, OversizedRequestClosesAfterAnsweringEarlierOnes) {
    const Conversation c = converse({"PING\n", "SET k " + std::string(100, 'v') + "\nPING\n"}, 16);
    EXPECT_EQ(c.output, "PONG\r\nERR TOO_LARGE request exceeds 16 bytes\r\n");
    EXPECT_TRUE(c.closed);
}

TEST(ConnectionHandler, NothingIsProcessedAfterClose) {
    cppkv::KvStore store;
    cppkv::CommandDispatcher dispatcher{store};
    cppkv::ConnectionHandler handler{dispatcher, 1024};

    std::string out;
    handler.feed("QUIT\nSET k v\n");
    EXPECT_EQ(handler.process(out, kNoOutputLimit), Progress::close);
    EXPECT_EQ(handler.close_reason(), "client quit");

    handler.feed("SET other v\n");
    EXPECT_EQ(handler.process(out, kNoOutputLimit), Progress::close);
    EXPECT_EQ(out, "BYE\r\n");
    EXPECT_EQ(store.size(), 0U) << "requests after QUIT must not be executed";
}

TEST(ConnectionHandler, OutputIsProducedInBoundedBatches) {
    cppkv::KvStore store;
    cppkv::CommandDispatcher dispatcher{store};
    cppkv::ConnectionHandler handler{dispatcher, 4096};

    const std::string value(1000, 'x');
    std::string input = "SET k " + value + "\n";
    constexpr int kGets = 100;
    for (int i = 0; i < kGets; ++i) {
        input += "GET k\n";
    }
    handler.feed(input);

    constexpr std::size_t kLimit = 4096;
    const std::string one_value = "VALUE 1000 " + value + "\r\n";
    std::string all_output;
    int batches = 0;
    for (;;) {
        std::string out;
        const Progress progress = handler.process(out, kLimit);
        ++batches;
        // A batch may overshoot the soft limit by at most one response.
        EXPECT_LT(out.size(), kLimit + one_value.size());
        all_output += out;
        if (progress == Progress::need_input) {
            break;
        }
        ASSERT_EQ(progress, Progress::output_ready);
        ASSERT_LT(batches, 1000) << "no progress";
    }

    std::string expected = "OK\r\n";
    for (int i = 0; i < kGets; ++i) {
        expected += one_value;
    }
    EXPECT_EQ(all_output, expected);
    EXPECT_GT(batches, 20) << "100 KB of output must not be produced in one batch";
}

TEST(ConnectionHandler, ReportsBufferedIncompleteRequest) {
    cppkv::KvStore store;
    cppkv::CommandDispatcher dispatcher{store};
    cppkv::ConnectionHandler handler{dispatcher, 1024};
    std::string out;

    handler.feed("PING\nSET k va");
    EXPECT_EQ(handler.process(out, kNoOutputLimit), Progress::need_input);
    EXPECT_EQ(out, "PONG\r\n");
    EXPECT_EQ(handler.buffered_bytes(), 8U);
}

} // namespace
