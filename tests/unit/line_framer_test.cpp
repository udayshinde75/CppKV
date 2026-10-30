#include "cppkv/protocol/line_framer.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

using Status = cppkv::LineFramer::Status;

namespace {

// Feeds `chunks` one after another and collects every complete line.
// Stops at the first too_large and records it as "<TOO_LARGE>".
std::vector<std::string> frame(const std::vector<std::string>& chunks, std::size_t max_line_bytes = 64) {
    cppkv::LineFramer framer{max_line_bytes};
    std::vector<std::string> lines;
    for (const std::string& chunk : chunks) {
        framer.feed(chunk);
        for (;;) {
            const auto next = framer.next();
            if (next.status == Status::incomplete) {
                break;
            }
            if (next.status == Status::too_large) {
                lines.emplace_back("<TOO_LARGE>");
                return lines;
            }
            lines.emplace_back(next.line);   // copy: the view dies at the next feed()
        }
    }
    return lines;
}

TEST(LineFramer, EmptyFramerHasNothing) {
    cppkv::LineFramer framer{64};
    EXPECT_EQ(framer.next().status, Status::incomplete);
    EXPECT_EQ(framer.buffered_bytes(), 0U);
}

TEST(LineFramer, SingleLineWithLf) {
    EXPECT_EQ(frame({"PING\n"}), (std::vector<std::string>{"PING"}));
}

TEST(LineFramer, CrLfTerminatorIsRemoved) {
    EXPECT_EQ(frame({"PING\r\n"}), (std::vector<std::string>{"PING"}));
}

TEST(LineFramer, OnlyOneCrIsRemoved) {
    // "\r\r\n": the first CR belongs to the line; the parser rejects it later.
    EXPECT_EQ(frame({"PING\r\r\n"}), (std::vector<std::string>{"PING\r"}));
}

TEST(LineFramer, IncompleteLineIsKept) {
    cppkv::LineFramer framer{64};
    framer.feed("SET name hel");
    EXPECT_EQ(framer.next().status, Status::incomplete);
    EXPECT_EQ(framer.buffered_bytes(), 12U);
}

TEST(LineFramer, TheExampleFromTheArchitectureDocument) {
    // One send("SET name hello\nGET name\nDBSIZE\n") received as three reads.
    EXPECT_EQ(frame({"SE", "T name hel", "lo\nGET name\nDB", "SIZE\n"}),
              (std::vector<std::string>{"SET name hello", "GET name", "DBSIZE"}));
}

TEST(LineFramer, CrAndLfInDifferentChunks) {
    EXPECT_EQ(frame({"PING\r", "\n"}), (std::vector<std::string>{"PING"}));
}

TEST(LineFramer, EmptyLinesAreReturnedAsEmpty) {
    // Ignoring empty lines is a protocol decision taken by the caller.
    EXPECT_EQ(frame({"\n\r\nPING\n"}), (std::vector<std::string>{"", "", "PING"}));
}

TEST(LineFramer, EveryTwoWaySplitGivesTheSameLines) {
    // Deterministic substitute for "TCP may split anywhere": try them all.
    const std::string stream = "SET a 1\r\nGET a\nSET b hello world\r\n\nDBSIZE\n";
    const std::vector<std::string> expected = frame({stream});
    ASSERT_EQ(expected.size(), 5U);
    for (std::size_t i = 0; i <= stream.size(); ++i) {
        EXPECT_EQ(frame({stream.substr(0, i), stream.substr(i)}), expected) << "split at " << i;
    }
}

TEST(LineFramer, ByteByByteGivesTheSameLines) {
    const std::string stream = "SET a 1\r\nGET a\nSET b hello world\r\n\nDBSIZE\n";
    std::vector<std::string> bytes;
    for (const char c : stream) {
        bytes.emplace_back(1, c);
    }
    EXPECT_EQ(frame(bytes), frame({stream}));
}

TEST(LineFramer, ViewsStayValidUntilNextFeed) {
    cppkv::LineFramer framer{64};
    framer.feed("one\ntwo\n");
    const auto first = framer.next();
    const auto second = framer.next();
    ASSERT_EQ(first.status, Status::line);
    ASSERT_EQ(second.status, Status::line);
    // Both views are still valid: next() never modifies the buffer contents.
    EXPECT_EQ(first.line, "one");
    EXPECT_EQ(second.line, "two");
}

// --- Size limit (max_line_bytes = 8) -----------------------------------------

TEST(LineFramer, LineAtLimitIsAccepted) {
    EXPECT_EQ(frame({"12345678\n"}, 8), (std::vector<std::string>{"12345678"}));
    EXPECT_EQ(frame({"12345678\r\n"}, 8), (std::vector<std::string>{"12345678"}));
}

TEST(LineFramer, TerminatedLineOverLimitIsTooLarge) {
    EXPECT_EQ(frame({"123456789\n"}, 8), (std::vector<std::string>{"<TOO_LARGE>"}));
}

TEST(LineFramer, UnterminatedLineIsRejectedAsSoonAsItCannotFit) {
    cppkv::LineFramer framer{8};
    framer.feed("12345678");   // 8 bytes: could still end with "\n"
    EXPECT_EQ(framer.next().status, Status::incomplete);
    framer.feed("9");          // 9 bytes and not '\r': can never be valid
    EXPECT_EQ(framer.next().status, Status::too_large);
}

TEST(LineFramer, CrAfterLimitWaitsForLf) {
    cppkv::LineFramer framer{8};
    framer.feed("12345678\r");   // 9 bytes, but may be "12345678\r\n"
    EXPECT_EQ(framer.next().status, Status::incomplete);
    framer.feed("\n");
    const auto next = framer.next();
    ASSERT_EQ(next.status, Status::line);
    EXPECT_EQ(next.line, "12345678");
}

TEST(LineFramer, CompleteLinesBeforeOversizedOneAreStillReturned) {
    EXPECT_EQ(frame({"PING\nPING\n123456789"}, 8), (std::vector<std::string>{"PING", "PING", "<TOO_LARGE>"}));
}

TEST(LineFramer, TooLargeIsSticky) {
    cppkv::LineFramer framer{8};
    framer.feed("123456789\n");
    EXPECT_EQ(framer.next().status, Status::too_large);
    framer.feed("PING\n");   // ignored: the stream cannot be resynchronised
    EXPECT_EQ(framer.next().status, Status::too_large);
}

TEST(LineFramer, BufferStaysBoundedForEndlessUnterminatedInput) {
    cppkv::LineFramer framer{8};
    for (int i = 0; i < 100; ++i) {
        framer.feed("x");
        if (framer.next().status == Status::too_large) {
            EXPECT_EQ(i, 8) << "rejected after 9 bytes";
            return;
        }
    }
    FAIL() << "never rejected";
}

} // namespace
