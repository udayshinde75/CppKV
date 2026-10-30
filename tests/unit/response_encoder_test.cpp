#include "cppkv/protocol/response_encoder.hpp"

#include <gtest/gtest.h>

#include <string>

using cppkv::ErrorCode;
using cppkv::Response;

namespace {

std::string encode(const Response& response) {
    std::string out;
    cppkv::encode_response(response, out);
    return out;
}

TEST(ResponseEncoder, SimpleResponses) {
    EXPECT_EQ(encode(Response::pong()), "PONG\r\n");
    EXPECT_EQ(encode(Response::ok()), "OK\r\n");
    EXPECT_EQ(encode(Response::nil()), "NIL\r\n");
    EXPECT_EQ(encode(Response::bye()), "BYE\r\n");
}

TEST(ResponseEncoder, Integer) {
    EXPECT_EQ(encode(Response::integer_value(0)), "INT 0\r\n");
    EXPECT_EQ(encode(Response::integer_value(18446744073709551615ULL)), "INT 18446744073709551615\r\n");
}

TEST(ResponseEncoder, ValueCarriesExactByteLength) {
    EXPECT_EQ(encode(Response::value("Frankfurt am Main")), "VALUE 17 Frankfurt am Main\r\n");
    EXPECT_EQ(encode(Response::value("")), "VALUE 0 \r\n");
    using namespace std::string_literals;
    EXPECT_EQ(encode(Response::value("a\0b"s)), "VALUE 3 a\0b\r\n"s);
}

TEST(ResponseEncoder, Errors) {
    EXPECT_EQ(encode(Response::failure(ErrorCode::unknown_command, "unknown command 'FLY'")),
              "ERR UNKNOWN_COMMAND unknown command 'FLY'\r\n");
    EXPECT_EQ(encode(Response::failure(ErrorCode::too_large, "request exceeds 16 bytes")),
              "ERR TOO_LARGE request exceeds 16 bytes\r\n");
}

TEST(ResponseEncoder, AppendsInsteadOfOverwriting) {
    std::string out = "PONG\r\n";
    cppkv::encode_response(Response::ok(), out);
    EXPECT_EQ(out, "PONG\r\nOK\r\n");
}

TEST(Response, ClosesConnection) {
    EXPECT_TRUE(Response::bye().closes_connection());
    EXPECT_TRUE(Response::failure(ErrorCode::too_large, "").closes_connection());
    EXPECT_TRUE(Response::failure(ErrorCode::busy, "").closes_connection());
    EXPECT_TRUE(Response::failure(ErrorCode::internal, "").closes_connection());
    EXPECT_FALSE(Response::failure(ErrorCode::wrong_args, "").closes_connection());
    EXPECT_FALSE(Response::failure(ErrorCode::protocol, "").closes_connection());
    EXPECT_FALSE(Response::ok().closes_connection());
}

} // namespace
