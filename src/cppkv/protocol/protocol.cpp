#include "cppkv/protocol/protocol.hpp"

namespace cppkv {

std::string_view to_string(CommandType command) noexcept {
    switch (command) {
    case CommandType::ping: return "PING";
    case CommandType::set: return "SET";
    case CommandType::get: return "GET";
    case CommandType::del: return "DEL";
    case CommandType::exists: return "EXISTS";
    case CommandType::dbsize: return "DBSIZE";
    case CommandType::quit: return "QUIT";
    }
    return "?";   // unreachable for valid enum values
}

std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::unknown_command: return "UNKNOWN_COMMAND";
    case ErrorCode::wrong_args: return "WRONG_ARGS";
    case ErrorCode::invalid_key: return "INVALID_KEY";
    case ErrorCode::protocol: return "PROTOCOL";
    case ErrorCode::too_large: return "TOO_LARGE";
    case ErrorCode::busy: return "BUSY";
    case ErrorCode::internal: return "INTERNAL";
    }
    return "INTERNAL";   // unreachable for valid enum values
}

bool Response::closes_connection() const noexcept {
    if (kind == Kind::bye) {
        return true;
    }
    if (kind != Kind::error) {
        return false;
    }
    // TOO_LARGE: the rest of the oversized line is still arriving, and there
    //            is no reliable way to find where the next request starts.
    // BUSY / INTERNAL: the server will not (or cannot) serve this client.
    // The other errors concern one well-framed line; the next line is fine.
    return error == ErrorCode::too_large || error == ErrorCode::busy || error == ErrorCode::internal;
}

} // namespace cppkv
