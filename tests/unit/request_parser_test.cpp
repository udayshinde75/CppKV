#include "cppkv/protocol/request_parser.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <variant>

using cppkv::CommandType;
using cppkv::ErrorCode;
using cppkv::Request;

namespace {

Request parse_ok(std::string_view line) {
    const cppkv::ParseResult result = cppkv::parse_request(line);
    if (const auto* error = std::get_if<cppkv::ProtocolError>(&result)) {
        ADD_FAILURE() << "unexpected error for '" << line << "': " << error->message;
        return {};
    }
    return std::get<Request>(result);
}

ErrorCode parse_error(std::string_view line) {
    const cppkv::ParseResult result = cppkv::parse_request(line);
    if (std::holds_alternative<Request>(result)) {
        ADD_FAILURE() << "'" << line << "' was accepted";
        return ErrorCode::internal;
    }
    return std::get<cppkv::ProtocolError>(result).code;
}

std::string error_message(std::string_view line) {
    return std::get<cppkv::ProtocolError>(cppkv::parse_request(line)).message;
}

// --- Valid requests ----------------------------------------------------------

TEST(RequestParser, CommandsWithoutArguments) {
    EXPECT_EQ(parse_ok("PING"), (Request{CommandType::ping, "", ""}));
    EXPECT_EQ(parse_ok("DBSIZE"), (Request{CommandType::dbsize, "", ""}));
    EXPECT_EQ(parse_ok("QUIT"), (Request{CommandType::quit, "", ""}));
}

TEST(RequestParser, CommandsWithKey) {
    EXPECT_EQ(parse_ok("GET user:42"), (Request{CommandType::get, "user:42", ""}));
    EXPECT_EQ(parse_ok("DEL k"), (Request{CommandType::del, "k", ""}));
    EXPECT_EQ(parse_ok("EXISTS k"), (Request{CommandType::exists, "k", ""}));
}

TEST(RequestParser, CommandNamesAreCaseInsensitiveKeysAreNot) {
    EXPECT_EQ(parse_ok("get Key"), (Request{CommandType::get, "Key", ""}));
    EXPECT_EQ(parse_ok("dbSize").command, CommandType::dbsize);
}

TEST(RequestParser, SetValueIsTheRestOfTheLineVerbatim) {
    EXPECT_EQ(parse_ok("SET city Frankfurt am Main"), (Request{CommandType::set, "city", "Frankfurt am Main"}));
    EXPECT_EQ(parse_ok("SET k  two  spaces  "), (Request{CommandType::set, "k", " two  spaces  "}));
}

TEST(RequestParser, SetWithTrailingSeparatorStoresEmptyValue) {
    EXPECT_EQ(parse_ok("SET k "), (Request{CommandType::set, "k", ""}));
}

TEST(RequestParser, SetValueMayContainNulAndHighBytes) {
    using namespace std::string_literals;
    EXPECT_EQ(parse_ok("SET k a\0b\xff"s), (Request{CommandType::set, "k", "a\0b\xff"s}));
}

TEST(RequestParser, KeyAtMaximumLengthIsAccepted) {
    const std::string key(cppkv::kMaxKeyBytes, 'k');
    EXPECT_EQ(parse_ok("GET " + key).key, key);
}

// --- Errors ------------------------------------------------------------------

TEST(RequestParser, UnknownCommand) {
    EXPECT_EQ(parse_error("FLY away"), ErrorCode::unknown_command);
    EXPECT_EQ(error_message("FLY away"), "unknown command 'FLY'");
    EXPECT_EQ(parse_error("PINGPONG"), ErrorCode::unknown_command);
}

TEST(RequestParser, UnknownCommandEchoIsSanitisedAndTruncated) {
    EXPECT_EQ(error_message("A\x01" "B"), "unknown command 'A?B'");
    EXPECT_EQ(error_message(std::string(40, 'X')), "unknown command '" + std::string(32, 'X') + "...'");
}

TEST(RequestParser, WrongArgumentCounts) {
    for (const std::string_view line : {"PING x", "DBSIZE x", "QUIT now", "GET", "GET a b", "DEL", "EXISTS a b c",
                                        "SET", "SET k"}) {
        EXPECT_EQ(parse_error(line), ErrorCode::wrong_args) << line;
    }
    EXPECT_EQ(error_message("GET"), "'GET' takes exactly one argument: <key>");
    EXPECT_EQ(error_message("SET k"), "'SET' takes a key and a value");
    EXPECT_EQ(error_message("PING x"), "'PING' takes no arguments");
}

TEST(RequestParser, EmptyTokensAreProtocolErrors) {
    for (const std::string_view line : {" PING", "PING ", "GET  k", "GET k ", "SET  k v", "DBSIZE  "}) {
        EXPECT_EQ(parse_error(line), ErrorCode::protocol) << "'" << line << "'";
    }
}

TEST(RequestParser, BareCrAnywhereIsAProtocolError) {
    EXPECT_EQ(parse_error("PING\r"), ErrorCode::protocol);
    EXPECT_EQ(parse_error("SET k va\rlue"), ErrorCode::protocol);
    EXPECT_EQ(error_message("GE\rT k"), "bare CR inside request");
}

TEST(RequestParser, InvalidKeys) {
    using namespace std::string_literals;
    const std::string too_long(cppkv::kMaxKeyBytes + 1, 'k');
    for (const std::string& line :
         {"GET " + too_long, "SET " + too_long + " v", "GET k\x01"s, "GET k\x7f"s, "GET k\xc3\xa4"s, "GET \0"s}) {
        EXPECT_EQ(parse_error(line), ErrorCode::invalid_key) << line.size() << " bytes";
    }
}

TEST(RequestParser, CheckOrderIsDocumented) {
    // CR is checked before everything else, unknown command before arguments,
    // empty tokens before argument count.
    EXPECT_EQ(parse_error("FLY\r"), ErrorCode::protocol);
    EXPECT_EQ(parse_error("FLY  a"), ErrorCode::unknown_command);
    EXPECT_EQ(parse_error("GET a b "), ErrorCode::protocol);
}

TEST(IsValidKey, Boundaries) {
    EXPECT_TRUE(cppkv::is_valid_key("!"));    // 0x21, lowest allowed
    EXPECT_TRUE(cppkv::is_valid_key("~"));    // 0x7E, highest allowed
    EXPECT_FALSE(cppkv::is_valid_key(" "));   // 0x20
    EXPECT_FALSE(cppkv::is_valid_key(""));
}

} // namespace
