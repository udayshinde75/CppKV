#pragma once

// Vocabulary types of the CppKV text protocol (docs/protocol.md).
// Pure data: no I/O, no store access.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace cppkv {

// Keys: 1..kMaxKeyBytes bytes, each printable ASCII 0x21-0x7E (no space).
inline constexpr std::size_t kMaxKeyBytes = 512;

enum class CommandType { ping, set, get, del, exists, dbsize, quit };

// Upper-case wire name, e.g. "GET".
[[nodiscard]] std::string_view to_string(CommandType command) noexcept;

// A validated request. Owns its key and value: a Request stays valid after the
// receive buffer it was parsed from is reused.
struct Request {
    CommandType command = CommandType::ping;
    std::string key;     // set for SET, GET, DEL, EXISTS
    std::string value;   // set for SET (may be empty)

    friend bool operator==(const Request&, const Request&) = default;
};

enum class ErrorCode { unknown_command, wrong_args, invalid_key, protocol, too_large, busy, internal };

// Wire name, e.g. "WRONG_ARGS".
[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

// Why a request line was rejected before execution.
struct ProtocolError {
    ErrorCode code = ErrorCode::protocol;
    std::string message;   // human-readable; never contains CR or LF

    friend bool operator==(const ProtocolError&, const ProtocolError&) = default;
};

using ParseResult = std::variant<Request, ProtocolError>;

// The outcome of one request, before encoding to bytes.
struct Response {
    enum class Kind { pong, ok, value, nil, integer, bye, error };

    Kind kind = Kind::ok;
    std::string payload;             // value bytes (value) or message (error)
    std::uint64_t integer = 0;       // integer
    ErrorCode error = ErrorCode::internal;   // error

    [[nodiscard]] static Response pong() { return of(Kind::pong); }
    [[nodiscard]] static Response ok() { return of(Kind::ok); }
    [[nodiscard]] static Response nil() { return of(Kind::nil); }
    [[nodiscard]] static Response bye() { return of(Kind::bye); }
    [[nodiscard]] static Response value(std::string bytes) {
        Response r = of(Kind::value);
        r.payload = std::move(bytes);
        return r;
    }
    [[nodiscard]] static Response integer_value(std::uint64_t n) {
        Response r = of(Kind::integer);
        r.integer = n;
        return r;
    }
    [[nodiscard]] static Response failure(ErrorCode code, std::string message) {
        Response r = of(Kind::error);
        r.payload = std::move(message);
        r.error = code;
        return r;
    }
    [[nodiscard]] static Response failure(ProtocolError error) {
        return failure(error.code, std::move(error.message));
    }

    // True if the server closes the connection after sending this response:
    // BYE, and the errors after which the stream cannot be trusted or served.
    [[nodiscard]] bool closes_connection() const noexcept;

    friend bool operator==(const Response&, const Response&) = default;

private:
    // Start from a default-constructed Response (every member has a default)
    // and set only what differs. Brace-initialising with some members left
    // out would trigger -Wmissing-field-initializers.
    [[nodiscard]] static Response of(Kind kind) {
        Response r;
        r.kind = kind;
        return r;
    }
};

} // namespace cppkv
