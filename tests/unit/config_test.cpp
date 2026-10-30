#include "cppkv/common/config.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string_view>
#include <vector>

namespace {

cppkv::CliParseResult parse(std::initializer_list<std::string_view> args) {
    const std::vector<std::string_view> storage{args};
    return cppkv::parse_command_line(storage);
}

TEST(ParseCommandLine, NoArgumentsRunsServerWithDefaults) {
    const auto result = parse({});
    EXPECT_EQ(result.action, cppkv::CliAction::run_server);
    EXPECT_EQ(result.config.bind_address, "127.0.0.1");
    EXPECT_EQ(result.config.port, 7379);
    EXPECT_EQ(result.config.max_line_bytes, 65536U);
    EXPECT_EQ(result.config.io_threads, 4U);
    EXPECT_EQ(result.config.max_connections, 128U);
    EXPECT_TRUE(result.error.empty());
}

TEST(ParseCommandLine, ParsesPortAndBind) {
    const auto result = parse({"--port", "6000", "--bind", "::1"});
    ASSERT_EQ(result.action, cppkv::CliAction::run_server) << result.error;
    EXPECT_EQ(result.config.port, 6000);
    EXPECT_EQ(result.config.bind_address, "::1");
}

TEST(ParseCommandLine, AcceptsPortBoundaries) {
    EXPECT_EQ(parse({"--port", "0"}).config.port, 0);
    EXPECT_EQ(parse({"--port", "65535"}).config.port, 65535);
}

TEST(ParseCommandLine, RejectsInvalidPorts) {
    for (const std::string_view bad : {"65536", "-1", "+80", "80x", "", " 80", "abc", "99999999999999999999"}) {
        const auto result = parse({"--port", bad});
        EXPECT_EQ(result.action, cppkv::CliAction::usage_error) << "accepted port '" << bad << "'";
        EXPECT_NE(result.error.find("invalid port"), std::string::npos) << result.error;
    }
}

TEST(ParseCommandLine, RejectsInvalidBindAddress) {
    const auto result = parse({"--bind", "localhost"});   // a hostname, not an address
    EXPECT_EQ(result.action, cppkv::CliAction::usage_error);
    EXPECT_NE(result.error.find("invalid bind address 'localhost'"), std::string::npos) << result.error;
}

TEST(ParseCommandLine, RejectsOptionWithoutValue) {
    const auto result = parse({"--port"});
    EXPECT_EQ(result.action, cppkv::CliAction::usage_error);
    EXPECT_EQ(result.error, "option '--port' requires a value");
}

TEST(ParseCommandLine, RejectsUnknownOption) {
    const auto result = parse({"--no-such-flag"});
    EXPECT_EQ(result.action, cppkv::CliAction::usage_error);
    EXPECT_EQ(result.error, "unknown option '--no-such-flag'");
}

TEST(ParseCommandLine, VersionAndHelpWinImmediately) {
    EXPECT_EQ(parse({"--version"}).action, cppkv::CliAction::print_version);
    EXPECT_EQ(parse({"--help"}).action, cppkv::CliAction::print_help);
    EXPECT_EQ(parse({"-h"}).action, cppkv::CliAction::print_help);
    EXPECT_EQ(parse({"--port", "1", "--version", "--bogus"}).action, cppkv::CliAction::print_version);
}

TEST(ParseCommandLine, LaterOptionOverridesEarlier) {
    EXPECT_EQ(parse({"--port", "1", "--port", "2"}).config.port, 2);
}

TEST(ParseCommandLine, ParsesThreadsAndMaxConnections) {
    const auto result = parse({"--threads", "8", "--max-connections", "1000"});
    ASSERT_EQ(result.action, cppkv::CliAction::run_server) << result.error;
    EXPECT_EQ(result.config.io_threads, 8U);
    EXPECT_EQ(result.config.max_connections, 1000U);
}

TEST(ParseCommandLine, RejectsOutOfRangeThreadsAndConnections) {
    for (const std::string_view bad : {"0", "65", "-1", "x"}) {
        EXPECT_EQ(parse({"--threads", bad}).action, cppkv::CliAction::usage_error) << bad;
    }
    for (const std::string_view bad : {"0", "10001", "1e3"}) {
        EXPECT_EQ(parse({"--max-connections", bad}).action, cppkv::CliAction::usage_error) << bad;
    }
    EXPECT_EQ(parse({"--threads", "0"}).error, "invalid thread count '0' (expected 1-64)");
    EXPECT_EQ(parse({"--threads", "1"}).config.io_threads, 1U);
    EXPECT_EQ(parse({"--max-connections", "10000"}).config.max_connections, 10000U);
}

} // namespace
