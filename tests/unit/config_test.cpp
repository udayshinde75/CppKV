#include "cppkv/common/config.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string_view>
#include <utility>
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

TEST(ParseCommandLine, ParsesOperationalOptions) {
    const auto result = parse({"--max-line-bytes", "1024", "--idle-timeout", "0", "--shutdown-timeout", "3",
                               "--log-level", "debug"});
    ASSERT_EQ(result.action, cppkv::CliAction::run_server) << result.error;
    EXPECT_EQ(result.config.max_line_bytes, 1024U);
    EXPECT_EQ(result.config.idle_timeout_seconds, 0U);
    EXPECT_EQ(result.config.shutdown_timeout_seconds, 3U);
    EXPECT_EQ(result.config.log_level, cppkv::LogLevel::debug);
}

TEST(ParseCommandLine, OperationalDefaults) {
    const auto result = parse({});
    EXPECT_EQ(result.config.idle_timeout_seconds, 300U);
    EXPECT_EQ(result.config.shutdown_timeout_seconds, 10U);
    EXPECT_EQ(result.config.log_level, cppkv::LogLevel::info);
}

TEST(ParseCommandLine, RejectsInvalidOperationalOptions) {
    EXPECT_EQ(parse({"--max-line-bytes", "15"}).action, cppkv::CliAction::usage_error);
    EXPECT_EQ(parse({"--max-line-bytes", "8388609"}).action, cppkv::CliAction::usage_error);
    EXPECT_EQ(parse({"--idle-timeout", "86401"}).action, cppkv::CliAction::usage_error);
    EXPECT_EQ(parse({"--log-level", "verbose"}).error,
              "invalid log level 'verbose' (expected debug, info, warn or error)");
}

TEST(ParseCommandLine, ParsesPersistenceOptions) {
    const auto result = parse({"--data-dir", "/tmp/x", "--sync", "never", "--recovery", "truncate"});
    ASSERT_EQ(result.action, cppkv::CliAction::run_server) << result.error;
    EXPECT_EQ(result.config.persistence.data_dir, "/tmp/x");
    EXPECT_EQ(result.config.persistence.sync, cppkv::SyncPolicy::never);
    EXPECT_EQ(result.config.persistence.recovery, cppkv::RecoveryMode::truncate);
    EXPECT_TRUE(result.config.persistence.enabled);
    EXPECT_FALSE(parse({"--in-memory"}).config.persistence.enabled);
    EXPECT_EQ(parse({"--sync", "sometimes"}).action, cppkv::CliAction::usage_error);
    EXPECT_EQ(parse({"--recovery", "lenient"}).action, cppkv::CliAction::usage_error);
    EXPECT_EQ(parse({"--data-dir", ""}).action, cppkv::CliAction::usage_error);
}

// Every accepted spelling of the enumerated options maps to its value; the
// defaults alone would not show that, e.g., "--sync always" is not misparsed.
TEST(ParseCommandLine, EveryEnumeratedValueIsAccepted) {
    using cppkv::LogLevel;
    for (const auto& [text, level] : std::vector<std::pair<std::string_view, LogLevel>>{
             {"debug", LogLevel::debug}, {"info", LogLevel::info}, {"warn", LogLevel::warn}, {"error", LogLevel::error}}) {
        const auto result = parse({"--log-level", "error", "--log-level", text});   // later wins
        ASSERT_EQ(result.action, cppkv::CliAction::run_server) << text;
        EXPECT_EQ(result.config.log_level, level) << text;
    }
    using cppkv::SyncPolicy;
    for (const auto& [text, policy] : std::vector<std::pair<std::string_view, SyncPolicy>>{
             {"always", SyncPolicy::always}, {"interval", SyncPolicy::interval}, {"never", SyncPolicy::never}}) {
        const auto result = parse({"--sync", "never", "--sync", text});
        ASSERT_EQ(result.action, cppkv::CliAction::run_server) << text;
        EXPECT_EQ(result.config.persistence.sync, policy) << text;
        EXPECT_EQ(cppkv::to_string(policy), text);
    }
    EXPECT_EQ(parse({"--recovery", "truncate", "--recovery", "strict"}).config.persistence.recovery,
              cppkv::RecoveryMode::strict);
    EXPECT_EQ(parse({"--shutdown-timeout", "86401"}).action, cppkv::CliAction::usage_error);
}

} // namespace
